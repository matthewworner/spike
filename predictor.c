// predictor.c
// Weight block prefetch predictor — implementation
//
// Core insight: transformer block access patterns are
// not random. They have strong sequential structure
// (layer N always precedes layer N+1) and expert clustering
// (if expert A fires, expert B fires 70-90% of the time).
//
// A Markov chain over observed transitions captures both.
// Order-2 captures more variance from MoE routing.
#define _POSIX_C_SOURCE 200809L

#include "predictor.h"
#include "tracer.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

// ── init ──────────────────────────────────────────────────

int predictor_init(Predictor* p, uint32_t n_blocks,
                   PredStrategy strategy) {
    memset(p, 0, sizeof(Predictor));

    p->n_blocks  = n_blocks;
    p->strategy  = strategy;
    p->history_len = 0;

    for (int i = 0; i < PRED_HISTORY; i++)
        p->history[i] = UINT32_MAX;  // sentinel: empty

    // order-1 matrix: n_blocks × n_blocks
    size_t m1_size = (size_t)n_blocks * n_blocks * sizeof(float);
    p->matrix1 = calloc(1, m1_size);
    if (!p->matrix1) {
        fprintf(stderr, "predictor: out of memory (matrix1)\n");
        return -1;
    }

    // order-2 matrix: n_blocks × n_blocks × n_blocks
    // only allocated for PRED_LOOKAHEAD — it's n³ floats
    if (strategy == PRED_LOOKAHEAD) {
        size_t m2_size = (size_t)n_blocks * n_blocks
                         * n_blocks * sizeof(float);
        p->matrix2 = calloc(1, m2_size);
        if (!p->matrix2) {
            fprintf(stderr,
                    "predictor: out of memory (matrix2 — "
                    "try PRED_MARKOV for large n_blocks)\n");
            free(p->matrix1);
            p->matrix1 = NULL;
            return -1;
        }
    }

    return 0;
}

// ── load: build matrices from trace ──────────────────────
//
// Reads a .wptr file and accumulates transition counts.
// Normalises to probabilities at the end.
// Can be called multiple times to merge multiple traces.

int predictor_load(Predictor* p, const char* wptr_path) {
    uint32_t N = p->n_blocks;

    // we accumulate raw counts then normalise
    // use uint64 to avoid overflow on large traces
    uint64_t* counts1 = calloc((size_t)N * N, sizeof(uint64_t));
    uint64_t* totals1 = calloc(N,              sizeof(uint64_t));

    uint64_t* counts2 = NULL;
    uint64_t* totals2 = NULL;

    if (p->strategy == PRED_LOOKAHEAD) {
        counts2 = calloc((size_t)N * N * N, sizeof(uint64_t));
        totals2 = calloc((size_t)N * N,     sizeof(uint64_t));
        if (!counts2 || !totals2) {
            fprintf(stderr, "predictor: out of memory (counts2)\n");
            free(counts1); free(totals1);
            free(counts2); free(totals2);
            return -1;
        }
    }

    if (!counts1 || !totals1) {
        fprintf(stderr, "predictor: out of memory (counts1)\n");
        free(counts1); free(totals1);
        return -1;
    }

    // open trace
    FILE* f = fopen(wptr_path, "rb");
    if (!f) {
        fprintf(stderr, "predictor: cannot open %s\n", wptr_path);
        free(counts1); free(totals1);
        free(counts2); free(totals2);
        return -1;
    }

    // skip header
    fseek(f, WPTR_HEADER_SIZE, SEEK_SET);

    #define LOAD_CHUNK 8192
    TraceEntry* chunk = malloc(sizeof(TraceEntry) * LOAD_CHUNK);
    if (!chunk) {
        fclose(f);
        free(counts1); free(totals1);
        free(counts2); free(totals2);
        return -1;
    }

    uint32_t prev  = UINT32_MAX;   // block at t-1
    uint32_t prev2 = UINT32_MAX;   // block at t-2

    uint64_t entries_read = 0;

    while (!feof(f)) {
        size_t got = fread(chunk, sizeof(TraceEntry),
                           LOAD_CHUNK, f);
        if (got == 0) break;

        for (size_t i = 0; i < got; i++) {
            uint32_t cur = chunk[i].block_id;
            if (cur >= N) continue;

            // order-1: P(cur | prev)
            if (prev != UINT32_MAX && prev != cur) {
                counts1[prev * N + cur]++;
                totals1[prev]++;
            }

            // order-2: P(cur | prev2, prev)
            if (p->strategy == PRED_LOOKAHEAD
                && prev  != UINT32_MAX
                && prev2 != UINT32_MAX
                && prev  != cur) {
                counts2[prev2 * N * N + prev * N + cur]++;
                totals2[prev2 * N + prev]++;
            }

            prev2 = prev;
            prev  = cur;
            entries_read++;
        }
    }

    free(chunk);
    fclose(f);

    printf("predictor: loaded %llu entries from %s\n",
           (unsigned long long)entries_read, wptr_path);

    // normalise order-1 → probabilities
    // merge with any existing matrix (multi-trace accumulation)
    for (uint32_t i = 0; i < N; i++) {
        if (totals1[i] == 0) continue;
        for (uint32_t j = 0; j < N; j++) {
            float new_p = (float)counts1[i * N + j]
                        / (float)totals1[i];
            // weighted average with existing matrix
            // (simple approach: just replace on first load,
            //  average on subsequent loads)
            float old_p = p->matrix1[i * N + j];
            p->matrix1[i * N + j] = (old_p == 0.0f)
                ? new_p
                : (old_p + new_p) * 0.5f;
        }
    }

    // normalise order-2
    if (p->strategy == PRED_LOOKAHEAD && counts2) {
        for (uint32_t i = 0; i < N; i++) {
            for (uint32_t j = 0; j < N; j++) {
                uint64_t tot = totals2[i * N + j];
                if (tot == 0) continue;
                for (uint32_t k = 0; k < N; k++) {
                    float new_p = (float)counts2[i*N*N + j*N + k]
                                / (float)tot;
                    float old_p = p->matrix2[i*N*N + j*N + k];
                    p->matrix2[i*N*N + j*N + k] = (old_p == 0.0f)
                        ? new_p
                        : (old_p + new_p) * 0.5f;
                }
            }
        }
    }

    free(counts1); free(totals1);
    free(counts2); free(totals2);
    return 0;
}

// ── predict ───────────────────────────────────────────────
//
// Called after every block access at inference time.
// Returns up to PREFETCH_MAX candidates, sorted by confidence.
// The caller starts async loads for each candidate immediately.

void predictor_next(Predictor* p, uint32_t current_block,
                    PrefetchList* out) {
    out->count = 0;

    uint32_t N = p->n_blocks;
    if (current_block >= N) return;

    // update history
    p->history[1]  = p->history[0];
    p->history[0]  = current_block;
    if (p->history_len < PRED_HISTORY)
        p->history_len++;

    p->predictions_made++;

    // score each candidate block
    // score = weighted combination of order-1 and order-2
    float* scores = calloc(N, sizeof(float));
    if (!scores) return;

    uint32_t prev  = p->history[0];  // = current_block
    uint32_t prev2 = p->history[1];  // one before

    for (uint32_t j = 0; j < N; j++) {
        if (j == current_block) continue;  // don't prefetch self

        float s1 = p->matrix1[prev * N + j];

        float s2 = 0.0f;
        if (p->strategy == PRED_LOOKAHEAD
            && p->history_len >= 2
            && prev2 != UINT32_MAX) {
            s2 = p->matrix2[prev2 * N * N + prev * N + j];
        }

        // blend: order-2 gets more weight when available
        scores[j] = (p->strategy == PRED_LOOKAHEAD
                     && p->history_len >= 2)
            ? s1 * 0.3f + s2 * 0.7f
            : s1;
    }

    // extract top PREFETCH_MAX candidates by score
    // simple selection sort — N is small (< 100 typically)
    uint8_t used[1024];
    memset(used, 0, sizeof(uint8_t) * (N < 1024 ? N : 1024));

    for (int slot = 0; slot < PREFETCH_MAX; slot++) {
        float    best_score = 0.05f;  // threshold: ignore noise
        uint32_t best_j     = UINT32_MAX;

        for (uint32_t j = 0; j < N; j++) {
            if (used[j]) continue;
            if (scores[j] > best_score) {
                best_score = scores[j];
                best_j     = j;
            }
        }

        if (best_j == UINT32_MAX) break;

        out->block_ids[out->count]  = best_j;
        out->confidence[out->count] = best_score;
        out->count++;
        used[best_j] = 1;
    }

    free(scores);
}

// ── feedback ──────────────────────────────────────────────
//
// Tell the predictor what block actually came next.
// Tracks accuracy and enables future adaptive weighting.

void predictor_feedback(Predictor* p, uint32_t actual_block) {
    // we need the prediction that was made one step ago
    // history[0] is the block that triggered the prediction
    // actual_block is what the GPU just asked for

    // generate the prediction that would have been made
    // from history[1] (the trigger point)
    if (p->history_len < 1) return;

    uint32_t trigger = p->history[1];
    if (trigger == UINT32_MAX) return;

    uint32_t N = p->n_blocks;
    if (actual_block >= N) return;

    // find best prediction from trigger
    float   best_p  = 0.0f;
    uint32_t best_j = UINT32_MAX;
    float   actual_p = p->matrix1[trigger * N + actual_block];

    for (uint32_t j = 0; j < N; j++) {
        if (p->matrix1[trigger * N + j] > best_p) {
            best_p = p->matrix1[trigger * N + j];
            best_j = j;
        }
    }

    if (best_j == actual_block)
        p->top1_correct++;

    // check if actual was in top-2
    float second_p = 0.0f;
    uint32_t second_j = UINT32_MAX;
    for (uint32_t j = 0; j < N; j++) {
        if (j == best_j) continue;
        if (p->matrix1[trigger * N + j] > second_p) {
            second_p = p->matrix1[trigger * N + j];
            second_j = j;
        }
    }

    if (best_j == actual_block || second_j == actual_block)
        p->top2_correct++;

    (void)actual_p;  // available for adaptive learning later
}

// ── stats ─────────────────────────────────────────────────

void predictor_print_stats(const Predictor* p) {
    printf("\n  predictor stats\n");
    printf("  ───────────────────────────────────\n");
    printf("  strategy:        %s\n",
           p->strategy == PRED_MARKOV ? "markov (order-1)"
                                      : "lookahead (order-2)");
    printf("  blocks:          %u\n", p->n_blocks);
    printf("  predictions:     %llu\n",
           (unsigned long long)p->predictions_made);

    if (p->predictions_made > 0) {
        float top1 = (float)p->top1_correct
                   / (float)p->predictions_made * 100.0f;
        float top2 = (float)p->top2_correct
                   / (float)p->predictions_made * 100.0f;
        printf("  top-1 accuracy:  %.1f%%\n", top1);
        printf("  top-2 accuracy:  %.1f%%\n", top2);
        printf("\n");

        if (top1 > 80.0f)
            printf("  verdict: strong — prefetch almost always ready\n");
        else if (top1 > 60.0f)
            printf("  verdict: good — most prefetches will land\n");
        else if (top1 > 40.0f)
            printf("  verdict: fair — worth running, room to improve\n");
        else
            printf("  verdict: weak — need more trace data\n");
    }
    printf("\n");
}

// ── free ──────────────────────────────────────────────────

void predictor_free(Predictor* p) {
    free(p->matrix1);
    free(p->matrix2);
    p->matrix1 = NULL;
    p->matrix2 = NULL;
}

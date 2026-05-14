// tracer.c
// Weight block access tracer — implementation
#define _POSIX_C_SOURCE 200809L
//
// Three responsibilities:
//   1. Write .wptr trace files efficiently (buffered I/O)
//   2. Survive partial writes (crash-safe format)
//   3. Analyse completed traces (transitions, hit rate)

#include "tracer.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#ifndef CLOCK_MONOTONIC_RAW
#  define CLOCK_MONOTONIC_RAW CLOCK_MONOTONIC
#endif

// ── internal helpers ──────────────────────────────────────

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static int write_header(FILE* f, const char* model_id) {
    uint8_t header[WPTR_HEADER_SIZE];
    memset(header, 0, sizeof(header));

    // magic
    memcpy(header, WPTR_MAGIC, 4);

    // version
    header[4] = WPTR_VERSION;

    // model id — null padded to 32 bytes
    strncpy((char*)(header + 5), model_id, WPTR_MODEL_ID_LEN - 1);

    // bytes 37-47: reserved, already zero

    size_t written = fwrite(header, 1, WPTR_HEADER_SIZE, f);
    return (written == WPTR_HEADER_SIZE) ? 0 : -1;
}

// ── open ──────────────────────────────────────────────────

int tracer_open(Tracer* tr, const char* path,
                const char* model_id) {
    memset(tr, 0, sizeof(Tracer));

    tr->file = fopen(path, "wb");
    if (!tr->file) {
        fprintf(stderr, "tracer: cannot open %s: %s\n",
                path, strerror(errno));
        return -1;
    }

    // write file header immediately
    if (write_header(tr->file, model_id) != 0) {
        fprintf(stderr, "tracer: failed to write header\n");
        fclose(tr->file);
        tr->file = NULL;
        return -1;
    }

    // allocate entry buffer
    tr->buf_cap   = TRACER_FLUSH_EVERY;
    tr->buf       = malloc(sizeof(TraceEntry) * tr->buf_cap);
    if (!tr->buf) {
        fprintf(stderr, "tracer: out of memory\n");
        fclose(tr->file);
        tr->file = NULL;
        return -1;
    }

    strncpy(tr->model_id, model_id, WPTR_MODEL_ID_LEN - 1);

    return 0;
}

// ── record ────────────────────────────────────────────────
//
// Hot path. Called for every weight block access.
// Must be fast — no syscalls until buffer is full.

void tracer_record(Tracer* tr,
                   uint32_t   block_id,
                   uint32_t   layer_id,
                   uint32_t   expert_id,
                   uint32_t   token_pos,
                   AccessType access_type,
                   uint8_t    was_hot) {

    if (!tr->file) return;

    TraceEntry* e = &tr->buf[tr->buf_count];

    e->block_id    = block_id;
    e->layer_id    = layer_id;
    e->expert_id   = expert_id;
    e->token_pos   = token_pos;
    e->timestamp_ns= now_ns();
    e->access_type = (uint8_t)access_type;
    e->was_hot     = was_hot;
    e->reserved[0] = 0;
    e->reserved[1] = 0;

    tr->buf_count++;
    tr->total++;

    if (was_hot) tr->total_hot++;
    else         tr->total_miss++;

    // flush when buffer is full
    if (tr->buf_count >= tr->buf_cap) {
        tracer_flush(tr);
    }
}

// ── flush ─────────────────────────────────────────────────

int tracer_flush(Tracer* tr) {
    if (!tr->file || tr->buf_count == 0) return 0;

    size_t n = fwrite(tr->buf,
                      sizeof(TraceEntry),
                      tr->buf_count,
                      tr->file);

    if (n != tr->buf_count) {
        fprintf(stderr, "tracer: flush wrote %zu of %u entries\n",
                n, tr->buf_count);
        return -1;
    }

    // fflush so partial traces are readable on crash
    fflush(tr->file);

    tr->buf_count = 0;
    return 0;
}

// ── close ─────────────────────────────────────────────────

void tracer_close(Tracer* tr) {
    if (!tr->file) return;

    tracer_flush(tr);
    fclose(tr->file);
    tr->file = NULL;

    free(tr->buf);
    tr->buf = NULL;

    // one-line summary
    float hot_rate = (tr->total > 0)
        ? (float)tr->total_hot / (float)tr->total * 100.0f
        : 0.0f;

    printf("tracer closed — %llu entries | "
           "hot %.1f%% | miss %.1f%%\n",
           (unsigned long long)tr->total,
           hot_rate,
           100.0f - hot_rate);
}

// ── analysis: summarise ───────────────────────────────────

int tracer_summarise(const char* path, TraceSummary* out) {
    memset(out, 0, sizeof(TraceSummary));

    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "tracer: cannot open %s: %s\n",
                path, strerror(errno));
        return -1;
    }

    // read and validate header
    uint8_t header[WPTR_HEADER_SIZE];
    if (fread(header, 1, WPTR_HEADER_SIZE, f) != WPTR_HEADER_SIZE) {
        fprintf(stderr, "tracer: short header in %s\n", path);
        fclose(f);
        return -1;
    }

    if (memcmp(header, WPTR_MAGIC, 4) != 0) {
        fprintf(stderr, "tracer: bad magic in %s\n", path);
        fclose(f);
        return -1;
    }

    if (header[4] != WPTR_VERSION) {
        fprintf(stderr, "tracer: unsupported version %d in %s\n",
                header[4], path);
        fclose(f);
        return -1;
    }

    strncpy(out->model_id, (char*)(header + 5),
            WPTR_MODEL_ID_LEN - 1);

    // track unique values with simple bitsets
    // supports up to 1024 blocks, 256 layers, 512 experts
    uint8_t seen_blocks[128];   // 128 * 8 = 1024 bits
    uint8_t seen_layers[32];    // 32  * 8 = 256 bits
    uint8_t seen_experts[64];   // 64  * 8 = 512 bits
    memset(seen_blocks,  0, sizeof(seen_blocks));
    memset(seen_layers,  0, sizeof(seen_layers));
    memset(seen_experts, 0, sizeof(seen_experts));

    #define BITSET_SET(arr, idx) \
        ((arr)[(idx) / 8] |= (1u << ((idx) % 8)))
    #define BITSET_COUNT(arr, n) bitset_count(arr, n)

    // read entries in chunks for speed
    #define READ_CHUNK 8192
    TraceEntry* chunk = malloc(sizeof(TraceEntry) * READ_CHUNK);
    if (!chunk) { fclose(f); return -1; }

    uint32_t max_token = 0;

    while (!feof(f)) {
        size_t got = fread(chunk, sizeof(TraceEntry),
                           READ_CHUNK, f);
        if (got == 0) break;

        for (size_t i = 0; i < got; i++) {
            TraceEntry* e = &chunk[i];

            out->total_accesses++;
            if (e->was_hot) out->hot_hits++;
            else            out->cold_misses++;

            if (e->block_id  < 1024) BITSET_SET(seen_blocks,  e->block_id);
            if (e->layer_id  < 256)  BITSET_SET(seen_layers,  e->layer_id);
            if (e->expert_id < 512)  BITSET_SET(seen_experts, e->expert_id);

            if (e->token_pos > max_token)
                max_token = e->token_pos;
        }
    }

    free(chunk);
    fclose(f);

    // count unique values from bitsets
    for (int i = 0; i < 1024; i++)
        if (seen_blocks[i/8]  & (1u << (i%8))) out->unique_blocks++;
    for (int i = 0; i < 256; i++)
        if (seen_layers[i/8]  & (1u << (i%8))) out->unique_layers++;
    for (int i = 0; i < 512; i++)
        if (seen_experts[i/8] & (1u << (i%8))) out->unique_experts++;

    out->token_count = max_token + 1;
    out->hot_rate    = (out->total_accesses > 0)
        ? (float)out->hot_hits / (float)out->total_accesses
        : 0.0f;

    return 0;
}

// ── analysis: print summary ───────────────────────────────

void tracer_print_summary(const TraceSummary* s) {
    printf("\n");
    printf("  model:           %s\n",    s->model_id);
    printf("  tokens:          %u\n",    s->token_count);
    printf("  total accesses:  %llu\n",
           (unsigned long long)s->total_accesses);
    printf("  unique blocks:   %u\n",    s->unique_blocks);
    printf("  unique layers:   %u\n",    s->unique_layers);
    printf("  unique experts:  %u\n",    s->unique_experts);
    printf("\n");
    printf("  hot (in RAM):    %llu  (%.1f%%)\n",
           (unsigned long long)s->hot_hits,
           s->hot_rate * 100.0f);
    printf("  cold (miss):     %llu  (%.1f%%)\n",
           (unsigned long long)s->cold_misses,
           (1.0f - s->hot_rate) * 100.0f);
    printf("\n");

    // qualitative assessment
    if (s->hot_rate > 0.85f)
        printf("  assessment: excellent — predictor working well\n");
    else if (s->hot_rate > 0.60f)
        printf("  assessment: good — predictor has headroom to improve\n");
    else if (s->hot_rate > 0.30f)
        printf("  assessment: baseline — prediction not yet helping\n");
    else
        printf("  assessment: cold — no prediction active\n");
    printf("\n");
}

// ── analysis: transition matrix ──────────────────────────
//
// Reads the trace and builds:
//   matrix[i * n_blocks + j] = P(j is next | i was last)
//
// This IS the first predictor.
// Feed it a block_id, get back probabilities for all next blocks.
// Prefetch the top-2 candidates while GPU uses current block.

int tracer_build_transitions(const char* path,
                              uint32_t    n_blocks,
                              float*      out_matrix) {

    // count matrix: how often did block j follow block i?
    uint64_t* counts = calloc(n_blocks * n_blocks,
                              sizeof(uint64_t));
    uint64_t* row_totals = calloc(n_blocks, sizeof(uint64_t));

    if (!counts || !row_totals) {
        free(counts);
        free(row_totals);
        fprintf(stderr, "tracer: out of memory for transition matrix\n");
        return -1;
    }

    FILE* f = fopen(path, "rb");
    if (!f) {
        free(counts);
        free(row_totals);
        return -1;
    }

    // skip header
    fseek(f, WPTR_HEADER_SIZE, SEEK_SET);

    #define READ_CHUNK 8192
    TraceEntry* chunk = malloc(sizeof(TraceEntry) * READ_CHUNK);
    if (!chunk) {
        free(counts); free(row_totals);
        fclose(f);
        return -1;
    }

    uint32_t prev_block = UINT32_MAX;  // sentinel: no previous

    while (!feof(f)) {
        size_t got = fread(chunk, sizeof(TraceEntry),
                           READ_CHUNK, f);
        if (got == 0) break;

        for (size_t i = 0; i < got; i++) {
            uint32_t cur = chunk[i].block_id;
            if (cur >= n_blocks) continue;

            if (prev_block != UINT32_MAX
                && prev_block != cur) {          // skip self-loops
                counts[prev_block * n_blocks + cur]++;
                row_totals[prev_block]++;
            }
            prev_block = cur;
        }
    }

    free(chunk);
    fclose(f);

    // normalise counts → probabilities
    for (uint32_t i = 0; i < n_blocks; i++) {
        uint64_t total = row_totals[i];
        for (uint32_t j = 0; j < n_blocks; j++) {
            out_matrix[i * n_blocks + j] = (total > 0)
                ? (float)counts[i * n_blocks + j] / (float)total
                : 0.0f;
        }
    }

    // print top transitions to stdout
    printf("  top transitions (block → block : probability)\n");
    printf("  ─────────────────────────────────────────────\n");

    int printed = 0;
    for (uint32_t i = 0; i < n_blocks && printed < 10; i++) {
        // find max in row i
        float   best_p   = 0.0f;
        uint32_t best_j  = 0;
        for (uint32_t j = 0; j < n_blocks; j++) {
            float p = out_matrix[i * n_blocks + j];
            if (p > best_p) { best_p = p; best_j = j; }
        }
        if (best_p > 0.5f) {
            printf("  block %2u → block %2u : %.1f%%\n",
                   i, best_j, best_p * 100.0f);
            printed++;
        }
    }
    printf("\n");

    free(counts);
    free(row_totals);
    return 0;
}

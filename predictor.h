// predictor.h
// Weight block prefetch predictor for Spike
//
// Takes a transition matrix built from traces and predicts
// which blocks to prefetch before the GPU asks for them.
//
// Two prediction strategies, selectable at runtime:
//
//   PRED_MARKOV   — single-step: P(next | current)
//                   fast, works well for sequential layers
//
//   PRED_LOOKAHEAD — multi-step: P(next | last N accesses)
//                   slower to build, better for MoE variance
//
// Usage:
//   Predictor p;
//   predictor_init(&p, n_blocks, PRED_LOOKAHEAD);
//   predictor_load(&p, "synthetic.wptr");   // learn from traces
//
//   // at inference time, after every block access:
//   PrefetchList pl;
//   predictor_next(&p, current_block, &pl);
//   for (int i = 0; i < pl.count; i++)
//       prefetch_async(pl.block_ids[i]);    // your async loader

#ifndef PREDICTOR_H
#define PREDICTOR_H

#include <stdint.h>

// ── strategy ──────────────────────────────────────────────

typedef enum {
    PRED_MARKOV    = 0,  // P(j | i)         — order-1 chain
    PRED_LOOKAHEAD = 1,  // P(j | i, i-1)    — order-2 chain
} PredStrategy;

// ── prefetch list ─────────────────────────────────────────
// Returned by predictor_next.
// block_ids ordered by confidence — prefetch [0] first.

#define PREFETCH_MAX 4

typedef struct {
    uint32_t block_ids[PREFETCH_MAX];
    float    confidence[PREFETCH_MAX];  // 0.0 – 1.0
    int      count;
} PrefetchList;

// ── predictor state ───────────────────────────────────────

#define PRED_HISTORY 2   // how many past blocks we track

typedef struct {
    // config
    uint32_t     n_blocks;
    PredStrategy strategy;

    // order-1 matrix: P(j | i)
    // matrix1[i * n_blocks + j] = probability
    float*       matrix1;

    // order-2 matrix: P(j | i, i-1)
    // matrix2[(i-1) * n_blocks*n_blocks + i * n_blocks + j]
    float*       matrix2;

    // runtime history — last PRED_HISTORY blocks seen
    uint32_t     history[PRED_HISTORY];
    int          history_len;

    // stats
    uint64_t     predictions_made;
    uint64_t     top1_correct;     // top-1 prediction was right
    uint64_t     top2_correct;     // correct was in top-2
} Predictor;

// ── API ───────────────────────────────────────────────────

// Initialise predictor. Call before predictor_load.
// n_blocks: total number of weight blocks in the model.
// Returns 0 on success, -1 on allocation failure.
int  predictor_init(Predictor* p, uint32_t n_blocks,
                    PredStrategy strategy);

// Learn transition probabilities from a .wptr trace file.
// Can be called multiple times to accumulate across traces.
// Returns 0 on success.
int  predictor_load(Predictor* p, const char* wptr_path);

// Predict next blocks to prefetch given current block access.
// Call this immediately after every block access at inference.
// out: filled with up to PREFETCH_MAX candidates, best first.
void predictor_next(Predictor* p, uint32_t current_block,
                    PrefetchList* out);

// Tell the predictor whether its top-1 prediction was right.
// Used to compute accuracy stats and optionally adapt weights.
void predictor_feedback(Predictor* p, uint32_t actual_block);

// Print accuracy report to stdout.
void predictor_print_stats(const Predictor* p);

// Free all allocated memory.
void predictor_free(Predictor* p);

#endif // PREDICTOR_H

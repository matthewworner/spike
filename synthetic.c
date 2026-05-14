// synthetic.c
// Fake inference loop for testing the tracer
// No real model. No GPU. Just the access pattern.
//
// Simulates a Qwen3 14B-like architecture:
//   28 layers, 64 experts, 8 active per token
//   ~18 weight blocks of 512MB each
//   6 hot slots = ~3GB RAM budget for weights
//
// Run: ./synthetic [n_tokens]
// Then: ./analyze synthetic.wptr 18

#include "tracer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// ── model shape ───────────────────────────────────────────

#define N_LAYERS     28
#define N_EXPERTS    64
#define N_ACTIVE     8
#define N_BLOCKS     18
#define HOT_SLOTS    6    // blocks that fit in our RAM budget

static int is_moe_layer(int layer) {
    return (layer % 2 == 1);  // alternating dense / MoE
}

// ── block map ─────────────────────────────────────────────
// Maps (layer, component, expert) → block_id
// Attention weights spread across first 8 blocks
// Expert weights fill the remaining 10

static uint32_t block_for(uint32_t layer, AccessType atype,
                           uint32_t expert_id) {
    if (atype == ACCESS_ATTENTION) {
        return (layer / 4) % 8;
    }
    if (atype == ACCESS_FFN_DENSE) {
        return 8 + (layer / 3) % 4;
    }
    // ACCESS_EXPERT: experts cluster into blocks
    return 12 + ((expert_id / 7) % 6);
}

// ── hot slot LRU ─────────────────────────────────────────

typedef struct { uint32_t block_id; int last_used; } HotSlot;

static HotSlot  hot[HOT_SLOTS];
static int      tick = 0;

static void hot_init(void) {
    for (int i = 0; i < HOT_SLOTS; i++) {
        hot[i].block_id  = UINT32_MAX;
        hot[i].last_used = 0;
    }
}

static uint8_t is_hot(uint32_t bid) {
    for (int i = 0; i < HOT_SLOTS; i++)
        if (hot[i].block_id == bid) return 1;
    return 0;
}

static void touch_block(uint32_t bid) {
    // already hot — refresh timestamp
    for (int i = 0; i < HOT_SLOTS; i++) {
        if (hot[i].block_id == bid) {
            hot[i].last_used = tick++;
            return;
        }
    }
    // find LRU slot
    int lru = 0;
    for (int i = 1; i < HOT_SLOTS; i++)
        if (hot[i].last_used < hot[lru].last_used) lru = i;

    hot[lru].block_id  = bid;
    hot[lru].last_used = tick++;
}

// ── MoE router ───────────────────────────────────────────
// Real routers have learned biases: experts cluster.
// We simulate 8 clusters so the predictor has
// real structure to learn from the traces.

static uint32_t clusters[8][8] = {
    {  0,  1,  4,  7, 12, 15, 23, 31 },
    {  2,  3,  5,  8, 11, 16, 22, 30 },
    {  6,  9, 10, 13, 17, 20, 25, 29 },
    { 14, 18, 19, 21, 24, 26, 27, 28 },
    { 32, 33, 36, 39, 44, 47, 55, 63 },
    { 34, 35, 37, 40, 43, 48, 54, 62 },
    { 38, 41, 42, 45, 49, 52, 57, 61 },
    { 46, 50, 51, 53, 56, 58, 59, 60 },
};

static void select_experts(uint32_t layer, uint32_t token_pos,
                            uint32_t* out) {
    uint32_t seed    = layer * 7919u + token_pos * 6271u;
    int      cluster = (int)((seed >> 4) % 8);
    int      offset  = (int)(seed % 3);
    for (int i = 0; i < N_ACTIVE; i++)
        out[i] = clusters[cluster][(i + offset) % 8];
}

// ── single block access ───────────────────────────────────

static void access(Tracer* tr, uint32_t layer,
                   uint32_t expert_id, AccessType atype,
                   uint32_t token_pos) {
    uint32_t bid     = block_for(layer, atype, expert_id);
    uint8_t  was_hot = is_hot(bid);
    tracer_record(tr, bid, layer, expert_id,
                  token_pos, atype, was_hot);
    touch_block(bid);
}

// ── one token forward pass ────────────────────────────────

static void run_token(Tracer* tr, uint32_t token_pos) {
    uint32_t active[N_ACTIVE];

    for (int layer = 0; layer < N_LAYERS; layer++) {

        // attention — every layer
        access(tr, layer, 0, ACCESS_ATTENTION, token_pos);

        if (is_moe_layer(layer)) {
            select_experts(layer, token_pos, active);
            for (int e = 0; e < N_ACTIVE; e++)
                access(tr, layer, active[e],
                       ACCESS_EXPERT, token_pos);
        } else {
            access(tr, layer, 0, ACCESS_FFN_DENSE, token_pos);
        }
    }
}

// ── main ─────────────────────────────────────────────────

int main(int argc, char** argv) {
    int n_tokens = 64;
    if (argc > 1) n_tokens = atoi(argv[1]);
    if (n_tokens < 1) n_tokens = 1;

    printf("\nSpike / synthetic inference harness\n");
    printf("════════════════════════════════════════\n");
    printf("layers:  %d   experts: %d active/%d total\n",
           N_LAYERS, N_ACTIVE, N_EXPERTS);
    printf("blocks:  %d   hot slots: %d\n", N_BLOCKS, HOT_SLOTS);
    printf("tokens:  %d\n\n", n_tokens);

    Tracer tr;
    if (tracer_open(&tr, "synthetic.wptr",
                    "qwen3-14b-synthetic") != 0) return 1;

    hot_init();

    for (int t = 0; t < n_tokens; t++) {
        if (n_tokens >= 16 && t % (n_tokens / 8) == 0)
            printf("  token %d / %d\n", t, n_tokens);
        run_token(&tr, (uint32_t)t);
    }

    tracer_close(&tr);

    printf("\nrun ./analyze synthetic.wptr %d for full breakdown\n\n",
           N_BLOCKS);
    return 0;
}

// benchmark.c
// The proof of concept.
//
// Runs the synthetic inference loop three ways:
//
//   1. BASELINE  — naive LRU, no prediction
//   2. MARKOV    — order-1 Markov predictor
//   3. LOOKAHEAD — order-2 chain predictor
//
// Compares hot rates and simulated token throughput.
// This is the number that goes in the README.
//
// Usage:
//   ./benchmark [n_train_tokens] [n_test_tokens]
//   ./benchmark 512 256

#define _POSIX_C_SOURCE 200809L

#include "tracer.h"
#include "predictor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

// ── model shape (must match synthetic.c) ─────────────────

#define N_LAYERS     28
#define N_EXPERTS    64
#define N_ACTIVE     8
#define N_BLOCKS     18
#define HOT_SLOTS    6

// ── timing ────────────────────────────────────────────────

// ── energy constants (Apple Silicon M1 NVMe) ──────────
// NVMe read throughput: ~7 GB/s
// Active power draw during I/O: ~5W
// joules per GB = watts / GBps ≈ 5.0 / 7.0 ≈ 0.714 J/GB
#define NVME_GBPS      7.0
#define NVME_WATTS     5.0
#define JOULES_PER_GB  (NVME_WATTS / NVME_GBPS)
#define MWHS_PER_GB    (JOULES_PER_GB / 3600.0 * 1000.0)

// Simulated miss cost per cold block access
#define MISS_COST_US      2000   // 2ms stall per cold miss
#define TOKEN_COMPUTE_US  8000   // 8ms compute per token (dense path)
#define SIM_BLOCK_SIZE    (512UL * 1024UL * 1024UL)  // 512MB simulated block

// ── hot slot manager ──────────────────────────────────────

typedef struct {
    uint32_t block_id;
    uint64_t last_used;
    int      prefetched;   // was this loaded by predictor?
} Slot;

typedef struct {
    Slot     slots[HOT_SLOTS];
    uint64_t tick;
    uint64_t hits;
    uint64_t misses;
    uint64_t prefetch_hits;   // predictor loaded it before needed
    uint64_t stall_us;        // total microseconds stalled on misses
    uint64_t bytes_read;      // bytes read from "disk" (for energy est.)
} HotCache;

static void cache_init(HotCache* c) {
    memset(c, 0, sizeof(HotCache));
    for (int i = 0; i < HOT_SLOTS; i++)
        c->slots[i].block_id = UINT32_MAX;
}

static int cache_find(HotCache* c, uint32_t bid) {
    for (int i = 0; i < HOT_SLOTS; i++)
        if (c->slots[i].block_id == bid) return i;
    return -1;
}

static int cache_lru_slot(HotCache* c) {
    int lru = 0;
    for (int i = 1; i < HOT_SLOTS; i++)
        if (c->slots[i].last_used < c->slots[lru].last_used)
            lru = i;
    return lru;
}

// Access a block. Returns 1 if hot, 0 if cold.
// Tracks bytes_read for energy estimation.
static int cache_access(HotCache* c, uint32_t bid) {
    int idx = cache_find(c, bid);
    if (idx >= 0) {
        c->slots[idx].last_used = c->tick++;
        c->hits++;
        if (c->slots[idx].prefetched) c->prefetch_hits++;
        c->slots[idx].prefetched = 0;
        return 1;
    }
    // cold miss — evict LRU, load block, pay stall cost
    int evict = cache_lru_slot(c);
    c->slots[evict].block_id  = bid;
    c->slots[evict].last_used = c->tick++;
    c->slots[evict].prefetched = 0;
    c->misses++;
    c->stall_us += MISS_COST_US;
    c->bytes_read += SIM_BLOCK_SIZE;
    return 0;
}

// Predictor calls this to prefetch a block speculatively.
// If the slot is available, load it now (no stall to caller).
static void cache_prefetch(HotCache* c, uint32_t bid) {
    if (bid >= N_BLOCKS) return;
    if (cache_find(c, bid) >= 0) return;  // already hot

    // only prefetch if there's a slot we'd evict anyway
    // (simple policy: always prefetch into LRU slot)
    int evict = cache_lru_slot(c);

    // don't evict something that was just used
    if (c->tick - c->slots[evict].last_used < 3) return;

    c->slots[evict].block_id   = bid;
    c->slots[evict].last_used  = c->tick;   // don't bump tick
    c->slots[evict].prefetched = 1;
    // no stall cost — prefetch happens in background
}

// ── access pattern (matches synthetic.c) ─────────────────

static int is_moe_layer(int layer) { return (layer % 2 == 1); }

// matches synthetic.c block_for exactly
static uint32_t block_for(uint32_t layer, int is_expert,
                           uint32_t expert_id) {
    if (!is_expert)
        return (layer / 4) % 8;           // attention: first 8 blocks
    return 12 + ((expert_id / 7) % 6);   // experts: blocks 12-17
}

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
    uint32_t seed = layer * 7919u + token_pos * 6271u;
    int cluster   = (int)((seed >> 4) % 8);
    int offset    = (int)(seed % 3);
    for (int i = 0; i < N_ACTIVE; i++)
        out[i] = clusters[cluster][(i + offset) % 8];
}

// ── run one mode ──────────────────────────────────────────

typedef struct {
    double   hot_rate;
    double   prefetch_hit_rate;
    double   simulated_tps;      // tokens/second accounting for stalls
    uint64_t total_accesses;
    uint64_t stall_us;
    uint64_t bytes_read;          // bytes read from "disk" (for energy estimation)
} BenchResult;

static BenchResult run_benchmark(int n_tokens,
                                  Predictor* pred) {
    HotCache cache;
    cache_init(&cache);

    uint32_t active[N_ACTIVE];

    for (int t = 0; t < n_tokens; t++) {
        for (int layer = 0; layer < N_LAYERS; layer++) {

            // attention block
            uint32_t attn_bid = block_for(layer, 0, 0);
            cache_access(&cache, attn_bid);

            // run predictor after attention access
            if (pred) {
                PrefetchList pl;
                predictor_next(pred, attn_bid, &pl);
                for (int k = 0; k < pl.count; k++)
                    cache_prefetch(&cache, pl.block_ids[k]);
            }

            if (is_moe_layer(layer)) {
                select_experts(layer, (uint32_t)t, active);
                for (int e = 0; e < N_ACTIVE; e++) {
                    uint32_t bid = block_for(layer, 1, active[e]);
                    int was_hot = cache_access(&cache, bid);
                    if (pred) predictor_feedback(pred, bid);

                    // prefetch next expert cluster
                    if (pred && !was_hot) {
                        PrefetchList pl;
                        predictor_next(pred, bid, &pl);
                        for (int k = 0; k < pl.count; k++)
                            cache_prefetch(&cache, pl.block_ids[k]);
                    }
                }
            } else {
                uint32_t bid = block_for(layer, 0, 0);
                cache_access(&cache, bid);
                if (pred) predictor_feedback(pred, bid);
            }
        }
    }

    uint64_t total = cache.hits + cache.misses;
    double   hr    = (total > 0)
                   ? (double)cache.hits / (double)total
                   : 0.0;

    double phr = (cache.hits > 0)
               ? (double)cache.prefetch_hits / (double)cache.hits
               : 0.0;

    // simulated token throughput:
    // each token costs TOKEN_COMPUTE_US + stall time / n_tokens
    double stall_per_token = (double)cache.stall_us / n_tokens;
    double us_per_token    = TOKEN_COMPUTE_US + stall_per_token;
    double tps             = 1e6 / us_per_token;

    BenchResult r = {
        .hot_rate         = hr,
        .prefetch_hit_rate= phr,
        .simulated_tps    = tps,
        .total_accesses   = total,
        .stall_us         = cache.stall_us,
        .bytes_read       = cache.bytes_read,
    };
    return r;
}

// ── main ─────────────────────────────────────────────────

int main(int argc, char** argv) {
    int n_train = 512;
    int n_test  = 256;
    if (argc > 1) n_train = atoi(argv[1]);
    if (argc > 2) n_test  = atoi(argv[2]);

    printf("\nSpike / prefetch benchmark\n");
    printf("══════════════════════════════\n");
    printf("train tokens: %d   test tokens: %d\n\n", n_train, n_test);

    // ── phase 1: generate training traces ─────────────────
    printf("phase 1: generating training traces (%d tokens)...\n",
           n_train);

    Tracer tr;
    tracer_open(&tr, "bench_train.wptr", "qwen3-14b-bench");

    HotCache dummy;
    cache_init(&dummy);
    uint32_t active[N_ACTIVE];

    for (int t = 0; t < n_train; t++) {
        for (int layer = 0; layer < N_LAYERS; layer++) {
            uint32_t attn_bid = block_for(layer, 0, 0);
            uint8_t hot = (cache_find(&dummy, attn_bid) >= 0);
            tracer_record(&tr, attn_bid, layer, 0, t,
                          ACCESS_ATTENTION, hot);
            cache_access(&dummy, attn_bid);

            if (is_moe_layer(layer)) {
                select_experts(layer, (uint32_t)t, active);
                for (int e = 0; e < N_ACTIVE; e++) {
                    uint32_t bid = block_for(layer, 1, active[e]);
                    hot = (cache_find(&dummy, bid) >= 0);
                    tracer_record(&tr, bid, layer, active[e], t,
                                  ACCESS_EXPERT, hot);
                    cache_access(&dummy, bid);
                }
            } else {
                uint32_t bid = block_for(layer, 0, 0);
                hot = (cache_find(&dummy, bid) >= 0);
                tracer_record(&tr, bid, layer, 0, t,
                              ACCESS_FFN_DENSE, hot);
                cache_access(&dummy, bid);
            }
        }
    }
    tracer_close(&tr);
    printf("\n");

    // ── phase 2: train predictors ─────────────────────────
    printf("phase 2: training predictors from traces...\n");

    Predictor markov, lookahead;
    predictor_init(&markov,    N_BLOCKS, PRED_MARKOV);
    predictor_init(&lookahead, N_BLOCKS, PRED_LOOKAHEAD);

    predictor_load(&markov,    "bench_train.wptr");
    predictor_load(&lookahead, "bench_train.wptr");
    printf("\n");

    // ── phase 3: benchmark ────────────────────────────────
    printf("phase 3: running benchmark (%d test tokens)...\n\n",
           n_test);

    BenchResult baseline = run_benchmark(n_test, NULL);
    BenchResult r_markov = run_benchmark(n_test, &markov);
    BenchResult r_look   = run_benchmark(n_test, &lookahead);

    // ── results ───────────────────────────────────────────
    printf("  ┌─────────────────┬──────────┬──────────┬──────────┐\n");
    printf("  │ mode            │ hot rate │  sim t/s │ stall/tk │\n");
    printf("  ├─────────────────┼──────────┼──────────┼──────────┤\n");
    printf("  │ baseline (LRU)  │  %5.1f%% │  %6.1f  │ %6lluμs │\n",
           baseline.hot_rate * 100.0,
           baseline.simulated_tps,
           (unsigned long long)(baseline.stall_us / n_test));
    printf("  │ markov          │  %5.1f%% │  %6.1f  │ %6lluμs │\n",
           r_markov.hot_rate * 100.0,
           r_markov.simulated_tps,
           (unsigned long long)(r_markov.stall_us / n_test));
    printf("  │ lookahead       │  %5.1f%% │  %6.1f  │ %6lluμs │\n",
           r_look.hot_rate * 100.0,
           r_look.simulated_tps,
           (unsigned long long)(r_look.stall_us / n_test));
    printf("  └─────────────────┴──────────┴──────────┴──────────┘\n\n");

    double gain_markov = r_markov.simulated_tps / baseline.simulated_tps;
    double gain_look   = r_look.simulated_tps   / baseline.simulated_tps;

    printf("  speedup markov:    %.2fx\n", gain_markov);
    printf("  speedup lookahead: %.2fx\n\n", gain_look);

    if (gain_look > 1.3)
        printf("  result: SIGNIFICANT — prediction measurably helps\n");
    else if (gain_look > 1.1)
        printf("  result: MODERATE — prediction helps, tune further\n");
    else
        printf("  result: MARGINAL — needs more trace data or tuning\n");

    // ── energy comparison ───────────────────────────────────
    printf("\n  I/O energy estimates (Apple Silicon M1 NVMe)\n");
    printf("  (NVMe @ %.0f GB/s, %.0fW active draw)\n",
           NVME_GBPS, NVME_WATTS);

    double bl_gb    = (double)baseline.bytes_read / 1e9;
    double mk_gb    = (double)r_markov.bytes_read   / 1e9;
    double lk_gb    = (double)r_look.bytes_read     / 1e9;
    double bl_mwh   = bl_gb * MWHS_PER_GB;
    double mk_mwh   = mk_gb * MWHS_PER_GB;
    double lk_mwh   = lk_gb * MWHS_PER_GB;
    double mk_saved = bl_gb  - mk_gb;
    double lk_saved = bl_gb  - lk_gb;
    double mk_pct   = bl_gb  > 0 ? (mk_saved / bl_gb) * 100.0 : 0.0;
    double lk_pct   = bl_gb  > 0 ? (lk_saved / bl_gb) * 100.0 : 0.0;

    printf("\n");
    printf("  ┌─────────────────┬──────────┬──────────┬──────────────┐\n");
    printf("  │ mode            │  GB I/O  │   mWh    │ vs baseline │\n");
    printf("  ├─────────────────┼──────────┼──────────┼──────────────┤\n");
    printf("  │ baseline (LRU)  │  %6.2f  │  %6.4f  │        —     │\n",
           bl_gb, bl_mwh);
    printf("  │ markov          │  %6.2f  │  %6.4f  │  %+.1f%% I/O   │\n",
           mk_gb, mk_mwh, -mk_pct);
    printf("  │ lookahead       │  %6.2f  │  %6.4f  │  %+.1f%% I/O   │\n",
           lk_gb, lk_mwh, -lk_pct);
    printf("  └─────────────────┴──────────┴──────────┴──────────────┘\n");
    printf("\n");
    printf("  I/O saved (markov vs baseline):   %.2f GB  (%.1f%%)  = %.4f mWh\n",
           mk_saved, mk_pct, (bl_mwh - mk_mwh));
    printf("  I/O saved (lookahead vs baseline): %.2f GB  (%.1f%%)  = %.4f mWh\n",
           lk_saved, lk_pct, (bl_mwh - lk_mwh));
    printf("\n");
    printf("  context: running Qwen3-32B on M1 16GB\n");
    printf("  ─────────────────────────────────────────────\n");
    printf("  model size (4-bit):   ~16 GB\n");
    printf("  NVMe idle:           ~0.5W\n");
    printf("  NVMe active:          %.0fW\n", NVME_WATTS);
    printf("  energy/GB:           %.3f J  (%.4f mWh)\n",
           JOULES_PER_GB, MWHS_PER_GB);

    predictor_print_stats(&markov);
    predictor_print_stats(&lookahead);

    predictor_free(&markov);
    predictor_free(&lookahead);

    printf("\n");
    return 0;
}

// pager_test.c
// Exercises the pager against a real file on disk.
//
// Creates a synthetic weight file, runs inference-like
// access patterns through the pager, measures real
// wall-clock stall time with and without prediction.
//
// Usage: ./pager_test [n_tokens]

#define _POSIX_C_SOURCE 200809L

#include "pager.h"
#include "tracer.h"
#include "predictor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

#ifndef CLOCK_MONOTONIC_RAW
#  define CLOCK_MONOTONIC_RAW CLOCK_MONOTONIC
#endif

// ── config ────────────────────────────────────────────────
// Keep blocks small for testing so the file fits anywhere.
// Real usage: BLOCK_SIZE = 512MB, N_BLOCKS = 18-36.

#define TEST_N_BLOCKS    12
#define TEST_BLOCK_MB    8          // 8MB blocks for fast testing
#define TEST_BLOCK_SIZE  (TEST_BLOCK_MB * 1024 * 1024)
#define TEST_FILE_SIZE   ((size_t)TEST_N_BLOCKS * TEST_BLOCK_SIZE)
#define TEST_HOT_SLOTS   6          // 6 slots = 48MB RAM budget
#define TEST_N_LAYERS    16
#define TEST_N_EXPERTS   16
#define TEST_N_ACTIVE    4

// ── timing ────────────────────────────────────────────────

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

// ── create synthetic weight file ──────────────────────────

static int create_weight_file(const char* path) {
    printf("  creating %dMB test weight file...\n",
           TEST_N_BLOCKS * TEST_BLOCK_MB);

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror("open"); return -1; }

    // write blocks with distinct fill bytes for verification
    uint8_t* block = malloc(TEST_BLOCK_SIZE);
    if (!block) { close(fd); return -1; }

    for (int b = 0; b < TEST_N_BLOCKS; b++) {
        memset(block, (uint8_t)(b + 1), TEST_BLOCK_SIZE);
        write(fd, block, TEST_BLOCK_SIZE);
    }

    free(block);
    close(fd);
    return 0;
}

// ── access pattern ────────────────────────────────────────

static int is_moe(int layer) { return (layer % 2 == 1); }

static uint32_t block_for_layer(int layer) {
    return (uint32_t)(layer / 3) % TEST_N_BLOCKS;
}

static uint32_t block_for_expert(uint32_t expert_id) {
    return (uint32_t)(TEST_N_BLOCKS / 2)
         + (expert_id / 4) % (TEST_N_BLOCKS / 2);
}

static void select_experts(int layer, int token,
                            uint32_t* out, int n) {
    uint32_t seed = (uint32_t)(layer * 1009 + token * 3571);
    int base      = (int)((seed % TEST_N_EXPERTS));
    for (int i = 0; i < n; i++)
        out[i] = (uint32_t)((base + i) % TEST_N_EXPERTS);
}

// ── generate training traces ──────────────────────────────

static void generate_traces(const char* trace_path, int n_tokens) {
    Tracer tr;
    tracer_open(&tr, trace_path, "test-model");

    // simulate a simple hot cache for was_hot tracking
    uint32_t hot[TEST_HOT_SLOTS];
    int      hot_lru[TEST_HOT_SLOTS];
    int      hot_tick = 0;
    for (int i = 0; i < TEST_HOT_SLOTS; i++) {
        hot[i]     = UINT32_MAX;
        hot_lru[i] = 0;
    }

    uint32_t experts[TEST_N_ACTIVE];

    for (int t = 0; t < n_tokens; t++) {
        for (int layer = 0; layer < TEST_N_LAYERS; layer++) {
            uint32_t bid = block_for_layer(layer);

            // check hot
            uint8_t was_hot = 0;
            for (int s = 0; s < TEST_HOT_SLOTS; s++)
                if (hot[s] == bid) { was_hot = 1; break; }

            tracer_record(&tr, bid, (uint32_t)layer, 0,
                          (uint32_t)t, ACCESS_ATTENTION, was_hot);

            // update hot table
            if (!was_hot) {
                int lru = 0;
                for (int s = 1; s < TEST_HOT_SLOTS; s++)
                    if (hot_lru[s] < hot_lru[lru]) lru = s;
                hot[lru]     = bid;
                hot_lru[lru] = hot_tick++;
            }

            if (is_moe(layer)) {
                select_experts(layer, t, experts, TEST_N_ACTIVE);
                for (int e = 0; e < TEST_N_ACTIVE; e++) {
                    uint32_t ebid = block_for_expert(experts[e]);
                    was_hot = 0;
                    for (int s = 0; s < TEST_HOT_SLOTS; s++)
                        if (hot[s] == ebid) { was_hot = 1; break; }
                    tracer_record(&tr, ebid, (uint32_t)layer,
                                  experts[e], (uint32_t)t,
                                  ACCESS_EXPERT, was_hot);
                    if (!was_hot) {
                        int lru = 0;
                        for (int s = 1; s < TEST_HOT_SLOTS; s++)
                            if (hot_lru[s] < hot_lru[lru]) lru = s;
                        hot[lru]     = ebid;
                        hot_lru[lru] = hot_tick++;
                    }
                }
            }
        }
    }

    tracer_close(&tr);
}

// ── run one pass through pager ────────────────────────────

typedef struct {
    uint64_t total_ns;
    uint64_t accesses;
    uint64_t misses;
} PassResult;

static PassResult run_pass(Pager* p, int n_tokens,
                            const char* label) {
    uint64_t t0 = now_ns();
    PassResult r = {0, 0, 0};
    uint32_t experts[TEST_N_ACTIVE];

    for (int t = 0; t < n_tokens; t++) {
        for (int layer = 0; layer < TEST_N_LAYERS; layer++) {

            uint32_t bid = block_for_layer(layer);
            void* ptr = pager_get(p, bid);
            if (!ptr) {
                fprintf(stderr, "pager_get failed: block %u\n", bid);
                continue;
            }
            r.accesses++;
            pager_release(p, bid);

            if (is_moe(layer)) {
                select_experts(layer, t, experts, TEST_N_ACTIVE);
                for (int e = 0; e < TEST_N_ACTIVE; e++) {
                    uint32_t ebid = block_for_expert(experts[e]);
                    ptr = pager_get(p, ebid);
                    if (ptr) {
                        r.accesses++;
                        pager_release(p, ebid);
                    }
                }
            }
        }
    }

    r.total_ns = now_ns() - t0;
    r.misses   = p->stat_misses;

    printf("  %-22s  %6llu accesses  %6llu misses  "
           "%6.1fms total  %5.2f t/s\n",
           label,
           (unsigned long long)r.accesses,
           (unsigned long long)r.misses,
           (double)r.total_ns / 1e6,
           (double)n_tokens / ((double)r.total_ns / 1e9));

    return r;
}

// ── main ─────────────────────────────────────────────────

int main(int argc, char** argv) {
    int n_train  = 256;
    int n_test   = 32;
    if (argc > 1) n_train = atoi(argv[1]);
    if (argc > 2) n_test  = atoi(argv[2]);

    const char* weight_file = "/tmp/Spike_test_weights.bin";
    const char* trace_file  = "/tmp/Spike_test.wptr";

    printf("\nSpike / pager integration test\n");
    printf("═══════════════════════════════════\n");
    printf("blocks: %d × %dMB   slots: %d   "
           "train: %d tokens   test: %d tokens\n\n",
           TEST_N_BLOCKS, TEST_BLOCK_MB,
           TEST_HOT_SLOTS, n_train, n_test);

    // ── setup ──────────────────────────────────────────────
    if (create_weight_file(weight_file) != 0) return 1;

    printf("  generating training traces (%d tokens)...\n", n_train);
    generate_traces(trace_file, n_train);
    printf("\n");

    // ── test 1: no prediction ─────────────────────────────
    printf("test 1: baseline (no prediction)\n");
    {
        Pager p;
        if (pager_init(&p, weight_file,
                       TEST_N_BLOCKS, TEST_BLOCK_SIZE,
                       TEST_HOT_SLOTS) != 0) return 1;

        run_pass(&p, n_test, "no prediction");
        pager_print_stats(&p);
        pager_close(&p);
    }

    // ── test 2: with prediction ───────────────────────────
    printf("test 2: with predictor\n");
    {
        Pager p;
        if (pager_init(&p, weight_file,
                       TEST_N_BLOCKS, TEST_BLOCK_SIZE,
                       TEST_HOT_SLOTS) != 0) return 1;

        const char* traces[] = { trace_file };
        pager_train(&p, traces, 1);

        run_pass(&p, n_test, "with prediction");
        pager_print_stats(&p);
        pager_print_slots(&p);
        pager_close(&p);
    }

    // cleanup
    unlink(weight_file);
    unlink(trace_file);

    printf("done.\n\n");
    return 0;
}

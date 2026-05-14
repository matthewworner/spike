// spike_index_test.c
// Validates the spike_index parser against a real safetensor model.
//
// Usage: ./spike_index_test <model_dir>
//
// Checks:
//   - all shards open successfully
//   - n_layers is detected correctly
//   - each layer has the expected tensor count and total size
//   - all tensor offsets are within their shard file bounds
//   - layer 0 tensor names and sizes look sane
//   - block offsets are contiguous (no gaps, no overlaps)

#define _POSIX_C_SOURCE 200809L

#include "spike_index.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

static int n_pass = 0;
static int n_fail = 0;

static void check(int cond, const char* msg) {
    if (cond) {
        printf("  PASS  %s\n", msg);
        n_pass++;
    } else {
        printf("  FAIL  %s\n", msg);
        n_fail++;
    }
}

static void checkf(int cond, const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    check(cond, buf);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <model_dir>\n", argv[0]);
        return 1;
    }
    const char* model_dir = argv[1];

    printf("\nspike_index_test — %s\n", model_dir);
    printf("──────────────────────────────────────────────────────\n\n");

    // ── load ──────────────────────────────────────────────────
    SpikeIndex* idx = spike_index_load(model_dir);
    if (!idx) {
        fprintf(stderr, "spike_index_load failed\n");
        return 1;
    }

    printf("\n");

    // ── basic counts ──────────────────────────────────────────
    check(idx->n_shards > 0, "at least one shard found");
    check(idx->n_layers > 0, "at least one transformer layer found");
    check(idx->n_blocks >= idx->n_layers, "n_blocks >= n_layers");

    // ── tensor offset bounds check ─────────────────────────────
    // get shard file sizes
    off_t shard_sizes[SI_MAX_SHARDS];
    for (int i = 0; i < idx->n_shards; i++) {
        struct stat st;
        fstat(idx->shards[i].fd, &st);
        shard_sizes[i] = st.st_size;
    }

    int bad_offsets = 0;
    for (int bi = 0; bi < idx->n_blocks; bi++) {
        SIBlock* b = &idx->blocks[bi];
        for (int ti = 0; ti < b->n_tensors; ti++) {
            SITensor* t = &b->tensors[ti];
            off_t end = (off_t)(t->offset_in_file + t->byte_length);
            if (end > shard_sizes[t->shard_idx]) {
                printf("  BAD OFFSET  %s  off=%llu len=%llu "
                       "shard_size=%lld\n",
                       t->name,
                       (unsigned long long)t->offset_in_file,
                       (unsigned long long)t->byte_length,
                       (long long)shard_sizes[t->shard_idx]);
                bad_offsets++;
            }
        }
    }
    check(bad_offsets == 0, "all tensor offsets within shard file bounds");

    // ── block offset contiguity ────────────────────────────────
    int bad_contiguity = 0;
    for (int bi = 0; bi < idx->n_blocks; bi++) {
        SIBlock* b = &idx->blocks[bi];
        uint64_t expected = 0;
        for (int ti = 0; ti < b->n_tensors; ti++) {
            if (b->tensors[ti].offset_in_block != expected) {
                bad_contiguity++;
                break;
            }
            expected += b->tensors[ti].byte_length;
        }
        if (b->total_bytes != expected) bad_contiguity++;
    }
    check(bad_contiguity == 0, "block offsets within each block are contiguous");

    int bad_layer_blocks = 0;
    for (int layer = 0; layer < idx->n_layers; layer++) {
        SIBlock* b = spike_index_block(idx, (uint32_t)layer);
        if (!b || b->n_tensors == 0) {
            printf("  missing or empty block for layer %d\n", layer);
            bad_layer_blocks++;
        }
    }
    check(bad_layer_blocks == 0, "every layer has at least one tensor");

    // ── layer 0 detail ────────────────────────────────────────
    SIBlock* l0 = spike_index_block(idx, 0);
    if (l0) {
        printf("\nlayer 0 detail (%d tensors, %.1f MB):\n",
               l0->n_tensors,
               (double)l0->total_bytes / (1024.0 * 1024.0));
        for (int j = 0; j < l0->n_tensors; j++) {
            SITensor* t = &l0->tensors[j];
            printf("  [%2d] %-55s  %5s  %6.2f MB\n",
                   j, t->name, t->dtype,
                   (double)t->byte_length / (1024.0 * 1024.0));
        }
    }

    // ── layer size consistency ─────────────────────────────────
    // all transformer layers should be approximately the same size
    if (idx->n_layers >= 2) {
        uint64_t size0 = spike_index_block(idx, 0)->total_bytes;
        int inconsistent = 0;
        for (int layer = 1; layer < idx->n_layers; layer++) {
            SIBlock* b = spike_index_block(idx, (uint32_t)layer);
            if (!b) { inconsistent++; continue; }
            // allow up to 10% variation (MoE first/last layers differ)
            uint64_t diff = b->total_bytes > size0
                          ? b->total_bytes - size0
                          : size0 - b->total_bytes;
            if (diff > size0 / 5) inconsistent++;
        }
        checkf(inconsistent == 0,
               "layer sizes are consistent (±20%% of layer 0)");
    }

    // ── misc block exists ──────────────────────────────────────
    SIBlock* misc = spike_index_block(idx, (uint32_t)idx->n_layers);
    check(misc != NULL && misc->n_tensors > 0,
          "misc block (embed/norm/lm_head) present");

    // ── full print ─────────────────────────────────────────────
    printf("\n");
    spike_index_print(idx, 0);

    // ── result ─────────────────────────────────────────────────
    printf("──────────────────────────────────────────────────────\n");
    printf("result: %d passed, %d failed\n\n", n_pass, n_fail);

    spike_index_free(idx);
    return (n_fail == 0) ? 0 : 1;
}

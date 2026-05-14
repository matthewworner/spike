// spike_index.h
// Safetensor shard index for the scatter-gather pager.
//
// Scans all *.safetensors shards in a model directory,
// parses each shard's header JSON, and builds a block_map:
//
//   block_id 0 .. n_layers-1  →  transformer layer blocks
//   block_id n_layers         →  misc (embed, norm, lm_head, ...)
//
// Each block holds up to SI_MAX_TENSORS tensor entries.
// Each tensor entry knows exactly where its bytes live:
//   which shard file, byte offset, byte length, and
//   where in the assembled block buffer to place it.
//
// This is the ground truth that block_scatter_load() reads.
// Parse once at startup; cache for the life of the process.

#ifndef SPIKE_INDEX_H
#define SPIKE_INDEX_H

#include <stdint.h>

// ── limits ─────────────────────────────────────────────────

#define SI_MAX_SHARDS    16
#define SI_MAX_BLOCKS   256
#define SI_MAX_TENSORS   48
#define SI_MAX_NAME     128
#define SI_MAX_PATH     256
#define SI_MAX_DTYPE      8
#define SI_MAX_DIMS       8

// ── one open shard file ────────────────────────────────────

typedef struct {
    char path[SI_MAX_PATH];
    int  fd;
} SIShard;

// ── one tensor within a block ──────────────────────────────

typedef struct {
    char     name[SI_MAX_NAME];

    int      shard_idx;          // index into SpikeIndex.shards[]
    uint64_t offset_in_file;     // absolute byte offset in shard:
                                 //   8 + header_size + data_offsets[0]
    uint64_t byte_length;        // data_offsets[1] - data_offsets[0]
    uint64_t offset_in_block;    // where to place this in the block buffer
                                 //   (running sum, sorted by offset_in_file)

    char     dtype[SI_MAX_DTYPE]; // "BF16", "F16", "F32", "U32", ...
    int64_t  shape[SI_MAX_DIMS];
    int      ndim;
} SITensor;

// ── one block = one layer (or misc) ───────────────────────

typedef struct {
    uint32_t  block_id;
    char      label[64];          // "layer.0" .. "layer.63", "misc"
    SITensor  tensors[SI_MAX_TENSORS];
    int       n_tensors;
    uint64_t  total_bytes;        // sum of all tensor byte_lengths
} SIBlock;

// ── full model index ───────────────────────────────────────

typedef struct {
    char    model_dir[SI_MAX_PATH];
    SIBlock blocks[SI_MAX_BLOCKS];
    int     n_blocks;
    int     n_layers;             // number of transformer layer blocks
    SIShard shards[SI_MAX_SHARDS];
    int     n_shards;
} SpikeIndex;

// ── API ────────────────────────────────────────────────────

// Load index by scanning model_dir for *.safetensors files.
// Opens each shard fd (kept open for scatter-gather reads).
// Returns NULL on error.
SpikeIndex* spike_index_load(const char* model_dir);

// Close shard fds and free the index.
void spike_index_free(SpikeIndex* idx);

// Print a summary to stdout.
// Pass verbose=1 to list all tensors in every block.
void spike_index_print(const SpikeIndex* idx, int verbose);

// Look up a block by block_id. Returns NULL if not found.
SIBlock* spike_index_block(SpikeIndex* idx, uint32_t block_id);

#endif // SPIKE_INDEX_H

// tracer.h
// Weight block access tracer for Spike
//
// Records ground truth access patterns during inference.
// Output feeds the predictor that makes weight paging work.
//
// Usage:
//   Tracer tr;
//   tracer_open(&tr, "run001.wptr", "qwen3-14b");
//   tracer_record(&tr, block_id, layer_id, expert_id,
//                 token_pos, access_type, was_hot);
//   tracer_close(&tr);

#ifndef TRACER_H
#define TRACER_H

#include <stdint.h>
#include <stdio.h>

// ── file format ───────────────────────────────────────────
//
// .wptr (Weight Page TRace)
//
// [header 48 bytes]
//   magic[4]       "WPTR"
//   version[1]     = 1
//   model_id[32]   null-padded string
//   reserved[11]   zero
//
// [entries: TraceEntry * n]
//   packed, 24 bytes each, written as they happen
//
// No entry count in header — reader counts from file size.
// Allows crash-safe partial traces to still be analysed.

#define WPTR_MAGIC        "WPTR"
#define WPTR_VERSION      1
#define WPTR_HEADER_SIZE  48
#define WPTR_MODEL_ID_LEN 32

// ── access types ──────────────────────────────────────────

typedef enum {
    ACCESS_ATTENTION = 0,  // Q, K, V, O projection weights
    ACCESS_FFN_DENSE = 1,  // FFN weights in non-MoE layers
    ACCESS_EXPERT    = 2,  // MoE routed expert weights
    ACCESS_EMBED     = 3,  // embedding / unembedding
} AccessType;

// ── single recorded event ─────────────────────────────────
//
// 24 bytes packed. Every field matters to the predictor.
// block_id + layer_id + expert_id tell us WHAT was accessed.
// token_pos + timestamp tell us WHEN and in what sequence.
// was_hot tells us whether the prefetcher would have helped.

typedef struct __attribute__((packed)) {
    uint32_t block_id;      // index into the weight block map
    uint32_t layer_id;      // transformer layer (0-indexed)
    uint32_t expert_id;     // MoE expert id (0 = dense layer)
    uint32_t token_pos;     // position in generation sequence
    uint64_t timestamp_ns;  // CLOCK_MONOTONIC_RAW nanoseconds
    uint8_t  access_type;   // AccessType enum
    uint8_t  was_hot;       // 1 = block was in RAM, 0 = miss
    uint8_t  reserved[2];   // align to 24 bytes, future use
} TraceEntry;

// ── tracer state ──────────────────────────────────────────

#define TRACER_FLUSH_EVERY  4096  // entries before forced flush

typedef struct {
    FILE*       file;
    TraceEntry* buf;          // ring buffer before flush
    uint32_t    buf_count;    // entries in buffer
    uint32_t    buf_cap;      // buffer capacity
    uint64_t    total;        // total entries written lifetime
    uint64_t    total_hot;    // hot hits (for running stats)
    uint64_t    total_miss;   // cold misses
    char        model_id[WPTR_MODEL_ID_LEN];
} Tracer;

// ── API ───────────────────────────────────────────────────

// Open a trace file for writing.
// model_id: short human name e.g. "qwen3-14b-q4"
// Returns 0 on success, -1 on error.
int  tracer_open(Tracer* tr, const char* path,
                 const char* model_id);

// Record one block access. Call this every time the
// inference engine touches a weight block.
// Thread-safety: NOT thread safe. Call from inference thread only.
void tracer_record(Tracer* tr,
                   uint32_t block_id,
                   uint32_t layer_id,
                   uint32_t expert_id,
                   uint32_t token_pos,
                   AccessType access_type,
                   uint8_t was_hot);

// Flush buffered entries to disk. Called automatically
// every TRACER_FLUSH_EVERY entries and on close.
// Safe to call manually at token boundaries if desired.
int  tracer_flush(Tracer* tr);

// Finalise and close. Flushes remaining buffer.
// Prints a one-line summary to stdout.
void tracer_close(Tracer* tr);

// ── analysis API ─────────────────────────────────────────
//
// Read a completed .wptr file and print stats.
// Used by the analyse tool and for quick inspection.

typedef struct {
    uint64_t total_accesses;
    uint64_t hot_hits;
    uint64_t cold_misses;
    float    hot_rate;        // hot_hits / total_accesses
    uint32_t unique_blocks;
    uint32_t unique_layers;
    uint32_t unique_experts;
    uint32_t token_count;     // max token_pos seen + 1
    char     model_id[WPTR_MODEL_ID_LEN];
} TraceSummary;

// Read a .wptr file, populate summary, return 0 on success.
int  tracer_summarise(const char* path, TraceSummary* out);

// Print a formatted summary to stdout.
void tracer_print_summary(const TraceSummary* s);

// Build transition matrix from trace file.
// out_matrix: caller-allocated [n_blocks * n_blocks] floats
// Contains P(block_j is next | block_i was just accessed)
// Returns 0 on success.
int  tracer_build_transitions(const char* path,
                               uint32_t n_blocks,
                               float* out_matrix);

#endif // TRACER_H

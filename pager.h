// pager.h
// Weight block pager for Spike
//
// Manages which weight blocks live in RAM at any moment.
// Sits between the inference engine and storage.
// Uses the predictor to prefetch blocks before they're needed.
//
// The pager is the core of the Spike idea:
//   - inference engine calls pager_get(block_id)
//   - pager checks if block is hot (RAM) or cold (disk/mmap)
//   - if cold: loads it, pays the stall cost
//   - simultaneously: predictor fires, prefetch thread loads next blocks
//   - next access: already hot, no stall
//
// Threading model:
//   - pager_get()      called from inference thread (hot path)
//   - prefetch worker  background pthread, loads predicted blocks
//   - lock is held only during slot table mutations, not I/O
//
// Usage:
//   Pager p;
//   pager_init(&p, "model.bin", n_blocks, BLOCK_SIZE, HOT_SLOTS);
//   pager_train(&p, "traces/*.wptr");   // load predictor
//
//   void* weights = pager_get(&p, block_id);  // inference hot path
//   // ... GPU uses weights ...
//   pager_release(&p, block_id);              // optional hint
//
//   pager_close(&p);

#ifndef PAGER_H
#define PAGER_H

#include "predictor.h"
#include "tracer.h"
#include "spike_index.h"

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>

// ── pager modes ───────────────────────────────────────────
//
// MMAP:    weights live in a single flat binary file.
//          pager_init() maps the file; slots point into the mmap.
//
// SCATTER: weights live in safetensor shards.
//          pager_init_safetensors() opens the shard index.
//          Each slot is a separately malloc'd buffer assembled
//          by block_scatter_load() via pread() on shard fds.

#define PAGER_MODE_MMAP    0
#define PAGER_MODE_SCATTER 1

// ── slot states ───────────────────────────────────────────

typedef enum {
    SLOT_EMPTY     = 0,  // no block loaded
    SLOT_LOADING   = 1,  // prefetch in progress (background)
    SLOT_HOT       = 2,  // loaded, ready for GPU
    SLOT_PINNED    = 3,  // in active use, cannot evict
} SlotState;

// ── one RAM slot ──────────────────────────────────────────

typedef struct {
    uint32_t   block_id;    // which block is here (UINT32_MAX = empty)
    SlotState  state;
    void*      data;        // pointer into the mmap'd weight file
    uint64_t   last_used;   // LRU timestamp
    int        pin_count;   // number of active pager_get callers
    int        prefetched;  // loaded by predictor, not by demand
} PagerSlot;

// ── prefetch request (sent to worker thread) ──────────────

#define PREFETCH_QUEUE_DEPTH  8

typedef struct {
    uint32_t block_id;
    float    confidence;
} PrefetchReq;

// ── pager state ───────────────────────────────────────────

typedef struct {
    // mode
    int      mode;          // PAGER_MODE_MMAP or PAGER_MODE_SCATTER

    // weight file (mmap mode)
    int      fd;            // file descriptor
    void*    mmap_base;     // base of mmap'd weight file
    size_t   file_size;     // total file size in bytes
    size_t   block_size;    // bytes per block

    // scatter mode — safetensor shard index
    SpikeIndex* index;      // non-NULL when mode == PAGER_MODE_SCATTER

    uint32_t n_blocks;      // total blocks in model

    // RAM slot table
    PagerSlot* slots;
    int        n_slots;     // HOT_SLOTS — slots that fit in budget
    pthread_mutex_t slot_lock;
    uint64_t   tick;        // LRU clock

    // predictor
    Predictor  pred;
    int        pred_ready;  // 1 once trained

    // tracer (optional — records live access for future training)
    Tracer     tracer;
    int        tracing;

    // prefetch worker thread
    pthread_t        worker;
    pthread_mutex_t  queue_lock;
    pthread_cond_t   queue_cond;
    PrefetchReq      queue[PREFETCH_QUEUE_DEPTH];
    int              queue_head;
    int              queue_tail;
    int              queue_len;
    int              worker_running;

    // stats
    uint64_t   stat_hits;
    uint64_t   stat_misses;
    uint64_t   stat_prefetch_hits;   // block was ready from prefetch
    uint64_t   stat_stall_ns;        // nanoseconds waiting on cold loads
    uint64_t   stat_bytes_read_demand;   // bytes read on demand
    uint64_t   stat_bytes_read_prefetch; // bytes read by prefetch worker
} Pager;

// ── API ───────────────────────────────────────────────────

// Initialise pager over a flat binary weight file (mmap mode).
// n_blocks:    number of equal-size blocks in the file
// block_size:  bytes per block (e.g. 512MB = 512*1024*1024)
// n_slots:     RAM budget in blocks (e.g. 6 for a 16GB machine)
// Returns 0 on success.
int  pager_init(Pager* p,
                const char* weight_path,
                uint32_t    n_blocks,
                size_t      block_size,
                int         n_slots);

// Initialise pager over safetensor shards (scatter-gather mode).
// idx:     preloaded shard index from spike_index_load().
// n_slots: RAM budget in blocks (one slot = one layer, ~262MB for Qwen3-32B).
// Returns 0 on success.
// The pager does NOT take ownership of idx — caller must free it after
// pager_close().
int  pager_init_safetensors(Pager* p, SpikeIndex* idx, int n_slots);

// Load predictor from one or more .wptr trace files.
// glob_pattern: e.g. "traces/run*.wptr" or a single path
// Must be called before inference for prediction to work.
// Safe to call after pager_init, before first pager_get.
int  pager_train(Pager* p, const char** wptr_paths, int n_paths);

// Enable live tracing of this inference session.
// Writes access log to trace_path for future predictor training.
// Optional — inference works without tracing.
int  pager_trace_start(Pager* p, const char* trace_path,
                        const char* model_id);

// Get a pointer to a weight block's data.
// Blocks until the block is in RAM if it isn't already.
// Pins the block — it won't be evicted until pager_release.
// Returns NULL on error.
void* pager_get(Pager* p, uint32_t block_id);

// Release a pinned block back to the LRU pool.
// Call after the GPU/CPU is done with the block's data.
// (For synchronous inference, call after each layer.)
void  pager_release(Pager* p, uint32_t block_id);

// Print current slot table state to stdout.
// Useful for debugging and tuning HOT_SLOTS count.
void  pager_print_slots(const Pager* p);

// Print lifetime stats.
void  pager_print_stats(const Pager* p);

// Flush tracer, stop worker thread, close file, free memory.
void  pager_close(Pager* p);

#endif // PAGER_H

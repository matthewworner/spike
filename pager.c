// pager.c
// Weight block pager — implementation
//
// Core loop:
//   inference calls pager_get(bid)
//     → if hot:  return pointer immediately (nanoseconds)
//     → if cold: load from mmap, pay stall (milliseconds)
//                simultaneously: fire predictor,
//                enqueue prefetch requests to worker thread
//   worker thread drains prefetch queue in background
//   next pager_get: block is already hot, no stall
#define _GNU_SOURCE

#include "pager.h"
#include "predictor.h"
#include "tracer.h"
#include "spike_index.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>


// ── internal timing ───────────────────────────────────────

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

// ── slot table helpers ────────────────────────────────────
// All called with slot_lock held.

static int slot_find(Pager* p, uint32_t bid) {
    for (int i = 0; i < p->n_slots; i++)
        if (p->slots[i].block_id == bid
            && p->slots[i].state != SLOT_EMPTY)
            return i;
    return -1;
}

static int slot_lru_evict(Pager* p) {
    // find slot that is HOT (not pinned, not loading) with
    // oldest last_used timestamp
    int      best   = -1;
    uint64_t oldest = UINT64_MAX;

    for (int i = 0; i < p->n_slots; i++) {
        if (p->slots[i].state != SLOT_HOT) continue;
        if (p->slots[i].pin_count > 0)     continue;
        if (p->slots[i].last_used < oldest) {
            oldest = p->slots[i].last_used;
            best   = i;
        }
    }
    return best;
}

static int slot_empty(Pager* p) {
    for (int i = 0; i < p->n_slots; i++)
        if (p->slots[i].state == SLOT_EMPTY) return i;
    return -1;
}

// ── block I/O ─────────────────────────────────────────────

// ── mmap mode helpers ─────────────────────────────────────

static void* block_data_ptr(Pager* p, uint32_t bid) {
    return (uint8_t*)p->mmap_base + (size_t)bid * p->block_size;
}

// Touch one byte per page to fault the block into physical RAM.
static void block_fault_in(Pager* p, uint32_t bid) {
    uint8_t* base    = (uint8_t*)block_data_ptr(p, bid);
    size_t   sz      = p->block_size;
    long     page_sz = sysconf(_SC_PAGESIZE);
    volatile uint8_t sink = 0;
    for (size_t off = 0; off < sz; off += (size_t)page_sz)
        sink += base[off];
    (void)sink;
    madvise(base, sz, MADV_WILLNEED);
}

// ── scatter mode helper ───────────────────────────────────
//
// Reads each tensor's exact byte range from the shard file
// into a contiguous malloc'd buffer at slots[slot_idx].data.
// Allocates the buffer on first call; reuses on warm reload.
// Called outside slot_lock — I/O must not hold the lock.

static uint64_t block_scatter_load(Pager* p, int slot_idx, uint32_t bid) {
    SIBlock* block = spike_index_block(p->index, bid);
    if (!block) {
        fprintf(stderr, "pager: no index entry for block %u\n", bid);
        return 0;
    }

    // allocate buffer once per slot (reused on evict/reload)
    if (!p->slots[slot_idx].data) {
        p->slots[slot_idx].data = malloc(block->total_bytes);
        if (!p->slots[slot_idx].data) {
            fprintf(stderr, "pager: malloc failed for block %u "
                    "(%llu bytes)\n", bid,
                    (unsigned long long)block->total_bytes);
            return 0;
        }
    }

    uint8_t* buf = (uint8_t*)p->slots[slot_idx].data;
    uint64_t bytes_read = 0;
    for (int i = 0; i < block->n_tensors; i++) {
        SITensor* t = &block->tensors[i];
        int fd      = p->index->shards[t->shard_idx].fd;
        ssize_t got = pread(fd, buf + t->offset_in_block,
                            t->byte_length,
                            (off_t)t->offset_in_file);
        if (got != (ssize_t)t->byte_length) {
            fprintf(stderr, "pager: short read on %s "
                    "(got %zd, want %llu)\n",
                    t->name, got,
                    (unsigned long long)t->byte_length);
        }
        bytes_read += (uint64_t)got;
    }
    return bytes_read;
}

// ── eviction ──────────────────────────────────────────────
//
// Evicts the current occupant of a slot.
// In mmap mode: advises the kernel to drop the pages.
// In scatter mode: frees the malloc'd buffer.
// Called with slot_lock held.

static void block_evict_slot(Pager* p, int slot_idx) {
    if (p->slots[slot_idx].state != SLOT_HOT) return;

    if (p->mode == PAGER_MODE_SCATTER) {
        // free the buffer — block_scatter_load will re-malloc when needed
        free(p->slots[slot_idx].data);
        p->slots[slot_idx].data = NULL;
    } else {
        uint8_t* base = (uint8_t*)block_data_ptr(
                            p, p->slots[slot_idx].block_id);
        madvise(base, p->block_size, MADV_DONTNEED);
    }
}

// ── prefetch queue ────────────────────────────────────────

static void queue_push(Pager* p, uint32_t bid, float conf) {
    pthread_mutex_lock(&p->queue_lock);

    // drop if queue full or block already hot/loading
    if (p->queue_len >= PREFETCH_QUEUE_DEPTH) {
        pthread_mutex_unlock(&p->queue_lock);
        return;
    }

    // check for duplicates in queue
    for (int i = 0; i < p->queue_len; i++) {
        int idx = (p->queue_head + i) % PREFETCH_QUEUE_DEPTH;
        if (p->queue[idx].block_id == bid) {
            pthread_mutex_unlock(&p->queue_lock);
            return;
        }
    }

    int tail = p->queue_tail;
    p->queue[tail].block_id   = bid;
    p->queue[tail].confidence = conf;
    p->queue_tail = (tail + 1) % PREFETCH_QUEUE_DEPTH;
    p->queue_len++;

    pthread_cond_signal(&p->queue_cond);
    pthread_mutex_unlock(&p->queue_lock);
}

static int queue_pop(Pager* p, PrefetchReq* out) {
    // called with queue_lock held
    if (p->queue_len == 0) return 0;

    *out = p->queue[p->queue_head];
    p->queue_head = (p->queue_head + 1) % PREFETCH_QUEUE_DEPTH;
    p->queue_len--;
    return 1;
}

// ── prefetch worker thread ────────────────────────────────
// Runs in background. Drains the prefetch queue.
// Loads blocks into slot table without blocking inference.

static void* prefetch_worker(void* arg) {
    Pager* p = (Pager*)arg;

    while (1) {
        pthread_mutex_lock(&p->queue_lock);

        // wait for work or shutdown signal
        while (p->queue_len == 0 && p->worker_running)
            pthread_cond_wait(&p->queue_cond, &p->queue_lock);

        if (!p->worker_running && p->queue_len == 0) {
            pthread_mutex_unlock(&p->queue_lock);
            break;
        }

        PrefetchReq req;
        int got = queue_pop(p, &req);
        pthread_mutex_unlock(&p->queue_lock);

        if (!got) continue;

        uint32_t bid = req.block_id;
        if (bid >= p->n_blocks) continue;

        // check slot table — maybe it was loaded by demand
        // while this request was in the queue
        pthread_mutex_lock(&p->slot_lock);
        int existing = slot_find(p, bid);
        if (existing >= 0) {
            pthread_mutex_unlock(&p->slot_lock);
            continue;  // already hot, nothing to do
        }

        // find a slot to load into
        int slot = slot_empty(p);
        if (slot < 0) slot = slot_lru_evict(p);
        if (slot < 0) {
            pthread_mutex_unlock(&p->slot_lock);
            continue;  // no evictable slot — skip this prefetch
        }

        // recency guard: don't evict a recently-used block for a
        // speculative load. if the LRU slot was touched within the
        // last 4 ticks it's likely still needed by inference.
        // this is the key fix: without it the prefetch worker races
        // the inference thread and causes more misses than it prevents.
        if (p->slots[slot].state == SLOT_HOT) {
            uint64_t age = p->tick - p->slots[slot].last_used;
            if (age < 4) {
                pthread_mutex_unlock(&p->slot_lock);
                continue;  // slot too fresh — skip this prefetch
            }
            block_evict_slot(p, slot);
            p->slots[slot].block_id = UINT32_MAX;
        }

        // mark as loading so demand-pager doesn't race us
        p->slots[slot].block_id   = bid;
        p->slots[slot].state      = SLOT_LOADING;
        // scatter mode: data will be set by block_scatter_load below
        // mmap mode: point into the mmap now
        if (p->mode == PAGER_MODE_MMAP)
            p->slots[slot].data = block_data_ptr(p, bid);
        p->slots[slot].prefetched = 1;
        p->slots[slot].pin_count  = 0;
        pthread_mutex_unlock(&p->slot_lock);

        // do the actual I/O outside the lock
        if (p->mode == PAGER_MODE_SCATTER)
            p->stat_bytes_read_prefetch += block_scatter_load(p, slot, bid);
        else {
            block_fault_in(p, bid);
            p->stat_bytes_read_prefetch += p->block_size;
        }

        // mark hot
        pthread_mutex_lock(&p->slot_lock);
        if (p->slots[slot].block_id == bid) {  // wasn't stolen
            p->slots[slot].state     = SLOT_HOT;
            p->slots[slot].last_used = p->tick++;
        }
        pthread_mutex_unlock(&p->slot_lock);
    }

    return NULL;
}

// ── init ──────────────────────────────────────────────────

int pager_init(Pager* p,
               const char* weight_path,
               uint32_t    n_blocks,
               size_t      block_size,
               int         n_slots) {
    memset(p, 0, sizeof(Pager));

    p->n_blocks   = n_blocks;
    p->block_size = block_size;
    p->n_slots    = n_slots;

    // open and mmap the weight file
    p->fd = open(weight_path, O_RDONLY);
    if (p->fd < 0) {
        fprintf(stderr, "pager: cannot open %s: %s\n",
                weight_path, strerror(errno));
        return -1;
    }

    struct stat st;
    if (fstat(p->fd, &st) < 0) {
        fprintf(stderr, "pager: fstat failed: %s\n",
                strerror(errno));
        close(p->fd);
        return -1;
    }
    p->file_size = (size_t)st.st_size;

    size_t expected = (size_t)n_blocks * block_size;
    if (p->file_size < expected) {
        fprintf(stderr,
                "pager: file too small "
                "(got %zu, need %zu for %u blocks × %zu bytes)\n",
                p->file_size, expected, n_blocks, block_size);
        close(p->fd);
        return -1;
    }

    // mmap the whole file — pages stay on disk until faulted
    p->mmap_base = mmap(NULL, p->file_size,
                        PROT_READ, MAP_PRIVATE,
                        p->fd, 0);
    if (p->mmap_base == MAP_FAILED) {
        fprintf(stderr, "pager: mmap failed: %s\n",
                strerror(errno));
        close(p->fd);
        return -1;
    }

    // advise random access pattern — disable readahead
    // we do our own prefetching
    madvise(p->mmap_base, p->file_size, MADV_RANDOM);

    // allocate slot table
    p->slots = calloc((size_t)n_slots, sizeof(PagerSlot));
    if (!p->slots) {
        munmap(p->mmap_base, p->file_size);
        close(p->fd);
        return -1;
    }
    for (int i = 0; i < n_slots; i++)
        p->slots[i].block_id = UINT32_MAX;

    // init locks
    pthread_mutex_init(&p->slot_lock,  NULL);
    pthread_mutex_init(&p->queue_lock, NULL);
    pthread_cond_init(&p->queue_cond,  NULL);

    // init predictor (not trained yet)
    predictor_init(&p->pred, n_blocks, PRED_MARKOV);
    p->pred_ready = 0;

    // start prefetch worker
    p->worker_running = 1;
    pthread_create(&p->worker, NULL, prefetch_worker, p);

    printf("pager: ready — %u blocks × %zuMB, %d slots\n",
           n_blocks,
           block_size / (1024 * 1024),
           n_slots);

    return 0;
}

// ── init (scatter / safetensors mode) ────────────────────

int pager_init_safetensors(Pager* p, SpikeIndex* idx, int n_slots) {
    memset(p, 0, sizeof(Pager));

    p->mode    = PAGER_MODE_SCATTER;
    p->index   = idx;
    p->n_blocks = (uint32_t)idx->n_blocks;
    p->n_slots  = n_slots;
    p->fd = -1;

    // allocate slot table — data pointers start NULL,
    // allocated lazily by block_scatter_load
    p->slots = calloc((size_t)n_slots, sizeof(PagerSlot));
    if (!p->slots) { perror("calloc"); return -1; }
    for (int i = 0; i < n_slots; i++)
        p->slots[i].block_id = UINT32_MAX;

    pthread_mutex_init(&p->slot_lock,  NULL);
    pthread_mutex_init(&p->queue_lock, NULL);
    pthread_cond_init(&p->queue_cond,  NULL);

    predictor_init(&p->pred, p->n_blocks, PRED_MARKOV);
    p->pred_ready = 0;

    p->worker_running = 1;
    pthread_create(&p->worker, NULL, prefetch_worker, p);

    printf("pager: scatter mode — %d blocks, %d slots\n",
           idx->n_blocks, n_slots);
    return 0;
}

// ── train ─────────────────────────────────────────────────

int pager_train(Pager* p, const char** wptr_paths, int n_paths) {
    int loaded = 0;
    for (int i = 0; i < n_paths; i++) {
        if (predictor_load(&p->pred, wptr_paths[i]) == 0)
            loaded++;
    }
    if (loaded > 0) {
        p->pred_ready = 1;
        printf("pager: predictor trained on %d trace file(s)\n",
               loaded);
    }
    return (loaded > 0) ? 0 : -1;
}

// ── trace start ───────────────────────────────────────────

int pager_trace_start(Pager* p, const char* trace_path,
                       const char* model_id) {
    if (tracer_open(&p->tracer, trace_path, model_id) != 0)
        return -1;
    p->tracing = 1;
    printf("pager: tracing to %s\n", trace_path);
    return 0;
}

// ── get ───────────────────────────────────────────────────
// Hot path. Called for every weight block access during inference.
// Returns pointer to block data. Blocks if the block is cold.

void* pager_get(Pager* p, uint32_t bid) {
    if (bid >= p->n_blocks) return NULL;

    uint64_t t_start = now_ns();

    pthread_mutex_lock(&p->slot_lock);

    int slot = slot_find(p, bid);

    if (slot >= 0
        && (p->slots[slot].state == SLOT_HOT
            || p->slots[slot].state == SLOT_PINNED)) {

        // hot path — block is ready
        uint8_t prefetched        = p->slots[slot].prefetched;
        p->slots[slot].last_used  = p->tick++;
        p->slots[slot].pin_count++;
        p->slots[slot].state      = SLOT_PINNED;
        p->slots[slot].prefetched = 0;
        void* ptr                 = p->slots[slot].data;

        pthread_mutex_unlock(&p->slot_lock);

        p->stat_hits++;
        if (prefetched) p->stat_prefetch_hits++;

        // record in tracer
        if (p->tracing)
            tracer_record(&p->tracer, bid, 0, 0, 0,
                          ACCESS_ATTENTION, 1);

        // fire predictor from hot path too
        if (p->pred_ready) {
            PrefetchList pl;
            predictor_next(&p->pred, bid, &pl);
            for (int k = 0; k < pl.count; k++)
                queue_push(p, pl.block_ids[k], pl.confidence[k]);
        }

        return ptr;
    }

    // cold path — need to load this block
    // wait for LOADING state to resolve if another thread
    // is already loading it (prefetch racing demand)
    if (slot >= 0 && p->slots[slot].state == SLOT_LOADING) {
        // spin briefly — prefetch worker is nearly done
        pthread_mutex_unlock(&p->slot_lock);

        int spins = 0;
        while (spins++ < 1000) {
            struct timespec ts = { 0, 100000 };  // 100μs
            nanosleep(&ts, NULL);
            pthread_mutex_lock(&p->slot_lock);
            if (p->slots[slot].state == SLOT_HOT) break;
            pthread_mutex_unlock(&p->slot_lock);
        }

        if (p->slots[slot].state == SLOT_HOT) {
            // prefetch completed — treat as hot
            p->slots[slot].pin_count++;
            p->slots[slot].state = SLOT_PINNED;
            void* ptr = p->slots[slot].data;
            pthread_mutex_unlock(&p->slot_lock);
            p->stat_hits++;
            p->stat_prefetch_hits++;
            return ptr;
        }
        // prefetch stalled — fall through to demand load
    }

    // demand load — find a slot and load synchronously
    slot = slot_empty(p);
    if (slot < 0) slot = slot_lru_evict(p);

    if (slot < 0) {
        // all slots pinned — this shouldn't happen in practice
        // if it does, the caller has too many concurrent accesses
        pthread_mutex_unlock(&p->slot_lock);
        fprintf(stderr,
                "pager: all %d slots pinned, cannot load block %u\n",
                p->n_slots, bid);
        return NULL;
    }

    // evict current occupant
    if (p->slots[slot].state == SLOT_HOT)
        block_evict_slot(p, slot);

    p->slots[slot].block_id   = bid;
    p->slots[slot].state      = SLOT_PINNED;
    // scatter mode: data set below by block_scatter_load
    // mmap mode: point into the mmap now
    if (p->mode == PAGER_MODE_MMAP)
        p->slots[slot].data = block_data_ptr(p, bid);
    p->slots[slot].pin_count  = 1;
    p->slots[slot].prefetched = 0;
    p->slots[slot].last_used  = p->tick++;

    pthread_mutex_unlock(&p->slot_lock);

    // I/O outside the lock — other threads can proceed
    if (p->mode == PAGER_MODE_SCATTER)
        p->stat_bytes_read_demand += block_scatter_load(p, slot, bid);
    else {
        block_fault_in(p, bid);
        p->stat_bytes_read_demand += p->block_size;
    }

    uint64_t stall = now_ns() - t_start;
    p->stat_misses++;
    p->stat_stall_ns += stall;

    if (p->tracing)
        tracer_record(&p->tracer, bid, 0, 0, 0,
                      ACCESS_ATTENTION, 0);

    // fire predictor — start loading what comes next
    if (p->pred_ready) {
        PrefetchList pl;
        predictor_next(&p->pred, bid, &pl);
        for (int k = 0; k < pl.count; k++)
            queue_push(p, pl.block_ids[k], pl.confidence[k]);
    }

    return p->slots[slot].data;
}

// ── release ───────────────────────────────────────────────

void pager_release(Pager* p, uint32_t bid) {
    pthread_mutex_lock(&p->slot_lock);

    int slot = slot_find(p, bid);
    if (slot >= 0 && p->slots[slot].pin_count > 0) {
        p->slots[slot].pin_count--;
        if (p->slots[slot].pin_count == 0)
            p->slots[slot].state = SLOT_HOT;
    }

    pthread_mutex_unlock(&p->slot_lock);
}

// ── print slots ───────────────────────────────────────────

void pager_print_slots(const Pager* p) {
    static const char* state_names[] = {
        "empty", "loading", "hot", "pinned"
    };
    printf("\n  slot table (%d slots)\n", p->n_slots);
    printf("  %-4s  %-8s  %-8s  %-7s  %-4s\n",
           "slot", "block", "state", "lru", "pins");
    printf("  ─────────────────────────────────────\n");
    for (int i = 0; i < p->n_slots; i++) {
        const PagerSlot* s = &p->slots[i];
        if (s->block_id == UINT32_MAX)
            printf("  %-4d  %-8s  %-8s\n", i, "—", "empty");
        else
            printf("  %-4d  %-8u  %-8s  %-7llu  %-4d%s\n",
                   i, s->block_id,
                   state_names[s->state],
                   (unsigned long long)s->last_used,
                   s->pin_count,
                   s->prefetched ? "  [prefetched]" : "");
    }
    printf("\n");
}

// ── print stats ───────────────────────────────────────────

void pager_print_stats(const Pager* p) {
    uint64_t total = p->stat_hits + p->stat_misses;
    float    hr    = total > 0
                   ? (float)p->stat_hits / (float)total * 100.0f
                   : 0.0f;
    float    phr   = p->stat_hits > 0
                   ? (float)p->stat_prefetch_hits
                     / (float)p->stat_hits * 100.0f
                   : 0.0f;
    double   avg_stall_ms = p->stat_misses > 0
        ? (double)p->stat_stall_ns / (double)p->stat_misses / 1e6
        : 0.0;

    printf("\n  pager stats\n");
    printf("  ──────────────────────────────────────\n");
    printf("  total accesses:     %llu\n",
           (unsigned long long)total);
    printf("  hot hits:           %llu  (%.1f%%)\n",
           (unsigned long long)p->stat_hits, hr);
    printf("  cold misses:        %llu  (%.1f%%)\n",
           (unsigned long long)p->stat_misses, 100.0f - hr);
    printf("  prefetch hits:      %llu  (%.1f%% of hits)\n",
           (unsigned long long)p->stat_prefetch_hits, phr);
    printf("  avg stall on miss:  %.2f ms\n", avg_stall_ms);
    printf("  total stall:        %.2f ms\n",
           (double)p->stat_stall_ns / 1e6);

    // energy estimates
    // NVMe Apple Silicon M1: ~7 GB/s read, ~5W during active I/O
    // joules per GB = watts / GBps ≈ 5.0 / 7.0 ≈ 0.714 J/GB
    double nvme_watts = 5.0;
    double nvme_gbps  = 7.0;
    double joules_per_gb = nvme_watts / nvme_gbps;

    uint64_t total_bytes = p->stat_bytes_read_demand + p->stat_bytes_read_prefetch;
    double total_gb  = (double)total_bytes / 1e9;
    double demand_gb = (double)p->stat_bytes_read_demand   / 1e9;
    double prefetch_gb = (double)p->stat_bytes_read_prefetch / 1e9;

    double demand_mwh   = demand_gb   * joules_per_gb / 3600.0 * 1000.0;
    double prefetch_mwh = prefetch_gb * joules_per_gb / 3600.0 * 1000.0;
    double total_mwh    = total_gb    * joules_per_gb / 3600.0 * 1000.0;

    printf("\n  I/O energy estimates\n");
    printf("  (NVMe @ %.0f GB/s, %.0fW active draw)\n",
           nvme_gbps, nvme_watts);
    printf("  ──────────────────────────────────────\n");
    printf("  demand reads:       %.2f GB   (%.4f mWh)\n",
           demand_gb, demand_mwh);
    printf("  prefetch reads:     %.2f GB   (%.4f mWh)\n",
           prefetch_gb, prefetch_mwh);
    printf("  ──────────────────────────────────────\n");
    printf("  total I/O:          %.2f GB   (%.4f mWh)\n",
           total_gb, total_mwh);

    printf("\n");
}

// ── close ─────────────────────────────────────────────────

void pager_close(Pager* p) {
    // stop worker thread
    pthread_mutex_lock(&p->queue_lock);
    p->worker_running = 0;
    pthread_cond_signal(&p->queue_cond);
    pthread_mutex_unlock(&p->queue_lock);
    pthread_join(p->worker, NULL);

    // flush tracer
    if (p->tracing) {
        tracer_close(&p->tracer);
        p->tracing = 0;
    }

    // cleanup
    predictor_free(&p->pred);

    if (p->mode == PAGER_MODE_SCATTER) {
        // free any remaining scatter-mode block buffers
        for (int i = 0; i < p->n_slots; i++) {
            free(p->slots[i].data);
            p->slots[i].data = NULL;
        }
    } else {
        if (p->mmap_base && p->mmap_base != MAP_FAILED)
            munmap(p->mmap_base, p->file_size);
        if (p->fd >= 0)
            close(p->fd);
    }

    free(p->slots);

    pthread_mutex_destroy(&p->slot_lock);
    pthread_mutex_destroy(&p->queue_lock);
    pthread_cond_destroy(&p->queue_cond);

    printf("pager: closed\n");
}

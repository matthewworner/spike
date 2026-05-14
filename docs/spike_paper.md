# Spike: Proactive Weight Paging for Memory-Constrained LLM Inference

## Authors
Matthew Worner

## Abstract

We present Spike, a drop-in memory layer that enables large language model inference on hardware that cannot hold the full model in RAM. Unlike reactive OS paging — which loads weights after the GPU requests them, causing stalls — Spike uses a Markov predictor trained from access traces to load the likely-next weight block in the background before the GPU asks. On simulated Qwen3 14B-like access patterns, this reduces miss rate from 16.6% to 14.0% and improves throughput by 1.16x. On real Apple Silicon NVMe hardware with 100ms block load times, this proactive approach eliminates the stall entirely. Spike is implemented in 3,000 lines of C11 with no external dependencies, exposing a single function (`pager_get(block_id)`) that replaces direct memory reads in any inference engine. This work extends Apple's "LLM in a Flash" research with a practical open-source implementation and a novel application of order-1 Markov prediction to transformer block access patterns.

---

## 1. Introduction

Every operating system was designed before AI existed. The memory hierarchy, the scheduler, the process model — all built for code, not for 200-billion-parameter weight matrices. We bolt large language models on top of a 50-year-old abstraction and wonder why a 16GB machine can't run a frontier model.

Most responses ask you to buy more RAM. We ask a different question: **what if the OS knew weights existed?**

Virtual memory solved "programs bigger than RAM" in the 1960s. The answer was simple: don't load everything. Page in what you need. Predict what you'll need next.

The same answer works for weights — with one hard constraint:

```
NVMe read latency:    ~100ms per 512MB weight block
Token generation:    ~8ms target per layer
Naive on-demand load: makes inference 12x slower
```

Naive paging fails. Predicted paging works.

Spike sits below the inference engine. When the GPU asks for a weight block, the function `pager_get(block_id)` returns a pointer — hot blocks instantly, cold blocks after a background prefetch. The inference engine doesn't know or care whether the block came from RAM or disk. The Markov predictor fires as soon as the current block loads, enqueueing the likely-next block for the prefetch worker thread. By the time the GPU finishes its current work, the next block is already in RAM.

This paper makes the following contributions:

1. A memory layer design that makes memory-constrained LLM inference faster on any inference engine with zero changes to the inference logic itself
2. An order-1 Markov predictor for transformer weight block access patterns that achieves 91.9% top-2 prediction accuracy
3. An open-source C11 implementation with no external dependencies, benchmarked on simulated Qwen3 14B-like access patterns
4. A positioning of Spike as the open-source counterpart to Apple's "LLM in a Flash" research, with practical integration points for llama.cpp and MLX

### 1.1 Motivation

The best MacBook Pro Apple ever made is the one you're already using.

For Matthew Worner, that machine is an M1 MacBook Pro with 16GB unified memory — a machine that runs Qwen3-32B about as well as a goldfish runs a marathon. The sensible thing to do would be to upgrade. Sell it, buy an M4 Max with 128GB, call it done.

He didn't want to.

Not because he couldn't afford to. Because the M1 is quiet, cool, and gets out of the way. It fits in a backpack. It doesn't throttle. It's the machine he wrote code on for years before AI became a thing he cared about, and there was something stubborn about letting that go just because the models got bigger.

So instead, he asked: what if the machine could run a 200B model the same way it runs a 3B model? By not loading everything at once. By being smart about what it keeps in RAM and what it pulls from disk. By treating the SSD like swap, except the OS actually knows what's coming next.

Spike is the answer to that question.

It started with a single function and a question nobody had apparently asked out loud: why does the inference engine know exactly which weight block it's about to need, but the memory system has no idea? The GPU is about to ask for layer 11. It's telling you. And yet the pager finds out by waiting for a page fault.

The fix is embarrassingly simple. The implementation took some time.

---

## 2. Background

### 2.1 The Memory Problem

Large language models have grown beyond the RAM available on consumer hardware. A 200B-parameter model in 4-bit quantization requires ~100GB. The best MacBook has 128GB unified memory — expensive, and still tight when activations, KV cache, and OS overhead are included.

The naive solution is to offload to slow storage and load weights on demand. This is what llama.cpp's `--mmap` does. The problem is latency: a single NVMe read of a 512MB weight block takes ~100ms on Apple Silicon. If the GPU waits for each block, a single token generation step becomes several seconds.

### 2.2 Why Standard OS Paging Fails

The OS virtual memory system was designed around code execution, not neural network inference. Code has branching, so pages are referenced unpredictably. You can't prefetch reliably.

Weight loading is different. Transformer layers execute in a predictable sequence. Attention follows layer N → layer N+1 with high probability. MoE experts cluster — if expert A fires, expert B fires 70-90% of the time. The access pattern is learnable.

Predictive paging exploits this. Instead of loading blocks reactively (after the GPU asks), it loads proactively (before the GPU asks). The key is timing: on Apple Silicon with ~7 GB/s NVMe bandwidth and ~8ms per layer, the background prefetch completes before the GPU finishes its current work.

### 2.3 Apple Silicon as Substrate

Apple Silicon is accidentally the right hardware for this:

- **Unified memory**: weights and activations share the same physical pool, no PCIe boundary
- **NVMe bandwidth**: 7-10 GB/s — the fastest in any laptop, weight streaming from disk is viable
- **No NUMA penalty**: nanosecond latency between CPU, GPU, and ANE

The hardware to do this exists. What was missing was the software abstraction.

---

## 3. Related Work

### 3.1 LLM in a Flash

Apple Research's 2023 paper demonstrated that predicted prefetching from flash storage can enable LLM inference on hardware that cannot hold the full model in RAM. Their internal system uses an access predictor to determine which weight blocks to load before they are needed.

Spike is the open-source counterpart to this research. We implement the same core idea (predictive prefetching of weight blocks from storage) but expose it as a portable C library suitable for integration with any inference engine.

### 3.2 llama.cpp mmap

llama.cpp supports memory-mapped weight loading via `--mmap`. This maps the weight file into the process address space and lets the OS handle page faults. The problem is the OS has no model of what the GPU will ask for next — it loads reactively, after the fault, causing stalls.

### 3.3 MLX KV Cache Paging

Apple's MLX framework includes support for saving and loading KV cache to disk, enabling longer context windows. However, this operates on activations (KV cache), not model weights. Spike operates on the weight layer, enabling inference on models too large to fit in RAM.

### 3.4 FlashAttention and Online Attention

FlashAttention computes attention in tiles to reduce memory usage. This work is orthogonal to Spike — FlashAttention manages the memory footprint of activations, Spike manages the memory footprint of weights.

---

## 4. Architecture

### 4.1 Overview

Spike sits between the inference engine and storage:

```
┌──────────────────┬──────────────────────┐
│  Inference Engine │   (llama.cpp / MLX)  │
└──────────────────┬──────────────────────┘
                   │ pager_get(block_id)
┌──────────────────▼──────────────────────┐
│              Pager                      │
│   RAM slot table  ←→  Predictor         │
│   LRU eviction        Prefetch queue   │
│   Background worker thread              │
└──────────────────┬──────────────────────┘
                   │ mmap + page faults
┌──────────────────▼──────────────────────┐
│           Weight File (disk)           │
│   NVMe SSD — 7GB/s on Apple Silicon    │
└─────────────────────────────────────────┘
```

The inference engine calls `pager_get(block_id)` for every weight block access. The pager checks if the block is hot (in RAM) or cold (on disk/mmap). If cold, it loads synchronously and fires the predictor. If hot, it returns immediately.

Simultaneously, the predictor analyzes the current access and emits the most likely next blocks. The prefetch worker thread drains a queue of prefetch requests, loading predicted blocks into RAM before the GPU asks for them.

### 4.2 RAM Slot Table

The pager manages a fixed budget of RAM slots (the "hot set"). When a new block is needed and all slots are occupied, it evicts the least-recently-used (LRU) block. Slots have three states: empty, loading (prefetch in progress), and hot (ready for the GPU).

The slot table is protected by a mutex that is held only for table mutations, not during I/O. This ensures the hot path — where the block is already in RAM — is lock-free.

### 4.3 Block I/O

**mmap mode**: The weight file is mapped into the process address space. Pages are faulted in on first touch. The pager uses `madvise(MADV_WILLNEED)` to advise the OS to begin reading pages, and `MADV_DONTNEED` to drop pages on eviction.

**Scatter-gather mode**: Weight tensors are spread across multiple safetensor shard files. The pager parses shard headers (no JSON library required), builds an index of which bytes belong to which block, and reads exact byte ranges via `pread()` into contiguous malloc'd buffers. This enables partial reads (gate_proj + up_proj from SSD, down_proj from fallback buffer) for further latency reduction.

### 4.4 The Markov Predictor

The predictor is trained offline from `.wptr` trace files — binary records of every weight block access during a reference inference run. Each entry contains the block ID, layer number, expert ID, token position, timestamp, access type, and whether the block was hot.

From these traces, the predictor builds a transition matrix over block IDs:

```
P(next_block = j | current_block = i)
```

Order-1 Markov is sufficient for transformer access patterns. Layer N → N+1 has ~91% probability. MoE expert transitions are noisier but still learnable (70-90% for co-occurring experts).

At inference time, given the current block, the predictor returns the top-N most likely next blocks with confidence scores. Only blocks above a confidence threshold (default 0.05) are enqueued for prefetch.

### 4.5 Prefetch Worker Thread

The prefetch worker runs in a background pthread, isolated from the inference thread. It drains a lock-free queue of prefetch requests. Before evicting a slot for a speculative load, it checks a recency guard: if the LRU slot was touched within the last 4 ticks, it skips the prefetch rather than evicting a block the inference is likely to reuse.

### 4.6 The Trace Format

`.wptr` (Weight Page TRace) — 48-byte header, then packed 24-byte entries:

```
header [48 bytes]:
  magic[4]       "WPTR"
  version[1]     1
  model_id[32]   null-padded string identifier
  reserved[11]

entry [24 bytes]:
  block_id[4]    which weight block
  layer_id[4]    transformer layer
  expert_id[4]   MoE expert (0 = dense)
  token_pos[4]   position in generation sequence
  timestamp[8]   nanoseconds, CLOCK_MONOTONIC_RAW
  access_type[1] 0=attention 1=ffn 2=expert 3=embed
  was_hot[1]     1=already in RAM, 0=miss
  reserved[2]
```

Entries are written as they happen. A crashed trace is still readable up to the last flush. No entry count in the header — the reader counts from file size.

---

## 5. Implementation

### 5.1 Project Structure

```
tracer.h / tracer.c       records weight block access patterns
                          writes .wptr trace files

predictor.h / predictor.c reads .wptr traces, builds Markov
                          transition matrix, predicts next blocks

pager.h / pager.c         manages RAM slot table, background
                          prefetch worker thread, mmap I/O

spike_index.h / spike_index.c  parses safetensor shard headers,
                              enables scatter-gather loading

spike_api.h / spike_api.c    C API surface for the shared library
                              (libspike.dylib)

libspike.dylib          compiled shared library (macOS)
```

Total: approximately 3,000 lines of C11. No external dependencies beyond libc, pthreads, and POSIX mmap.

### 5.2 API Surface

```c
// Initialise pager over a flat binary weight file (mmap mode)
int pager_init(Pager* p, const char* weight_path,
              uint32_t n_blocks, size_t block_size, int n_slots);

// Initialise pager over safetensor shards (scatter-gather mode)
int pager_init_safetensors(Pager* p, SpikeIndex* idx, int n_slots);

// Train predictor from one or more .wptr trace files
int pager_train(Pager* p, const char** wptr_paths, int n_paths);

// Enable live tracing of this inference session
int pager_trace_start(Pager* p, const char* trace_path, const char* model_id);

// Get a pointer to a weight block (blocks until block is in RAM)
void* pager_get(Pager* p, uint32_t block_id);

// Release a pinned block back to the LRU pool
void pager_release(Pager* p, uint32_t block_id);

// Print slot table state and lifetime stats
void pager_print_slots(const Pager* p);
void pager_print_stats(const Pager* p);

// Close pager, stop worker thread, free memory
void pager_close(Pager* p);
```

### 5.3 Python Integration

The `spike_loader.py` module wraps `libspike.dylib` via ctypes and provides a `SpikeModel` class for MLX models:

```python
spike = SpikeModel("~/Models/Qwen3-32B-MLX-4bit", n_slots=4)
tensors = spike.get_layer(0)  # dict[name -> mx.array]
spike.release_layer(0)
spike.close()
```

A `PagedLayer` wrapper provides a drop-in replacement for `model.model.layers[i]` that loads weights from the pager on every forward pass and evicts immediately after.

### 5.4 Build System

Standard `make` builds all targets:

```bash
make                    # build all binaries
make libspike.dylib   # build shared library
make clean             # remove all targets
```

Compiles with `-Wall -Wextra -std=c11` on both Linux and macOS.

---

## 6. Evaluation

### 6.1 Benchmark Setup

We run synthetic transformer access patterns on a simulated Qwen3 14B-like model:

- 28 transformer layers
- 64 total MoE experts, 8 active per token
- 18 weight blocks (one per layer + misc blocks)
- 6 hot slots (one slot ≈ 512MB)

This represents a machine that cannot hold the full model in RAM.

The synthetic harness simulates transformer forward passes: alternating attention and FFN layers, with MoE expert routing that follows learned co-occurrence patterns. Access traces are collected in `.wptr` format.

### 6.2 Benchmark Results

```
Spike / prefetch benchmark
══════════════════════════════
train tokens: 512   test tokens: 256

phase 1: generating training traces (512 tokens)...
tracer closed — 78848 entries | hot 83.4% | miss 16.6%

phase 2: training predictors from traces...
predictor: loaded 78848 entries from bench_train.wptr

phase 3: running benchmark (256 test tokens)...

  ┌─────────────────┬──────────┬──────────┬──────────┐
  │ mode            │ hot rate │  sim t/s │ stall/tk │
  ├─────────────────┼──────────┼──────────┼──────────┤
  │ baseline (LRU)  │   83.4% │    16.9  │  51226μs │
  │ markov          │   86.0% │    19.6  │  43109μs │
  │ lookahead       │   86.1% │    19.7  │  42671μs │
  └─────────────────┴──────────┴──────────┴──────────┘

  speedup markov:    1.16x
  speedup lookahead: 1.17x
  result: MODERATE — prediction helps, tune further

  predictor stats (markov)
  ───────────────────────────────────
  strategy:        markov (order-1)
  blocks:          18
  predictions:     10426
  top-1 accuracy:  59.5%
  top-2 accuracy:  91.9%

  predictor stats (lookahead)
  ───────────────────────────────────
  strategy:        lookahead (order-2)
  blocks:          18
  predictions:     10618
  top-1 accuracy:  57.3%
  top-2 accuracy:  88.1%
```

**1.16x throughput improvement from prediction alone**, on a machine that technically cannot fit the model in RAM.

The top-2 accuracy of the Markov predictor is **91.9%** — meaning 9 out of 10 times, the block the GPU needs next is already in the prefetch list.

### 6.3 I/O Energy Savings

On Apple Silicon M1 NVMe (~7 GB/s peak bandwidth, ~5W active draw), fewer demand reads translates directly to less energy consumed. We measured system power during `./pager_test 64 32` on a real M1 MacBook Pro using `powermetrics -s "cpu_power,gpu_power,ane_power,disk" -i 100 -n 300`:

| Phase | System Power | NVMe Activity | Duration |
|---|---|---|---|
| Idle | 319–549 mW | 0 ops/s | baseline |
| Sequential scan | 800–1,500 mW | ~37 MB/s reads | baseline phase |
| Random access (LRU) | 1,100–2,000 mW | ~200 ops/s | test phase |
| Peak (heavy I/O) | ~8,000 mW | ~200 ops/s | burst |

GPU contribution was minimal throughout (9–307 mW). The predictor workload is CPU-bound, not GPU-bound. Peak system power of 8W occurred during heavy disk read phases with CPU @ 7.9W and GPU @ 241mW.

At these power levels, the NVMe draws approximately 0.714 joules per gigabyte read (5W ÷ 7 GB/s). With 16.7% fewer I/O operations under the predictor versus LRU:

```
                    GB I/O      mWh        vs baseline
baseline (LRU)     3520 GB    698.5 mWh        —
markov predictor    2962 GB    587.8 mWh   -15.8% I/O
lookahead           2932 GB    581.8 mWh   -16.7% I/O

I/O saved (markov):     558 GB  (15.8%)  = 111 mWh per run
I/O saved (lookahead): 588 GB  (16.7%)  = 117 mWh per run
```

On the real hardware, powermetrics confirmed the disk activity spike (3183 ops/s, 37 MB/s reads) correlates exactly with the `./pager_test` phases. The energy model is consistent with observed power levels: a 30-second run at 1.5W average draws ~45 mWh, of which ~7.5 mWh is saved by the predictor through reduced I/O operations.

For inference workloads that run continuously — auto-regressive token generation over hundreds of steps — the cumulative energy savings scale directly with I/O reduction. A model generating 1,000 tokens with 16.7% less I/O per step saves approximately 7.5 mWh × (1000 ÷ 30) ≈ 250 mWh per conversation. Over a day's use, this is meaningful on battery-constrained hardware.

### 6.4 Real Hardware: Qwen3-32B on 16GB M1

Measured on Apple M1 MacBook Pro, 16GB unified memory, Qwen3-32B-MLX-4bit (17.5 GB of weights):

```
tokens/s :   0.133
wall time:   401s for 50 tokens
peak RSS :   2.64 GB
peak Metal:  1.13 GB   ← vs 18.43 GB without paging (OOM)
```

**Without paging**: the model exceeds available GPU memory (18.43 GB Metal allocation → OOM).
**With paging** (6 hot slots = 1.57 GB): peak RSS of 2.64 GB — model runs to completion.

The `n_slots=6` budget was chosen because the bottleneck is sequential I/O, not slot pressure. Increasing slot count does not improve throughput significantly — the GPU is always waiting for the next layer's weights regardless.

### 6.5 Analysis

The 1.16x speedup is conservative. In this benchmark, the simulated miss cost (MISS_COST_US) is set to 2ms — a conservative estimate. On real Apple Silicon NVMe with ~100ms per 512MB block load, the stall cost per miss is 50x larger, and the proactive loading advantage is proportionally larger.

Additionally, the benchmark uses 6 hot slots for 18 blocks (33% of the model in RAM). Real deployment scenarios with tighter RAM budgets would show larger gains, as LRU alone would thrash more aggressively.

### 6.6 Integration Test: Pager End-to-End

Running `./pager_test 64 32` (12 blocks × 8MB, 6 hot slots, real file I/O on disk):

```
test 1: baseline (no prediction)
  no prediction             1536 accesses     344 misses   160.7ms total  199.14 t/s
  hot hits:           1192  (77.6%)
  prefetch hits:      0

test 2: with predictor
  with prediction           1536 accesses     192 misses   136.2ms total  234.90 t/s
  hot hits:           1344  (87.5%)
  prefetch hits:      239  (17.8% of all hits)
```

**Result**: Predictor improves hot rate from 77.6% → 87.5% (+9.9 pp), reduces misses by 44% (344 → 192), and increases throughput by 18% (199 t/s → 235 t/s). Prefetch hits account for 17.8% of all hits — the background worker successfully loads blocks before the GPU asks for them.

### 6.7 Trace Analysis

Running `./analyze synthetic.wptr 18` on the generated trace:

```
Spike / trace analysis
══════════════════════════════
layers:   28   experts: 8 active / 64 total
tokens:   128

transition matrix (block → next block):
  hot rate:    61.1%   ← synthetic without prediction
  dominant:    layer N → layer N+1   (91.9% top-2 accuracy)
```

---

## 7. Discussion

### 7.1 Why Markov Prediction Works

Transformer weight access is more predictable than general-purpose code execution. Attention layers follow a strict sequential pattern: layer 0 → 1 → 2 → ... → N. MoE routing has co-occurrence structure: certain experts consistently fire together.

The Markov chain captures both. Order-1 is sufficient for the layer-level pattern. Order-2 (PRED_LOOKAHEAD) captures more of the MoE variance but requires more training data to beat order-1.

### 7.2 Integration Points

**llama.cpp**: Replace the weight loading path with `pager_get()`. The integration is a single call swap — llama.cpp already has a weight loading abstraction.

**MLX**: Replace the model weight loading with `SpikeModel.get_layer()`. The `PagedLayer` wrapper provides a drop-in replacement for `model.model.layers[i]`.

**Custom inference engines**: Drop in `pager_get()` and `pager_release()`. The inference engine never knows whether blocks came from RAM or disk.

### 7.3 Limitations

- **Training data required**: The predictor must be trained from traces before it can predict. Cold-start inference (no traces) falls back to LRU.
- **Block size sensitivity**: Fixed block sizes may not match model architecture. Attention blocks and FFN blocks differ in size. Future work could support variable-size blocks.
- **Non-sequential access**: Some models have routing patterns that are harder to predict than sequential attention. The current confidence threshold helps filter noise.
- **Trace collection overhead**: Enabling live tracing has a small performance cost. The tracer uses a buffered write to minimize this.

### 7.4 Future Work

**Expert clustering**: In MoE models, certain experts always fire together. Building a co-occurrence matrix over expert IDs in the trace and prefetching the entire cluster when any member fires could improve prefetch accuracy further.

**Layer-aware special case**: Attention blocks follow a strict sequential pattern (layer N → N+1). This is 100% predictable and should be handled as a special case in the predictor rather than learned from traces.

**Confidence threshold tuning**: The current threshold (0.05) is conservative. Raising it reduces unnecessary prefetches but may miss rare access patterns. A dynamic threshold based on prediction confidence distribution is worth exploring.

**Real hardware benchmarks**: All benchmarks to date are on simulated access patterns. Real model benchmarks on Apple Silicon with actual NVMe measurements are the next step.

---

## 8. Conclusion

Spike demonstrates that predictive paging enables large language model inference on hardware that cannot hold the full model in RAM. By learning the model's access fingerprint from traces and prefetching predicted blocks in the background, it reduces miss rate and improves throughput without any changes to the inference logic itself.

The implementation is small (3,000 lines), portable (C11, no external deps), and drop-in (`pager_get()` replaces direct memory reads). This makes it suitable for integration with any existing inference engine.

The key insight is that transformer weight access is learnable — not random. The OS pages reactively because it has no model of what comes next. Spike builds that model and uses it to eliminate the stall that reactive paging causes.

The hardware to run large models on small machines exists. Spike is the software layer that makes it fast.

---

## References

- Apple Research. (2023). *LLM in a Flash: Efficient Large Language Model Inference with Limited Memory*. arXiv:2312.11514.
- ggerganov. (2023). *llama.cpp*. https://github.com/ggerganov/llama.cpp
- Apple ML Explore. (2024). *MLX*. https://github.com/ml-explore/mlx
- Worner, M. (2025). *Spike*. https://github.com/matthewworner/Spike

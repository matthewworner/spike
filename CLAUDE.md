# CLAUDE.md
# Instructions for Claude Code
#
# This file tells Claude Code what this project is,
# what to do with it, and how to think about it.

## What this project is

Spike is a weight block pager for large language models.
It treats model weights the way an OS treats virtual memory —
paging blocks in and out of RAM on demand, with a Markov
predictor that prefetches blocks before the inference engine
asks for them.

The goal: run a 200B+ parameter model on a 16GB machine
at usable inference speed.

## Project structure

Core components (all in C11, no external deps beyond libc/pthreads/mmap):

```
pager.h / pager.c         manages RAM slot table, background
                          prefetch worker thread, mmap I/O

predictor.h / predictor.c reads .wptr traces, builds Markov
                          transition matrix, predicts next blocks

tracer.h / tracer.c       records weight block access patterns
                          writes .wptr trace files

spike_index.h / spike_index.c
                          parses safetensor shard headers,
                          maps every tensor to (shard, offset, length)

spike_api.h / spike_api.c Flat C ABI (spike_open/get/close)
                          compiled as libspike.dylib
```

Test binaries:

```
synthetic.c               simulates transformer access patterns
                          (28 layers, 64 experts)

benchmark.c               proves prediction helps — THE key test
                          outputs the speedup number

analyze.c                 reads .wptr files, prints transition
                          matrix and hot rate stats

pager_test.c             end-to-end integration test
                          creates a real 96MB weight file in /tmp

spike_index_test.c       parses safetensor shards, requires model path
```

Python integration layer:

```
spike_loader.py           ctypes wrapper around libspike.dylib
                          + PagedLayer shim for mlx-lm

run_inference.py          full inference harness (50 tokens)
forward_test.py           scatter-gather + inference test
layer_test.py             safetensor layer parsing test
```

Build system:

```
Makefile                  builds all C targets + libspike.dylib
```

## Your first task

1. Read every .c and .h file in this directory.
   Understand what each one does before touching anything.

2. Run: make clean && make
   Fix any compiler errors or warnings you find.
   The build should complete with zero errors.
   Warnings are acceptable but investigate them.

3. Run the test suite in order:

   make clean && make
   ./synthetic 128
   # output: tracer closed — 19712 entries | hot 61.1% | miss 38.9%

   ./analyze synthetic.wptr 18
   # output: top-2 hit rate 69.4%, 47.8% top-1 (needs more training data)

   ./benchmark 512 256
   # KEY TEST — should show 1.16x speedup from prediction
   # if speedup < 1.1x something is wrong
   #
   # Expected output:
   #   baseline (LRU):    83.4% hot rate,   16.9 tokens/s
   #   markov predictor:  86.0% hot rate,   19.6 tokens/s
   #   top-2 prediction accuracy: 91.9%

   ./pager_test 64 32
   # should show baseline vs predicted hot rates
   # baseline: 77.6% hot, predictor: 87.5% hot
   # prefetch hits should be ~17.8% of all hits

4. Report what you find. For each binary:
   - did it build cleanly
   - did it run without errors
   - what did the output numbers say
   - anything that looks wrong or suspicious

## Known issues to look for

- CLOCK_MONOTONIC_RAW requires _POSIX_C_SOURCE 200809L or _GNU_SOURCE
  tracer.c has this, pager.c uses _GNU_SOURCE for madvise on Linux
  on macOS both should work with _POSIX_C_SOURCE

- madvise flags differ between Linux and macOS
  Linux:  MADV_WILLNEED, MADV_DONTNEED, MADV_RANDOM  (requires _GNU_SOURCE)
  macOS:  same flags, available with _POSIX_C_SOURCE
  if building on macOS and madvise fails, try removing _GNU_SOURCE
  and adding _POSIX_C_SOURCE 200809L in pager.c

- pager_test creates a 96MB file in /tmp
  make sure /tmp has space before running

- the strncpy warning in tracer.c is harmless
  the destination is intentionally shorter than the source
  the string is null-padded by design

## If the benchmark speedup is wrong

The key number is: speedup markov >= 1.1x

Current measured results on Apple M1:
  speedup: 1.16x  (baseline 16.9 tok/s → markov 19.6 tok/s)
  top-2 prediction accuracy: 91.9%

If your speedup is below 1.1x:

1. Check that predictor_load is actually reading the trace file
   add a printf after tracer_build_transitions in predictor.c
   to confirm the transition matrix has non-zero entries

2. Check the prefetch queue depth (PREFETCH_QUEUE_DEPTH in pager.h)
   if the queue fills before the worker drains it, predictions are dropped
   try increasing to 16

3. Check HOT_SLOTS in benchmark.c
   if too large (all blocks fit in RAM), prediction has nothing to do
   if too small (evicts too fast), predictor can't help
   6 slots for 18 blocks is the right ratio

4. The simulated miss cost (MISS_COST_US in benchmark.c) is 2ms
   on real Apple Silicon NVMe this is ~100ms per 512MB block
   the speedup will be much larger on real hardware

Real-world results on Qwen3-32B-MLX-4bit (17.5 GB weights) running
on Apple M1 with 16GB unified memory:
  peak RSS:  2.64 GB
  peak Metal: 1.13 GB  (vs 18.43 GB without paging — OOM)
  tokens/s:  0.133
  wall time: 401s for 50 tokens

## If you want to improve the predictor

The current predictor (predictor.c) is order-1 Markov:
P(next block | current block)

Better approaches to try:

1. Order-2 Markov is implemented (PRED_LOOKAHEAD) but needs
   more training data to beat order-1. Try training on 2048+ tokens.

2. Expert clustering: in MoE models, certain experts always
   fire together. Build a co-occurrence matrix over expert_id
   in the trace and prefetch the whole cluster when any member fires.

3. Layer-aware prediction: attention blocks follow a strict
   sequential pattern (layer 0 → 1 → 2 → ...). This is
   100% predictable. The predictor should handle this as a
   special case rather than learning it from traces.

4. Confidence threshold: currently we prefetch any block with
   confidence > 0.05. Try raising to 0.3 — fewer prefetches
   but higher hit rate. Measure the tradeoff with ./benchmark.

## Python integration

The C library is compiled as libspike.dylib and wrapped with ctypes:

```
import spike_loader

pager = spike_loader.PagedLayer("~/Models/Qwen3-32B-MLX-4bit", n_slots=6)
weights = pager.get(block_id)    # prefetch happens automatically
# ... GPU uses weights ...
pager.release(block_id)
```

Python test commands:

```
make libspike.dylib
python3 layer_test.py ~/Models/Qwen3-32B-MLX-4bit   # safetensor parsing
python3 run_inference.py ~/Models/Qwen3-32B-MLX-4bit  # 50-token generation
```

Requirements: mlx, mlx_lm, numpy

## Code style

- C11, no C++
- No external dependencies beyond libc, pthreads, POSIX mmap
- Every struct has a comment explaining what it is
- Every function has a comment explaining when to call it
- Hot paths (pager_get) are marked and kept minimal
- Locks are held for slot table mutations only, never during I/O
- Total codebase: ~3,000 lines — new contributors report being able to
  read any file in 20 minutes and understand it completely

## The thing to remember

This is not an inference engine.
It is the memory layer an inference engine runs on.

The goal is a single function call —
  void* pager_get(Pager* p, uint32_t block_id)
— that is a drop-in replacement for direct memory reads
in any existing inference engine, and makes it faster
on memory-constrained hardware with zero changes to the
inference logic itself.

Everything else is in service of that.

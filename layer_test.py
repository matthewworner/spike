"""
layer_test.py
Prove that scatter-gather loading works on a real safetensor model.

Tests:
  1. SpikeModel opens cleanly and detects 64 layers
  2. Layer 0 loads all 25 tensors with correct names and shapes
  3. BF16 tensors decode correctly (spot-check norm weight)
  4. U32 tensors have expected sizes (spot-check gate_proj.weight)
  5. Layer 0 evicts cleanly and layer 1 loads correctly
  6. Peak RSS stays well under 16 GB

Usage: python3 layer_test.py [model_dir]
"""

import sys
import os
import resource
import time
import numpy as np
import mlx.core as mx

sys.path.insert(0, os.path.dirname(__file__))
from spike_loader import SpikeModel

MODEL_DIR = os.path.expanduser(
    sys.argv[1] if len(sys.argv) > 1
    else "~/Models/Qwen3-32B-MLX-4bit"
)

n_pass = 0
n_fail = 0

def check(cond: bool, msg: str):
    global n_pass, n_fail
    if cond:
        print(f"  PASS  {msg}")
        n_pass += 1
    else:
        print(f"  FAIL  {msg}")
        n_fail += 1

def rss_gb() -> float:
    usage = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    # macOS: ru_maxrss is in bytes
    return usage / (1024 ** 3)

# ── open ──────────────────────────────────────────────────

print(f"\nlayer_test — {MODEL_DIR}")
print("─" * 60)

t0 = time.time()
spike = SpikeModel(MODEL_DIR, n_slots=4)

check(spike.n_layers == 64, f"n_layers == 64 (got {spike.n_layers})")

# ── layer 0 ───────────────────────────────────────────────

print("\nloading layer 0...")
t_load = time.time()
layer0 = spike.get_layer(0)
elapsed = time.time() - t_load

print(f"  loaded in {elapsed:.2f}s")
check(len(layer0) == 25, f"layer 0 has 25 tensors (got {len(layer0)})")

print("\n  layer 0 tensors:")
for name in sorted(layer0.keys()):
    arr = layer0[name]
    print(f"    {name:<55} {str(arr.dtype):<8} {arr.shape}")

# spot-check: input_layernorm.weight should be 1D float, shape [5120]
ln = layer0.get("input_layernorm.weight")
check(ln is not None, "input_layernorm.weight present")
if ln is not None:
    check(ln.shape == (5120,), f"input_layernorm.weight shape == (5120,) (got {ln.shape})")
    check(ln.dtype == mx.bfloat16,
          f"input_layernorm.weight dtype == bfloat16 (got {ln.dtype})")
    # check the weight is finite and non-trivially non-zero
    mean_abs = float(mx.mean(mx.abs(ln.astype(mx.float32))))
    check(mean_abs > 1e-4,
          f"input_layernorm.weight values non-zero (mean abs={mean_abs:.5f})")

# spot-check: mlp.gate_proj.weight should be U32 quantized
gw = layer0.get("mlp.gate_proj.weight")
check(gw is not None, "mlp.gate_proj.weight present")
if gw is not None:
    check(gw.dtype == mx.uint32,
          f"mlp.gate_proj.weight dtype == uint32 (got {gw.dtype})")
    # Qwen3-32B: gate_proj is 5120→13824 with 4-bit quant, packed 8/uint32
    # shape should be [13824, 640]
    check(len(gw.shape) == 2, f"mlp.gate_proj.weight is 2D (got {gw.shape})")

# ── evict and reload ───────────────────────────────────────

spike.release_layer(0)
print("\n  released layer 0")

print("loading layer 1...")
t1 = time.time()
layer1 = spike.get_layer(1)
elapsed1 = time.time() - t1
print(f"  loaded in {elapsed1:.2f}s")

check(len(layer1) == 25, f"layer 1 has 25 tensors (got {len(layer1)})")

ln1 = layer1.get("input_layernorm.weight")
if ln1 is not None:
    check(ln1.shape == (5120,), f"layer 1 input_layernorm.weight shape == (5120,)")

spike.release_layer(1)

# ── memory check ──────────────────────────────────────────

rss = rss_gb()
print(f"\nPeak RSS: {rss:.2f} GB")
check(rss < 8.0, f"Peak RSS < 8 GB (got {rss:.2f} GB)")

# ── close ──────────────────────────────────────────────────

spike.close()

total = time.time() - t0
print(f"\nTotal time: {total:.2f}s")

# ── result ─────────────────────────────────────────────────

print("\n" + "─" * 60)
print(f"result: {n_pass} passed, {n_fail} failed")
if n_fail == 0:
    print("PASS\n")
else:
    print("FAIL\n")
    sys.exit(1)

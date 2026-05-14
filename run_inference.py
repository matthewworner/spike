"""
run_inference.py
Full paged forward pass with Qwen3-32B-MLX-4bit via Spike.
Generates 50 tokens on "explain how transformers work".
Reports tokens/s, wall time, peak RSS, peak Metal.

Usage: python3 run_inference.py [model_dir]
"""

import sys
import os
import time
import resource
from pathlib import Path

import mlx.core as mx

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from spike_loader import SpikeModel, PagedLayer

MODEL_DIR  = Path(os.path.expanduser(
    sys.argv[1] if len(sys.argv) > 1 else "~/Models/Qwen3-32B-MLX-4bit"
))
N_SLOTS    = 6
PROMPT     = "explain how transformers work"
MAX_TOKENS = 50

# ── imports after path setup ──────────────────────────────────
from mlx_lm.utils import load_model, load_tokenizer
from mlx_lm import generate

# ── model skeleton (lazy=True → no Metal allocation at load) ──
print(f"model: {MODEL_DIR}")
print("loading model skeleton (lazy)...")
result = load_model(MODEL_DIR, lazy=True)
model = result[0] if isinstance(result, tuple) else result
tokenizer = load_tokenizer(MODEL_DIR)

# ── open spike pager ──────────────────────────────────────────
spike = SpikeModel(str(MODEL_DIR), n_slots=N_SLOTS)

# ── misc weights: embed_tokens, norm, lm_head (~834 MB) ───────
# These are accessed on every token — load once, keep in Metal.
print("loading misc weights (~834 MB)...")
misc = spike.get_misc()
model.load_weights(list(misc.items()), strict=False)
mx.eval(list(misc.values()))
spike.release_misc()
del misc

# ── wrap transformer layers with PagedLayer ───────────────────
print(f"wrapping {spike.n_layers} layers (n_slots={N_SLOTS})...")
for i in range(spike.n_layers):
    model.model.layers[i] = PagedLayer(spike, i, model.model.layers[i])

# ── generate ──────────────────────────────────────────────────
print(f"\nprompt: {PROMPT!r}")
print(f"max_tokens: {MAX_TOKENS}")
print("─" * 60)

t0 = time.time()
response = generate(model, tokenizer, prompt=PROMPT,
                    max_tokens=MAX_TOKENS, verbose=True)
elapsed = time.time() - t0

# ── stats ──────────────────────────────────────────────────────
rss_gb     = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / (1024 ** 3)
peak_metal = mx.get_peak_memory() / (1024 ** 3)
tps        = MAX_TOKENS / elapsed

print("─" * 60)
print(f"tokens/s :   {tps:.1f}")
print(f"wall time:   {elapsed:.1f}s")
print(f"peak RSS :   {rss_gb:.2f} GB")
print(f"peak Metal:  {peak_metal:.2f} GB")

spike.close()

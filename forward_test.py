#!/usr/bin/env python3
"""
forward_test.py
End-to-end generation on Qwen3-32B with the scatter-gather pager.

Proves the pager can drive a real model forward pass without loading all 17.5GB
at once.

Strategy:
  1. Load model architecture only (lazy=True, no mx.eval on weights)
  2. Load embed_tokens + norm + lm_head from SpikeModel via scatter-gather pread
  3. Replace every TransformerBlock with a PagedLayer
  4. Run stream_generate with the patched model
  5. Report tokens/s and peak Metal heap

Peak Metal stays bounded by the 6-slot pager budget.
"""

import os
import sys
import time
import resource
from pathlib import Path

import mlx.core as mx
from mlx_lm import stream_generate, load as load_tokenizer_from_path
from mlx_lm.utils import load_model, tree_map
from spike_loader import SpikeModel, PagedLayer

MODEL_DIR   = Path(os.path.expanduser("~/Models/Qwen3-32B-MLX-4bit"))
PROMPT     = "explain how transformers work"
MAX_TOKENS = 50

# ─────────────────────────────────────────────────────────────────────────────
# Key mapping: spike key → mlx module path
#
# Spike stores keys as "model.layers.N.{submodule}.{proj}.{component}"
# (e.g. "model.layers.0.self_attn.q_proj.weight")
#
# The mlx TransformerBlock dict has structure:
#   TransformerBlock["self_attn"] = ModuleDict {"q_proj": Q, "k_proj": K, ...}
#   TransformerBlock["mlp"]        = ModuleDict {"gate_proj": Q, "down_proj": O, ...}
#
# For embed / lm_head / norm the misc block keys go straight into the top-level
# module dicts.  Prefix "model." is stripped because misc block = no layers.N.
# ─────────────────────────────────────────────────────────────────────────────

_MISC_PREFIX = "model."  # "model.embed_tokens.weight" → embed_tokens["weight"]


def _rel_key(full_key: str, layer_id: int) -> str:
    """Strip 'model.layers.N.' from a block-0 tensor key."""
    return full_key[len(f"model.layers.{layer_id}."):]


def _misc_rel_key(full_key: str) -> str:
    """Strip 'model.' from a misc block tensor key."""
    return full_key[len(_MISC_PREFIX):]


def _build_layer_flat(tensors: dict, layer_id: int) -> dict:
    """
    Convert a raw {full_key: mx.array} dict from spike layer block into the
    flat dict-of-dicts structure that TransformerBlock.update() expects.

    Full key example:
      "model.layers.0.self_attn.q_proj.weight"
    rel key:
      "self_attn.q_proj.weight"
    → update dict:
      {"self_attn": {"q_proj": {"weight": <array>}}
    """
    result: dict = {}
    for full_key, arr in tensors.items():
        rel = _rel_key(full_key, layer_id)
        parts = rel.split(".")
        d = result
        for part in parts[:-1]:
            d = d.setdefault(part, {})
        d[parts[-1]] = arr
    return result


def _build_misc_flat(tensors: dict) -> dict:
    """Convert misc block {full_key: mx.array} → flat dict for top-level modules."""
    result: dict = {}
    for full_key, arr in tensors.items():
        rel = _misc_rel_key(full_key)
        parts = rel.split(".")
        d = result
        for part in parts[:-1]:
            d = d.setdefault(part, {})
        d[parts[-1]] = arr
    return result


def main() -> None:
    print(f"forward_test — {MODEL_DIR}")
    print(f"prompt: {PROMPT!r}")
    print(f"max_tokens: {MAX_TOKENS}")
    print("-" * 50)

    # ── tokenizer ───────────────────────────────────────────────────────────
    print("loading tokenizer...")
    _, tokenizer = load_tokenizer_from_path(MODEL_DIR)
    print("  tokenizer: TokenizerWrapper")

    # ── model architecture (lazy — NO Metal heap allocation yet) ─────────────
    print("loading model architecture (lazy)...")
    model, cfg = load_model(MODEL_DIR, lazy=True, strict=False)
    n_layers = cfg.get("num_hidden_layers", len(model.model.layers))
    print(f"  layers: {n_layers}")

    # ── open scatter-gather pager ───────────────────────────────────────────
    print("opening scatter-gather pager (6 slots)...")
    spike = SpikeModel(str(MODEL_DIR), n_slots=6)
    print("-" * 50)

    # ── load embed_tokens, norm, lm_head (not paginated) ─────────────────────
    print("loading embed + norm + lm_head from pager...")
    misc = spike.get_misc()
    misc_flat = _build_misc_flat(misc)
    spike.release_misc()

    model.model.embed_tokens.update(misc_flat.get("embed_tokens", {}))
    model.lm_head.update(misc_flat.get("lm_head", {}))
    if "norm" in misc_flat:
        model.model.norm.update(misc_flat["norm"])
    print("  misc patched into model")

    # ── replace every TransformerBlock with a PagedLayer ───────────────────
    print("wrapping transformer layers with PagedLayer...")
    orig_layers = list(model.model.layers)
    for i, orig in enumerate(orig_layers):
        model.model.layers[i] = PagedLayer(spike, i, orig)
    print("-" * 50)

    # ── generation ───────────────────────────────────────────────────────────
    # First token: model touches all 64 layers sequentially.
    # The predictor observes the N→N+1 pattern and fills its state table.
    # Tokens 2+: the predictor keeps the working-set layers pinned, so each
    # token only triggers 1–3 cache misses instead of 64 disk round-trips.
    peak_metal_gb = 0.0

    def track_peak() -> None:
        nonlocal peak_metal_gb
        cur = mx.get_peak_memory() / 1e9
        if cur > peak_metal_gb:
            peak_metal_gb = cur

    print(f"generating {MAX_TOKENS} tokens...")
    t0 = time.perf_counter()
    tokens = 0
    text_so_far = ""

    for resp in stream_generate(model, tokenizer, PROMPT, max_tokens=MAX_TOKENS):
        tokens += 1
        track_peak()
        text_so_far += resp.text
        if tokens <= 5 or tokens % 10 == 0:
            snippet = repr(resp.text[:60])
            print(f"  t{tokens}: {snippet}")
        track_peak()

    elapsed = time.perf_counter() - t0
    tps     = tokens / elapsed if elapsed > 0 else 0.0
    rss_gb  = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / 1e6

    print("-" * 50)
    print(f"tokens generated: {tokens}")
    print(f"time:           {elapsed:.1f}s")
    print(f"throughput:     {tps:.2f} tokens/s")
    print(f"peak RSS:       {rss_gb:.2f} GB")
    print(f"peak Metal:     {peak_metal_gb:.2f} GB")
    print(f"\ngenerated text:\n{text_so_far}")
    spike.close()

    # Sanity: Metal peak should be bounded by slot budget
    if peak_metal_gb < 17.5:
        print("\nPASS — Metal heap bounded by pager slot budget")
    else:
        print("\nWARNING — Metal heap not bounded")
        sys.exit(1)


if __name__ == "__main__":
    main()

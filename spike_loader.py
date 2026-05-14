"""
spike_loader.py
ctypes wrapper around libspike.dylib + PagedLayer shim.

SpikeModel      — loads and serves one layer at a time from disk
PagedLayer      — drop-in replacement for mlx model.model.layers[i]
                  loads weights on __call__, evicts immediately after
"""

import ctypes
import os
import numpy as np
import mlx.core as mx

# ── load the dylib ─────────────────────────────────────────

_LIB_PATH = os.path.join(os.path.dirname(__file__), "libspike.dylib")
_lib = ctypes.CDLL(_LIB_PATH)

# void* spike_open(const char* model_dir, int n_slots)
_lib.spike_open.argtypes  = [ctypes.c_char_p, ctypes.c_int]
_lib.spike_open.restype   = ctypes.c_void_p

# void spike_close(void* handle)
_lib.spike_close.argtypes = [ctypes.c_void_p]
_lib.spike_close.restype  = None

# void* spike_get(void* handle, uint32_t block_id)
_lib.spike_get.argtypes  = [ctypes.c_void_p, ctypes.c_uint32]
_lib.spike_get.restype   = ctypes.c_void_p

# void spike_release(void* handle, uint32_t block_id)
_lib.spike_release.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
_lib.spike_release.restype  = None

# int spike_n_layers(void* handle)
_lib.spike_n_layers.argtypes = [ctypes.c_void_p]
_lib.spike_n_layers.restype  = ctypes.c_int

# uint64_t spike_block_bytes(void* handle, uint32_t block_id)
_lib.spike_block_bytes.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
_lib.spike_block_bytes.restype  = ctypes.c_uint64

# int spike_block_n_tensors(void* handle, uint32_t block_id)
_lib.spike_block_n_tensors.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
_lib.spike_block_n_tensors.restype  = ctypes.c_int

# int spike_tensor_info(void* handle, uint32_t block_id, int tensor_idx,
#     uint64_t* offset_in_block_out, uint64_t* byte_length_out,
#     char* name_out, int name_maxlen,
#     char* dtype_out, int dtype_maxlen,
#     int64_t* shape_out, int* ndim_out)
_lib.spike_tensor_info.argtypes = [
    ctypes.c_void_p, ctypes.c_uint32, ctypes.c_int,
    ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(ctypes.c_uint64),
    ctypes.c_char_p, ctypes.c_int,
    ctypes.c_char_p, ctypes.c_int,
    ctypes.POINTER(ctypes.c_int64), ctypes.POINTER(ctypes.c_int),
]
_lib.spike_tensor_info.restype = ctypes.c_int

# ── dtype mapping ──────────────────────────────────────────

_NUMPY_DTYPE = {
    "BF16": np.uint16,   # reinterpreted as bfloat16 via .view()
    "F16":  np.float16,
    "F32":  np.float32,
    "F64":  np.float64,
    "U8":   np.uint8,
    "U16":  np.uint16,
    "U32":  np.uint32,
    "U64":  np.uint64,
    "I8":   np.int8,
    "I16":  np.int16,
    "I32":  np.int32,
    "I64":  np.int64,
}

def _to_mx_array(raw_bytes: bytes, dtype_str: str, shape: tuple) -> mx.array:
    np_dtype = _NUMPY_DTYPE.get(dtype_str.upper())
    if np_dtype is None:
        raise ValueError(f"Unknown dtype: {dtype_str!r}")

    np_arr = np.frombuffer(raw_bytes, dtype=np_dtype).reshape(shape)
    mx_arr = mx.array(np_arr)

    if dtype_str.upper() == "BF16":
        # reinterpret uint16 bits as bfloat16 — no value conversion
        mx_arr = mx_arr.view(mx.bfloat16)

    return mx_arr

# ── dict helpers ───────────────────────────────────────────

def _unflatten(flat: dict) -> dict:
    """Convert {'a.b.c': v} → {'a': {'b': {'c': v}}}"""
    result: dict = {}
    for key, val in flat.items():
        parts = key.split(".")
        d = result
        for part in parts[:-1]:
            d = d.setdefault(part, {})
        d[parts[-1]] = val
    return result

def _zero_layer(layer) -> None:
    """Replace every mx.array in the layer's parameter tree with a (1,)-shaped
    sentinel so Metal can reclaim the ~262 MB of weight buffers after eval."""
    def _zero_tree(tree):
        if isinstance(tree, mx.array):
            return mx.zeros((1,), dtype=tree.dtype)
        if isinstance(tree, dict):
            return {k: _zero_tree(v) for k, v in tree.items()}
        if isinstance(tree, list):
            return [_zero_tree(v) for v in tree]
        return tree
    layer.update(_zero_tree(layer.parameters()))

# ── SpikeModel ─────────────────────────────────────────────

class SpikeModel:
    """
    Manages scatter-gather paged access to a safetensor model.

    Usage:
        spike = SpikeModel("~/Models/Qwen3-32B-MLX-4bit", n_slots=4)
        tensors = spike.get_layer(0)   # dict[name → mx.array]
        spike.release_layer(0)
        spike.close()
    """

    def __init__(self, model_dir: str, n_slots: int = 4):
        model_dir = os.path.expanduser(model_dir)
        self._handle = _lib.spike_open(model_dir.encode(), n_slots)
        if not self._handle:
            raise RuntimeError(f"spike_open failed for {model_dir!r}")
        self.model_dir = model_dir
        self.n_layers  = _lib.spike_n_layers(self._handle)

    def get_layer(self, layer_id: int) -> dict:
        """
        Load block layer_id and return {tensor_name: mx.array}.
        Tensor names have the 'model.layers.N.' prefix stripped.
        Blocks (with pin) until the data is in RAM.
        Call release_layer() when the forward pass is done.
        """
        block_id = ctypes.c_uint32(layer_id)

        buf_ptr = _lib.spike_get(self._handle, block_id)
        if not buf_ptr:
            raise RuntimeError(f"spike_get failed for layer {layer_id}")

        n_tensors = _lib.spike_block_n_tensors(self._handle, block_id)

        # query tensor metadata
        offset_out = ctypes.c_uint64(0)
        length_out = ctypes.c_uint64(0)
        name_buf   = ctypes.create_string_buffer(128)
        dtype_buf  = ctypes.create_string_buffer(8)
        shape_buf  = (ctypes.c_int64 * 8)()
        ndim_out   = ctypes.c_int(0)

        prefix = f"model.layers.{layer_id}."
        flat: dict = {}

        for i in range(n_tensors):
            ret = _lib.spike_tensor_info(
                self._handle, block_id, i,
                ctypes.byref(offset_out), ctypes.byref(length_out),
                name_buf, 128,
                dtype_buf, 8,
                shape_buf, ctypes.byref(ndim_out),
            )
            if ret != 0:
                raise RuntimeError(f"spike_tensor_info failed at {i}")

            name     = name_buf.value.decode()
            dtype_s  = dtype_buf.value.decode()
            ndim     = ndim_out.value
            shape    = tuple(int(shape_buf[d]) for d in range(ndim))
            offset   = int(offset_out.value)
            n_bytes  = int(length_out.value)

            # view the C buffer as raw bytes — zero-copy until mx.array()
            raw = (ctypes.c_uint8 * n_bytes).from_address(buf_ptr + offset)
            tensor = _to_mx_array(bytes(raw), dtype_s, shape)

            # strip 'model.layers.N.' prefix for the update dict
            rel_name = name[len(prefix):] if name.startswith(prefix) else name
            flat[rel_name] = tensor

        return flat

    def get_misc(self) -> dict:
        """Load the misc block (embed_tokens, norm, lm_head)."""
        block_id = ctypes.c_uint32(self.n_layers)
        buf_ptr  = _lib.spike_get(self._handle, block_id)
        if not buf_ptr:
            raise RuntimeError("spike_get failed for misc block")

        n_tensors = _lib.spike_block_n_tensors(self._handle, block_id)
        offset_out = ctypes.c_uint64(0)
        length_out = ctypes.c_uint64(0)
        name_buf   = ctypes.create_string_buffer(128)
        dtype_buf  = ctypes.create_string_buffer(8)
        shape_buf  = (ctypes.c_int64 * 8)()
        ndim_out   = ctypes.c_int(0)

        flat: dict = {}
        for i in range(n_tensors):
            _lib.spike_tensor_info(
                self._handle, block_id, i,
                ctypes.byref(offset_out), ctypes.byref(length_out),
                name_buf, 128, dtype_buf, 8,
                shape_buf, ctypes.byref(ndim_out),
            )
            name    = name_buf.value.decode()
            dtype_s = dtype_buf.value.decode()
            ndim    = ndim_out.value
            shape   = tuple(int(shape_buf[d]) for d in range(ndim))
            offset  = int(offset_out.value)
            n_bytes = int(length_out.value)

            raw    = (ctypes.c_uint8 * n_bytes).from_address(buf_ptr + offset)
            tensor = _to_mx_array(bytes(raw), dtype_s, shape)
            flat[name] = tensor

        return flat

    def release_layer(self, layer_id: int):
        """Release the pin on a layer block — allows eviction."""
        _lib.spike_release(self._handle, ctypes.c_uint32(layer_id))

    def release_misc(self):
        _lib.spike_release(self._handle, ctypes.c_uint32(self.n_layers))

    def close(self):
        if self._handle:
            _lib.spike_close(self._handle)
            self._handle = None

    def __del__(self):
        self.close()

# ── PagedLayer ─────────────────────────────────────────────

class PagedLayer:
    """
    Drop-in replacement for mlx_lm model.model.layers[i].

    On every __call__:
      1. Load layer weights from disk via SpikeModel.get_layer()
      2. Update the original MLX layer with the loaded tensors
      3. Release the C buffer (Metal copy is independent)
      4. Run the original layer forward pass
      5. Materialize result with mx.eval()
      6. Zero out all weight arrays in the layer (_zero_layer)
      7. mx.clear_cache() — Metal reclaims the ~262 MB

    This keeps at most one layer's weights in Metal at a time.
    """

    def __init__(self, spike_model: SpikeModel,
                 layer_id: int, original_layer):
        self._spike   = spike_model
        self._id      = layer_id
        self._layer   = original_layer

    def __call__(self, *args, **kwargs):
        tensors_flat = self._spike.get_layer(self._id)
        self._layer.update(_unflatten(tensors_flat))
        del tensors_flat                    # layer is now sole ref holder
        self._spike.release_layer(self._id) # free C buffer; Metal copy is independent

        result = self._layer(*args, **kwargs)
        mx.eval(result)                     # materialise output before clearing weights

        _zero_layer(self._layer)            # drop all 262 MB Metal weight refs
        mx.clear_cache()                    # Metal reclaims them

        return result

    def make_cache(self):
        return self._layer.make_cache()

    def __getattr__(self, name):
        return getattr(self._layer, name)

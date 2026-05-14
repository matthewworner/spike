// spike_api.h
// Flat C API for libspike.dylib — safe to call from ctypes.
//
// Wraps SpikeIndex + Pager behind an opaque handle.
// The caller never sees internal structs.
//
// Lifecycle:
//   handle = spike_open(model_dir, n_slots)
//   ptr    = spike_get(handle, block_id)   // blocks until ready
//   // use ptr — it is valid until spike_release()
//   spike_release(handle, block_id)
//   spike_close(handle)

#ifndef SPIKE_API_H
#define SPIKE_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Open a paged model. Scans model_dir for *.safetensors shards,
// builds the index, and starts the prefetch worker thread.
// n_slots: number of layer-sized RAM slots (4 = ~1 GB for Qwen3-32B).
// Returns an opaque handle, or NULL on error.
void* spike_open(const char* model_dir, int n_slots);

// Close and free everything. Blocks until the worker thread exits.
void  spike_close(void* handle);

// Get a pointer to the assembled block buffer for block_id.
// Blocks until the block is loaded. Pins the block so it won't
// be evicted until spike_release(). Returns NULL on error.
void* spike_get(void* handle, uint32_t block_id);

// Release a pinned block back to the LRU pool.
void  spike_release(void* handle, uint32_t block_id);

// Number of transformer layer blocks (not counting the misc block).
int  spike_n_layers(void* handle);

// Total bytes in the assembled buffer for block_id.
uint64_t spike_block_bytes(void* handle, uint32_t block_id);

// Number of tensors in block_id.
int  spike_block_n_tensors(void* handle, uint32_t block_id);

// Get metadata for one tensor within a block.
// tensor_idx: 0 .. spike_block_n_tensors(handle, block_id) - 1
// All pointer args are output parameters; pass NULL to skip.
// Returns 0 on success, -1 if block_id or tensor_idx is out of range.
int  spike_tensor_info(void*     handle,
                       uint32_t  block_id,
                       int       tensor_idx,
                       uint64_t* offset_in_block_out,
                       uint64_t* byte_length_out,
                       char*     name_out,    int name_maxlen,
                       char*     dtype_out,   int dtype_maxlen,
                       int64_t*  shape_out,   int* ndim_out);

#ifdef __cplusplus
}
#endif

#endif // SPIKE_API_H

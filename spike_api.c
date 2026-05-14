// spike_api.c
// Flat C API implementation — wraps SpikeIndex + Pager.

#define _POSIX_C_SOURCE 200809L

#include "spike_api.h"
#include "spike_index.h"
#include "pager.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    SpikeIndex* index;
    Pager       pager;
} SpikeHandle;

void* spike_open(const char* model_dir, int n_slots) {
    SpikeHandle* h = calloc(1, sizeof(SpikeHandle));
    if (!h) return NULL;

    h->index = spike_index_load(model_dir);
    if (!h->index) {
        free(h);
        return NULL;
    }

    if (pager_init_safetensors(&h->pager, h->index, n_slots) != 0) {
        spike_index_free(h->index);
        free(h);
        return NULL;
    }

    return h;
}

void spike_close(void* handle) {
    if (!handle) return;
    SpikeHandle* h = (SpikeHandle*)handle;
    pager_close(&h->pager);
    spike_index_free(h->index);
    free(h);
}

void* spike_get(void* handle, uint32_t block_id) {
    if (!handle) return NULL;
    return pager_get(&((SpikeHandle*)handle)->pager, block_id);
}

void spike_release(void* handle, uint32_t block_id) {
    if (!handle) return;
    pager_release(&((SpikeHandle*)handle)->pager, block_id);
}

int spike_n_layers(void* handle) {
    if (!handle) return 0;
    return ((SpikeHandle*)handle)->index->n_layers;
}

uint64_t spike_block_bytes(void* handle, uint32_t block_id) {
    if (!handle) return 0;
    SIBlock* b = spike_index_block(((SpikeHandle*)handle)->index, block_id);
    return b ? b->total_bytes : 0;
}

int spike_block_n_tensors(void* handle, uint32_t block_id) {
    if (!handle) return 0;
    SIBlock* b = spike_index_block(((SpikeHandle*)handle)->index, block_id);
    return b ? b->n_tensors : 0;
}

int spike_tensor_info(void*     handle,
                      uint32_t  block_id,
                      int       tensor_idx,
                      uint64_t* offset_in_block_out,
                      uint64_t* byte_length_out,
                      char*     name_out,    int name_maxlen,
                      char*     dtype_out,   int dtype_maxlen,
                      int64_t*  shape_out,   int* ndim_out) {
    if (!handle) return -1;
    SpikeIndex* idx = ((SpikeHandle*)handle)->index;
    SIBlock*    b   = spike_index_block(idx, block_id);
    if (!b || tensor_idx < 0 || tensor_idx >= b->n_tensors) return -1;

    SITensor* t = &b->tensors[tensor_idx];

    if (offset_in_block_out) *offset_in_block_out = t->offset_in_block;
    if (byte_length_out)     *byte_length_out      = t->byte_length;

    if (name_out && name_maxlen > 0)
        strncpy(name_out, t->name, (size_t)name_maxlen - 1);

    if (dtype_out && dtype_maxlen > 0)
        strncpy(dtype_out, t->dtype, (size_t)dtype_maxlen - 1);

    if (shape_out && ndim_out) {
        *ndim_out = t->ndim;
        for (int i = 0; i < t->ndim; i++)
            shape_out[i] = t->shape[i];
    }

    return 0;
}

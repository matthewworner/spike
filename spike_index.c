// spike_index.c
// Safetensor shard index — implementation.
//
// Reads every *.safetensors shard in the model directory,
// parses each shard's header JSON (format: 8-byte LE uint64
// header_size, then header_size bytes of JSON, then raw data),
// and builds a block_map grouping tensors by transformer layer.
//
// No external JSON library — the format is constrained enough
// for a straightforward recursive descent parser.

#define _POSIX_C_SOURCE 200809L

#include "spike_index.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <ctype.h>

// ── minimal JSON helpers ───────────────────────────────────

static const char* json_skip_ws(const char* p) {
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

// Parse a JSON string at *p (must point to opening '"').
// Advances *p past the closing '"'. Returns 0 on success.
static int json_parse_string(const char** p, char* out, size_t maxlen) {
    if (**p != '"') return -1;
    (*p)++;
    size_t i = 0;
    while (**p && **p != '"') {
        char c = **p;
        if (c == '\\') {
            (*p)++;
            switch (**p) {
                case '"': c = '"';  break;
                case '\\':c = '\\'; break;
                case '/': c = '/';  break;
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                default:  c = **p; break;
            }
        }
        if (i + 1 < maxlen) out[i++] = c;
        (*p)++;
    }
    out[i] = '\0';
    if (**p == '"') (*p)++;
    return 0;
}

// Parse a JSON integer array at *p (must point to '[').
// Advances *p past the closing ']'. Returns element count.
static int json_parse_int64_array(const char** p, int64_t* out, int maxn) {
    if (**p != '[') return 0;
    (*p)++;
    int n = 0;
    while (**p && **p != ']' && n < maxn) {
        *p = json_skip_ws(*p);
        if (**p == ']') break;
        char* end;
        out[n++] = (int64_t)strtoll(*p, &end, 10);
        *p = end;
        *p = json_skip_ws(*p);
        if (**p == ',') (*p)++;
    }
    if (**p == ']') (*p)++;
    return n;
}

// Advance *p past a JSON value (string, number, object, array,
// boolean, or null). Handles arbitrary nesting depth.
static const char* json_skip_value(const char* p) {
    p = json_skip_ws(p);
    if (*p == '"') {
        p++;
        while (*p && *p != '"') {
            if (*p == '\\') p++;
            if (*p) p++;
        }
        if (*p == '"') p++;
        return p;
    }
    if (*p == '{' || *p == '[') {
        char open = *p, close = (*p == '{') ? '}' : ']';
        int depth = 1;
        p++;
        while (*p && depth > 0) {
            if (*p == '"') {
                p++;
                while (*p && *p != '"') {
                    if (*p == '\\') p++;
                    if (*p) p++;
                }
                if (*p) p++;
            } else if (*p == open) {
                depth++; p++;
            } else if (*p == close) {
                depth--; p++;
            } else {
                p++;
            }
        }
        return p;
    }
    // number, boolean, null
    while (*p && *p != ',' && *p != '}' && *p != ']' && !isspace((unsigned char)*p))
        p++;
    return p;
}

// ── JSON object iterator ───────────────────────────────────
//
// Iterates over key-value pairs in a JSON object.
// After json_obj_iter_next returns 1, it->p points to the
// start of the value. The caller MUST advance it->p past
// the value (via json_obj_iter_skip_value or by parsing it).

typedef struct {
    const char* p;
} JsonObjIter;

static void json_obj_iter_init(JsonObjIter* it, const char* p) {
    p = json_skip_ws(p);
    if (*p == '{') p++;
    it->p = p;
}

// Returns 1 and fills key if a pair is found; 0 at end.
static int json_obj_iter_next(JsonObjIter* it,
                               char* key, size_t key_maxlen) {
    it->p = json_skip_ws(it->p);
    if (*it->p == '}' || *it->p == '\0') return 0;
    if (*it->p == ',') { it->p++; it->p = json_skip_ws(it->p); }
    if (*it->p == '}' || *it->p == '\0') return 0;
    if (json_parse_string(&it->p, key, key_maxlen) != 0) return 0;
    it->p = json_skip_ws(it->p);
    if (*it->p != ':') return 0;
    it->p++;
    it->p = json_skip_ws(it->p);
    return 1;
}

static void json_obj_iter_skip_value(JsonObjIter* it) {
    it->p = json_skip_value(it->p);
}

// ── layer number extraction ────────────────────────────────
//
// Supports the common naming patterns across architectures:
//   model.layers.N.*       Llama, Qwen, Mistral, Phi, ...
//   transformer.h.N.*      Falcon
//   gpt_neox.layers.N.*    GPT-NeoX
//   transformer.blocks.N.* MPT
//   encoder.layers.N.*     BERT-style encoders
//   decoder.layers.N.*     T5-style decoders
//
// Returns the layer number, or -1 if not a layer tensor.

static const char* LAYER_PREFIXES[] = {
    "model.layers.",
    "transformer.h.",
    "gpt_neox.layers.",
    "transformer.blocks.",
    "encoder.layers.",
    "decoder.layers.",
    NULL
};

static int extract_layer_number(const char* name) {
    for (int i = 0; LAYER_PREFIXES[i]; i++) {
        const char* prefix = LAYER_PREFIXES[i];
        size_t      plen   = strlen(prefix);
        if (strncmp(name, prefix, plen) != 0) continue;
        const char* p = name + plen;
        if (!isdigit((unsigned char)*p)) continue;
        char* end;
        long layer = strtol(p, &end, 10);
        if (end > p && (*end == '.' || *end == '\0'))
            return (int)layer;
    }
    return -1;
}

// ── block table helpers ────────────────────────────────────

// block_id used as sentinel for non-layer tensors during parse
#define MISC_BLOCK_ID_SENTINEL UINT32_MAX

static SIBlock* find_or_create_block(SpikeIndex* idx,
                                      uint32_t block_id,
                                      const char* label) {
    for (int i = 0; i < idx->n_blocks; i++)
        if (idx->blocks[i].block_id == block_id)
            return &idx->blocks[i];
    if (idx->n_blocks >= SI_MAX_BLOCKS) {
        fprintf(stderr, "spike_index: SI_MAX_BLOCKS exceeded\n");
        return NULL;
    }
    SIBlock* b = &idx->blocks[idx->n_blocks++];
    memset(b, 0, sizeof(SIBlock));
    b->block_id = block_id;
    strncpy(b->label, label, sizeof(b->label) - 1);
    return b;
}

// ── parse one tensor entry ─────────────────────────────────
//
// p points to the '{' of the tensor's metadata object:
//   {"dtype":"BF16","shape":[H,W],"data_offsets":[start,end]}
// Fills t->dtype, t->shape, t->ndim, t->offset_in_file (temporarily
// the relative data offset), and t->byte_length.
// Caller adjusts offset_in_file to absolute after return.

static int parse_tensor_entry(const char* p, SITensor* t) {
    int64_t offsets[2] = {0, 0};
    JsonObjIter inner;
    json_obj_iter_init(&inner, p);
    char key[32];
    while (json_obj_iter_next(&inner, key, sizeof(key))) {
        if (strcmp(key, "dtype") == 0) {
            json_parse_string(&inner.p, t->dtype, SI_MAX_DTYPE);
        } else if (strcmp(key, "shape") == 0) {
            t->ndim = json_parse_int64_array(&inner.p, t->shape, SI_MAX_DIMS);
        } else if (strcmp(key, "data_offsets") == 0) {
            json_parse_int64_array(&inner.p, offsets, 2);
        } else {
            json_obj_iter_skip_value(&inner);
        }
    }
    t->offset_in_file = (uint64_t)offsets[0]; // relative; caller makes absolute
    t->byte_length    = (uint64_t)(offsets[1] - offsets[0]);
    return 0;
}

// ── parse one shard ────────────────────────────────────────

static int parse_shard(SpikeIndex* idx, int shard_i) {
    SIShard* shard = &idx->shards[shard_i];
    int fd = shard->fd;

    // read 8-byte little-endian header size
    uint8_t hdr_bytes[8];
    if (pread(fd, hdr_bytes, 8, 0) != 8) {
        fprintf(stderr, "spike_index: short read on %s\n", shard->path);
        return -1;
    }
    uint64_t header_size = (uint64_t)hdr_bytes[0]
                         | ((uint64_t)hdr_bytes[1] << 8)
                         | ((uint64_t)hdr_bytes[2] << 16)
                         | ((uint64_t)hdr_bytes[3] << 24)
                         | ((uint64_t)hdr_bytes[4] << 32)
                         | ((uint64_t)hdr_bytes[5] << 40)
                         | ((uint64_t)hdr_bytes[6] << 48)
                         | ((uint64_t)hdr_bytes[7] << 56);

    if (header_size == 0 || header_size > 512 * 1024 * 1024ULL) {
        fprintf(stderr, "spike_index: implausible header_size %llu in %s\n",
                (unsigned long long)header_size, shard->path);
        return -1;
    }

    char* json_buf = malloc(header_size + 1);
    if (!json_buf) { perror("malloc"); return -1; }

    ssize_t got = pread(fd, json_buf, header_size, 8);
    if (got < 0 || (uint64_t)got != header_size) {
        fprintf(stderr, "spike_index: short header read on %s\n", shard->path);
        free(json_buf);
        return -1;
    }
    json_buf[header_size] = '\0';

    uint64_t data_section_start = 8 + header_size;

    // iterate over top-level JSON object
    JsonObjIter outer;
    json_obj_iter_init(&outer, json_buf);
    char tensor_name[SI_MAX_NAME];

    while (json_obj_iter_next(&outer, tensor_name, sizeof(tensor_name))) {
        // skip metadata key
        if (strcmp(tensor_name, "__metadata__") == 0) {
            json_obj_iter_skip_value(&outer);
            continue;
        }

        // parse the tensor entry object
        SITensor t;
        memset(&t, 0, sizeof(t));
        strncpy(t.name, tensor_name, SI_MAX_NAME - 1);
        t.shard_idx = shard_i;

        const char* val_start = outer.p; // points to '{'
        parse_tensor_entry(val_start, &t);
        outer.p = json_skip_value(val_start); // advance past the object

        // make offset absolute
        t.offset_in_file += data_section_start;

        // assign to block by layer number
        int layer = extract_layer_number(tensor_name);

        SIBlock* block;
        if (layer >= 0) {
            char label[64];
            snprintf(label, sizeof(label), "layer.%d", layer);
            block = find_or_create_block(idx, (uint32_t)layer, label);
        } else {
            block = find_or_create_block(idx,
                        MISC_BLOCK_ID_SENTINEL, "misc");
        }

        if (!block) { free(json_buf); return -1; }

        if (block->n_tensors >= SI_MAX_TENSORS) {
            fprintf(stderr, "spike_index: SI_MAX_TENSORS exceeded "
                    "for block %u\n", block->block_id);
            free(json_buf);
            return -1;
        }
        block->tensors[block->n_tensors++] = t;
    }

    free(json_buf);
    return 0;
}

// ── finalize: sort, compute offsets ───────────────────────

static int tensor_cmp_by_offset(const void* a, const void* b) {
    const SITensor* ta = (const SITensor*)a;
    const SITensor* tb = (const SITensor*)b;
    if (ta->offset_in_file < tb->offset_in_file) return -1;
    if (ta->offset_in_file > tb->offset_in_file) return  1;
    return 0;
}

static int block_cmp_by_id(const void* a, const void* b) {
    const SIBlock* ba = (const SIBlock*)a;
    const SIBlock* bb = (const SIBlock*)b;
    if (ba->block_id < bb->block_id) return -1;
    if (ba->block_id > bb->block_id) return  1;
    return 0;
}

static void finalize_blocks(SpikeIndex* idx) {
    // compute n_layers from max layer block_id (excluding MISC sentinel)
    uint32_t max_layer_id = 0;
    for (int i = 0; i < idx->n_blocks; i++) {
        uint32_t bid = idx->blocks[i].block_id;
        if (bid != MISC_BLOCK_ID_SENTINEL && bid > max_layer_id)
            max_layer_id = bid;
    }
    idx->n_layers = (idx->n_blocks > 0) ? (int)max_layer_id + 1 : 0;

    // assign misc block its real block_id
    for (int i = 0; i < idx->n_blocks; i++) {
        if (idx->blocks[i].block_id == MISC_BLOCK_ID_SENTINEL)
            idx->blocks[i].block_id = (uint32_t)idx->n_layers;
    }

    // sort blocks by block_id
    qsort(idx->blocks, idx->n_blocks, sizeof(SIBlock), block_cmp_by_id);

    // for each block: sort tensors by file offset, compute block offsets
    for (int i = 0; i < idx->n_blocks; i++) {
        SIBlock* b = &idx->blocks[i];
        qsort(b->tensors, b->n_tensors, sizeof(SITensor),
              tensor_cmp_by_offset);

        uint64_t running = 0;
        for (int j = 0; j < b->n_tensors; j++) {
            b->tensors[j].offset_in_block = running;
            running += b->tensors[j].byte_length;
        }
        b->total_bytes = running;
    }
}

// ── scan shards ────────────────────────────────────────────

static int path_cmp(const void* a, const void* b) {
    return strcmp((const char*)a, (const char*)b);
}

static int scan_shards(SpikeIndex* idx, const char* model_dir) {
    // collect paths of all *.safetensors files
    char paths[SI_MAX_SHARDS][SI_MAX_PATH];
    int  n = 0;

    DIR* dir = opendir(model_dir);
    if (!dir) {
        fprintf(stderr, "spike_index: cannot open %s: %s\n",
                model_dir, strerror(errno));
        return -1;
    }

    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL) {
        const char* name = ent->d_name;
        size_t len = strlen(name);
        if (len <= 12) continue;
        if (strcmp(name + len - 12, ".safetensors") != 0) continue;
        if (n >= SI_MAX_SHARDS) {
            fprintf(stderr, "spike_index: more than %d shards\n",
                    SI_MAX_SHARDS);
            closedir(dir);
            return -1;
        }
        snprintf(paths[n], SI_MAX_PATH, "%s/%s", model_dir, name);
        n++;
    }
    closedir(dir);

    if (n == 0) {
        fprintf(stderr, "spike_index: no .safetensors files in %s\n",
                model_dir);
        return -1;
    }

    // sort lexicographically so model-00001 < model-00002
    qsort(paths, n, SI_MAX_PATH, path_cmp);

    // open each shard
    for (int i = 0; i < n; i++) {
        int fd = open(paths[i], O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "spike_index: cannot open %s: %s\n",
                    paths[i], strerror(errno));
            return -1;
        }
        strncpy(idx->shards[i].path, paths[i], SI_MAX_PATH - 1);
        idx->shards[i].fd = fd;
    }
    idx->n_shards = n;
    return 0;
}

// ── public API ─────────────────────────────────────────────

SpikeIndex* spike_index_load(const char* model_dir) {
    SpikeIndex* idx = calloc(1, sizeof(SpikeIndex));
    if (!idx) { perror("calloc"); return NULL; }

    strncpy(idx->model_dir, model_dir, SI_MAX_PATH - 1);

    if (scan_shards(idx, model_dir) != 0) {
        free(idx);
        return NULL;
    }

    printf("spike_index: loading %d shards from %s\n",
           idx->n_shards, model_dir);

    for (int i = 0; i < idx->n_shards; i++) {
        printf("  parsing %s\n", idx->shards[i].path);
        if (parse_shard(idx, i) != 0) {
            spike_index_free(idx);
            return NULL;
        }
    }

    finalize_blocks(idx);

    printf("spike_index: %d blocks (%d layers + misc)\n",
           idx->n_blocks, idx->n_layers);

    return idx;
}

void spike_index_free(SpikeIndex* idx) {
    if (!idx) return;
    for (int i = 0; i < idx->n_shards; i++) {
        if (idx->shards[i].fd >= 0) {
            close(idx->shards[i].fd);
            idx->shards[i].fd = -1;
        }
    }
    free(idx);
}

void spike_index_print(const SpikeIndex* idx, int verbose) {
    printf("\nspike_index summary\n");
    printf("  model_dir : %s\n", idx->model_dir);
    printf("  shards    : %d\n", idx->n_shards);
    printf("  layers    : %d\n", idx->n_layers);
    printf("  blocks    : %d\n\n", idx->n_blocks);

    for (int i = 0; i < idx->n_blocks; i++) {
        const SIBlock* b = &idx->blocks[i];
        printf("  block %3u  %-12s  %2d tensors  %6.1f MB\n",
               b->block_id, b->label, b->n_tensors,
               (double)b->total_bytes / (1024.0 * 1024.0));

        if (verbose) {
            for (int j = 0; j < b->n_tensors; j++) {
                const SITensor* t = &b->tensors[j];
                printf("    [%2d] %-50s  %5s  %6.1f MB"
                       "  shard=%d  off=%llu  blkoff=%llu\n",
                       j, t->name, t->dtype,
                       (double)t->byte_length / (1024.0 * 1024.0),
                       t->shard_idx,
                       (unsigned long long)t->offset_in_file,
                       (unsigned long long)t->offset_in_block);
            }
        }
    }
    printf("\n");
}

SIBlock* spike_index_block(SpikeIndex* idx, uint32_t block_id) {
    for (int i = 0; i < idx->n_blocks; i++)
        if (idx->blocks[i].block_id == block_id)
            return &idx->blocks[i];
    return NULL;
}

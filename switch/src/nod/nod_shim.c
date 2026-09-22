/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Disc handles own the GcDisc. Partition and file handles are borrowed views
 * with independent cursors and must be freed before their disc handle. */
#include "nod.h"
#include "gc_disc.h"

#include <stdio.h>
#include <stdlib.h>

struct NodHandle {
    GcDisc* disc;
    int owns_disc;
    uint64_t base_offset;
    uint64_t length;
    uint64_t pos;
};

static NodHandle* make_view(GcDisc* disc, int owns_disc, uint64_t base_offset, uint64_t length) {
    NodHandle* h = calloc(1, sizeof(NodHandle));
    if (h == NULL) {
        gc_disc_set_error("out of memory");
        return NULL;
    }
    h->disc = disc;
    h->owns_disc = owns_disc;
    h->base_offset = base_offset;
    h->length = length;
    h->pos = 0;
    return h;
}

NodResult nod_disc_open(const char* path, const void* reserved, NodHandle** out) {
    (void)reserved;
    if (out == NULL) {
        return NOD_RESULT_ERR_INVALID;
    }
    *out = NULL;
    GcDisc* disc = gc_disc_open_path(path);
    if (disc == NULL) {
        return NOD_RESULT_ERR_IO;
    }
    NodHandle* h = make_view(disc, 1, 0, disc->disc_size);
    if (h == NULL) {
        gc_disc_close(disc);
        return NOD_RESULT_ERR_OTHER;
    }
    *out = h;
    return NOD_RESULT_OK;
}

NodResult nod_disc_open_stream(const NodDiscStream* stream, const NodDiscOptions* options,
                               NodHandle** out) {
    (void)options; /* preloader_threads: this shim reads synchronously. */
    if (stream == NULL || out == NULL) {
        return NOD_RESULT_ERR_INVALID;
    }
    *out = NULL;
    GcStream gs = {
        .user_data = stream->user_data,
        .read_at = stream->read_at,
        .stream_len = stream->stream_len,
        .close = stream->close,
    };
    GcDisc* disc = gc_disc_open(&gs, 1);
    if (disc == NULL) {
        return NOD_RESULT_ERR_IO;
    }
    NodHandle* h = make_view(disc, 1, 0, disc->disc_size);
    if (h == NULL) {
        gc_disc_close(disc);
        return NOD_RESULT_ERR_OTHER;
    }
    *out = h;
    return NOD_RESULT_OK;
}

NodResult nod_disc_open_partition_kind(NodHandle* disc, NodPartitionKind kind, const void* reserved,
                                       NodHandle** out) {
    (void)reserved;
    if (disc == NULL || out == NULL) {
        return NOD_RESULT_ERR_INVALID;
    }
    *out = NULL;
    if (kind != NOD_PARTITION_KIND_DATA) {
        /* GameCube discs have no update/channel partitions. */
        gc_disc_set_error("GameCube discs only have a data partition");
        return NOD_RESULT_ERR_INVALID;
    }
    NodHandle* h = make_view(disc->disc, 0, 0, disc->disc->disc_size);
    if (h == NULL) {
        return NOD_RESULT_ERR_OTHER;
    }
    *out = h;
    return NOD_RESULT_OK;
}

NodResult nod_disc_header(NodHandle* handle, NodDiscHeader* out) {
    if (handle == NULL || out == NULL) {
        return NOD_RESULT_ERR_INVALID;
    }
    const GcDisc* disc = handle->disc;
    __builtin_memcpy(out->game_id, disc->game_id, sizeof out->game_id);
    out->disc_num = disc->disc_num;
    out->disc_version = disc->disc_version;
    out->audio_streaming = disc->audio_streaming;
    out->audio_stream_buf_size = disc->audio_stream_buf_size;
    __builtin_memcpy(out->gcn_magic, disc->gcn_magic, sizeof out->gcn_magic);
    return NOD_RESULT_OK;
}

NodResult nod_partition_meta(NodHandle* partition, NodPartitionMeta* out) {
    if (partition == NULL || out == NULL) {
        return NOD_RESULT_ERR_INVALID;
    }
    size_t size = 0;
    const void* data = gc_disc_dol(partition->disc, &size);
    if (data == NULL) {
        return NOD_RESULT_ERR_IO;
    }
    out->raw_dol.data = data;
    out->raw_dol.size = size;
    return NOD_RESULT_OK;
}

NodResult nod_partition_open_file(NodHandle* partition, uint32_t entry_num, NodHandle** out) {
    if (partition == NULL || out == NULL) {
        return NOD_RESULT_ERR_INVALID;
    }
    *out = NULL;
    if (!gc_disc_ensure_fst(partition->disc)) {
        return NOD_RESULT_ERR_IO;
    }
    GcDisc* disc = partition->disc;
    if (entry_num == 0 || entry_num >= disc->fst_count || disc->fst[entry_num].is_dir) {
        gc_disc_set_error("invalid FST file entry number");
        return NOD_RESULT_ERR_INVALID;
    }
    const GcFstEntry* e = &disc->fst[entry_num];
    NodHandle* h = make_view(disc, 0, e->file_offset, e->file_length);
    if (h == NULL) {
        return NOD_RESULT_ERR_OTHER;
    }
    *out = h;
    return NOD_RESULT_OK;
}

struct FstTrampolineCtx {
    NodFstCallback callback;
    void* user_data;
};

static uint32_t fst_trampoline(uint32_t index, int is_dir, const char* name, uint32_t size_or_next,
                               void* user_data) {
    struct FstTrampolineCtx* ctx = (struct FstTrampolineCtx*)user_data;
    NodNodeKind kind = is_dir ? NOD_NODE_KIND_DIRECTORY : NOD_NODE_KIND_FILE;
    return ctx->callback(index, kind, name, size_or_next, ctx->user_data);
}

void nod_partition_iterate_fst(NodHandle* partition, NodFstCallback callback, void* user_data) {
    if (partition == NULL || callback == NULL) {
        return;
    }
    struct FstTrampolineCtx ctx = {.callback = callback, .user_data = user_data};
    gc_disc_walk_fst(partition->disc, fst_trampoline, &ctx);
}

int64_t nod_read(NodHandle* handle, void* buf, size_t len) {
    if (handle == NULL || buf == NULL) {
        return -1;
    }
    if (handle->pos >= handle->length) {
        return 0;
    }
    uint64_t remaining = handle->length - handle->pos;
    size_t want = (len < remaining) ? len : (size_t)remaining;
    int64_t got = gc_disc_read_at(handle->disc, handle->base_offset + handle->pos, buf, want);
    if (got < 0) {
        return -1;
    }
    handle->pos += (uint64_t)got;
    return got;
}

int64_t nod_seek(NodHandle* handle, int64_t offset, int whence) {
    if (handle == NULL) {
        return -1;
    }
    int64_t base;
    switch (whence) {
    case SEEK_SET:
        base = 0;
        break;
    case SEEK_CUR:
        base = (int64_t)handle->pos;
        break;
    case SEEK_END:
        base = (int64_t)handle->length;
        break;
    default:
        return -1;
    }
    int64_t target = base + offset;
    if (target < 0 || (uint64_t)target > handle->length) {
        return -1;
    }
    handle->pos = (uint64_t)target;
    return target;
}

uint64_t nod_disc_size(NodHandle* handle) {
    if (handle == NULL) {
        return 0;
    }
    return handle->disc->disc_size;
}

void nod_free(NodHandle* handle) {
    if (handle == NULL) {
        return;
    }
    if (handle->owns_disc) {
        gc_disc_close(handle->disc);
    }
    free(handle);
}

const char* nod_error_message(void) { return gc_disc_last_error(); }

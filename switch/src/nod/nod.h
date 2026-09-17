/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * A from-scratch C reimplementation of the small slice of the `nod` C ABI
 * (https://github.com/encounter/nod) that melee-pc and aurora's DVD layer
 * actually call. Backed by switch/src/nod/gc_disc.c, a plain-C reader for
 * single-partition, unencrypted GameCube disc images.
 *
 * Why this exists: real `nod` is a Rust crate, and Rust has no official
 * Nintendo Switch/Horizon target. melee-pc always talks to it through this
 * C header, never through Rust internals directly, so a compatible C
 * implementation is a legal, drop-in substitute for this platform. It only
 * covers what a plain NTSC-U GameCube disc needs: no Wii partitions, no
 * WIA/RVZ compression.
 *
 * Struct field names/order and function signatures here match exactly what
 * ref/melee-pc's src/pc sources and extern/aurora's lib/dolphin/dvd sources
 * reference (reverse-engineered from those call sites, not from upstream
 * nod's own headers).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NodHandle NodHandle; /* opaque: disc, partition, or per-file handle */

typedef enum NodResult {
    NOD_RESULT_OK = 0,
    NOD_RESULT_ERR_IO = 1,
    NOD_RESULT_ERR_INVALID = 2,
    NOD_RESULT_ERR_OTHER = 3,
} NodResult;

typedef enum NodPartitionKind {
    NOD_PARTITION_KIND_DATA = 0,
    NOD_PARTITION_KIND_UPDATE = 1,
    NOD_PARTITION_KIND_CHANNEL = 2,
} NodPartitionKind;

typedef enum NodNodeKind {
    NOD_NODE_KIND_FILE = 0,
    NOD_NODE_KIND_DIRECTORY = 1,
} NodNodeKind;

typedef int64_t (*NodStreamReadAt)(void* user_data, uint64_t offset, void* out, size_t len);
typedef int64_t (*NodStreamLen)(void* user_data);
typedef void (*NodStreamClose)(void* user_data);

typedef struct NodDiscStream {
    void* user_data;
    NodStreamReadAt read_at;
    NodStreamLen stream_len;
    NodStreamClose close;
} NodDiscStream;

typedef struct NodDiscOptions {
    uint32_t preloader_threads;
} NodDiscOptions;

typedef struct NodDiscHeader {
    char game_id[6];
    uint8_t disc_num;
    uint8_t disc_version;
    uint8_t audio_streaming;
    uint8_t audio_stream_buf_size;
    unsigned char gcn_magic[4];
} NodDiscHeader;

typedef struct NodRawSlice {
    const uint8_t* data; /* DVDGetDOLLocation returns this straight through as const u8* */
    size_t size;
} NodRawSlice;

typedef struct NodPartitionMeta {
    NodRawSlice raw_dol;
} NodPartitionMeta;

/* Returns the FST index to resume from (normally index + 1; a directory may
 * return its own "next" index to skip the whole subtree). */
typedef uint32_t (*NodFstCallback)(
    uint32_t index, NodNodeKind kind, const char* name, uint32_t size, void* user_data);

NodResult nod_disc_open(const char* path, const void* reserved, NodHandle** out);
NodResult nod_disc_open_stream(const NodDiscStream* stream, const NodDiscOptions* options, NodHandle** out);
NodResult nod_disc_open_partition_kind(
    NodHandle* disc, NodPartitionKind kind, const void* reserved, NodHandle** out);
NodResult nod_disc_header(NodHandle* handle, NodDiscHeader* out);
NodResult nod_partition_meta(NodHandle* partition, NodPartitionMeta* out);
NodResult nod_partition_open_file(NodHandle* partition, uint32_t entry_num, NodHandle** out);
void nod_partition_iterate_fst(NodHandle* partition, NodFstCallback callback, void* user_data);

int64_t nod_read(NodHandle* handle, void* buf, size_t len);
int64_t nod_seek(NodHandle* handle, int64_t offset, int whence);
uint64_t nod_disc_size(NodHandle* handle);
void nod_free(NodHandle* handle);

/* Last error on this thread; valid after any of the above returns non-OK.
 * Not reset on success. May return NULL. */
const char* nod_error_message(void);

#ifdef __cplusplus
}
#endif

/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Minimal reader for single-partition, unencrypted GameCube disc images
 * (raw .iso/.gcm — no Wii partition table, no compression). Shared by
 * nod_shim.c (the nod.h-compatible C ABI) and the on-device disc extractor.
 *
 * Layout reference (all fields big-endian, standard since the format was
 * first reverse-engineered ~2003, unchanged since):
 *   0x00        game_id[6]            e.g. "GALE01"
 *   0x06        disc_num       (u8)
 *   0x07        disc_version   (u8)
 *   0x08        audio_streaming(u8)
 *   0x09        audio_stream_buf_size (u8)
 *   0x1C        gcn_magic[4]          0xC2339F3D
 *   0x420       dol_offset     (u32)
 *   0x424       fst_offset     (u32)
 *   0x428       fst_size       (u32)
 * FST: array of 12-byte entries starting at fst_offset. Entry 0 is the
 * implicit root directory (parent/name unused, "next" = total entry count).
 * Entry layout: u8 is_dir; u24 name_offset (into the string table, which
 * starts right after the last entry); u32 (file_offset | parent_index);
 * u32 (file_length | next_index). melee-pc/aurora's DOL_OFFSET_FIELD (0x420)
 * and FST_OFFSET_FIELD (0x424) constants (src/pc/discfont.c) confirm these
 * offsets independently.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int64_t (*GcStreamReadAt)(void* user_data, uint64_t offset, void* out, size_t len);
typedef int64_t (*GcStreamLen)(void* user_data);
typedef void (*GcStreamClose)(void* user_data);

typedef struct GcStream {
    void* user_data;
    GcStreamReadAt read_at;
    GcStreamLen stream_len;
    GcStreamClose close; /* may be NULL */
} GcStream;

typedef struct GcFstEntry {
    int is_dir;
    uint32_t parent_index;  /* directories only */
    uint32_t next_index;    /* directories only: index after this subtree */
    uint32_t file_offset;   /* files only: absolute disc offset */
    uint32_t file_length;   /* files only */
    char* name;              /* heap-owned, this entry's own name (not a path) */
} GcFstEntry;

typedef struct GcDisc {
    GcStream stream;
    int owns_stream;
    uint64_t disc_size;

    char game_id[6];
    uint8_t disc_num;
    uint8_t disc_version;
    uint8_t audio_streaming;
    uint8_t audio_stream_buf_size;
    unsigned char gcn_magic[4];
    uint32_t dol_offset;
    uint32_t fst_offset;
    uint32_t fst_size;

    GcFstEntry* fst;
    uint32_t fst_count;
    int fst_parsed;

    void* dol_cache;
    uint64_t dol_cache_size;
} GcDisc;

/* Opens a disc from caller-supplied I/O callbacks and parses the boot header.
 * Does not parse the FST yet (gc_disc_ensure_fst does that lazily).
 * Returns NULL on failure (see gc_disc_last_error()). */
GcDisc* gc_disc_open(const GcStream* stream, int take_ownership);

/* Convenience: opens `path` via plain stdio and calls gc_disc_open(). */
GcDisc* gc_disc_open_path(const char* path);

/* Parses the FST on first call; a no-op afterward. Returns 0 on failure. */
int gc_disc_ensure_fst(GcDisc* disc);

/* Return this from a GcFstWalkFn to stop the walk early (e.g. user canceled
 * an extraction in progress). Safe because real FST entry counts never reach
 * this: GC_FST_MAX_ENTRIES caps parsing well below it. */
#define GC_FST_WALK_STOP 0xFFFFFFFFu

/* Walks FST entries [1, fst_count), depth-first pre-order (matching on-disc
 * order). `callback` returns the index to resume from (normally index + 1;
 * see GC_FST_WALK_STOP to abort). Requires gc_disc_ensure_fst() to have
 * succeeded. */
typedef uint32_t (*GcFstWalkFn)(
    uint32_t index, int is_dir, const char* name, uint32_t size_or_next, void* user_data);
void gc_disc_walk_fst(GcDisc* disc, GcFstWalkFn callback, void* user_data);

/* Returns the cached main.dol bytes, reading+caching on first call. */
const void* gc_disc_dol(GcDisc* disc, size_t* out_size);

/* Byte-range read against the raw disc stream (absolute offset). */
int64_t gc_disc_read_at(GcDisc* disc, uint64_t offset, void* out, size_t len);

void gc_disc_close(GcDisc* disc);

const char* gc_disc_last_error(void);
void gc_disc_set_error(const char* message);

#ifdef __cplusplus
}
#endif

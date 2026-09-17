/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Disc extraction driver: user's NTSC-U 1.02 (GALE01) ISO/GCM -> the loose
 * `files/` directory melee-pc's own file_cache.cpp already knows how to read
 * from (src/pc/file_cache.cpp's resolve_loose_path checks `./files` before
 * ever touching the disc). Unlike KartPad-NX's Wii port, there is no `sys/`
 * mirror to write: melee-pc never reads main.dol/fst.bin from loose files,
 * only individual archive files by their disc path, so extraction only needs
 * to reproduce the FST's file tree.
 *
 * Same shape as KartPad-NX's switch/src/runtime/disc_extractor.h (progress
 * struct/callback, staged writes, a completion manifest so a partial
 * extraction never looks "done" on the next launch) minus the Wii
 * encryption/partition-table layers GameCube doesn't have.
 */
#pragma once

#include <cstdint>
#include <string>

namespace melee_nx::disc {

struct ExtractProgress {
    const char* stage = "";
    std::uint64_t filesDone = 0;
    std::uint64_t filesTotal = 0;
    std::uint64_t bytesDone = 0;
    std::uint64_t bytesTotal = 0;
};

/* Called periodically during extraction. May be null. Return false to abort. */
using ExtractProgressFn = bool (*)(const ExtractProgress& p, void* user);

struct ExtractOptions {
    ExtractProgressFn progress = nullptr;
    void* progressUser = nullptr;
};

struct ExtractResult {
    bool ok = false;
    std::string message; /* human-readable, safe to display */
    std::uint64_t filesExtracted = 0;
    std::uint64_t bytesExtracted = 0;
};

/* Opens discPath, validates it is Melee NTSC-U revision 2 (1.02, GALE01), and
 * writes every FST file into filesDirOut, preserving the disc's own directory
 * structure (this must match verbatim what game code passes to DVDOpen/
 * file_cache's loose-path lookup). Returns ok=false with a message on any
 * failure; partial output may exist on disk and a re-run overwrites it. */
ExtractResult extractGameFiles(
    const std::string& discPath, const std::string& filesDirOut, const ExtractOptions& opts);

/* True only when filesDirOut has a matching, successfully-published
 * completion manifest -- the authoritative "is extraction done" check. */
bool isExtractionComplete(const std::string& filesDirOut);

/* Quick check that `path` looks like a Melee NTSC-U 1.02 disc image, without
 * extracting anything. Used to decide whether to offer extraction at all. */
bool looksLikeMeleeDisc(const std::string& path, std::string& outReason);

}  // namespace melee_nx::disc

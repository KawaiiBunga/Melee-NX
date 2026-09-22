/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Extract the Melee NTSC-U 1.02 FST into the loose-file cache.
 * Publish a completion manifest only after all files are written. */
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
ExtractResult extractGameFiles(const std::string& discPath, const std::string& filesDirOut,
                               const ExtractOptions& opts);

/* True only when filesDirOut has a matching, successfully-published
 * completion manifest -- the authoritative "is extraction done" check. */
bool isExtractionComplete(const std::string& filesDirOut);

/* Quick check that `path` looks like a Melee NTSC-U 1.02 disc image, without
 * extracting anything. Used to decide whether to offer extraction at all. */
bool looksLikeMeleeDisc(const std::string& path, std::string& outReason);

} // namespace melee_nx::disc

/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "disc_extractor.h"

#include "../nod/gc_disc.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace melee_nx::disc {
namespace {

namespace fs = std::filesystem;

constexpr const char* kManifestName = ".melee-nx-extraction-complete";
constexpr const char* kManifestMagic = "MELEE-NX-DVD-EXTRACT-V1";
constexpr size_t kChunkSize = 1u << 20; /* 1 MiB */

struct WalkFrame {
    std::string path; /* relative to filesDirOut, "" for the disc root */
    uint32_t endIndex;
};

bool readHeaderFields(GcDisc* disc, std::string& gameId, uint8_t& discNum, uint8_t& discVersion) {
    gameId.assign(disc->game_id, disc->game_id + sizeof disc->game_id);
    discNum = disc->disc_num;
    discVersion = disc->disc_version;
    return true;
}

} // namespace

bool looksLikeMeleeDisc(const std::string& path, std::string& outReason) {
    GcDisc* disc = gc_disc_open_path(path.c_str());
    if (disc == nullptr) {
        outReason = gc_disc_last_error() ? gc_disc_last_error() : "Could not open disc image.";
        return false;
    }
    std::string gameId;
    uint8_t discNum = 0, discVersion = 0;
    readHeaderFields(disc, gameId, discNum, discVersion);
    gc_disc_close(disc);

    if (gameId != "GALE01") {
        outReason =
            (gameId.rfind("GAL", 0) == 0)
                ? "This region/game is not supported. Choose Melee USA revision 2 (NTSC-U 1.02)."
                : "Wrong game. Choose Super Smash Bros. Melee USA revision 2.";
        return false;
    }
    if (discVersion != 2 || discNum != 0) {
        outReason = "Unsupported revision. This port requires Melee NTSC-U 1.02 (revision 2).";
        return false;
    }
    return true;
}

bool isExtractionComplete(const std::string& filesDirOut) {
    std::error_code ec;
    if (!fs::is_directory(filesDirOut, ec)) {
        return false;
    }
    std::ifstream manifest(fs::path(filesDirOut).parent_path() / kManifestName, std::ios::binary);
    if (!manifest) {
        return false;
    }
    std::string firstLine;
    std::getline(manifest, firstLine);
    if (!firstLine.empty() && firstLine.back() == '\r') {
        firstLine.pop_back();
    }
    return firstLine == kManifestMagic;
}

namespace {

struct CountCtx {
    uint64_t files = 0;
    uint64_t bytes = 0;
};

uint32_t countCallback(uint32_t index, int is_dir, const char* /*name*/, uint32_t sizeOrNext,
                       void* userData) {
    auto* ctx = static_cast<CountCtx*>(userData);
    if (!is_dir) {
        ctx->files++;
        ctx->bytes += sizeOrNext;
    }
    return index + 1;
}

struct ExtractCtx {
    GcDisc* disc = nullptr;
    fs::path outRoot;
    ExtractOptions opts;
    std::vector<WalkFrame> stack;
    ExtractProgress progress;
    bool failed = false;
    bool canceled = false;
    std::string failMessage;
};

bool reportProgress(ExtractCtx& ctx, const char* stage) {
    if (ctx.opts.progress == nullptr) {
        return true;
    }
    ctx.progress.stage = stage;
    bool keepGoing = ctx.opts.progress(ctx.progress, ctx.opts.progressUser);
    if (!keepGoing) {
        ctx.canceled = true;
    }
    return keepGoing;
}

/* Buffer big enough for a single 1 MiB chunk read straight off the disc
 * stream; static because this only ever runs on the (single-threaded)
 * extraction path. */
uint8_t s_chunkBuf[kChunkSize];

bool extractOneFile(ExtractCtx& ctx, uint32_t entryIndex, const fs::path& fullPath,
                    uint32_t fileOffsetBase, uint64_t length) {
    fs::path tmpPath = fullPath;
    tmpPath += ".part";

    FILE* out = std::fopen(tmpPath.string().c_str(), "wb");
    if (out == nullptr) {
        ctx.failMessage = "Could not create " + fullPath.string();
        return false;
    }

    uint64_t written = 0;
    bool ok = true;
    while (written < length) {
        size_t want = static_cast<size_t>(std::min<uint64_t>(kChunkSize, length - written));
        int64_t got = gc_disc_read_at(ctx.disc, fileOffsetBase + written, s_chunkBuf, want);
        if (got <= 0) {
            ctx.failMessage = "Disc read failed while extracting " + fullPath.string();
            ok = false;
            break;
        }
        if (std::fwrite(s_chunkBuf, 1, static_cast<size_t>(got), out) != static_cast<size_t>(got)) {
            ctx.failMessage = "SD card write failed for " + fullPath.string();
            ok = false;
            break;
        }
        written += static_cast<uint64_t>(got);
        ctx.progress.bytesDone += static_cast<uint64_t>(got);
        if (!reportProgress(ctx, "Extracting game data")) {
            ok = false;
            break;
        }
    }
    (void)entryIndex;
    std::fclose(out);

    if (!ok) {
        std::error_code ec;
        fs::remove(tmpPath, ec);
        return false;
    }

    std::error_code ec;
    fs::remove(fullPath,
               ec); /* rename() over an existing file is platform-dependent; clear first */
    fs::rename(tmpPath, fullPath, ec);
    if (ec) {
        ctx.failMessage = "Could not finalize " + fullPath.string();
        return false;
    }
    return true;
}

uint32_t extractCallback(uint32_t index, int is_dir, const char* name, uint32_t sizeOrNext,
                         void* userData) {
    auto* ctx = static_cast<ExtractCtx*>(userData);
    if (ctx->failed || ctx->canceled) {
        return GC_FST_WALK_STOP;
    }

    while (index >= ctx->stack.back().endIndex) {
        ctx->stack.pop_back();
    }
    const std::string& dirPath = ctx->stack.back().path;
    std::string relPath = dirPath.empty() ? name : (dirPath + "/" + name);
    fs::path fullPath = ctx->outRoot / relPath;

    if (is_dir) {
        std::error_code ec;
        fs::create_directories(fullPath, ec);
        if (ec) {
            ctx->failed = true;
            ctx->failMessage = "Could not create directory " + fullPath.string();
            return GC_FST_WALK_STOP;
        }
        ctx->stack.push_back({std::move(relPath), sizeOrNext});
        return index + 1;
    }

    const GcFstEntry* entry = &ctx->disc->fst[index];
    if (!extractOneFile(*ctx, index, fullPath, entry->file_offset, entry->file_length)) {
        ctx->failed = true;
        return GC_FST_WALK_STOP;
    }
    ctx->progress.filesDone++;
    if (!reportProgress(*ctx, "Extracting game data")) {
        return GC_FST_WALK_STOP;
    }
    return index + 1;
}

} // namespace

ExtractResult extractGameFiles(const std::string& discPath, const std::string& filesDirOut,
                               const ExtractOptions& opts) {
    ExtractResult result;

    std::string reason;
    if (!looksLikeMeleeDisc(discPath, reason)) {
        result.message = reason;
        return result;
    }

    GcDisc* disc = gc_disc_open_path(discPath.c_str());
    if (disc == nullptr) {
        result.message = gc_disc_last_error() ? gc_disc_last_error() : "Could not open disc image.";
        return result;
    }
    if (!gc_disc_ensure_fst(disc)) {
        result.message =
            gc_disc_last_error() ? gc_disc_last_error() : "Could not read the disc's file table.";
        gc_disc_close(disc);
        return result;
    }

    CountCtx counts;
    gc_disc_walk_fst(disc, countCallback, &counts);

    std::error_code ec;
    fs::create_directories(filesDirOut, ec);
    if (ec) {
        result.message = "Could not create " + filesDirOut;
        gc_disc_close(disc);
        return result;
    }

    ExtractCtx ctx;
    ctx.disc = disc;
    ctx.outRoot = filesDirOut;
    ctx.opts = opts;
    ctx.stack.push_back({"", 0xFFFFFFFFu});
    ctx.progress.filesTotal = counts.files;
    ctx.progress.bytesTotal = counts.bytes;

    reportProgress(ctx, "Extracting game data");
    gc_disc_walk_fst(disc, extractCallback, &ctx);

    gc_disc_close(disc);

    if (ctx.canceled) {
        result.message = "Extraction canceled.";
        return result;
    }
    if (ctx.failed) {
        result.message = ctx.failMessage.empty() ? "Extraction failed." : ctx.failMessage;
        return result;
    }

    /* Completion manifest lives next to filesDirOut (not inside it), same
     * spirit as KartPad-NX's: proves every file was written, not just that
     * the directory exists (which a half-finished run would already satisfy). */
    fs::path manifestPath = fs::path(filesDirOut).parent_path() / kManifestName;
    fs::path manifestTmp = manifestPath;
    manifestTmp += ".part";
    {
        std::ofstream manifest(manifestTmp, std::ios::binary | std::ios::trunc);
        if (!manifest) {
            result.message = "Could not write completion marker.";
            return result;
        }
        manifest << kManifestMagic << "\n"
                 << "files=" << ctx.progress.filesDone << "\n"
                 << "bytes=" << ctx.progress.bytesDone << "\n";
    }
    fs::remove(manifestPath, ec);
    fs::rename(manifestTmp, manifestPath, ec);
    if (ec) {
        result.message = "Could not finalize completion marker.";
        return result;
    }

    result.ok = true;
    result.message = "Extraction complete.";
    result.filesExtracted = ctx.progress.filesDone;
    result.bytesExtracted = ctx.progress.bytesDone;
    return result;
}

} // namespace melee_nx::disc

// SPDX-License-Identifier: GPL-3.0-or-later
#include "data/game_data.h"

#include <cctype>
#include <filesystem>

#include <sys/stat.h>

#include "data/paths.h"
#include "disc_extractor.h"

namespace mnx::gamedata {
namespace {

namespace fs = std::filesystem;

// The literal the game's launcher recognises (game_data_gate.h kLooseBootSentinel).
constexpr const char* kLooseBootSentinel = "@melee-nx-loose";

bool IsFile(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool IsDiscName(const std::string& name) {
    if (name.size() < 4) return false;
    std::string ext = name.substr(name.size() - 4);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".iso" || ext == ".gcm";
}

}  // namespace

std::string Status::BootArg() const { return !discPath.empty() ? discPath : (looseReady ? kLooseBootSentinel : ""); }

std::vector<DiscCandidate> FindDiscs() {
    std::vector<DiscCandidate> out;
    const char* dirs[] = {paths::kRoot, "sdmc:/", "sdmc:/roms/gc", "sdmc:/roms/gamecube", "sdmc:/games"};
    for (const char* dir : dirs) {
        std::error_code ec;
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code kind;
            const std::string name = it->path().filename().string();
            if (!IsDiscName(name) || !it->is_regular_file(kind)) continue;
            DiscCandidate c;
            c.path = std::string(dir) + (std::string(dir).back() == '/' ? "" : "/") + name;
            c.valid = melee_nx::disc::looksLikeMeleeDisc(c.path, c.reason);
            out.push_back(std::move(c));
        }
    }
    return out;
}

Status Probe(const std::string& preferred) {
    Status s;
    std::string reason;
    if (!preferred.empty() && IsFile(preferred) && melee_nx::disc::looksLikeMeleeDisc(preferred, reason)) {
        s.discPath = preferred;
    } else {
        for (const auto& c : FindDiscs()) {
            if (c.valid) {
                s.discPath = c.path;
                break;
            }
        }
    }
    s.extracted = melee_nx::disc::isExtractionComplete(paths::kFilesDir);
    s.looseReady = s.extracted && IsFile(paths::kDiscMeta) && IsFile(paths::kDiscBoot);
    return s;
}

namespace {

struct ExtractCtx {
    Job::Reporter* r;
};

bool OnProgress(const melee_nx::disc::ExtractProgress& p, void* user) {
    auto* ctx = static_cast<ExtractCtx*>(user);
    float f = -1.0f;
    if (p.bytesTotal) f = static_cast<float>(static_cast<double>(p.bytesDone) / static_cast<double>(p.bytesTotal));
    std::string stage = p.stage ? p.stage : "";
    if (p.filesTotal) stage += "  (" + std::to_string(p.filesDone) + " / " + std::to_string(p.filesTotal) + " files)";
    ctx->r->Stage(stage, f);
    return !ctx->r->Cancelled();
}

}  // namespace

Job::Result Extract(const std::string& disc, Job::Reporter& r) {
    ExtractCtx ctx{&r};
    melee_nx::disc::ExtractOptions opts;
    opts.progress = OnProgress;
    opts.progressUser = &ctx;
    r.Stage("Opening the disc image...");
    const auto res = melee_nx::disc::extractGameFiles(disc, paths::kFilesDir, opts);
    if (!res.ok) return {false, res.message.empty() ? "Extraction stopped." : res.message};
    return {true, std::to_string(res.filesExtracted) + " files (" + std::to_string(res.bytesExtracted >> 20) +
                      " MiB) extracted. Start the game once with the disc image present so it can boot without it later."};
}

}  // namespace mnx::gamedata

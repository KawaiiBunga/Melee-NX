/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "launch_request.h"

#include "disc_extractor.h"
#include "game_data_gate.h"

#include <cstdio>
#include <cstring>
#include <sys/stat.h>

namespace melee_nx {
namespace {

bool isFile(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r')) {
        ++b;
    }
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) {
        --e;
    }
    return s.substr(b, e - b);
}

} // namespace

bool HasLaunchFlag(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (argv[i] != nullptr && std::strcmp(argv[i], "--launch") == 0) {
            return true;
        }
    }
    return false;
}

LaunchRequest ParseLaunchConfig(const std::string& text) {
    LaunchRequest r;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) {
            end = text.size();
        }
        const std::string line = trim(text.substr(pos, end - pos));
        pos = end + 1;
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = trim(line.substr(0, eq));
        const std::string value = trim(line.substr(eq + 1));
        if (key == "disc") {
            r.disc = value;
        } else if (key == "mods") {
            r.mods = value;
        } else if (key == "return") {
            r.returnTo = value;
        } else if (key == "mode") {
            r.mode = value;
        } else if (key == "code") {
            r.code = value;
        }
    }
    r.present = !r.disc.empty();
    return r;
}

LaunchRequest ReadLaunchRequest(int argc, char** argv) {
    if (!HasLaunchFlag(argc, argv)) {
        return {};
    }
    std::FILE* f = std::fopen(kLaunchCfg, "rb");
    if (f == nullptr) {
        return {};
    }
    std::string text;
    char buf[1024];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0 && text.size() < 16384;) {
        text.append(buf, n);
    }
    std::fclose(f);
    return ParseLaunchConfig(text);
}

bool ValidateLaunchDisc(const std::string& disc, std::string& reason) {
    if (disc == kLooseBootSentinel) {
        if (!disc::isExtractionComplete(kFilesDir)) {
            reason = "extracted files are incomplete";
            return false;
        }
        if (!isFile(std::string(kDataRoot) + "/disc.meta") || !isFile(std::string(kDataRoot) + "/disc-boot.bin")) {
            reason = "disc.meta/disc-boot.bin missing";
            return false;
        }
        return true;
    }
    if (!isFile(disc)) {
        reason = "disc image not found";
        return false;
    }
    return disc::looksLikeMeleeDisc(disc, reason);
}

} // namespace melee_nx

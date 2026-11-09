#include "data/launch.h"

#include <sys/stat.h>

#include <switch.h>

#include "data/paths.h"
#include "data/settings.h"

namespace mnx::launch {
namespace {

bool IsFile(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool Clean(const std::string& v) { return v.find('\n') == std::string::npos && v.find('\r') == std::string::npos; }

std::string& Self() {
    static std::string path = paths::kLauncherNroDefault;
    return path;
}

}  // namespace

void SetSelfPath(const char* argv0) {
    // hbloader passes the NRO path as argv[0] ("sdmc:/switch/..."). Anything else keeps the default.
    if (!argv0) return;
    const std::string p(argv0);
    if (p.rfind("sdmc:/", 0) == 0 && p.size() > 4 && p.compare(p.size() - 4, 4, ".nro") == 0) Self() = p;
}

const std::string& SelfPath() { return Self(); }

std::string FindGameNro() {
    for (const char* p : paths::kGameNroCandidates) {
        if (IsFile(p)) return p;
    }
    return {};
}

std::string FormatConfig(const Request& r) {
    return "# Written by the Melee NX launcher; read by melee.nro --launch\n"
           "disc=" + r.bootArg + "\nmods=" + r.modsManifest + "\nreturn=" + r.returnTo + "\nmode=" + r.mode +
           "\ncode=" + r.code + "\n";
}

bool NormalizeConnectCode(std::string& code) {
    std::string out;
    size_t hash = std::string::npos;
    for (char c : code) {
        if (c == ' ') continue;
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        const bool digit = c >= '0' && c <= '9', letter = c >= 'A' && c <= 'Z';
        if (c == '#' && hash == std::string::npos) hash = out.size();
        else if (!(digit || (letter && hash == std::string::npos))) return false;
        out += c;
    }
    if (hash == std::string::npos || hash < 1 || hash > 8) return false;
    const size_t digits = out.size() - hash - 1;
    if (digits < 1 || digits > 3) return false;
    code = out;
    return true;
}

bool Start(const std::string& gameNro, const Request& r, std::string& error) {
    if (gameNro.empty()) {
        error = "melee.nro was not found. Put it in /switch/melee-nx/bin/ or /switch/melee-nx/.";
        return false;
    }
    if (r.bootArg.empty()) {
        error = "No game data yet. Set it up in Game Files.";
        return false;
    }
    if (!Clean(r.bootArg) || !Clean(r.modsManifest) || !Clean(r.returnTo) || !Clean(r.code)) {
        error = "A path contains a line break.";
        return false;
    }
    if (!envHasNextLoad()) {
        error = "This loader cannot start another program. Open the launcher from the Homebrew Menu.";
        return false;
    }
    if (!WriteFileAtomic(paths::kLaunchCfg, FormatConfig(r))) {
        error = "Could not write launch.cfg to the SD card.";
        return false;
    }
    const std::string argv = gameNro + " --launch";
    if (gameNro.size() >= 512 || argv.size() >= 2048) {
        error = "The game path is too long.";
        return false;
    }
    if (R_FAILED(envSetNextLoad(gameNro.c_str(), argv.c_str()))) {
        error = "The loader refused to queue the game.";
        return false;
    }
    return true;
}

}  // namespace mnx::launch

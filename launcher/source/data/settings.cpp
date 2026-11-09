#include "data/settings.h"

#include <cstdio>
#include <filesystem>

#include <nlohmann/json.hpp>

#include "data/fsutil.h"
#include "data/paths.h"

namespace mnx {

bool ReadFile(const std::string& path, std::string& out, size_t maxBytes) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    out.clear();
    char buf[16384];
    bool ok = true;
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) {
        if (out.size() + n > maxBytes) {
            ok = false;
            break;
        }
        out.append(buf, n);
    }
    ok = ok && !std::ferror(f);
    std::fclose(f);
    return ok;
}

bool WriteFileAtomic(const std::string& path, const std::string& text) {
    const std::filesystem::path p(path);
    if (p.has_parent_path() && mkw::fsx::MakeDirs(p.parent_path())) return false;
    const std::string tmp = path + ".tmp";
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    ok = (std::fclose(f) == 0) && ok;
    if (!ok) {
        std::remove(tmp.c_str());
        return false;
    }
    // Horizon's rename does not replace an existing file.
    std::remove(path.c_str());
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

Settings LoadSettings() {
    Settings s;
    std::string text;
    if (!ReadFile(paths::kSettings, text)) return s;
    const auto j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_object()) return s;
    s.accent = j.value("accent", s.accent);
    s.modsEnabled = j.value("modsEnabled", s.modsEnabled);
    s.quickLaunch = j.value("quickLaunch", s.quickLaunch);
    s.returnToLauncher = j.value("returnToLauncher", s.returnToLauncher);
    s.discPath = j.value("discPath", s.discPath);
    s.mode = j.value("mode", s.mode) == 1 ? 1 : 0;
    s.opponentCode = j.value("opponentCode", s.opponentCode);
    return s;
}

bool SaveSettings(const Settings& s) {
    const nlohmann::json j = {{"accent", s.accent},
                              {"modsEnabled", s.modsEnabled},
                              {"quickLaunch", s.quickLaunch},
                              {"returnToLauncher", s.returnToLauncher},
                              {"discPath", s.discPath},
                              {"mode", s.mode},
                              {"opponentCode", s.opponentCode}};
    return WriteFileAtomic(paths::kSettings, j.dump(2) + "\n");
}

}  // namespace mnx

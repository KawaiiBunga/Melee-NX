// SPDX-License-Identifier: GPL-3.0-or-later
#include "data/mods.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>

#include <sys/stat.h>

#include <nlohmann/json.hpp>

#include "data/dat_identity.h"
#include "data/fsutil.h"
#include "data/settings.h"
#include "data/zip_extract.h"

namespace mnx {
namespace {

namespace fs = std::filesystem;

constexpr size_t kMaxDatBytes = 64u << 20;  // matches Melee Unlocked's per-DAT import limit

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool EndsWith(const std::string& s, const char* suffix) {
    const size_t n = std::char_traits<char>::length(suffix);
    return s.size() >= n && Lower(s.substr(s.size() - n)) == suffix;
}

bool IsFile(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool IsDir(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// Files inside a mod folder that are about the mod, not for the game.
bool IsMetaFile(const std::string& rel) {
    const std::string l = Lower(rel);
    // Images too: costume packs ship csp/stock PNGs, which need texture replacement (not supported).
    return l == "mod.json" || EndsWith(l, ".png") || EndsWith(l, ".jpg") || EndsWith(l, ".txt") || EndsWith(l, ".md") ||
           EndsWith(l, ".zip") || l.rfind("__macosx/", 0) == 0 || fs::path(l).filename().string()[0] == '.';
}

std::string Label(const std::string& discPath) {
    const std::string name = fs::path(discPath).filename().string();
    if (std::string costume = dat::DescribeCostumeFile(name); !costume.empty()) return costume;
    if (name.size() > 2 && name[0] == 'G' && name[1] == 'r') return "Stage file " + name;
    if (name.rfind("Ef", 0) == 0) return "Effects " + name;
    if (name.rfind("Mn", 0) == 0 || name.rfind("If", 0) == 0) return "Menus " + name;
    return {};
}

// The disc path a mod file replaces. Costume sites name DATs freely ("PlMrYe sbsp.dat"), so a DAT
// whose name is not a real game file is identified from its own roots instead.
std::string DiscPathFor(const std::string& path, const std::string& rel, const std::string& filesDir, bool discComplete) {
    if (!EndsWith(rel, ".dat") && !EndsWith(rel, ".usd")) return rel;
    const bool known = discComplete ? IsFile(filesDir + "/" + rel)
                                    : !dat::DescribeCostumeFile(fs::path(rel).filename().string()).empty();
    if (known) return rel;
    std::string text;
    if (!ReadFile(path, text, kMaxDatBytes)) return rel;
    const dat::Identity id = dat::IdentifyCostume(std::vector<std::uint8_t>(text.begin(), text.end()));
    return id.ok ? id.targetFile : rel;
}

// A loose costume DAT in mods/: identify it and move it into mods/<stem>/<slot file>.
bool FileLooseDat(const std::string& modsDir, const std::string& file, ScanReport& report) {
    std::string bytesText;
    if (!ReadFile(modsDir + "/" + file, bytesText, kMaxDatBytes)) {
        report.problems.push_back(file + ": could not be read (over 64 MB?)");
        return false;
    }
    const std::vector<std::uint8_t> bytes(bytesText.begin(), bytesText.end());
    const dat::Identity id = dat::IdentifyCostume(bytes);
    if (!id.ok) {
        report.problems.push_back(file + ": " + id.error);
        return false;
    }
    std::string stem = fs::path(file).stem().string();
    std::string folder = modsDir + "/" + stem;
    for (int n = 2; IsDir(folder) || IsFile(folder); ++n) folder = modsDir + "/" + stem + " " + std::to_string(n);
    if (mkw::fsx::MakeDirs(folder)) {
        report.problems.push_back(file + ": could not create its mod folder");
        return false;
    }
    if (std::rename((modsDir + "/" + file).c_str(), (folder + "/" + id.targetFile).c_str()) != 0) {
        report.problems.push_back(file + ": could not be moved into " + folder);
        return false;
    }
    const nlohmann::json meta = {{"name", stem},
                                 {"description", id.fighter + " costume, replaces " + id.costume + " (" + id.targetFile + ")"}};
    WriteFileAtomic(folder + "/mod.json", meta.dump(2) + "\n");
    ++report.identified;
    return true;
}

}  // namespace

ScanReport ModCatalog::Scan(const std::string& modsDir, const std::string& discFilesDir, bool discComplete) {
    ScanReport report;
    mkw::fsx::MakeDirs(modsDir);

    // Pass 1: unpack zips and file loose DATs.
    std::vector<std::string> entries;
    std::error_code ec;
    for (fs::directory_iterator it(modsDir, ec), end; !ec && it != end; it.increment(ec)) {
        entries.push_back(it->path().filename().string());
    }
    for (const std::string& name : entries) {
        const std::string full = modsDir + "/" + name;
        if (!IsFile(full)) continue;
        if (EndsWith(name, ".zip")) {
            const std::string dest = modsDir + "/" + fs::path(name).stem().string();
            if (IsDir(dest)) continue;  // already unpacked
            std::string err;
            if (mkw::zipx::Extract(full, dest, err))
                ++report.unpacked;
            else
                report.problems.push_back(name + ": " + err);
        } else if (EndsWith(name, ".dat") || EndsWith(name, ".usd")) {
            FileLooseDat(modsDir, name, report);
        }
    }

    // Pass 2: every folder is a mod.
    std::vector<Mod> found;
    for (fs::directory_iterator it(modsDir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code kind;
        if (!it->is_directory(kind)) continue;
        Mod m;
        m.id = it->path().filename().string();
        m.name = m.id;
        const std::string root = modsDir + "/" + m.id;
        std::string metaText;
        if (ReadFile(root + "/mod.json", metaText, 64 * 1024)) {
            const auto j = nlohmann::json::parse(metaText, nullptr, false);
            if (j.is_object()) {
                m.name = j.value("name", m.name);
                m.author = j.value("author", "");
                m.version = j.value("version", "");
                m.description = j.value("description", "");
            }
        }
        // ISO-extraction packs keep the game files under files/.
        const std::string base = IsDir(root + "/files") ? root + "/files" : root;
        std::map<std::string, std::string> byLower;  // lower disc path -> file, first one wins
        mkw::fsx::ForEachFile(base, [&](const std::string& path, const std::string& rel) {
            if (IsMetaFile(rel)) return;
            const std::string disc = DiscPathFor(path, rel, discFilesDir, discComplete);
            if (!byLower.emplace(Lower(disc), disc).second) return;  // two files for one slot: first wins
            m.files.push_back({disc, path});
        });
        if (m.files.empty()) continue;

        // The disc keeps a few files twice (.dat and an English .usd, e.g. PlCaRe, GrPs). A mod that
        // ships one of the pair should cover both, or it would not show in one of the languages.
        int missing = 0;
        if (discComplete) {
            const size_t original = m.files.size();
            for (size_t i = 0; i < original; ++i) {
                const std::string rel = m.files[i].discPath;
                if (EndsWith(rel, ".dat") || EndsWith(rel, ".usd")) {
                    const std::string twin = rel.substr(0, rel.size() - 4) + (EndsWith(rel, ".dat") ? ".usd" : ".dat");
                    if (!byLower.count(Lower(twin)) && IsFile(discFilesDir + "/" + twin)) {
                        m.files.push_back({twin, m.files[i].sdPath});
                        byLower.emplace(Lower(twin), twin);
                    }
                }
                if (!IsFile(discFilesDir + "/" + rel)) ++missing;
            }
        }
        std::set<std::string> seen;
        int other = 0;
        for (const auto& f : m.files) {
            const std::string label = Label(f.discPath);
            if (label.empty())
                ++other;
            else if (seen.insert(label).second)
                m.notes.push_back(label);
        }
        if (other > 0) m.notes.push_back(std::to_string(other) + (other == 1 ? " other file" : " other files"));
        if (missing > 0)
            m.warning = std::to_string(missing) + " file(s) are not on the disc and will never load.";
        found.push_back(std::move(m));
    }
    std::sort(found.begin(), found.end(), [](const Mod& a, const Mod& b) { return Lower(a.name) < Lower(b.name); });
    mods_ = std::move(found);
    return report;
}

int ModCatalog::EnabledCount() const {
    return static_cast<int>(std::count_if(mods_.begin(), mods_.end(), [](const Mod& m) { return m.enabled; }));
}

void ModCatalog::SetEnabled(const std::string& id, bool on) {
    auto it = std::find_if(mods_.begin(), mods_.end(), [&](const Mod& m) { return m.id == id; });
    if (it == mods_.end() || it->enabled == on) return;
    Mod m = std::move(*it);
    mods_.erase(it);
    m.enabled = on;
    // A newly enabled mod goes to the bottom of the enabled block; a disabled one joins the rest.
    auto pos = std::find_if(mods_.begin(), mods_.end(), [](const Mod& x) { return !x.enabled; });
    if (!on) {
        pos = std::find_if(pos, mods_.end(), [&](const Mod& x) { return Lower(x.name) > Lower(m.name); });
    }
    mods_.insert(pos, std::move(m));
}

void ModCatalog::Move(const std::string& id, int delta) {
    auto it = std::find_if(mods_.begin(), mods_.end(), [&](const Mod& m) { return m.id == id; });
    if (it == mods_.end() || !it->enabled) return;
    const long i = it - mods_.begin(), j = i + delta;
    if (j < 0 || j >= static_cast<long>(mods_.size()) || !mods_[static_cast<size_t>(j)].enabled) return;
    std::swap(mods_[static_cast<size_t>(i)], mods_[static_cast<size_t>(j)]);
}

std::vector<ModConflict> ModCatalog::Conflicts() const {
    std::vector<ModConflict> out;
    std::map<std::string, std::string> owner;  // lower disc path -> mod id
    for (const Mod& m : mods_) {
        if (!m.enabled) continue;
        for (const ModFile& f : m.files) {
            auto [it, inserted] = owner.emplace(Lower(f.discPath), m.id);
            if (!inserted && it->second != m.id) out.push_back({f.discPath, it->second, m.id});
        }
    }
    return out;
}

bool ModCatalog::SaveState(const std::string& statePath) const {
    nlohmann::json enabled = nlohmann::json::array();
    for (const Mod& m : mods_) {
        if (m.enabled) enabled.push_back(m.id);
    }
    const nlohmann::json j = {{"version", 1}, {"enabled", enabled}};
    return WriteFileAtomic(statePath, j.dump(2) + "\n");
}

void ModCatalog::LoadState(const std::string& statePath) {
    std::string text;
    if (!ReadFile(statePath, text, 256 * 1024)) return;
    const auto j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_object() || !j.contains("enabled") || !j["enabled"].is_array()) return;
    // SetEnabled appends to the enabled block, so applying the saved list in order restores it.
    for (auto& m : mods_) m.enabled = false;
    for (const auto& id : j["enabled"]) {
        if (id.is_string()) SetEnabled(id.get<std::string>(), true);
    }
}

bool ModCatalog::WriteManifest(const std::string& manifestPath, bool active, std::string* error) const {
    if (!active || EnabledCount() == 0) {
        std::remove(manifestPath.c_str());
        return true;
    }
    std::string text = "# melee-nx mod overlay v1: disc path<TAB>SD path, first entry wins\n";
    std::set<std::string> written;
    for (const Mod& m : mods_) {
        if (!m.enabled) continue;
        for (const ModFile& f : m.files) {
            if (!written.insert(Lower(f.discPath)).second) continue;
            if (f.discPath.find('\t') != std::string::npos || f.sdPath.find('\t') != std::string::npos ||
                f.discPath.find('\n') != std::string::npos || f.sdPath.find('\n') != std::string::npos)
                continue;
            text += f.discPath + "\t" + f.sdPath + "\n";
        }
    }
    if (!WriteFileAtomic(manifestPath, text)) {
        if (error) *error = "Could not write " + manifestPath;
        return false;
    }
    return true;
}

}  // namespace mnx

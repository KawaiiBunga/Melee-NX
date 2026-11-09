// The mod catalog: folders under sdmc:/switch/melee-nx/mods, which ones are on, in what order, and
// the manifest the game reads to swap those files in.
//
// A mod is a folder of replacement game files laid out like the disc ("PlFxGr.dat",
// "audio/..."), optionally inside a "files/" folder and with a mod.json for its name. A .zip in
// mods/ is unpacked into a folder of the same name. A costume DAT dropped loose into mods/ is
// identified from its own roots (dat_identity.h) and filed as a one-file mod under its real slot.
// The design follows Melee Unlocked's cosmetic catalog: the game files are never modified, an
// enabled mod only changes which file the loader serves, and turning a mod off restores vanilla.
#pragma once

#include <string>
#include <vector>

namespace mnx {

struct ModFile {
    std::string discPath;  // "PlFxGr.dat", as the game asks for it
    std::string sdPath;    // where the replacement lives
};

struct Mod {
    std::string id;  // folder name, stable across scans
    std::string name;
    std::string author;
    std::string version;
    std::string description;
    std::vector<ModFile> files;
    std::vector<std::string> notes;  // "Fox, Green" style labels for what it replaces
    std::string warning;             // shown under the mod, e.g. files the disc does not have
    bool enabled = false;
};

struct ModConflict {
    std::string discPath;
    std::string winner;  // mod id that is served
    std::string loser;   // mod id that is shadowed
};

struct ScanReport {
    int unpacked = 0;    // zips unpacked this scan
    int identified = 0;  // loose costume DATs filed into their own mod
    std::vector<std::string> problems;
};

class ModCatalog {
public:
    // `discFilesDir` is the extracted game data; when it holds a finished extraction the scan
    // flags files the disc does not have and adds .usd twins (see mods.cpp). May be empty.
    ScanReport Scan(const std::string& modsDir, const std::string& discFilesDir, bool discComplete);

    const std::vector<Mod>& Mods() const { return mods_; }
    int EnabledCount() const;
    void SetEnabled(const std::string& id, bool on);
    // Priority: the higher of two enabled mods wins a file both replace.
    void Move(const std::string& id, int delta);
    std::vector<ModConflict> Conflicts() const;

    bool SaveState(const std::string& statePath) const;
    void LoadState(const std::string& statePath);
    // Writes the game's manifest. With `active` false (or nothing enabled) the manifest is
    // removed, so the game runs vanilla. Returns false and sets `error` on a write failure.
    bool WriteManifest(const std::string& manifestPath, bool active, std::string* error) const;

    // Test hook: build the catalog from a list instead of the SD card.
    void SetForTest(std::vector<Mod> mods) { mods_ = std::move(mods); }

private:
    std::vector<Mod> mods_;  // enabled ones first, in priority order, then the rest by name
};

}  // namespace mnx

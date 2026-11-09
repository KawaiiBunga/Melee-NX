// Launcher preferences, kept in launcher.json on the SD card.
#pragma once

#include <string>

namespace mnx {

struct Settings {
    int accent = 0;            // ui::Accent
    bool modsEnabled = true;   // master switch: off boots the game with no mods at all
    bool quickLaunch = false;  // start the game straight away; hold L while starting to see the launcher
    bool returnToLauncher = true;  // the game reopens the launcher when it exits
    std::string discPath;          // the disc image chosen in Game Files; empty: pick automatically
    int mode = 0;                  // 0 Vanilla, 1 Slippi (passed to the game as launch.cfg mode=)
    std::string opponentCode;      // Slippi: the last opponent's connect code
};

Settings LoadSettings();
bool SaveSettings(const Settings& s);

// Writes `text` to `path` through a temporary file, so a crash never leaves half a file.
bool WriteFileAtomic(const std::string& path, const std::string& text);
bool ReadFile(const std::string& path, std::string& out, size_t maxBytes = 1 << 20);

}  // namespace mnx

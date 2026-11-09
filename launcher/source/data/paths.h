// Every SD card location the launcher and the game agree on. The game side lives in
// switch/src/game_data/game_data_gate.h (kDataRoot, kFilesDir) and switch/src/main_switch.cpp
// (launch.cfg); keep the two in step. docs/LAUNCHER.md lists them for users.
#pragma once

namespace mnx::paths {

inline constexpr const char* kRoot = "sdmc:/switch/melee-nx";
inline constexpr const char* kFilesDir = "sdmc:/switch/melee-nx/files";
inline constexpr const char* kDiscMeta = "sdmc:/switch/melee-nx/disc.meta";
inline constexpr const char* kDiscBoot = "sdmc:/switch/melee-nx/disc-boot.bin";

// Mods: one folder (or .zip, unpacked on the next scan) per mod.
inline constexpr const char* kModsDir = "sdmc:/switch/melee-nx/mods";
// Which mods are on, in priority order. Written by the launcher only.
inline constexpr const char* kModsState = "sdmc:/switch/melee-nx/mods/mods.json";
// "disc path<TAB>SD path" for every file the enabled mods replace. Read by the game.
inline constexpr const char* kModsManifest = "sdmc:/switch/melee-nx/mods/active.txt";

// Slippi: the account file the Slippi Launcher writes on a PC, copied here.
inline constexpr const char* kSlippiDir = "sdmc:/switch/melee-nx/slippi";
inline constexpr const char* kSlippiUser = "sdmc:/switch/melee-nx/slippi/user.json";

inline constexpr const char* kSettings = "sdmc:/switch/melee-nx/launcher.json";
// Handoff to the game: key=value lines, read by switch/src/main_switch.cpp.
inline constexpr const char* kLaunchCfg = "sdmc:/switch/melee-nx/launch.cfg";

// Where the game NRO may live, most specific first. bin/ keeps it out of the Homebrew Menu.
inline constexpr const char* kGameNroCandidates[] = {
    "sdmc:/switch/melee-nx/bin/melee.nro",
    "sdmc:/switch/melee-nx/melee.nro",
    "sdmc:/switch/melee.nro",
};

inline constexpr const char* kLauncherNroDefault = "sdmc:/switch/melee-nx/melee-nx-launcher.nro";

}  // namespace mnx::paths

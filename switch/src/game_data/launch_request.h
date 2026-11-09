/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The Melee NX launcher's handoff. The launcher (launcher/source/data/launch.cpp) writes
 * launch.cfg and starts melee.nro with "--launch"; only then is the file read. A direct start
 * from the Homebrew Menu ignores it and uses the console gate as before. */
#pragma once

#include <string>

namespace melee_nx {

inline constexpr const char* kLaunchCfg = "sdmc:/switch/melee-nx/launch.cfg";

struct LaunchRequest {
    bool present = false;  // started by the launcher with a readable launch.cfg
    std::string disc;      // disc image path or kLooseBootSentinel
    std::string mods;      // mod overlay manifest, or empty for none
    std::string returnTo;  // launcher NRO to reopen on exit, or empty
    std::string mode;      // "slippi" or "vanilla" (empty: vanilla)
    std::string code;      // Slippi: the opponent's connect code (Direct mode)
};

bool HasLaunchFlag(int argc, char** argv);
/* Parses launch.cfg text: key=value lines, '#' comments, unknown keys ignored. */
LaunchRequest ParseLaunchConfig(const std::string& text);
/* Reads launch.cfg when argv carries --launch; otherwise returns present=false. */
LaunchRequest ReadLaunchRequest(int argc, char** argv);
/* True when `disc` is bootable: the loose-boot sentinel with a completed extraction and its
 * captured metadata, or a disc image that validates as Melee NTSC-U 1.02. */
bool ValidateLaunchDisc(const std::string& disc, std::string& reason);

} // namespace melee_nx

// Starting the game. The launcher writes launch.cfg (what to boot, which mods, where to come back
// to) and asks the Homebrew loader to run melee.nro next with "--launch"; then the launcher exits.
// The game reads launch.cfg only when it gets that flag, so starting melee.nro directly from the
// Homebrew Menu still works exactly as before.
#pragma once

#include <string>

namespace mnx::launch {

// The first game NRO that exists (paths::kGameNroCandidates), or empty.
std::string FindGameNro();

struct Request {
    std::string bootArg;       // disc image path or the loose-boot sentinel
    std::string modsManifest;  // empty: no mods
    std::string returnTo;      // the launcher NRO to reopen when the game exits; empty: none
    std::string mode = "vanilla";  // "vanilla" or "slippi"
    std::string code;              // Slippi: the opponent's connect code (Direct mode)
};

// A Slippi connect code as typed ("abcd#123") to its canonical form ("ABCD#123"): up to eight
// letters or digits, '#', up to three digits. False when it is not one.
bool NormalizeConnectCode(std::string& code);

// Renders launch.cfg's text. Values never contain newlines (they are SD paths).
std::string FormatConfig(const Request& r);

// Writes launch.cfg and queues the game. On success the caller must exit the launcher.
bool Start(const std::string& gameNro, const Request& r, std::string& error);

void SetSelfPath(const char* argv0);
const std::string& SelfPath();

}  // namespace mnx::launch

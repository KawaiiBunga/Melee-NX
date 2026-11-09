// Game data: finding the user's Melee disc image, extracting it to the SD card, and deciding what
// the game boots from. Uses the same extractor and the same rules as the game's own console gate
// (switch/src/game_data/game_data_gate.cpp), so the launcher and a direct launch agree.
#pragma once

#include <string>
#include <vector>

#include "data/job.h"

namespace mnx::gamedata {

struct DiscCandidate {
    std::string path;
    bool valid = false;
    std::string reason;  // why an image was rejected
};

// Looks for .iso/.gcm images in the melee-nx folder, the SD root and common rom folders.
std::vector<DiscCandidate> FindDiscs();

struct Status {
    std::string discPath;   // a valid disc image, or empty
    bool extracted = false; // files/ holds a finished extraction
    bool looseReady = false; // extracted plus the metadata needed to boot with no disc image
    bool CanPlay() const { return !discPath.empty() || looseReady; }
    // The argument the game is started with: the disc image, or the loose-boot sentinel.
    std::string BootArg() const;
};

// `preferred` is the saved choice; it is used when it still validates.
Status Probe(const std::string& preferred);

// Extracts `disc` into files/. Runs on a job thread; honours cancel at file boundaries.
Job::Result Extract(const std::string& disc, Job::Reporter& r);

}  // namespace mnx::gamedata

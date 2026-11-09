// Identifies which costume slot a fighter costume DAT belongs to from the DAT's own root symbols
// (for example PlyFox5KGr_Share_joint -> PlFxGr.dat, Fox green), never from its file name.
//
// Adapted from Melee Unlocked's cosmetic importer (port/runtime/host/cosmetic_mods.cpp,
// https://github.com/Hero88go/melee-unlocked, GPL-2.0-or-later). The fighter table and the root
// symbol rule are theirs; the parsing here is a smaller rewrite for the Switch launcher.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mnx::dat {

struct Identity {
    bool ok = false;
    std::string error;       // why it was not identified
    std::string targetFile;  // "PlFxGr.dat"
    std::string fighter;     // "Fox"
    std::string costume;     // "Green"
    std::vector<std::string> roots;
};

// Parses an HSD archive and maps its _Share_joint root to an NTSC 1.02 costume slot.
Identity IdentifyCostume(const std::vector<std::uint8_t>& bytes);

// "PlFxGr.dat" -> "Fox, Green"; empty if the name is not a stock costume file.
std::string DescribeCostumeFile(const std::string& fileName);

}  // namespace mnx::dat

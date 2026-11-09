// SPDX-License-Identifier: GPL-3.0-or-later
// Fighter table and root rule from Melee Unlocked (GPL-2.0-or-later); see dat_identity.h.
#include "data/dat_identity.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace mnx::dat {
namespace {

struct Family {
    const char* fileCode;
    const char* rootName;
    const char* displayName;
    const char* colors;  // costume order, space separated
};

// Existing NTSC 1.02 costume files only. The default costume's root has no colour suffix even
// though its file name ends in Nr.
constexpr Family kFamilies[] = {
    {"Ca", "Captain", "Captain Falcon", "Nr Gy Re Wh Gr Bu"},
    {"Cl", "Clink", "Young Link", "Nr Re Bu Wh Bk"},
    {"Dk", "Donkey", "Donkey Kong", "Nr Bk Re Bu Gr"},
    {"Dr", "Drmario", "Dr. Mario", "Nr Re Bu Gr Bk"},
    {"Fc", "Falco", "Falco", "Nr Re Bu Gr"},
    {"Fe", "Emblem", "Roy", "Nr Re Bu Gr Ye"},
    {"Fx", "Fox", "Fox", "Nr Or La Gr"},
    {"Gn", "Ganon", "Ganondorf", "Nr Re Bu Gr La"},
    {"Gw", "Gamewatch", "Mr. Game & Watch", "Nr"},
    {"Kb", "Kirby", "Kirby", "Nr Ye Bu Re Gr Wh"},
    {"Kp", "Koopa", "Bowser", "Nr Re Bu Bk"},
    {"Lg", "Luigi", "Luigi", "Nr Wh Aq Pi"},
    {"Lk", "Link", "Link", "Nr Re Bu Bk Wh"},
    {"Mr", "Mario", "Mario", "Nr Ye Bk Bu Gr"},
    {"Ms", "Mars", "Marth", "Nr Re Gr Bk Wh"},
    {"Mt", "Mewtwo", "Mewtwo", "Nr Re Bu Gr"},
    {"Nn", "Nana", "Ice Climbers (Nana)", "Nr Ye Aq Wh"},
    {"Ns", "Ness", "Ness", "Nr Ye Bu Gr"},
    {"Pc", "Pichu", "Pichu", "Nr Re Bu Gr"},
    {"Pe", "Peach", "Peach", "Nr Ye Wh Bu Gr"},
    {"Pk", "Pikachu", "Pikachu", "Nr Re Bu Gr"},
    {"Pp", "Popo", "Ice Climbers (Popo)", "Nr Gr Or Re"},
    {"Pr", "Purin", "Jigglypuff", "Nr Re Bu Gr Ye"},
    {"Sk", "Seak", "Sheik", "Nr Re Bu Gr Wh"},
    {"Ss", "Samus", "Samus", "Nr Pi Bk Gr La"},
    {"Ys", "Yoshi", "Yoshi", "Nr Re Bu Ye Pi Aq"},
    {"Zd", "Zelda", "Zelda", "Nr Re Bu Gr Wh"},
};

struct ColorName {
    const char* code;
    const char* label;
};
constexpr ColorName kColors[] = {
    {"Nr", "Default"}, {"Re", "Red"},   {"Bu", "Blue"},   {"Gr", "Green"},    {"Wh", "White"}, {"Bk", "Black"},
    {"Ye", "Yellow"},  {"Or", "Orange"}, {"La", "Lavender"}, {"Pi", "Pink"}, {"Aq", "Aqua"},  {"Gy", "Gray"},
};

bool HasColor(const Family& f, const char* code) {
    return (std::string(" ") + f.colors + " ").find(std::string(" ") + code + " ") != std::string::npos;
}

std::uint32_t Be32(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}

}  // namespace

Identity IdentifyCostume(const std::vector<std::uint8_t>& bytes) {
    Identity id;
    if (bytes.size() < 0x20) {
        id.error = "Not a DAT archive (too small).";
        return id;
    }
    const std::uint64_t fileSize = Be32(&bytes[0]), dataSize = Be32(&bytes[4]), relocs = Be32(&bytes[8]),
                        roots = Be32(&bytes[12]), refs = Be32(&bytes[16]);
    if (fileSize != bytes.size()) {
        id.error = "The DAT's length field does not match the file.";
        return id;
    }
    if (roots == 0 || roots > 1024 || refs > 1024) {
        id.error = "The DAT root table is not plausible.";
        return id;
    }
    const std::uint64_t rootTable = 0x20 + dataSize + relocs * 4;
    const std::uint64_t strings = rootTable + (roots + refs) * 8;
    if (strings > bytes.size()) {
        id.error = "The DAT tables point outside the file.";
        return id;
    }
    for (std::uint64_t i = 0; i < roots; ++i) {
        const std::uint64_t rec = rootTable + i * 8;
        const std::uint64_t objectOffset = Be32(&bytes[rec]);
        const std::uint64_t nameOffset = Be32(&bytes[rec + 4]);
        if (objectOffset >= dataSize || strings + nameOffset >= bytes.size()) {
            id.error = "A DAT root points outside its data or string table.";
            return id;
        }
        const size_t begin = static_cast<size_t>(strings + nameOffset);
        size_t end = begin;
        while (end < bytes.size() && bytes[end]) {
            if (bytes[end] < 0x20 || bytes[end] > 0x7e || end - begin > 255) {
                id.error = "A DAT root name is not printable text.";
                return id;
            }
            ++end;
        }
        if (end == bytes.size()) {
            id.error = "A DAT root name is not terminated.";
            return id;
        }
        id.roots.emplace_back(reinterpret_cast<const char*>(&bytes[begin]), end - begin);
    }

    const Family* family = nullptr;
    const ColorName* color = nullptr;
    int matches = 0;
    for (const auto& f : kFamilies) {
        for (const auto& c : kColors) {
            if (!HasColor(f, c.code)) continue;
            const std::string want = std::string("Ply") + f.rootName + "5K" +
                                     (std::strcmp(c.code, "Nr") ? c.code : "") + "_Share_joint";
            if (std::find(id.roots.begin(), id.roots.end(), want) != id.roots.end()) {
                family = &f;
                color = &c;
                ++matches;
            }
        }
    }
    if (matches > 1) {
        id.error = "The DAT names more than one costume slot.";
        return id;
    }
    if (matches == 0) {
        id.error = "Not a costume of an existing fighter colour.";
        return id;
    }
    id.ok = true;
    id.fighter = family->displayName;
    id.costume = color->label;
    id.targetFile = std::string("Pl") + family->fileCode + color->code + ".dat";
    return id;
}

std::string DescribeCostumeFile(const std::string& fileName) {
    // PlXxYy.dat (or .usd, which the disc keeps for a few English-language costumes)
    if (fileName.size() != 10 || fileName[0] != 'P' || fileName[1] != 'l') return {};
    std::string ext = fileName.substr(6);
    for (char& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (ext != ".dat" && ext != ".usd") return {};
    const std::string code = fileName.substr(2, 2), col = fileName.substr(4, 2);
    for (const auto& f : kFamilies) {
        if (code != f.fileCode || !HasColor(f, col.c_str())) continue;
        for (const auto& c : kColors) {
            if (col == c.code) return std::string(f.displayName) + ", " + c.label;
        }
    }
    return {};
}

}  // namespace mnx::dat

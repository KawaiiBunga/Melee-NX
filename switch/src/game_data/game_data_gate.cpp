/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_data_gate.h"
#include "disc_extractor.h"

#include <switch.h>

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace melee_nx {
namespace {

namespace fs = std::filesystem;

PadState g_pad;
bool g_padReady = false;

void ensurePad() {
    if (g_padReady) {
        return;
    }
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);
    g_padReady = true;
}

bool hasExtension(const fs::path& p, const char* ext) {
    std::string have = p.extension().string();
    for (char& c : have) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return have == ext;
}

/* Scans `dir` (non-recursive) for the first .iso/.gcm that validates as
 * Melee NTSC-U 1.02. Returns "" if none found; `outReason` gets the last
 * rejection reason seen (useful when a disc image exists but is the wrong
 * game/revision). */
std::string findCandidateDisc(const fs::path& dir, std::string& outReason) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        return "";
    }
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file()) {
            continue;
        }
        const fs::path& p = entry.path();
        if (!hasExtension(p, ".iso") && !hasExtension(p, ".gcm")) {
            continue;
        }
        std::string reason;
        if (melee_nx::disc::looksLikeMeleeDisc(p.string(), reason)) {
            return p.string();
        }
        outReason = reason;
    }
    return "";
}

void clearScreen() { std::printf("\x1b[2J\x1b[1;1H"); }

void waitForAnyExit() {
    ensurePad();
    while (appletMainLoop()) {
        padUpdate(&g_pad);
        if (padGetButtonsDown(&g_pad) != 0) {
            return;
        }
        consoleUpdate(nullptr);
    }
}

bool g_extractCanceled = false;

bool onExtractProgress(const melee_nx::disc::ExtractProgress& p, void*) {
    if (!appletMainLoop()) {
        g_extractCanceled = true;
        return false;
    }
    ensurePad();
    padUpdate(&g_pad);
    if (padGetButtonsDown(&g_pad) & HidNpadButton_Plus) {
        g_extractCanceled = true;
    }

    static u64 lastDraw = 0;
    const u64 now = armGetSystemTick();
    if (lastDraw != 0 && armTicksToNs(now - lastDraw) < 250000000ULL) {
        return !g_extractCanceled;
    }
    lastDraw = now;

    clearScreen();
    std::printf("melee-nx -- Extracting game data\n\n%s\n", p.stage);
    if (p.filesTotal != 0) {
        std::printf("Files: %llu / %llu\n", (unsigned long long)p.filesDone,
                    (unsigned long long)p.filesTotal);
    }
    if (p.bytesTotal != 0) {
        std::printf("Data:  %llu / %llu MiB\n", (unsigned long long)(p.bytesDone >> 20),
                    (unsigned long long)(p.bytesTotal >> 20));
    }
    std::printf("\n+ cancels safely at the next file boundary.\n");
    consoleUpdate(nullptr);
    return !g_extractCanceled;
}

} // namespace

std::string EnsureGameDataAvailable(const std::string& nroDir) {
    std::error_code ec;
    fs::create_directories(kDataRoot, ec);

    clearScreen();
    std::printf("melee-nx -- looking for your Melee disc image...\n");
    consoleUpdate(nullptr);

    std::string rejectReason;
    std::string discPath = findCandidateDisc(nroDir, rejectReason);
    if (discPath.empty() && nroDir != kDataRoot) {
        discPath = findCandidateDisc(kDataRoot, rejectReason);
    }

    if (discPath.empty() && melee_nx::disc::isExtractionComplete(kFilesDir)) {
        // No disc image, but a completed extraction plus the metadata captured
        // on a prior disc boot can boot on their own. Both files are written
        // automatically the first time the game runs with the disc present.
        const std::string meta = std::string(kDataRoot) + "/disc.meta";
        const std::string boot = std::string(kDataRoot) + "/disc-boot.bin";
        std::error_code metaEc;
        if (fs::is_regular_file(meta, metaEc) && fs::is_regular_file(boot, metaEc)) {
            clearScreen();
            std::printf("melee-nx -- booting from extracted game data.\n"
                        "No disc image needed.\n");
            consoleUpdate(nullptr);
            return kLooseBootSentinel;
        }
    }

    if (discPath.empty()) {
        clearScreen();
        std::printf("melee-nx -- game data not found\n\n");
        if (!rejectReason.empty()) {
            std::printf("Found a disc image, but: %s\n\n", rejectReason.c_str());
        }
        std::printf("Place your Super Smash Bros. Melee disc image\n"
                    "(NTSC-U, revision 2 / v1.02, GALE01, .iso or .gcm)\n"
                    "next to melee.nro, or in:\n%s\n\n"
                    "Then relaunch. Press any button to exit.\n",
                    kDataRoot);
        consoleUpdate(nullptr);
        waitForAnyExit();
        return "";
    }

    if (melee_nx::disc::isExtractionComplete(kFilesDir)) {
        return discPath;
    }

    clearScreen();
    std::printf("melee-nx -- found disc image:\n%s\n\n", discPath.c_str());
    std::printf("Extracting game data to the SD card speeds up loading a lot.\n\n");
    std::printf("Press A to extract now (one-time, several minutes).\n");
    std::printf("Press B to play directly from the disc image instead.\n");
    std::printf("Press + to exit.\n");
    consoleUpdate(nullptr);

    ensurePad();
    int choice = 0; /* 0 = pending, 1 = extract, 2 = play from disc, -1 = exit */
    while (appletMainLoop() && choice == 0) {
        padUpdate(&g_pad);
        const u64 down = padGetButtonsDown(&g_pad);
        if (down & HidNpadButton_Plus) {
            choice = -1;
        } else if (down & HidNpadButton_A) {
            choice = 1;
        } else if (down & HidNpadButton_B) {
            choice = 2;
        }
        consoleUpdate(nullptr);
    }
    if (choice != 1) {
        return (choice == 2) ? discPath : "";
    }

    g_extractCanceled = false;
    melee_nx::disc::ExtractOptions options;
    options.progress = onExtractProgress;

    melee_nx::disc::ExtractResult result =
        melee_nx::disc::extractGameFiles(discPath, kFilesDir, options);

    clearScreen();
    if (result.ok) {
        std::printf("melee-nx -- extraction complete!\n\n");
        std::printf("%llu files, %llu MiB written to:\n%s\n\n",
                    (unsigned long long)result.filesExtracted,
                    (unsigned long long)(result.bytesExtracted >> 20), kFilesDir);
    } else {
        std::printf("melee-nx -- extraction stopped:\n%s\n\n", result.message.c_str());
        std::printf("Continuing directly from the disc image instead.\n\n");
    }
    std::printf("Press A to start the game.\n");
    consoleUpdate(nullptr);
    while (appletMainLoop()) {
        padUpdate(&g_pad);
        if (padGetButtonsDown(&g_pad) & HidNpadButton_A) {
            break;
        }
        consoleUpdate(nullptr);
    }
    return discPath;
}

} // namespace melee_nx

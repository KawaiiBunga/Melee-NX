/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Boot-time "do we have game data" gate, shown as a libnx text console
 * before Aurora/Dawn ever touch the framebuffer. Mirrors the flow (and the
 * completion-manifest trick) of KartPad-NX's KartPadMobileEnsureGameDataAvailable
 * in runtime_host_switch.cpp, minus the Wii decryption step GameCube doesn't
 * need -- built directly into melee-nx's own main.nro rather than as a
 * separate extractor tool, per the request that started this file.
 */
#pragma once

#include <string>

namespace melee_nx {

/* Root of melee-nx's data on the SD card. */
inline constexpr const char* kDataRoot = "sdmc:/switch/melee-nx";
/* Where melee-pc's own src/pc/file_cache.cpp looks for loose files (its
 * "./files" / MELEE_FILES_DIR convention -- see docs/PORTING-NOTES.md). */
inline constexpr const char* kFilesDir = "sdmc:/switch/melee-nx/files";

/* melee-pc always needs an actual disc image open (aurora_dvd_open(), font
 * extraction) even when every archive is served from the kFilesDir loose-file
 * cache -- that cache only skips the slower disc-seek path per file, it is
 * never a full replacement for the source image. So this always searches for
 * the disc image itself:
 *   1. Searches `nroDir` and kDataRoot for a Melee NTSC-U 1.02 (GALE01) disc
 *      image (.iso/.gcm). None found -> tells the user to place one next to
 *      melee.nro, waits for a button, and returns "".
 *   2. If kFilesDir is not yet a completed extraction, asks the user
 *      (A = extract now, B = play directly from the disc image, + = exit)
 *      and, on A, extracts into kFilesDir with an on-screen progress display.
 *      A canceled/failed extraction still falls through to "play from disc".
 *   3. Returns the disc image path to open (never empty on success) -- pass
 *      this straight through as the command-line disc argument.
 * Call after consoleInit() and before Aurora/Dawn initialize; call
 * consoleExit() once this returns, whether or not it succeeded. */
std::string EnsureGameDataAvailable(const std::string& nroDir);

}  // namespace melee_nx

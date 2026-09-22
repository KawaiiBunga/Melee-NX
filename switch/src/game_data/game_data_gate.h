/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Select or extract game data using the libnx console before graphics startup. */
#pragma once

#include <string>

namespace melee_nx {

/* Root of melee-nx's data on the SD card. */
inline constexpr const char* kDataRoot = "sdmc:/switch/melee-nx";
/* Loose-file cache selected through MELEE_FILES_DIR. */
inline constexpr const char* kFilesDir = "sdmc:/switch/melee-nx/files";

/* Sentinel disc argument returned when the source disc image is gone but a
 * completed extraction plus captured disc.meta/disc-boot.bin can boot on their
 * own. melee-pc's launcher (src/pc/launcher.cpp) recognizes the same literal. */
inline constexpr const char* kLooseBootSentinel = "@melee-nx-loose";

/* Return a validated disc path, kLooseBootSentinel for a completed extraction
 * with disc.meta and disc-boot.bin, or an empty string to exit. When needed,
 * offer extraction or direct disc play; failed extraction falls back to disc.
 * Call after consoleInit() and release the console before starting Aurora. */
std::string EnsureGameDataAvailable(const std::string& nroDir);

} // namespace melee_nx

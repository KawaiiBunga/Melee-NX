// SPDX-License-Identifier: GPL-3.0-or-later
// Replaces src/pc/updater.cpp on Switch (excluded in switch/CMakeLists.txt).
//
// melee-pc's auto-updater (GitHub releases API + JSON parsing + self-replace)
// makes no sense for homebrew: there's no background process to relaunch
// into, and users get new builds by replacing melee.nro themselves. It's also
// the one PC source file that doesn't compile clean here -- its recursive
// JsonValue (a vector<pair<string, JsonValue>> member) trips a completeness
// static_assert in devkitA64's libstdc++ 15 that other platforms' standard
// libraries tolerate. Rather than rework upstream's JSON type for a feature
// we don't want, launcher.cpp's pc::updater:: calls (never reached in
// melee-nx's flow -- see main_switch.cpp/game_data_gate, which always hands
// main() a resolved disc path, skipping the RmlUi launcher UI entirely) are
// satisfied by this permanently-up-to-date stub instead.
#include "pc/updater.hpp"

namespace pc::updater {

void check_for_updates_async(bool /*include_prereleases*/) {}
void start_download_async() {}
void cancel() {}

UpdateState get_state() {
    UpdateState state;
    state.status = Status::UpToDate;
    return state;
}

void open_release_in_browser() {}
void open_downloaded_location() {}

bool apply_update_and_restart(std::string& error) {
    error = "Updates are not supported on Switch.";
    return false;
}

}  // namespace pc::updater

// SPDX-License-Identifier: GPL-3.0-or-later
// Switch updates are installed by replacing the NRO. This implements the
// launcher updater interface without the desktop updater dependency.
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

} // namespace pc::updater

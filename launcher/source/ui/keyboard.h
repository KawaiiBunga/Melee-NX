// The system software keyboard (libnx swkbd) for short text fields such as Mii and license names.
#pragma once

#include <string>

namespace ui {

// Shows the keyboard. Returns true and fills `out` when the user confirmed a non-empty value.
bool PromptText(const std::string& header, const std::string& initial, int maxChars, std::string& out);

}  // namespace ui

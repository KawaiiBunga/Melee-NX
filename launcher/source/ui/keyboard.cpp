#include "ui/keyboard.h"

#include <switch.h>

namespace ui {

bool PromptText(const std::string& header, const std::string& initial, int maxChars, std::string& out) {
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0))) return false;
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetHeaderText(&kbd, header.c_str());
    swkbdConfigSetInitialText(&kbd, initial.c_str());
    swkbdConfigSetStringLenMax(&kbd, static_cast<u32>(maxChars));
    swkbdConfigSetStringLenMin(&kbd, 1);
    char buf[64] = {};
    const Result rc = swkbdShow(&kbd, buf, sizeof buf);
    swkbdClose(&kbd);
    if (R_FAILED(rc) || buf[0] == '\0') return false;
    out = buf;
    return true;
}

}  // namespace ui

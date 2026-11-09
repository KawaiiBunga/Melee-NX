#include "ui/theme.h"

namespace ui {

Theme MakeTheme(Accent accent) {
    Color acc{0xB6, 0xF2, 0x3D, 255};
    Color hi{0xD9, 0xFF, 0x8A, 255};
    switch (accent) {
        case Accent::Lime:    acc = {0xB6, 0xF2, 0x3D, 255}; hi = {0xD9, 0xFF, 0x8A, 255}; break;
        case Accent::Violet:  acc = {0xA3, 0x8B, 0xFF, 255}; hi = {0xCB, 0xBE, 0xFF, 255}; break;
        case Accent::Amber:   acc = {0xFF, 0xB8, 0x30, 255}; hi = {0xFF, 0xD7, 0x80, 255}; break;
        case Accent::Crimson: acc = {0xFF, 0x5A, 0x6E, 255}; hi = {0xFF, 0x9A, 0xA6, 255}; break;
        case Accent::Cyan:    acc = {0x3D, 0xD8, 0xF2, 255}; hi = {0x9A, 0xEE, 0xFF, 255}; break;
    }
    Theme t{};
    t.bg = {0x1B, 0x16, 0x3D, 255};
    t.bgLow = {0x0C, 0x0A, 0x1E, 255};
    t.rail = {0x12, 0x0F, 0x2A, 255};
    t.panel = {0x26, 0x20, 0x50, 255};
    t.panelHi = {0x34, 0x2C, 0x68, 255};
    t.panelHead = Mix(acc, t.panel, 0.7f);
    t.edge = {0x3E, 0x36, 0x75, 255};
    t.text = {0xF3, 0xF1, 0xFF, 255};
    t.textDim = {0xB4, 0xAD, 0xD9, 255};
    t.textFaint = {0x75, 0x6E, 0xA3, 255};
    t.accent = acc;
    t.accentHi = hi;
    t.onAccent = {0x12, 0x0F, 0x2A, 255};
    t.good = {0x6B, 0xE3, 0x8E, 255};
    t.warn = {0xFF, 0xC2, 0x4D, 255};
    t.bad = {0xFF, 0x6B, 0x7A, 255};
    return t;
}

const char* AccentName(Accent accent) {
    switch (accent) {
        case Accent::Lime:    return "Lime";
        case Accent::Violet:  return "Violet";
        case Accent::Amber:   return "Amber";
        case Accent::Crimson: return "Crimson";
        case Accent::Cyan:    return "Cyan";
    }
    return "Lime";
}

Accent AccentFromIndex(int index) {
    if (index < 0 || index >= kAccentCount) return Accent::Lime;
    return static_cast<Accent>(index);
}

}  // namespace ui

// Theme tokens: Melee NX's own look. A deep indigo night with one bright, user-selectable accent.
#pragma once

#include <cstdint>

#include "ui/geometry.h"

namespace ui {

enum class Accent : std::uint8_t { Lime = 0, Violet = 1, Amber = 2, Crimson = 3, Cyan = 4 };
inline constexpr int kAccentCount = 5;

struct Theme {
    Color bg;         // page background (top of the gradient)
    Color bgLow;      // page background (bottom of the gradient)
    Color rail;       // the navigation rail on the left
    Color panel;      // card body
    Color panelHi;    // focused card body
    Color edge;       // hairline borders
    Color text;       // primary text
    Color textDim;    // secondary text
    Color textFaint;  // hints, disabled labels
    Color accent;     // the one accent colour
    Color accentHi;   // lighter end of the accent gradient
    Color onAccent;   // text drawn on the accent
    Color panelHead;  // header strip of panels
    Color good;
    Color warn;
    Color bad;
};

Theme MakeTheme(Accent accent);
const char* AccentName(Accent accent);
Accent AccentFromIndex(int index);

}  // namespace ui

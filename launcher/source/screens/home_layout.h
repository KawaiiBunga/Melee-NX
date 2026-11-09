// Layout of the home screen in 1280x720 logical units, shared by its page files.
#pragma once

#include "ui/geometry.h"

namespace screens::layout {

inline constexpr float kRailW = 264.0f;
inline constexpr float kContentX = 300.0f;
inline constexpr float kContentW = 940.0f;
inline constexpr float kHeaderY = 34.0f;
inline constexpr float kBodyY = 112.0f;
inline constexpr float kHintH = 48.0f;
inline constexpr float kRowH = 60.0f;
inline constexpr float kRowGap = 12.0f;

// The n-th full-width row of the page body.
inline constexpr ui::Rect Row(int n, float h = kRowH) {
    return {kContentX, kBodyY + n * (kRowH + kRowGap), kContentW, h};
}

}  // namespace screens::layout

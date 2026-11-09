// Basic 2D types in 1280x720 logical units.
#pragma once

#include <cstdint>

namespace ui {

inline constexpr float kLogicalW = 1280.0f;
inline constexpr float kLogicalH = 720.0f;

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

struct Rect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;

    constexpr Vec2 Center() const { return {x + w * 0.5f, y + h * 0.5f}; }
    constexpr bool Contains(float px, float py) const {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
    constexpr Rect Inset(float d) const { return {x + d, y + d, w - 2 * d, h - 2 * d}; }
};

struct Color {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 255;
};

constexpr Color WithAlpha(Color c, std::uint8_t a) { return {c.r, c.g, c.b, a}; }

// Mixes two colours; t = 0 gives a, t = 1 gives b.
constexpr Color Mix(Color a, Color b, float t) {
    auto lerp = [t](std::uint8_t p, std::uint8_t q) {
        return static_cast<std::uint8_t>(p + (q - p) * t + 0.5f);
    };
    return {lerp(a.r, b.r), lerp(a.g, b.g), lerp(a.b, b.b), lerp(a.a, b.a)};
}

}  // namespace ui

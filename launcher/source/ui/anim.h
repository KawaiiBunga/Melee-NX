// Easing and smoothing helpers. All times are in seconds.
#pragma once

#include <cmath>

namespace ui {

inline float Clamp01(float t) { return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t); }

inline float EaseOutCubic(float t) {
    const float u = 1.0f - Clamp01(t);
    return 1.0f - u * u * u;
}

inline float EaseInOutSine(float t) {
    return 0.5f - 0.5f * std::cos(3.14159265f * Clamp01(t));
}

// Frame-rate independent exponential approach: moves `current` toward `target`.
// `rate` is roughly the reciprocal of the time constant in seconds (higher = snappier).
inline float Approach(float current, float target, float dt, float rate) {
    const float k = 1.0f - std::exp(-rate * dt);
    return current + (target - current) * k;
}

}  // namespace ui

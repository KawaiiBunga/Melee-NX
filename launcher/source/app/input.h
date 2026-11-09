// Per-frame input snapshot, built from libnx pad and touch state.
#pragma once

#include <cstdint>

namespace app {

enum class Dir : std::uint8_t { None, Up, Down, Left, Right };

struct Input {
    // Edge-triggered (pressed this frame) button actions.
    bool confirm = false;   // A
    bool back = false;      // B
    bool tabPrev = false;   // L
    bool tabNext = false;   // R
    bool plusHeld = false;  // held Plus, for the quick-launch gesture
    bool actionX = false;   // X: secondary action on the focused item
    bool actionY = false;   // Y: tertiary action on the focused item
    Dir nav = Dir::None;    // d-pad or left stick, with auto-repeat

    // Touch, in logical 1280x720 coordinates.
    bool touchDown = false;   // finger went down this frame
    bool touchUp = false;     // finger lifted this frame
    bool touching = false;    // finger is down now
    float touchX = 0.0f;
    float touchY = 0.0f;
};

// Reads libnx input once per frame and turns it into an Input. Holds repeat timers across frames.
class InputReader {
public:
    void Init();
    void Read(float dt, Input& out);

private:
    Dir lastDir_ = Dir::None;
    float repeatTimer_ = 0.0f;
    bool touchWasDown_ = false;
    float lastTouchX_ = 0.0f;
    float lastTouchY_ = 0.0f;
    bool initialised_ = false;
};

// True when L is held right now; read once at startup for quick launch.
bool StartupHoldL();

}  // namespace app

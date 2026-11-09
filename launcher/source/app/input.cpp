#include "app/input.h"

#include <switch.h>


namespace app {
namespace {

constexpr float kRepeatDelay = 0.40f;     // hold time before repeat starts
constexpr float kRepeatInterval = 0.09f;  // repeat cadence
constexpr int kStickThreshold = 16000;    // out of 32767

}  // namespace

void InputReader::Init() {
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    hidInitializeTouchScreen();
    initialised_ = true;
}

void InputReader::Read(float dt, Input& out) {
    out = Input{};
    if (!initialised_) return;

    // libnx keeps one PadState per player; a static is fine for the single-player launcher.
    static PadState pad;
    static bool padReady = false;
    if (!padReady) {
        padInitializeDefault(&pad);
        padReady = true;
    }
    padUpdate(&pad);
    const u64 down = padGetButtonsDown(&pad);
    const u64 held = padGetButtons(&pad);

    out.confirm = down & HidNpadButton_A;
    out.back = down & HidNpadButton_B;
    out.tabPrev = down & HidNpadButton_L;
    out.tabNext = down & HidNpadButton_R;
    out.plusHeld = held & HidNpadButton_Plus;
    out.actionX = down & HidNpadButton_X;
    out.actionY = down & HidNpadButton_Y;

    // Direction: d-pad or left stick. Fire once on the edge, then repeat while held.
    const HidAnalogStickState stick = padGetStickPos(&pad, 0);
    Dir dir = Dir::None;
    if (held & HidNpadButton_Up || stick.y > kStickThreshold) dir = Dir::Up;
    else if (held & HidNpadButton_Down || stick.y < -kStickThreshold) dir = Dir::Down;
    else if (held & HidNpadButton_Left || stick.x < -kStickThreshold) dir = Dir::Left;
    else if (held & HidNpadButton_Right || stick.x > kStickThreshold) dir = Dir::Right;

    if (dir == Dir::None) {
        lastDir_ = Dir::None;
        repeatTimer_ = 0.0f;
    } else if (dir != lastDir_) {
        lastDir_ = dir;
        repeatTimer_ = kRepeatDelay;
        out.nav = dir;
    } else {
        repeatTimer_ -= dt;
        if (repeatTimer_ <= 0.0f) {
            repeatTimer_ += kRepeatInterval;
            out.nav = dir;
        }
    }

    // Touch: the first touch point drives the UI. Coordinates are already 1280x720.
    HidTouchScreenState touch{};
    const size_t count = hidGetTouchScreenStates(&touch, 1);
    const bool down_now = count > 0 && touch.count > 0;
    if (down_now) {
        lastTouchX_ = static_cast<float>(touch.touches[0].x);
        lastTouchY_ = static_cast<float>(touch.touches[0].y);
    }
    // Lift events carry the last position, so a release can be hit-tested.
    out.touchX = lastTouchX_;
    out.touchY = lastTouchY_;
    out.touching = down_now;
    out.touchDown = down_now && !touchWasDown_;
    out.touchUp = !down_now && touchWasDown_;
    touchWasDown_ = down_now;
}

bool StartupHoldL() {
    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    padUpdate(&pad);
    return (padGetButtons(&pad) & HidNpadButton_L) != 0;
}

}  // namespace app

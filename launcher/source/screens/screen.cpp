#include "screens/screen.h"

#include "app/app.h"

namespace screens {

void Screen::ResetWidgets() {
    focus_.Clear();
    widgets_.clear();
    pressed_ = -1;
}

void Screen::Update(app::App& app, const app::Input& in, float dt) {
    focus_.EnsureValid();
    const ui::Widget* cur = focus_.Current();
    for (auto& w : widgets_) w->Update(dt, w.get() == cur);
    HandleFocus(in);
}

void Screen::HandleFocus(const app::Input& in) {
    // Left and right first go to the focused widget (sliders, toggles); otherwise they move focus.
    if (in.nav != app::Dir::None) {
        ui::Widget* cur = focus_.Current();
        const bool horizontalConsumed =
            cur && (in.nav == app::Dir::Left || in.nav == app::Dir::Right) && cur->Nav(in.nav);
        if (!horizontalConsumed) {
            if (cur && cur->Editing()) cur->StopEditing();
            focus_.Move(in.nav);
        }
    }

    if (in.confirm) {
        if (ui::Widget* cur = focus_.Current()) cur->Activate();
    }

    // Touch: pressing focuses the widget; lifting on the same widget activates it.
    if (in.touchDown) {
        const int hit = focus_.HitTest(in.touchX, in.touchY);
        pressed_ = hit;
        if (hit >= 0 && hit != focus_.Index()) focus_.SetIndex(hit);
    }
    if (in.touchUp) {
        const int hit = focus_.HitTest(in.touchX, in.touchY);
        if (hit >= 0 && hit == pressed_) {
            focus_.SetIndex(hit);
            focus_.Current()->Activate();
        }
        pressed_ = -1;
    }
}

void Screen::Draw(app::App& app, ui::Canvas& canvas) {
    const ui::Theme th = app.theme();
    const ui::Widget* cur = focus_.Current();
    for (auto& w : widgets_) {
        if (w->visible) w->Draw(canvas, th, w.get() == cur);
    }
}

}  // namespace screens

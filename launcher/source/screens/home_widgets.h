// Widgets that only the home screen uses: the rail entries, mod rows and free-drawn cards.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "ui/widgets.h"

namespace screens {

// One entry of the navigation rail.
class NavItem : public ui::Widget {
public:
    NavItem(ui::Rect r, std::string label, char32_t icon, bool active, std::function<void()> onActivate)
        : label_(std::move(label)), icon_(icon), active_(active), onActivate_(std::move(onActivate)) {
        rect = r;
    }
    void Draw(ui::Canvas& c, const ui::Theme& th, bool focused) override {
        if (active_) {
            c.FillRoundRect(rect, 14, th.panel);
            c.FillRoundRect({rect.x, rect.y + 12, 5, rect.h - 24}, 2.5f, th.accent);
        }
        if (focused) c.StrokeRoundRect(rect, 14, 2.0f, th.accent);
        const ui::Color fg = active_ ? th.text : th.textDim;
        c.Icon(icon_, 26, rect.x + 34, rect.y + rect.h * 0.5f, active_ ? th.accent : th.textFaint);
        c.Text(ui::kBodyStrong, label_, rect.x + 62, rect.y + rect.h * 0.5f, fg, ui::Align::Left, ui::VAlign::Middle);
    }
    void Activate() override {
        if (onActivate_) onActivate_();
    }

private:
    std::string label_;
    char32_t icon_;
    bool active_;
    std::function<void()> onActivate_;
};

// A mod in the Mods list: check box, name, what it replaces, and its priority when enabled.
class ModRow : public ui::Widget {
public:
    ModRow(ui::Rect r, std::string name, std::string detail, bool enabled, int priority, bool warn,
           std::function<void()> onActivate)
        : name_(std::move(name)), detail_(std::move(detail)), enabled_(enabled), priority_(priority), warn_(warn),
          onActivate_(std::move(onActivate)) {
        rect = r;
    }
    void Draw(ui::Canvas& c, const ui::Theme& th, bool focused) override {
        c.FillRoundRect(rect, 14, focused ? th.panelHi : th.panel);
        c.StrokeRoundRect(rect, 14, focused ? 2.0f : 1.0f, focused ? th.accent : th.edge);
        const ui::Rect box{rect.x + 20, rect.y + rect.h * 0.5f - 14, 28, 28};
        if (enabled_) {
            c.FillRoundRect(box, 8, th.accent);
            c.Icon(0xE5CA, 22, box.x + 14, box.y + 14, th.onAccent);  // "check"
        } else {
            c.StrokeRoundRect(box, 8, 2.0f, th.textFaint);
        }
        c.Text(ui::kBodyStrong, name_, rect.x + 66, rect.y + 12, enabled_ ? th.text : th.textDim);
        c.Text(ui::kSmall, detail_, rect.x + 66, rect.y + 40, warn_ ? th.warn : th.textFaint);
        if (enabled_ && priority_ > 0) {
            const std::string p = "#" + std::to_string(priority_);
            const float w = c.TextWidth(ui::kLabel, p) + 22;
            const ui::Rect pill{rect.x + rect.w - w - 18, rect.y + rect.h * 0.5f - 15, w, 30};
            c.StrokeRoundRect(pill, 15, 1.5f, th.accent);
            c.Text(ui::kLabel, p, pill.x + w * 0.5f, pill.y + 15, th.accent, ui::Align::Center, ui::VAlign::Middle);
        }
    }
    void Activate() override {
        if (onActivate_) onActivate_();
    }

private:
    std::string name_, detail_;
    bool enabled_;
    int priority_;
    bool warn_;
    std::function<void()> onActivate_;
};

// A non-focusable card drawn by a callback, for status panels.
class Card : public ui::Widget {
public:
    using DrawFn = std::function<void(ui::Canvas&, const ui::Theme&, const ui::Rect&)>;
    Card(ui::Rect r, DrawFn fn) : fn_(std::move(fn)) {
        rect = r;
        focusable = false;
    }
    void Draw(ui::Canvas& c, const ui::Theme& th, bool) override {
        c.FillRoundRect(rect, 18, th.panel);
        c.StrokeRoundRect(rect, 18, 1.0f, th.edge);
        if (fn_) fn_(c, th, rect);
    }

private:
    DrawFn fn_;
};

}  // namespace screens

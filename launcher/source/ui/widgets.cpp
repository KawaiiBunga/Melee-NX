#include "ui/widgets.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "ui/anim.h"

namespace ui {
namespace {

constexpr float kCardRadius = 22.0f;
constexpr float kControlRadius = 14.0f;


}  // namespace

// ---- Label -------------------------------------------------------------------------------

Label::Label(std::string text, TextStyle style, Align align, VAlign valign)
    : text_(std::move(text)), style_(style), align_(align), valign_(valign) {
    focusable = false;
}

void Label::Draw(Canvas& canvas, const Theme& theme, bool focused) {
    (void)focused;
    Color c = useCustomColor_ ? color_ : theme.text;
    if (!useCustomColor_ && dim_) c = theme.textDim;
    float x = rect.x;
    if (align_ == Align::Center) x = rect.x + rect.w * 0.5f;
    if (align_ == Align::Right) x = rect.x + rect.w;
    float y = rect.y;
    if (valign_ == VAlign::Middle) y = rect.y + rect.h * 0.5f;
    if (valign_ == VAlign::Bottom) y = rect.y + rect.h;
    canvas.Text(style_, text_, x, y, c, align_, valign_);
}

// ---- Panel -------------------------------------------------------------------------------

Panel::Panel(Rect r, float radius, bool highlighted) : radius_(radius), highlighted_(highlighted) {
    rect = r;
    focusable = false;
}

void Panel::Draw(Canvas& canvas, const Theme& theme, bool focused) {
    (void)focused;
    canvas.Shadow(rect, radius_, 14.0f, 40);
    canvas.FillRoundRect(rect, radius_, highlighted_ ? theme.accent : theme.panel);
    canvas.StrokeRoundRect(rect, radius_, 1.0f, theme.edge);
}

// ---- ItemBox -----------------------------------------------------------------------------

ItemBox::ItemBox(Rect r, std::string title, std::string subtitle)
    : title_(std::move(title)), subtitle_(std::move(subtitle)) {
    rect = r;
}

void ItemBox::SetBadge(std::string badge, Color color) {
    badge_ = std::move(badge);
    badgeColor_ = color;
}

void ItemBox::SetDisabled(std::string reason) {
    disabledReason_ = std::move(reason);
    enabled = false;
}

void ItemBox::Update(float dt, bool focused) {
    focusAmount_ = Approach(focusAmount_, focused ? 1.0f : 0.0f, dt, 14.0f);
    pulse_ += dt;
}

void ItemBox::Draw(Canvas& canvas, const Theme& theme, bool focused) {
    (void)focused;
    const float lift = focusAmount_ * 4.0f;
    Rect r = rect;
    r.y -= lift;
    const float glow = selected_ ? 1.0f : focusAmount_;
    canvas.Shadow(r, kCardRadius, 18.0f + 10.0f * glow, static_cast<std::uint8_t>(45 + 50 * glow));
    if (glow > 0.01f) {
        canvas.FillRoundRect(r.Inset(-6.0f), kCardRadius + 6, WithAlpha(theme.accentHi, static_cast<std::uint8_t>(110 * glow)));
    }
    canvas.FillRoundRect(r, kCardRadius, enabled ? theme.panel : theme.panelHi);
    if (glow > 0.01f) {
        canvas.StrokeRoundRect(r, kCardRadius, 3.0f, WithAlpha(theme.accent, static_cast<std::uint8_t>(255 * glow)));
    } else {
        canvas.StrokeRoundRect(r, kCardRadius, 1.0f, theme.edge);
    }

    float textTop = r.y + 30;
    if (!image_.empty()) {
        const float pad = 12.0f;
        const float iw = r.w - 2 * pad;
        const float ih = iw * (330.0f / 520.0f);
        canvas.Image(image_, Rect{r.x + pad, r.y + pad, iw, ih}, enabled ? 255 : 130);
        textTop = r.y + pad + ih + 18;
    }
    const float cx = r.x + r.w * 0.5f;
    canvas.Text(kDisplay, title_, cx, textTop, enabled ? theme.text : theme.textFaint, Align::Center);
    canvas.Text(kSmall, subtitle_, cx, textTop + 46, enabled ? theme.textDim : theme.textFaint, Align::Center);

    if (!badge_.empty()) {
        const float bw = canvas.TextWidth(kLabel, badge_) + 30;
        Rect pill{r.x + r.w - bw - 22, r.y + 22, bw, 34};
        canvas.FillRoundRect(pill, 17, theme.panel);
        canvas.StrokeRoundRect(pill, 17, 1.0f, WithAlpha(badgeColor_, 160));
        canvas.Text(kLabel, badge_, pill.x + bw * 0.5f, pill.y + 17, badgeColor_, Align::Center, VAlign::Middle);
    }
    if (!enabled && !disabledReason_.empty()) {
        canvas.Text(kSmall, disabledReason_, cx, r.y + r.h - 34, theme.warn, Align::Center);
    }
}

void ItemBox::Activate() {
    if (enabled && onActivate_) onActivate_();
}

// ---- ListRow -----------------------------------------------------------------------------

ListRow::ListRow(Rect r, std::string label, ValueFn value, std::function<void()> onActivate)
    : label_(std::move(label)), value_(std::move(value)), onActivate_(std::move(onActivate)) {
    rect = r;
}

void ListRow::Draw(Canvas& canvas, const Theme& theme, bool focused) {
    canvas.FillRoundRect(rect, kControlRadius, focused ? theme.panelHi : theme.panel);
    canvas.StrokeRoundRect(rect, kControlRadius, focused ? 2.0f : 1.0f, focused ? theme.accent : theme.edge);
    if (focused) canvas.FillRoundRect({rect.x + 6, rect.y + 10, 5, rect.h - 20}, 2.5f, theme.accent);
    float lx = rect.x + 22;
    if (icon_ != 0) {
        canvas.Icon(icon_, 26, rect.x + 34, rect.y + rect.h * 0.5f, theme.textDim);
        lx = rect.x + 62;
    }
    canvas.Text(kBody, label_, lx, rect.y + rect.h * 0.5f, enabled ? theme.text : theme.textFaint, Align::Left,
                VAlign::Middle);
    float vx = rect.x + rect.w - 22;
    if (onActivate_ && enabled) {
        canvas.Icon(U'\ue5cc', 26, vx - 6, rect.y + rect.h * 0.5f, theme.textFaint);
        vx -= 30;
    }
    if (value_) {
        canvas.Text(kBodyStrong, value_(), vx, rect.y + rect.h * 0.5f, enabled ? theme.accent : theme.textFaint,
                    Align::Right, VAlign::Middle);
    }
}

void ListRow::Activate() {
    if (enabled && onActivate_) onActivate_();
}

// ---- ToggleRow ---------------------------------------------------------------------------

ToggleRow::ToggleRow(Rect r, std::string label, bool* value, std::function<void(bool)> onChange)
    : label_(std::move(label)), value_(value), onChange_(std::move(onChange)) {
    rect = r;
    knob_ = (value_ && *value_) ? 1.0f : 0.0f;
}

void ToggleRow::Draw(Canvas& canvas, const Theme& theme, bool focused) {
    canvas.FillRoundRect(rect, kControlRadius, focused ? theme.panelHi : theme.panel);
    canvas.StrokeRoundRect(rect, kControlRadius, focused ? 2.0f : 1.0f, focused ? theme.accent : theme.edge);
    canvas.Text(kBody, label_, rect.x + 22, rect.y + rect.h * 0.5f, enabled ? theme.text : theme.textFaint, Align::Left,
                VAlign::Middle);

    Rect track{rect.x + rect.w - 84, rect.y + rect.h * 0.5f - 15, 62, 30};
    canvas.FillRoundRect(track, 15, Mix(theme.edge, theme.accent, knob_));
    const float kx = track.x + 15 + knob_ * (track.w - 30);
    canvas.FillCircle({kx, track.y + 16}, 12, Color{40, 70, 120, 40});
    canvas.FillCircle({kx, track.y + 15}, 12, Color{255, 255, 255, 255});
}

void ToggleRow::Update(float dt, bool focused) {
    (void)focused;
    const float target = (value_ && *value_) ? 1.0f : 0.0f;
    knob_ = Approach(knob_, target, dt, 18.0f);
}

void ToggleRow::Activate() {
    if (!value_ || !enabled) return;
    *value_ = !*value_;
    if (onChange_) onChange_(*value_);
}

bool ToggleRow::Nav(app::Dir dir) {
    // Toggles change only with A; left and right move focus.
    (void)dir;
    return false;
}

// ---- SliderRow ---------------------------------------------------------------------------

SliderRow::SliderRow(Rect r, std::string label, int* value, int min, int max, int step, std::string suffix)
    : label_(std::move(label)), value_(value), min_(min), max_(max), step_(step), suffix_(std::move(suffix)) {
    rect = r;
}

void SliderRow::Draw(Canvas& canvas, const Theme& theme, bool focused) {
    canvas.FillRoundRect(rect, kControlRadius, focused ? theme.panelHi : theme.panel);
    canvas.StrokeRoundRect(rect, kControlRadius, focused ? 2.0f : 1.0f, focused ? theme.accent : theme.edge);
    canvas.Text(kBody, label_, rect.x + 22, rect.y + rect.h * 0.5f, enabled ? theme.text : theme.textFaint, Align::Left,
                VAlign::Middle);

    const float track_w = rect.w * 0.32f;
    const float track_x = rect.x + rect.w - 110 - track_w;
    const float track_y = rect.y + rect.h * 0.5f;
    canvas.FillRoundRect({track_x, track_y - 4, track_w, 8}, 4, theme.edge);
    const float f = max_ > min_ ? static_cast<float>(*value_ - min_) / static_cast<float>(max_ - min_) : 0.0f;
    canvas.FillRoundRect({track_x, track_y - 4, track_w * Clamp01(f), 8}, 4, theme.accent);
    canvas.FillCircle({track_x + track_w * Clamp01(f), track_y + 1}, 11, Color{40, 70, 120, 40});
    canvas.FillCircle({track_x + track_w * Clamp01(f), track_y}, 11, Color{255, 255, 255, 255});
    canvas.FillCircle({track_x + track_w * Clamp01(f), track_y}, 5, theme.accent);

    char buf[32];
    std::snprintf(buf, sizeof buf, editing_ ? "< %d%s >" : "%d%s", *value_, suffix_.c_str());
    canvas.Text(kBodyStrong, buf, rect.x + rect.w - 22, rect.y + rect.h * 0.5f, theme.accent, Align::Right,
                VAlign::Middle);
    if (editing_) canvas.StrokeRoundRect(rect, kControlRadius, 3.0f, theme.accent);
}

bool SliderRow::Nav(app::Dir dir) {
    if (!editing_) return false;
    if (dir != app::Dir::Left && dir != app::Dir::Right) return false;
    const int delta = dir == app::Dir::Right ? step_ : -step_;
    *value_ = std::clamp(*value_ + delta, min_, max_);
    if (onChange_) onChange_();
    return true;
}

// ---- ChoiceRow ---------------------------------------------------------------------------

ChoiceRow::ChoiceRow(Rect r, std::string label, int* value, int min, int max, std::vector<std::string> labels,
                     std::function<void()> onChange)
    : label_(std::move(label)), value_(value), min_(min), max_(max), labels_(std::move(labels)),
      onChange_(std::move(onChange)) {
    rect = r;
}

void ChoiceRow::Draw(Canvas& canvas, const Theme& theme, bool focused) {
    canvas.FillRoundRect(rect, kControlRadius, focused ? theme.panelHi : theme.panel);
    canvas.StrokeRoundRect(rect, kControlRadius, editing_ ? 3.0f : (focused ? 2.0f : 1.0f),
                           focused || editing_ ? theme.accent : theme.edge);
    canvas.Text(kBody, label_, rect.x + 22, rect.y + rect.h * 0.5f, theme.text, Align::Left, VAlign::Middle);
    const int v = std::clamp(*value_, min_, max_);
    std::string text;
    const int idx = v - min_;
    if (idx >= 0 && idx < static_cast<int>(labels_.size())) {
        text = labels_[static_cast<size_t>(idx)];
    } else {
        text = std::to_string(v - min_ + 1) + " / " + std::to_string(max_ - min_ + 1);
    }
    if (editing_) text = "<  " + text + "  >";
    canvas.Text(kBodyStrong, text, rect.x + rect.w - 22, rect.y + rect.h * 0.5f, theme.accent, Align::Right,
                VAlign::Middle);
}

bool ChoiceRow::Nav(app::Dir dir) {
    if (!editing_) return false;
    if (dir != app::Dir::Left && dir != app::Dir::Right) return false;
    const int span = max_ - min_ + 1;
    int v = *value_ - min_ + (dir == app::Dir::Right ? 1 : -1);
    v = ((v % span) + span) % span;  // wrap around
    *value_ = v + min_;
    if (onChange_) onChange_();
    return true;
}

// ---- ProgressBar -------------------------------------------------------------------------

ProgressBar::ProgressBar(Rect r) {
    rect = r;
    focusable = false;
}

void ProgressBar::Update(float dt, bool focused) {
    (void)focused;
    sweep_ += dt * 0.6f;
    if (sweep_ > 1.0f) sweep_ -= 1.0f;
}

void ProgressBar::Draw(Canvas& canvas, const Theme& theme, bool focused) {
    (void)focused;
    canvas.FillRoundRect(rect, rect.h * 0.5f, theme.edge);
    if (fraction_ >= 0.0f) {
        canvas.FillGradientRoundRect({rect.x, rect.y, std::max(rect.h, rect.w * Clamp01(fraction_)), rect.h}, rect.h * 0.5f,
                                     theme.accentHi, theme.accent);
    } else {
        // Indeterminate: a short segment sweeping across the track.
        const float seg = rect.w * 0.22f;
        const float x = rect.x - seg + (rect.w + seg) * sweep_;
        const float left = std::max(rect.x, x);
        const float right = std::min(rect.x + rect.w, x + seg);
        if (right > left) canvas.FillRoundRect({left, rect.y, right - left, rect.h}, rect.h * 0.5f, theme.accent);
    }
}

// ---- Button ------------------------------------------------------------------------------

Button::Button(Rect r, std::string label, bool primary, std::function<void()> onActivate)
    : label_(std::move(label)), primary_(primary), onActivate_(std::move(onActivate)) {
    rect = r;
}

void Button::Draw(Canvas& canvas, const Theme& theme, bool focused) {
    const float radius = rect.h * 0.5f;
    if (focused) canvas.FillRoundRect(rect.Inset(-5), radius + 5, WithAlpha(theme.accentHi, 120));
    if (primary_ && enabled) {
        canvas.Shadow(rect, radius, 16.0f, 70);
        canvas.FillGradientRoundRect(rect, radius, theme.accentHi, theme.accent);
    } else {
        canvas.Shadow(rect, radius, 10.0f, 30);
        canvas.FillRoundRect(rect, radius, theme.panel);
        canvas.StrokeRoundRect(rect, radius, focused ? 2.0f : 1.0f, focused ? theme.accent : theme.edge);
    }
    const Color text = !enabled ? theme.textFaint : (primary_ ? theme.onAccent : theme.text);
    const TextStyle style = large_ ? kTitle : kBodyStrong;
    const float tw = canvas.TextWidth(style, label_);
    const float iconW = icon_ != 0 ? (large_ ? 44.0f : 32.0f) : 0.0f;
    const float x0 = rect.x + (rect.w - tw - iconW) * 0.5f;
    canvas.Text(style, label_, x0, rect.y + rect.h * 0.5f, text, Align::Left, VAlign::Middle);
    if (icon_ != 0) canvas.Icon(icon_, large_ ? 38 : 26, x0 + tw + iconW * 0.5f + 6, rect.y + rect.h * 0.5f, text);
}

void Button::Activate() {
    if (enabled && onActivate_) onActivate_();
}

// ---- HintBar -----------------------------------------------------------------------------

HintBar::HintBar(std::vector<Hint> hints) : hints_(std::move(hints)) { focusable = false; }

void HintBar::Draw(Canvas& canvas, const Theme& theme, bool focused) {
    (void)focused;
    float x = rect.x + 48;
    const float y = rect.y + rect.h * 0.5f;
    for (const auto& hint : hints_) {
        const float gw = canvas.TextWidth(kLabel, hint.first) + 18;
        Rect glyph{x, y - 14, std::max(30.0f, gw), 28};
        canvas.FillRoundRect(glyph, 14, theme.text);
        canvas.Text(kLabel, hint.first, glyph.x + glyph.w * 0.5f, y, theme.panel, Align::Center, VAlign::Middle);
        canvas.Text(kSmall, hint.second, glyph.x + glyph.w + 10, y, theme.textDim, Align::Left, VAlign::Middle);
        x += glyph.w + 10 + canvas.TextWidth(kSmall, hint.second) + 30;
    }
}

}  // namespace ui

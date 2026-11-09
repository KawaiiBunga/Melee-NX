#include "ui/overlay.h"

#include <algorithm>
#include <utility>

#include "ui/anim.h"
#include "ui/widgets.h"

namespace ui {
namespace {

constexpr float kModalX = 280.0f;
constexpr float kModalY = 170.0f;
constexpr float kModalW = 720.0f;
constexpr float kModalH = 380.0f;
constexpr std::size_t kMaxToasts = 3;

}  // namespace

void Overlay::Toast(std::string text, float seconds) {
    toasts_.push_back({std::move(text), 0.0f, seconds});
    if (toasts_.size() > kMaxToasts) toasts_.erase(toasts_.begin());
}

void Overlay::ShowModal(Modal m) {
    modal_ = std::move(m);
    modalSel_ = 0;
    modalAge_ = 0.0f;
}

void Overlay::CloseModal() { modal_.reset(); }

Rect Overlay::ButtonRect(std::size_t index, std::size_t count) {
    constexpr float kBtnW = 220.0f;
    constexpr float kGap = 24.0f;
    const float total = kBtnW * static_cast<float>(count) + kGap * static_cast<float>(count - 1);
    const float x0 = kModalX + (kModalW - total) * 0.5f;
    return {x0 + static_cast<float>(index) * (kBtnW + kGap), kModalY + kModalH - 96, kBtnW, 60};
}

bool Overlay::HandleInput(const app::Input& in) {
    if (!modal_) return false;
    const std::size_t count = modal_->buttons.size();
    auto pick = [this](int index) {
        if (index < 0 || index >= static_cast<int>(modal_->buttons.size())) return;
        auto fn = modal_->buttons[static_cast<std::size_t>(index)].onPick;
        CloseModal();
        if (fn) fn();
    };

    if (count > 0) {
        if (in.nav == app::Dir::Left) {
            modalSel_ = std::max(0, modalSel_ - 1);
        } else if (in.nav == app::Dir::Right) {
            modalSel_ = std::min(static_cast<int>(count) - 1, modalSel_ + 1);
        }
        if (in.confirm) pick(modalSel_);
        if (in.touchDown) {
            for (std::size_t i = 0; i < count; ++i) {
                if (ButtonRect(i, count).Contains(in.touchX, in.touchY)) {
                    modalSel_ = static_cast<int>(i);
                    pick(modalSel_);
                    break;
                }
            }
        }
    }
    if (in.back && modal_) {
        CloseModal();
    }
    return true;  // a modal swallows all input while it is open
}

void Overlay::Update(float dt) {
    for (auto& t : toasts_) t.age += dt;
    toasts_.erase(std::remove_if(toasts_.begin(), toasts_.end(),
                                 [](const ToastItem& t) { return t.age >= t.life; }),
                  toasts_.end());
    if (modal_) modalAge_ += dt;
}

void Overlay::Draw(Canvas& canvas, const Theme& theme) {
    // Toasts stack upward from above the hint bar.
    float y = kLogicalH - 86.0f;
    for (auto it = toasts_.rbegin(); it != toasts_.rend(); ++it) {
        const float fadeIn = Clamp01(it->age / 0.18f);
        const float fadeOut = Clamp01((it->life - it->age) / 0.35f);
        const auto a = static_cast<std::uint8_t>(255 * std::min(fadeIn, fadeOut));
        const float tw = canvas.TextWidth(kBodyStrong, it->text) + 56;
        Rect r{(kLogicalW - tw) * 0.5f, y - 46, tw, 46};
        canvas.FillRoundRect(r, 14, WithAlpha(theme.panelHi, a));
        canvas.StrokeRoundRect(r, 14, 1.0f, WithAlpha(theme.accent, a));
        canvas.Text(kBodyStrong, it->text, kLogicalW * 0.5f, r.y + r.h * 0.5f, WithAlpha(theme.text, a),
                    Align::Center, VAlign::Middle);
        y -= 56;
    }

    if (!modal_) return;
    const float fade = EaseOutCubic(modalAge_ / 0.18f);
    canvas.FillRect({0, 0, kLogicalW, kLogicalH}, WithAlpha({0, 0, 0, 255}, static_cast<std::uint8_t>(170 * fade)));

    const Rect panel{kModalX, kModalY + (1.0f - fade) * 18.0f, kModalW, kModalH};
    canvas.FillRoundRect(panel, 24, WithAlpha(theme.panel, static_cast<std::uint8_t>(255 * fade)));
    canvas.StrokeRoundRect(panel, 24, 2.0f, WithAlpha(theme.accent, static_cast<std::uint8_t>(255 * fade)));

    canvas.Text(kDisplay, modal_->title, panel.x + 40, panel.y + 36, theme.text);
    // Body text is drawn line by line.
    float by = panel.y + 110;
    std::string line;
    for (char ch : modal_->body + "\n") {
        if (ch == '\n') {
            canvas.Text(kBody, line, panel.x + 40, by, theme.textDim);
            by += canvas.TextHeight(kBody) + 4;
            line.clear();
        } else {
            line.push_back(ch);
        }
    }

    const std::size_t count = modal_->buttons.size();
    for (std::size_t i = 0; i < count; ++i) {
        const ModalButton& b = modal_->buttons[i];
        const Rect r = ButtonRect(i, count);
        const bool sel = static_cast<int>(i) == modalSel_;
        Color fill = b.primary ? theme.accent : theme.panelHi;
        Color text = b.primary ? theme.onAccent : theme.text;
        canvas.FillRoundRect(r, 12, fill);
        if (sel) canvas.StrokeRoundRect(r.Inset(-4), 16, 2.0f, theme.text);
        canvas.Text(kBodyStrong, b.label, r.x + r.w * 0.5f, r.y + r.h * 0.5f, text, Align::Center, VAlign::Middle);
    }
}

}  // namespace ui

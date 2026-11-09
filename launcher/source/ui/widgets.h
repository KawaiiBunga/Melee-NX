// Retained widgets. Each widget owns its geometry and knows how to draw and react to input.
// Screens own their widgets and decide which ones take part in focus navigation.
#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "app/input.h"
#include "ui/canvas.h"
#include "ui/geometry.h"
#include "ui/theme.h"

namespace ui {

class Widget {
public:
    Rect rect;
    bool focusable = true;
    bool enabled = true;
    bool visible = true;

    virtual ~Widget() = default;
    virtual void Update(float dt, bool focused) {
        (void)dt;
        (void)focused;
    }
    virtual void Draw(Canvas& canvas, const Theme& theme, bool focused) = 0;
    // A or a tap on the widget.
    virtual void Activate() {}
    // Left or right while focused. Returns true if the widget consumed the press. Only a widget in
    // edit mode (a slider after A) consumes it; otherwise the d-pad always moves focus.
    virtual bool Nav(app::Dir dir) {
        (void)dir;
        return false;
    }
    // Edit mode: entered with A on widgets that adjust a value, left with A or B.
    virtual bool Editing() const { return false; }
    virtual void StopEditing() {}
};

// Static text.
class Label : public Widget {
public:
    Label(std::string text, TextStyle style, Align align = Align::Left, VAlign valign = VAlign::Top);
    void SetText(std::string text) { text_ = std::move(text); }
    void SetColor(Color c) {
        color_ = c;
        useCustomColor_ = true;
    }
    void SetDim(bool dim) { dim_ = dim; }
    void Draw(Canvas& canvas, const Theme& theme, bool focused) override;

private:
    std::string text_;
    TextStyle style_;
    Align align_;
    VAlign valign_;
    bool useCustomColor_ = false;
    bool dim_ = false;
    Color color_{};
};

// Decorative rounded panel. Never focusable.
class Panel : public Widget {
public:
    Panel(Rect r, float radius, bool highlighted = false);
    void Draw(Canvas& canvas, const Theme& theme, bool focused) override;

private:
    float radius_;
    bool highlighted_;
};

// Big glassy tile used on the home grid and anywhere a single choice is made.
class ItemBox : public Widget {
public:
    using Callback = std::function<void()>;
    ItemBox(Rect r, std::string title, std::string subtitle);
    void SetTitle(std::string t) { title_ = std::move(t); }
    void SetSubtitle(std::string s) { subtitle_ = std::move(s); }
    void SetBadge(std::string badge, Color color);
    // A disabled box keeps its place but shows why it cannot be used.
    void SetDisabled(std::string reason);
    // Artwork shown in the top part of the card (a romfs PNG).
    void SetImage(std::string path) { image_ = std::move(path); }
    // A selected card keeps its accent outline even without focus.
    void SetSelected(bool selected) { selected_ = selected; }
    void OnActivate(Callback cb) { onActivate_ = std::move(cb); }

    void Update(float dt, bool focused) override;
    void Draw(Canvas& canvas, const Theme& theme, bool focused) override;
    void Activate() override;

private:
    std::string title_;
    std::string subtitle_;
    std::string badge_;
    Color badgeColor_{};
    std::string disabledReason_;
    std::string image_;
    bool selected_ = false;
    Callback onActivate_;
    float focusAmount_ = 0.0f;
    float pulse_ = 0.0f;
};

// Full-width row with a label on the left and a value on the right. Activating runs the callback.
class ListRow : public Widget {
public:
    using ValueFn = std::function<std::string()>;
    ListRow(Rect r, std::string label, ValueFn value, std::function<void()> onActivate);
    void SetLabel(std::string l) { label_ = std::move(l); }
    void SetIcon(char32_t icon) { icon_ = icon; }
    void Draw(Canvas& canvas, const Theme& theme, bool focused) override;
    void Activate() override;

private:
    std::string label_;
    ValueFn value_;
    std::function<void()> onActivate_;
    char32_t icon_ = 0;
};

// On/off switch. Left, right, and A all flip the value.
class ToggleRow : public Widget {
public:
    ToggleRow(Rect r, std::string label, bool* value, std::function<void(bool)> onChange = nullptr);
    void Update(float dt, bool focused) override;
    void Draw(Canvas& canvas, const Theme& theme, bool focused) override;
    void Activate() override;
    bool Nav(app::Dir dir) override;

private:
    std::string label_;
    bool* value_;
    std::function<void(bool)> onChange_;
    float knob_ = 0.0f;
};

// Picks one of a list of named values. A enters edit mode, left and right cycle, A or B leaves.
// With no labels it shows "n / max" (numbered styles such as hair 1..72).
class ChoiceRow : public Widget {
public:
    ChoiceRow(Rect r, std::string label, int* value, int min, int max, std::vector<std::string> labels = {},
              std::function<void()> onChange = nullptr);
    void Draw(Canvas& canvas, const Theme& theme, bool focused) override;
    void Activate() override { editing_ = !editing_; }
    bool Nav(app::Dir dir) override;
    bool Editing() const override { return editing_; }
    void StopEditing() override { editing_ = false; }

private:
    std::string label_;
    int* value_;
    int min_;
    int max_;
    std::vector<std::string> labels_;
    std::function<void()> onChange_;
    bool editing_ = false;
};

// Integer slider with a fixed step. Left and right move the value.
class SliderRow : public Widget {
public:
    SliderRow(Rect r, std::string label, int* value, int min, int max, int step, std::string suffix = "");
    void OnChange(std::function<void()> cb) { onChange_ = std::move(cb); }
    void Draw(Canvas& canvas, const Theme& theme, bool focused) override;
    void Activate() override { editing_ = !editing_; }
    bool Nav(app::Dir dir) override;
    bool Editing() const override { return editing_; }
    void StopEditing() override { editing_ = false; }

private:
    std::string label_;
    int* value_;
    int min_;
    int max_;
    int step_;
    std::string suffix_;
    bool editing_ = false;
    std::function<void()> onChange_;
};

// Horizontal progress bar. A negative fraction draws an indeterminate sweep.
class ProgressBar : public Widget {
public:
    explicit ProgressBar(Rect r);
    void SetFraction(float f) { fraction_ = f; }
    void Update(float dt, bool focused) override;
    void Draw(Canvas& canvas, const Theme& theme, bool focused) override;

private:
    float fraction_ = -1.0f;
    float sweep_ = 0.0f;
};

// Rounded action button. Primary buttons use the accent colour.
class Button : public Widget {
public:
    Button(Rect r, std::string label, bool primary, std::function<void()> onActivate);
    void SetLabel(std::string l) { label_ = std::move(l); }
    void SetIcon(char32_t icon) { icon_ = icon; }
    void SetLarge(bool large) { large_ = large; }
    void Draw(Canvas& canvas, const Theme& theme, bool focused) override;
    void Activate() override;

private:
    std::string label_;
    bool primary_;
    std::function<void()> onActivate_;
    char32_t icon_ = 0;
    bool large_ = false;
};

// Bottom strip listing the button actions for the current screen. Never focusable.
class HintBar : public Widget {
public:
    using Hint = std::pair<std::string, std::string>;  // glyph label, action
    HintBar(std::vector<Hint> hints);
    void Draw(Canvas& canvas, const Theme& theme, bool focused) override;

private:
    std::vector<Hint> hints_;
};

}  // namespace ui

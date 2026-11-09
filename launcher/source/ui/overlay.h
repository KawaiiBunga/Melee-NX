// App-wide overlays: transient toasts and a modal dialog that takes over input while open.
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "app/input.h"
#include "ui/canvas.h"
#include "ui/geometry.h"
#include "ui/theme.h"

namespace ui {

struct ModalButton {
    std::string label;
    std::function<void()> onPick;  // runs after the dialog closes
    bool primary = false;
};

struct Modal {
    std::string title;
    std::string body;  // may contain '\n'
    std::vector<ModalButton> buttons;
};

class Overlay {
public:
    void Toast(std::string text, float seconds);
    void ShowModal(Modal m);
    bool ModalOpen() const { return modal_.has_value(); }
    void CloseModal();

    // Handles input while a modal is open. Returns true if the modal used the frame's input.
    bool HandleInput(const app::Input& in);
    void Update(float dt);
    void Draw(Canvas& canvas, const Theme& theme);

private:
    struct ToastItem {
        std::string text;
        float age = 0.0f;
        float life = 2.5f;
    };
    static Rect ButtonRect(std::size_t index, std::size_t count);

    std::vector<ToastItem> toasts_;
    std::optional<Modal> modal_;
    int modalSel_ = 0;
    float modalAge_ = 0.0f;
};

}  // namespace ui

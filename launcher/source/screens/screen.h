// Base class for every screen. A screen owns its widgets and its focus order.
#pragma once

#include <memory>
#include <utility>
#include <vector>

#include "app/input.h"
#include "ui/canvas.h"
#include "ui/focus.h"
#include "ui/widgets.h"

namespace app {
class App;
}

namespace screens {

class Screen {
public:
    virtual ~Screen() = default;

    virtual void OnEnter(app::App& app) { (void)app; }
    virtual void OnLeave(app::App& app) { (void)app; }
    // Return true to consume B (for example to cancel a running action). False pops the screen.
    virtual bool OnBack(app::App& app) {
        (void)app;
        return false;
    }

    // B pressed: leaves a widget's edit mode first, otherwise asks OnBack.
    bool HandleBack(app::App& app) {
        if (ui::Widget* cur = focus_.Current(); cur && cur->Editing()) {
            cur->StopEditing();
            return true;
        }
        return OnBack(app);
    }

    // Default: updates widgets, runs focus navigation, A, and touch. Subclasses call this last.
    virtual void Update(app::App& app, const app::Input& in, float dt);
    virtual void Draw(app::App& app, ui::Canvas& canvas);

protected:
    // Creates a widget owned by this screen. Its draw order is creation order.
    template <typename W, typename... Args>
    W* Add(Args&&... args) {
        auto w = std::make_unique<W>(std::forward<Args>(args)...);
        W* raw = w.get();
        widgets_.push_back(std::move(w));
        return raw;
    }

    // Puts a widget into the focus order. Only focus-order widgets can be selected.
    void Track(ui::Widget* w) { focus_.Add(w); }

    // Drops every widget and the focus order. Use before rebuilding a page.
    void ResetWidgets();

    // The bottom strip of button hints.
    void AddHints(std::vector<ui::HintBar::Hint> hints) {
        Add<ui::HintBar>(std::move(hints))->rect = {0, ui::kLogicalH - 48, ui::kLogicalW, 48};
    }

    ui::FocusGroup focus_;

private:
    void HandleFocus(const app::Input& in);

    std::vector<std::unique_ptr<ui::Widget>> widgets_;
    int pressed_ = -1;
};

}  // namespace screens

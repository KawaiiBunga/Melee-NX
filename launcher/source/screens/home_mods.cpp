// The Mods page. See home.h and data/mods.h.
#include <algorithm>

#include "app/app.h"
#include "screens/home.h"
#include "screens/home_layout.h"
#include "screens/home_widgets.h"

namespace screens {

using namespace layout;
using ui::Align;
using ui::Rect;

namespace {
constexpr int kPerPage = 6;
constexpr float kModRowH = 64.0f;
}  // namespace

void HomeScreen::BuildMods(app::App& app) {
    const auto& mods = app.mods().Mods();
    auto* rescan = Add<ui::Button>(Rect{kContentX + kContentW - 170, kHeaderY + 2, 170, 48}, "Rescan", false, [this, &app]() {
        Later([this, &app]() {
            app.RescanMods(true);
            Build(app);
        });
    });
    rescan->SetIcon(ui::icon::kRefresh);
    Track(rescan);

    if (mods.empty()) {
        Add<Card>(Rect{kContentX, kBodyY, kContentW, 220}, [](ui::Canvas& c, const ui::Theme& th, const Rect& r) {
            c.Icon(ui::icon::kExtension, 54, r.x + r.w * 0.5f, r.y + 64, th.accent);
            c.Text(ui::kTitle, "No mods yet", r.x + r.w * 0.5f, r.y + 104, th.text, Align::Center);
            c.Text(ui::kSmall, "Put mods in /switch/melee-nx/mods, then Rescan", r.x + r.w * 0.5f, r.y + 152, th.textDim,
                   Align::Center);
        });
        return;
    }

    const int pages = (static_cast<int>(mods.size()) + kPerPage - 1) / kPerPage;
    modPage_ = std::clamp(modPage_, 0, pages - 1);
    int priority = 0;
    for (size_t i = 0; i < mods.size(); ++i) {
        if (mods[i].enabled) ++priority;
        if (static_cast<int>(i) < modPage_ * kPerPage || static_cast<int>(i) >= (modPage_ + 1) * kPerPage) continue;
        const auto& m = mods[i];
        const std::string detail = !m.warning.empty() ? m.warning : (m.notes.empty() ? "" : m.notes.front());
        const int slot = static_cast<int>(i) - modPage_ * kPerPage;
        const std::string id = m.id;
        auto* row = Add<ModRow>(Rect{kContentX, kBodyY + slot * (kModRowH + 8), kContentW, kModRowH}, m.name, detail,
                                m.enabled, m.enabled ? priority : 0, !m.warning.empty(), [this, &app, id]() {
                                    Later([this, &app, id]() {
                                        bool on = false;
                                        for (const auto& x : app.mods().Mods())
                                            if (x.id == id) on = x.enabled;
                                        app.mods().SetEnabled(id, !on);
                                        app.SaveMods();
                                        Build(app);
                                    });
                                });
        Track(row);
        modRows_.push_back({row, id});
    }
    if (pages > 1) {
        const float y = kBodyY + kPerPage * (kModRowH + 8) + 4;
        auto* prev = Add<ui::Button>(Rect{kContentX, y, 150, 44}, "Back", false, [this, &app]() {
            Later([this, &app]() {
                --modPage_;
                Build(app);
            });
        });
        prev->enabled = modPage_ > 0;
        Track(prev);
        auto* next = Add<ui::Button>(Rect{kContentX + 166, y, 150, 44}, "More", false, [this, &app]() {
            Later([this, &app]() {
                ++modPage_;
                Build(app);
            });
        });
        next->enabled = modPage_ + 1 < pages;
        Track(next);
    }
}

// X raises the focused enabled mod one place: the higher of two mods wins a file both replace.
void HomeScreen::ModAction(app::App& app, bool x) {
    if (!x) return;
    const ui::Widget* cur = focus_.Current();
    for (const auto& [w, id] : modRows_) {
        if (w != cur) continue;
        app.mods().Move(id, -1);
        app.SaveMods();
        const int keep = focus_.Index();
        Build(app);
        focus_.SetIndex(keep - 1 >= static_cast<int>(Page::Count) + 2 ? keep - 1 : keep);
        return;
    }
}

}  // namespace screens

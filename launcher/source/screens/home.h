// The home screen: the navigation rail on the left (pages and the Play button) and the current
// page on the right. Pages: home.cpp (frame, Play, Settings), home_mods.cpp, home_slippi.cpp,
// home_files.cpp.
#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "screens/screen.h"

namespace screens {

class HomeScreen : public Screen {
public:
    enum class Page : int { Play, Mods, Slippi, Files, Settings, Count };

    void OnEnter(app::App& app) override;
    void Update(app::App& app, const app::Input& in, float dt) override;
    void Draw(app::App& app, ui::Canvas& canvas) override;

private:
    // Rebuilds the widgets. `focusRail` puts focus on the current page's rail entry, otherwise
    // the previously focused index is kept when it still exists.
    void Build(app::App& app, bool focusRail = false);
    void SetPage(app::App& app, Page p, bool fromRail);
    void BuildRail(app::App& app);
    void BuildPlay(app::App& app);
    void BuildMods(app::App& app);
    void BuildSlippi(app::App& app);
    void BuildFiles(app::App& app);
    void BuildSettings(app::App& app);
    void ModAction(app::App& app, bool x);
    // Widget callbacks must not rebuild the widget list they live in; they queue the work here and
    // it runs at the start of the next Update.
    void Later(std::function<void()> fn) { later_.push_back(std::move(fn)); }
    std::vector<std::function<void()>> later_;

    Page page_ = Page::Play;
    int modPage_ = 0;
    bool plusWasHeld_ = false;
    std::vector<std::pair<const ui::Widget*, std::string>> modRows_;  // focused row -> mod id
    std::string title_;
};

}  // namespace screens

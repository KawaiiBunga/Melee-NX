// Home screen frame, rail, Play and Settings pages. See home.h.
#include "screens/home.h"

#include <memory>

#include "app/app.h"
#include "screens/home_layout.h"
#include "screens/home_widgets.h"

// The build passes the version as a bare token (0.5.0) so no shell has to quote it.
#define MNX_STR(x) #x
#define MNX_XSTR(x) MNX_STR(x)
#ifdef MNX_LAUNCHER_VERSION_TOKEN
#define MNX_LAUNCHER_VERSION MNX_XSTR(MNX_LAUNCHER_VERSION_TOKEN)
#else
#define MNX_LAUNCHER_VERSION "dev"
#endif

namespace screens {

using namespace layout;
using ui::Align;
using ui::Rect;
using ui::VAlign;

namespace {

struct PageInfo {
    const char* name;
    char32_t icon;
};
constexpr PageInfo kPages[] = {
    {"Play", ui::icon::kGamepad},   {"Mods", ui::icon::kExtension},   {"Slippi", ui::icon::kPublic},
    {"Game Files", ui::icon::kFolder}, {"Settings", ui::icon::kSettings},
};
static_assert(sizeof(kPages) / sizeof(kPages[0]) == static_cast<size_t>(HomeScreen::Page::Count));

}  // namespace

void HomeScreen::OnEnter(app::App& app) { Build(app); }

void HomeScreen::SetPage(app::App& app, Page p, bool fromRail) {
    if (p != page_) modPage_ = 0;
    page_ = p;
    Build(app, fromRail);
    if (!fromRail) {
        // Land on the first thing in the page body (rail entries and Play come first).
        focus_.SetIndex(static_cast<int>(Page::Count) + 1);
        focus_.EnsureValid();
    }
}

void HomeScreen::Build(app::App& app, bool focusRail) {
    const int keep = focus_.Index();
    ResetWidgets();
    modRows_.clear();
    title_ = kPages[static_cast<int>(page_)].name;
    BuildRail(app);
    switch (page_) {
        case Page::Play: BuildPlay(app); break;
        case Page::Mods: BuildMods(app); break;
        case Page::Slippi: BuildSlippi(app); break;
        case Page::Files: BuildFiles(app); break;
        case Page::Settings: BuildSettings(app); break;
        case Page::Count: break;
    }
    std::vector<ui::HintBar::Hint> hints = {{"A", "Select"}, {"L R", "Pages"}, {"+", "Play"}};
    if (page_ == Page::Mods && !app.mods().Mods().empty()) hints = {{"A", "On / off"}, {"X", "Move up"}, {"+", "Play"}};
    Add<ui::HintBar>(std::move(hints))->rect = {kRailW, ui::kLogicalH - kHintH, ui::kLogicalW - kRailW, kHintH};
    focus_.SetIndex(focusRail ? static_cast<int>(page_) : keep);
    focus_.EnsureValid();
}

void HomeScreen::BuildRail(app::App& app) {
    for (int i = 0; i < static_cast<int>(Page::Count); ++i) {
        auto* item = Add<NavItem>(Rect{18, 130.0f + i * 62.0f, kRailW - 36, 54}, kPages[i].name, kPages[i].icon,
                                  i == static_cast<int>(page_),
                                  [this, &app, i]() { Later([this, &app, i]() { SetPage(app, static_cast<Page>(i), true); }); });
        Track(item);
    }
    auto* play = Add<ui::Button>(Rect{22, 590, kRailW - 44, 70}, "Play", true, [&app]() { app.LaunchGame(); });
    play->SetIcon(ui::icon::kPlay);
    play->SetLarge(true);
    play->enabled = app.game().CanPlay();
    Track(play);
}

void HomeScreen::Update(app::App& app, const app::Input& in, float dt) {
    if (!later_.empty()) {
        auto work = std::move(later_);
        later_.clear();
        for (auto& fn : work) fn();
        return;
    }
    const int count = static_cast<int>(Page::Count);
    if (in.tabPrev || in.tabNext) {
        SetPage(app, static_cast<Page>((static_cast<int>(page_) + (in.tabNext ? 1 : -1) + count) % count), false);
        return;
    }
    if (in.plusHeld && !plusWasHeld_) {
        plusWasHeld_ = true;
        app.LaunchGame();
        return;
    }
    plusWasHeld_ = in.plusHeld;
    if (page_ == Page::Mods && (in.actionX || in.actionY)) {
        ModAction(app, in.actionX);
        return;
    }
    Screen::Update(app, in, dt);
}

void HomeScreen::Draw(app::App& app, ui::Canvas& c) {
    const ui::Theme th = app.theme();
    c.FillRect({0, 0, kRailW, ui::kLogicalH}, th.rail);
    c.Text(ui::kTitle, "MELEE", 32, 48, th.text);
    c.Text(ui::kTitle, "NX", 32 + c.TextWidth(ui::kTitle, "MELEE") + 8, 48, th.accent);
    c.Text(ui::kSmall, MNX_LAUNCHER_VERSION, 34, 82, th.textFaint);
    c.Text(ui::kDisplay, title_, kContentX, kHeaderY, th.text);
    if (app.AppletMode()) c.Text(ui::kLabel, "Low memory: open from a game (hold R)", kContentX, 630, th.warn);
    Screen::Draw(app, c);
}

// ---- Play ------------------------------------------------------------------------------------

void HomeScreen::BuildPlay(app::App& app) {
    const bool ready = app.game().CanPlay();
    const int mods = app.settings().modsEnabled ? app.mods().EnabledCount() : 0;
    const bool signedIn = app.slippi().state == mnx::slippi::State::Valid;
    // Two modes, picked like KartPadNX's game cards: Play starts the selected one.
    const float cardW = (kContentW - 20) * 0.5f;
    struct ModeCard {
        int mode;
        const char* title;
        std::string subtitle;
    };
    const ModeCard cards[] = {
        {0, "Vanilla", !ready ? "Add your disc in Game Files"
                              : mods ? std::to_string(mods) + (mods == 1 ? " mod on" : " mods on") : "Ready"},
        {1, "Slippi", signedIn ? app.slippi().account.connectCode : "Sign in on the Slippi page"},
    };
    for (int i = 0; i < 2; ++i) {
        const ModeCard m = cards[i];
        auto* box = Add<ui::ItemBox>(Rect{kContentX + i * (cardW + 20), kBodyY + 10, cardW, 290}, m.title, m.subtitle);
        box->SetSelected(app.settings().mode == m.mode);
        if (app.settings().mode == m.mode) box->SetBadge("Selected", app.theme().accent);
        box->OnActivate([this, &app, m]() {
            if (app.settings().mode == m.mode) {
                app.LaunchGame();
                return;
            }
            app.settings().mode = m.mode;
            app.SaveSettings();
            Later([this, &app]() { Build(app); });
        });
        Track(box);
    }
    auto* toggle = Add<ui::ToggleRow>(Rect{kContentX, kBodyY + 320, kContentW, kRowH}, "Load mods", &app.settings().modsEnabled, [this, &app](bool) {
                                          app.SaveSettings();
                                          Later([this, &app]() { Build(app); });
                                      });
    Track(toggle);
}

// ---- Settings --------------------------------------------------------------------------------

void HomeScreen::BuildSettings(app::App& app) {
    std::vector<std::string> accents;
    for (int i = 0; i < ui::kAccentCount; ++i) accents.push_back(ui::AccentName(ui::AccentFromIndex(i)));
    Track(Add<ui::ChoiceRow>(Row(0), "Accent colour", &app.settings().accent, 0, ui::kAccentCount - 1, accents,
                             [&app]() { app.SaveSettings(); }));
    Track(Add<ui::ToggleRow>(Row(1), "Skip the launcher (hold L to open it)", &app.settings().quickLaunch,
                             [&app](bool) { app.SaveSettings(); }));
    Track(Add<ui::ToggleRow>(Row(2), "Return here after playing", &app.settings().returnToLauncher,
                             [&app](bool) { app.SaveSettings(); }));
}

}  // namespace screens

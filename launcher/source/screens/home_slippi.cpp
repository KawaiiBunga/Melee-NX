// The Slippi page. See home.h, data/slippi_account.h and data/lan_signin.h.
#include <memory>

#include "app/app.h"
#include "data/paths.h"
#include "screens/flows.h"
#include "screens/home.h"
#include "screens/home_layout.h"
#include "screens/home_widgets.h"

namespace screens {

using namespace layout;
using ui::Align;
using ui::Rect;
using ui::VAlign;

void HomeScreen::BuildSlippi(app::App& app) {
    const auto s = app.slippi();
    const bool signedIn = s.state == mnx::slippi::State::Valid;

    Add<Card>(Rect{kContentX, kBodyY, kContentW, 160}, [s, signedIn](ui::Canvas& c, const ui::Theme& th, const Rect& r) {
        const ui::Vec2 av{r.x + 80, r.y + 80};
        c.FillCircle(av, 46, signedIn ? th.accent : th.panelHi);
        c.Icon(ui::icon::kPerson, 54, av.x, av.y, signedIn ? th.onAccent : th.textFaint);
        if (signedIn) {
            c.Text(ui::kDisplay, s.account.displayName.empty() ? "Slippi player" : s.account.displayName, r.x + 156, r.y + 34,
                   th.text);
            c.Text(ui::kTitle, s.account.connectCode, r.x + 158, r.y + 88, th.accent);
        } else {
            c.Text(ui::kDisplay, "Not signed in", r.x + 156, r.y + 56, th.text);
            if (s.state == mnx::slippi::State::Invalid) c.Text(ui::kSmall, s.error, r.x + 158, r.y + 106, th.warn);
        }
    });

    const float y = kBodyY + 180, half = (kContentW - 16) * 0.5f;
    auto* signin = Add<ui::Button>(Rect{kContentX, y, half, 60}, signedIn ? "Switch account" : "Sign in", !signedIn,
                                   [&app]() { app.Push(std::make_unique<SigninScreen>()); });
    signin->SetIcon(ui::icon::kLogin);
    Track(signin);
    if (!signedIn) return;

    auto* out = Add<ui::Button>(Rect{kContentX + half + 16, y, half, 60}, "Sign out", false, [this, &app]() {
        app.overlay().ShowModal({"Sign out?", "", {{"Cancel", nullptr, false}, {"Sign out", [this, &app]() {
                                                        mnx::slippi::DeleteAccount(mnx::paths::kSlippiUser);
                                                        app.ReloadSlippi();
                                                        Later([this, &app]() { Build(app); });
                                                    },
                                                    true}}});
    });
    out->SetIcon(ui::icon::kLogout);
    Track(out);
}

}  // namespace screens

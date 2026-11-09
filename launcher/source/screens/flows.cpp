#include "screens/flows.h"

#include <algorithm>
#include <cmath>

#include "app/app.h"
#include "data/paths.h"
#include "data/slippi_account.h"
#include "screens/home_layout.h"

namespace screens {

using ui::Align;
using ui::Rect;
using ui::VAlign;

namespace {

constexpr Rect kSheet{170, 90, 940, 540};

void DrawSheet(ui::Canvas& c, const ui::Theme& th, const std::string& title) {
    c.FillRect({0, 0, ui::kLogicalW, ui::kLogicalH}, ui::WithAlpha(th.bgLow, 200));
    c.FillRoundRect(kSheet, 24, th.panel);
    c.StrokeRoundRect(kSheet, 24, 1.0f, th.edge);
    c.FillRoundRect({kSheet.x + 40, kSheet.y + 34, 6, 34}, 3, th.accent);
    c.Text(ui::kDisplay, title, kSheet.x + 62, kSheet.y + 30, th.text);
}

void Spinner(ui::Canvas& c, const ui::Theme& th, float cx, float cy, float t) {
    for (int i = 0; i < 8; ++i) {
        const float a = t * 5.0f + i * 0.785f;
        const auto alpha = static_cast<std::uint8_t>(40 + 215 * i / 7);
        c.FillCircle({cx + std::cos(a) * 18, cy + std::sin(a) * 18}, 4, ui::WithAlpha(th.accent, alpha));
    }
}

// The sign-in server stores the account through this; it runs on the job thread.
bool SaveUploadedAccount(const std::string& text, std::string& error) {
    return mnx::slippi::SaveAccountText(mnx::paths::kSlippiUser, text, error);
}

}  // namespace

// ---- JobScreen -------------------------------------------------------------------------------

JobScreen::JobScreen(std::string title, mnx::Job::Work work, Done done, bool cancellable)
    : title_(std::move(title)), work_(std::move(work)), done_(std::move(done)), cancellable_(cancellable) {}

void JobScreen::OnEnter(app::App& app) {
    (void)app;
    if (!started_) {
        started_ = true;
        job_.Run(work_);
    }
}

bool JobScreen::OnBack(app::App& app) {
    (void)app;
    if (cancellable_) job_.Cancel();
    return true;  // the screen leaves when the job reports back
}

void JobScreen::Update(app::App& app, const app::Input& in, float dt) {
    spin_ += dt;
    const mnx::JobStatus s = job_.Status();
    if (s.done && !finished_) {
        finished_ = true;
        job_.Join();
        app.Pop();
        if (done_) done_(app, s.ok, s.message);
    }
    Screen::Update(app, in, dt);
}

void JobScreen::Draw(app::App& app, ui::Canvas& c) {
    const ui::Theme th = app.theme();
    DrawSheet(c, th, title_);
    const mnx::JobStatus s = job_.Status();
    c.Text(ui::kBody, s.stage.empty() ? "Working..." : s.stage, kSheet.x + 62, kSheet.y + 140, th.textDim);
    const Rect bar{kSheet.x + 62, kSheet.y + 210, kSheet.w - 124, 14};
    c.FillRoundRect(bar, 7, th.edge);
    if (s.fraction >= 0.0f) {
        c.FillGradientRoundRect({bar.x, bar.y, std::max(14.0f, bar.w * std::min(1.0f, s.fraction)), bar.h}, 7, th.accentHi,
                                th.accent);
        c.Text(ui::kTitle, std::to_string(static_cast<int>(s.fraction * 100.0f + 0.5f)) + "%", kSheet.x + kSheet.w - 62,
               kSheet.y + 240, th.text, Align::Right);
    } else {
        Spinner(c, th, kSheet.x + kSheet.w * 0.5f, kSheet.y + 280, spin_);
    }
    if (cancellable_) c.Text(ui::kSmall, "B to cancel", kSheet.x + 62, kSheet.y + kSheet.h - 56, th.textFaint);
    Screen::Draw(app, c);
}

// ---- DiscPickerScreen ------------------------------------------------------------------------

void DiscPickerScreen::OnEnter(app::App& app) {
    ResetWidgets();
    discs_ = mnx::gamedata::FindDiscs();
    float y = kSheet.y + 120;
    for (size_t i = 0; i < discs_.size() && i < 6; ++i) {
        const auto d = discs_[i];
        std::string shown = d.path.rfind("sdmc:", 0) == 0 ? "SD:" + d.path.substr(5) : d.path;
        if (shown.size() > 52) shown = "..." + shown.substr(shown.size() - 49);
        auto* row = Add<ui::ListRow>(
            Rect{kSheet.x + 40, y, kSheet.w - 80, 56}, shown,
            [d]() { return std::string(d.valid ? "Melee 1.02" : "Not usable"); },
            [this, &app, d]() {
                if (picked_) return;
                if (!d.valid) {
                    app.overlay().ShowModal({"Not this one", d.reason.substr(0, 200), {{"OK", nullptr, true}}});
                    return;
                }
                picked_ = true;
                app.settings().discPath = d.path;
                app.SaveSettings();
                app.RefreshGame();
                app.Toast("Disc image selected.");
                app.Pop();
            });
        row->SetIcon(d.valid ? ui::icon::kCheck : ui::icon::kWarning);
        Track(row);
        y += 66;
    }
}

void DiscPickerScreen::Draw(app::App& app, ui::Canvas& c) {
    const ui::Theme th = app.theme();
    DrawSheet(c, th, "Disc image");
    if (discs_.empty()) {
        c.Text(ui::kBody, "No disc image found", kSheet.x + 62, kSheet.y + 130, th.text);
        c.Text(ui::kSmall, "Copy your Melee NTSC-U 1.02 .iso to /switch/melee-nx/", kSheet.x + 62, kSheet.y + 170, th.textDim);
    }
    Screen::Draw(app, c);
}
// ---- SigninScreen ----------------------------------------------------------------------------

void SigninScreen::OnEnter(app::App& app) {
    (void)app;
    if (started_) return;
    started_ = true;
    code_ = mnx::signin::NewPairCode();
    if (!server_.Open(error_)) return;
    const std::string code = code_;
    job_.Run([this, code](mnx::Job::Reporter& r) { return server_.Serve(r, code, SaveUploadedAccount); });
}

bool SigninScreen::OnBack(app::App& app) {
    if (!error_.empty() || finished_) {
        app.Pop();
        return true;
    }
    job_.Cancel();
    return true;
}

void SigninScreen::Update(app::App& app, const app::Input& in, float dt) {
    if (error_.empty() && !finished_) {
        const mnx::JobStatus s = job_.Status();
        if (s.done) {
            finished_ = true;
            job_.Join();
            server_.Close();
            app.ReloadSlippi();
            app.Pop();
            if (s.ok)
                app.Toast(s.message, 4.0f);
            else if (s.message != "Sign-in cancelled.")
                app.overlay().ShowModal({"Sign-in stopped", s.message, {{"OK", nullptr, true}}});
        }
    }
    Screen::Update(app, in, dt);
}

void SigninScreen::Draw(app::App& app, ui::Canvas& c) {
    const ui::Theme th = app.theme();
    DrawSheet(c, th, "Sign in to Slippi");
    const float x = kSheet.x + 62;
    if (!error_.empty()) {
        c.Text(ui::kBody, error_, x, kSheet.y + 130, th.warn);
        c.Text(ui::kSmall, "Or copy user.json to /switch/melee-nx/slippi/", x, kSheet.y + 170, th.textDim);
        return;
    }
    c.Text(ui::kBody, "On your phone or PC, open", x, kSheet.y + 120, th.textDim);
    const Rect urlBox{x, kSheet.y + 158, kSheet.w - 124, 80};
    c.FillRoundRect(urlBox, 16, th.bgLow);
    c.Text(ui::kDisplay, server_.Url(), urlBox.x + urlBox.w * 0.5f, urlBox.y + urlBox.h * 0.5f, th.accent, Align::Center,
           VAlign::Middle);
    c.Text(ui::kBody, "and enter this code", x, kSheet.y + 266, th.textDim);
    const float tile = 64, gap = 14, total = 6 * tile + 5 * gap;
    for (int i = 0; i < 6 && i < static_cast<int>(code_.size()); ++i) {
        const Rect t{kSheet.x + (kSheet.w - total) * 0.5f + i * (tile + gap), kSheet.y + 308, tile, 78};
        c.FillRoundRect(t, 14, th.panelHi);
        c.Text(ui::kDisplay, std::string(1, code_[static_cast<size_t>(i)]), t.x + tile * 0.5f, t.y + 39, th.text,
               Align::Center, VAlign::Middle);
    }
    const mnx::JobStatus s = job_.Status();
    if (s.stage.rfind("Wrong", 0) == 0) c.Text(ui::kLabel, "Wrong code, try again", x, kSheet.y + 420, th.warn);
    Screen::Draw(app, c);
}
}  // namespace screens

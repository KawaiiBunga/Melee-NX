// The Game Files page. See home.h and data/game_data.h.
#include <memory>

#include "app/app.h"
#include "screens/flows.h"
#include "screens/home.h"
#include "screens/home_layout.h"
#include "screens/home_widgets.h"

namespace screens {

using namespace layout;

namespace {

std::string FileName(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

}  // namespace

void HomeScreen::BuildFiles(app::App& app) {
    const auto& g = app.game();
    auto* disc = Add<ui::ListRow>(
        Row(0), "Disc image", [g]() { return g.discPath.empty() ? std::string("None") : FileName(g.discPath); },
        [&app]() { app.Push(std::make_unique<DiscPickerScreen>()); });
    disc->SetIcon(ui::icon::kFolder);
    Track(disc);

    auto* extract = Add<ui::ListRow>(
        Row(1), "Extracted files", [g]() { return std::string(g.extracted ? "Ready" : "Extract"); },
        [this, &app]() {
            const std::string discPath = app.game().discPath;
            if (discPath.empty()) {
                app.overlay().ShowModal({"No disc image", "Choose a disc image first.", {{"OK", nullptr, true}}});
                return;
            }
            app.overlay().ShowModal(
                {"Extract game files?", "Faster loading. Takes a few minutes and about 1.4 GB.",
                 {{"Cancel", nullptr, false},
                  {"Extract",
                   [this, &app, discPath]() {
                       app.Push(std::make_unique<JobScreen>(
                           "Extracting",
                           [discPath](mnx::Job::Reporter& r) { return mnx::gamedata::Extract(discPath, r); },
                           [this](app::App& a, bool ok, const std::string& msg) {
                               a.RefreshGame();
                               a.RescanMods(false);
                               if (ok)
                                   a.Toast("Extraction finished.");
                               else
                                   a.overlay().ShowModal({"Extraction stopped", msg, {{"OK", nullptr, true}}});
                               Later([this, &a]() { Build(a); });
                           }));
                   },
                   true}}});
        });
    extract->SetIcon(ui::icon::kLayers);
    Track(extract);

    auto* rescan = Add<ui::Button>(ui::Rect{kContentX, Row(2).y + 8, 220, 52}, "Rescan", false, [this, &app]() {
        app.RefreshGame();
        Later([this, &app]() { Build(app); });
    });
    rescan->SetIcon(ui::icon::kRefresh);
    Track(rescan);
}

}  // namespace screens

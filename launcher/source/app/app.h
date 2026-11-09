// The launcher application: SDL setup, the main loop, the screen stack, and the state every screen
// shares (settings, game data status, mods, Slippi account). Screen changes are deferred to the end
// of the frame so a screen never frees itself inside its own Update.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include <SDL.h>

#include "app/input.h"
#include "data/game_data.h"
#include "data/mods.h"
#include "data/settings.h"
#include "data/slippi_account.h"
#include "ui/canvas.h"
#include "ui/overlay.h"
#include "ui/theme.h"

namespace screens {
class Screen;
}

namespace app {

class App {
public:
    App();
    ~App();
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    int Run();

    void Push(std::unique_ptr<screens::Screen> screen);
    void Pop();
    void Replace(std::unique_ptr<screens::Screen> screen);
    void RequestExit() { exitRequested_ = true; }

    ui::Overlay& overlay() { return overlay_; }
    ui::Theme theme() const { return ui::MakeTheme(ui::AccentFromIndex(settings_.accent)); }
    void Toast(std::string text, float seconds = 2.8f) { overlay_.Toast(std::move(text), seconds); }

    mnx::Settings& settings() { return settings_; }
    void SaveSettings();

    // Shared state, refreshed on demand.
    const mnx::gamedata::Status& game() const { return game_; }
    void RefreshGame();
    mnx::ModCatalog& mods() { return mods_; }
    void RescanMods(bool announce);
    void SaveMods();
    const mnx::slippi::Loaded& slippi() const { return slippi_; }
    void ReloadSlippi();
    void SetSlippiAccount(const mnx::slippi::Account& a);

    // Writes the mod manifest and queues the game; exits the launcher on success.
    void LaunchGame();
    // Writes the handoff and starts the game. `code`: the Direct opponent; empty searches Unranked.
    void StartGame(bool slippi, const std::string& code);

    bool AppletMode() const;

private:
    bool Init();
    void Shutdown();
    void ApplyDeferred();
    void Frame(float dt);
    void DrawBackdrop(ui::Canvas& c, const ui::Theme& th);

    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    std::unique_ptr<ui::Canvas> canvas_;
    InputReader input_;
    ui::Overlay overlay_;
    mnx::Settings settings_;
    mnx::gamedata::Status game_;
    mnx::ModCatalog mods_;
    mnx::slippi::Loaded slippi_;
    float time_ = 0.0f;

    enum class OpKind : std::uint8_t { Push, Pop, Replace };
    struct Op {
        OpKind kind;
        std::unique_ptr<screens::Screen> screen;
    };
    std::vector<std::unique_ptr<screens::Screen>> stack_;
    std::vector<Op> ops_;
    bool exitRequested_ = false;
    bool sdlReady_ = false;
};

}  // namespace app

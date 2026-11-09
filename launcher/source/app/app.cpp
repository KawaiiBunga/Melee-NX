#include "app/app.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <SDL_ttf.h>
#include <switch.h>

#include "data/launch.h"
#include "data/paths.h"
#include "screens/home.h"
#include "screens/screen.h"
#include "ui/fonts.h"
#include "ui/keyboard.h"

#ifdef MNX_PREVIEW
bool PreviewFrame(SDL_Renderer* renderer);
#endif

namespace app {
namespace {

constexpr int kWindowW = 1280;
constexpr int kWindowH = 720;
constexpr float kMaxFrameTime = 0.1f;

}  // namespace

App::App() = default;
App::~App() { Shutdown(); }

bool App::AppletMode() const { return appletGetAppletType() != AppletType_Application; }

bool App::Init() {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "[launcher] SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    sdlReady_ = true;
    if (TTF_Init() != 0) {
        std::fprintf(stderr, "[launcher] TTF_Init failed: %s\n", TTF_GetError());
        return false;
    }
#ifdef MNX_PREVIEW
    window_ = SDL_CreateWindow("Melee NX", 0, 0, kWindowW, kWindowH, 0);
#else
    window_ = SDL_CreateWindow("Melee NX", 0, 0, kWindowW, kWindowH, SDL_WINDOW_FULLSCREEN);
#endif
    if (!window_) {
        std::fprintf(stderr, "[launcher] SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }
#ifdef MNX_PREVIEW
    renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
#else
    renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
#endif
    if (!renderer_) {
        std::fprintf(stderr, "[launcher] SDL_CreateRenderer failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_RenderSetLogicalSize(renderer_, kWindowW, kWindowH);
    canvas_ = std::make_unique<ui::Canvas>(renderer_);
    input_.Init();
    settings_ = mnx::LoadSettings();
    RefreshGame();
    RescanMods(false);
    ReloadSlippi();
    return true;
}

void App::Shutdown() {
    ops_.clear();
    stack_.clear();
    canvas_.reset();
    ui::CloseFonts();
    if (renderer_) SDL_DestroyRenderer(renderer_);
    if (window_) SDL_DestroyWindow(window_);
    renderer_ = nullptr;
    window_ = nullptr;
    if (sdlReady_) {
        TTF_Quit();
        SDL_Quit();
        sdlReady_ = false;
    }
}

void App::SaveSettings() {
    if (!mnx::SaveSettings(settings_)) Toast("Could not save settings to the SD card.", 3.5f);
}

void App::RefreshGame() { game_ = mnx::gamedata::Probe(settings_.discPath); }

void App::RescanMods(bool announce) {
    const mnx::ScanReport rep = mods_.Scan(mnx::paths::kModsDir, mnx::paths::kFilesDir, game_.extracted);
    mods_.LoadState(mnx::paths::kModsState);
    if (!announce) return;
    if (!rep.problems.empty()) {
        std::string body;
        for (size_t i = 0; i < rep.problems.size() && i < 5; ++i) body += rep.problems[i] + "\n";
        overlay_.ShowModal({"Some mods need attention", body, {{"OK", nullptr, true}}});
    } else {
        std::string msg = std::to_string(mods_.Mods().size()) + " mods found";
        if (rep.unpacked) msg += ", " + std::to_string(rep.unpacked) + " unpacked";
        if (rep.identified) msg += ", " + std::to_string(rep.identified) + " costumes identified";
        Toast(msg + ".");
    }
}

void App::SaveMods() {
    if (!mods_.SaveState(mnx::paths::kModsState)) Toast("Could not save the mod list.", 3.5f);
}

void App::ReloadSlippi() { slippi_ = mnx::slippi::LoadAccount(mnx::paths::kSlippiUser); }

void App::SetSlippiAccount(const mnx::slippi::Account& a) {
    slippi_.state = mnx::slippi::State::Valid;
    slippi_.account = a;
    slippi_.error.clear();
}

void App::LaunchGame() {
    RefreshGame();
    if (!game_.CanPlay()) {
        overlay_.ShowModal({"No game data", "Melee NX needs your own Melee disc image (NTSC-U 1.02).\nSet it up in Game Files.",
                            {{"OK", nullptr, true}}});
        return;
    }
    const bool slippi = settings_.mode == 1;
    if (slippi && slippi_.state != mnx::slippi::State::Valid) {
        overlay_.ShowModal({"Sign in first", "Slippi mode needs your Slippi account.", {{"OK", nullptr, true}}});
        return;
    }
    if (slippi) {
        // The game has no Slippi menus of its own, so who to play is chosen here.
        overlay_.ShowModal({"Slippi",
                            "Unranked finds an opponent.\nDirect plays a connect code.",
                            {{"Unranked", [this] { StartGame(true, ""); }, true},
                             {"Direct",
                              [this] {
                                  std::string code;
                                  if (!ui::PromptText("Opponent's connect code (ABCD#123)", settings_.opponentCode, 12, code)) return;
                                  if (!mnx::launch::NormalizeConnectCode(code)) {
                                      overlay_.ShowModal({"Not a connect code", "It looks like ABCD#123.", {{"OK", nullptr, true}}});
                                      return;
                                  }
                                  settings_.opponentCode = code;
                                  mnx::SaveSettings(settings_);
                                  StartGame(true, code);
                              }},
                             {"Cancel", nullptr}}});
        return;
    }
    StartGame(false, "");
}

void App::StartGame(bool slippi, const std::string& code) {
    std::string error;
    const bool useMods = settings_.modsEnabled && mods_.EnabledCount() > 0;
    if (!mods_.WriteManifest(mnx::paths::kModsManifest, useMods, &error)) {
        overlay_.ShowModal({"Mods were not applied", error, {{"OK", nullptr, true}}});
        return;
    }
    mnx::launch::Request req;
    req.bootArg = game_.BootArg();
    req.modsManifest = useMods ? mnx::paths::kModsManifest : "";
    req.returnTo = settings_.returnToLauncher ? mnx::launch::SelfPath() : "";
    req.mode = slippi ? "slippi" : "vanilla";
    req.code = code;
    if (!mnx::launch::Start(mnx::launch::FindGameNro(), req, error)) {
        overlay_.ShowModal({"Could not start the game", error, {{"OK", nullptr, true}}});
        return;
    }
    RequestExit();
}

void App::Push(std::unique_ptr<screens::Screen> screen) { ops_.push_back({OpKind::Push, std::move(screen)}); }
void App::Pop() { ops_.push_back({OpKind::Pop, nullptr}); }
void App::Replace(std::unique_ptr<screens::Screen> screen) { ops_.push_back({OpKind::Replace, std::move(screen)}); }

void App::ApplyDeferred() {
    for (auto& op : ops_) {
        switch (op.kind) {
            case OpKind::Push:
                if (!stack_.empty()) stack_.back()->OnLeave(*this);
                stack_.push_back(std::move(op.screen));
                stack_.back()->OnEnter(*this);
                break;
            case OpKind::Pop:
                if (!stack_.empty()) {
                    stack_.back()->OnLeave(*this);
                    stack_.pop_back();
                }
                if (!stack_.empty()) stack_.back()->OnEnter(*this);
                break;
            case OpKind::Replace:
                if (!stack_.empty()) {
                    stack_.back()->OnLeave(*this);
                    stack_.pop_back();
                }
                stack_.push_back(std::move(op.screen));
                stack_.back()->OnEnter(*this);
                break;
        }
    }
    ops_.clear();
}

void App::Frame(float dt) {
    Input in;
    input_.Read(dt, in);
    if (overlay_.ModalOpen()) {
        overlay_.HandleInput(in);
    } else if (!stack_.empty()) {
        screens::Screen& top = *stack_.back();
        if (in.back && top.HandleBack(*this)) {
        } else if (in.back && stack_.size() > 1) {
            Pop();
        } else {
            top.Update(*this, in, dt);
        }
    }
    overlay_.Update(dt);
    ApplyDeferred();
}

void App::DrawBackdrop(ui::Canvas& c, const ui::Theme& th) {
    c.FillGradientRoundRect(ui::Rect{0, 0, ui::kLogicalW, ui::kLogicalH}, 0.0f, th.bg, th.bgLow);
}

int App::Run() {
    if (!Init()) return 1;
    // Quick launch: straight into the game unless L is held while the launcher starts.
    if (settings_.quickLaunch && !StartupHoldL()) {
        LaunchGame();
        if (exitRequested_) return 0;
    }
    Push(std::make_unique<screens::HomeScreen>());
    ApplyDeferred();

    Uint64 last = SDL_GetPerformanceCounter();
    const double freq = static_cast<double>(SDL_GetPerformanceFrequency());
    while (!exitRequested_) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
        }
        const Uint64 now = SDL_GetPerformanceCounter();
        const float dt = std::min(kMaxFrameTime, static_cast<float>(static_cast<double>(now - last) / freq));
        last = now;
        time_ += dt;
#ifndef MNX_PREVIEW
        if (!appletMainLoop()) break;
#endif
        Frame(dt);
        if (exitRequested_) break;
        const ui::Theme th = theme();
        SDL_SetRenderDrawColor(renderer_, th.bgLow.r, th.bgLow.g, th.bgLow.b, 255);
        SDL_RenderClear(renderer_);
        DrawBackdrop(*canvas_, th);
        if (!stack_.empty()) stack_.back()->Draw(*this, *canvas_);
        overlay_.Draw(*canvas_, th);
#ifdef MNX_PREVIEW
        if (PreviewFrame(renderer_)) break;
#endif
        SDL_RenderPresent(renderer_);
    }
    return 0;
}

}  // namespace app

// Screens pushed over the home screen: a progress screen for background jobs, the disc image
// picker, and Slippi sign-in from another device.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "data/game_data.h"
#include "data/job.h"
#include "data/lan_signin.h"
#include "screens/screen.h"

namespace screens {

// Runs a Job and shows its progress. B cancels. When the job ends, `done` runs and the screen pops.
class JobScreen : public Screen {
public:
    using Done = std::function<void(app::App&, bool ok, const std::string& message)>;
    JobScreen(std::string title, mnx::Job::Work work, Done done, bool cancellable = true);
    void OnEnter(app::App& app) override;
    void Update(app::App& app, const app::Input& in, float dt) override;
    void Draw(app::App& app, ui::Canvas& canvas) override;
    bool OnBack(app::App& app) override;

private:
    std::string title_;
    mnx::Job::Work work_;
    Done done_;
    bool cancellable_;
    mnx::Job job_;
    bool started_ = false;
    bool finished_ = false;
    float spin_ = 0.0f;
};

// Lists the disc images on the SD card; picking a valid one saves it as the game's disc.
class DiscPickerScreen : public Screen {
public:
    void OnEnter(app::App& app) override;
    void Draw(app::App& app, ui::Canvas& canvas) override;

private:
    std::vector<mnx::gamedata::DiscCandidate> discs_;
    bool picked_ = false;
};

// Shows the address and pairing code, and waits for user.json from a browser on the same network.
class SigninScreen : public Screen {
public:
    void OnEnter(app::App& app) override;
    void Update(app::App& app, const app::Input& in, float dt) override;
    void Draw(app::App& app, ui::Canvas& canvas) override;
    bool OnBack(app::App& app) override;

private:
    mnx::signin::Server server_;
    mnx::Job job_;
    std::string code_;
    std::string error_;
    bool started_ = false;
    bool finished_ = false;
};

}  // namespace screens

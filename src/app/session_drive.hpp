// Driving mission session of the `nightfire` app: DRIVING.ELF Mission with
// the chase camera, a text HUD overlay (speed / objective / banner) and
// pause (Resume / Restart / Quit to frontend).
#pragma once

#include <memory>
#include <optional>
#include <string>

#include "app/app.hpp"
#include "render/window.hpp"
#include "ui/renderer.hpp"
#include "ui/text.hpp"

namespace nf::app {

enum class DriveExit {
    QuitToMenu,
    Restart,
};

struct DriveResult {
    DriveExit exit = DriveExit::QuitToMenu;
    bool won = false;
};

struct DriveHeadless {
    long frames = 600;
    std::string shot;
    std::string inputs;  // driving input_script file (empty = straight gas)
};

class DriveSessionApp {
public:
    DriveSessionApp(AppContext& ctx, Window& window, ui::Renderer& ui, ui::TextRenderer& text,
                    const std::string& level, const std::string& car, const AppConfig& cfg);
    ~DriveSessionApp();

    DriveSessionApp(const DriveSessionApp&) = delete;
    DriveSessionApp& operator=(const DriveSessionApp&) = delete;

    bool ready() const { return ready_; }

    DriveResult run_interactive();
    DriveResult run_headless(const DriveHeadless& headless);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool ready_ = false;
};

}  // namespace nf::app

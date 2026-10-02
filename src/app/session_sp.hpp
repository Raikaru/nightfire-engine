// Single-player ACTION session of the `nightfire` app: the mission's level
// with the player, weapons, placed NPCs (SpSystem over DroneSystem), HUD,
// audio (SFX + music) and the pause / end-mission pages. Mission success has
// no trigger yet (Scripting owns the mission flow); the session ends on
// player death, a mission-fail hook, pause restart/quit, or window close.
#pragma once

#include <memory>
#include <optional>
#include <string>

#include "app/app.hpp"
#include "render/window.hpp"
#include "ui/frontend.hpp"
#include "ui/hud.hpp"
#include "ui/renderer.hpp"
#include "ui/text.hpp"

namespace nf::app {

struct SpLaunch {
    std::string bin;      // FILES.BIN level name, e.g. "07000001.bin"
    int difficulty = 2;   // GameState+0x28: 1 easy, 2 normal, 3 hard
};

enum class SpExit {
    QuitToMenu,   // pause quit, P_ENDMISSION quit, or window close
    Restart,      // pause restart or P_ENDMISSION retry
};

struct SpResult {
    SpExit exit = SpExit::QuitToMenu;
    bool failed = false;  // death or the mission-fail hook fired
};

struct SpHeadless {
    long frames = 300;          // scripted ticks with an idle pad
    std::string shot;           // screenshot after the frames (empty = none)
    std::string inputs;         // nfgame --inputs replay file (empty = idle)
};

class SpSession {
public:
    SpSession(AppContext& ctx, Window& window, ui::Renderer& ui, ui::TextRenderer& text, const SpLaunch& launch,
              const AppConfig& cfg);
    ~SpSession();

    SpSession(const SpSession&) = delete;
    SpSession& operator=(const SpSession&) = delete;

    bool ready() const { return ready_; }

    // Blocking interactive loop (60/30 Hz ticks, HUD, pause menu). Returns why it ended.
    SpResult run_interactive();
    // Scripted run for verification: `headless.frames` ticks, then `shot`.
    SpResult run_headless(const SpHeadless& headless);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool ready_ = false;
};

}  // namespace nf::app

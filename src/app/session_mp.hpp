// Multiplayer arena session of the `nightfire` app: ArenaSession + bots with
// split-screen views, per-viewer HUD (HudState from WeaponSystem + ArenaHud),
// pause and the P_MPDEBRIEFING results page back to the frontend.
#pragma once

#include <array>
#include <memory>
#include <optional>
#include <string>

#include "app/app.hpp"
#include "game/arena_session.hpp"
#include "render/window.hpp"
#include "ui/frontend.hpp"
#include "ui/hud.hpp"
#include "ui/renderer.hpp"
#include "ui/text.hpp"

namespace nf::app {

enum class MpExit {
    QuitToMenu,  // debriefing Continue, pause quit, or window close
    Rematch,     // debriefing Replay or pause restart
};

struct MpResult {
    MpExit exit = MpExit::QuitToMenu;
    bool played = false;  // the match ran (phase left Running)
};

// Direct launch (headless / debug): the nfgame --mp option set. The frontend
// path instead passes the P_MPCONFIRM MpLaunch (see from_launch).
struct MpDirect {
    MatchOptions options;
    std::string level_bin = "07000024.bin";  // empty = Skyrail default
    std::string bot_characters;
    long frames = -1;  // headless ticks (< 0 = interactive)
    std::string shot;
    std::array<std::string, 4> inputs;
};

class MpSession {
public:
    MpSession(AppContext& ctx, Window& window, ui::Renderer& ui, ui::TextRenderer& text, const MpDirect& direct,
              const AppConfig& cfg);
    // Frontend path: options converted from the P_MPCONFIRM record.
    static MpDirect from_launch(const MpLaunch& launch);
    ~MpSession();

    MpSession(const MpSession&) = delete;
    MpSession& operator=(const MpSession&) = delete;

    bool ready() const { return ready_; }

    MpResult run_interactive();
    MpResult run_headless();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool ready_ = false;
};

}  // namespace nf::app

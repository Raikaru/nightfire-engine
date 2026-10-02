#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "assets/menu_file.hpp"
#include "assets/mp_data.hpp"
#include "assets/sp_menu.hpp"
#include "assets/ui_assets.hpp"
#include "game/input.hpp"
#include "ui/menu.hpp"
#include "ui/mp_setup.hpp"

namespace nf {

// What the front end (or the in-game pause menu) asks the application to do when it closes.
struct FrontendResult {
    enum class Action {
        None,             // still open
        StartMultiplayer, // P_MPCONFIRM "start": `launch` holds everything MP_Start consumes
        StartMission,     // P_NFSELECT/P_NFMAP: `level_bin` + `difficulty`
        Resume,           // the pause menu was closed (start button / "Resume")
        RestartMission,   // pause menu "Restart"
        QuitToMenu,       // pause menu "Quit"
        Quit,
    };
    Action action = Action::None;
    std::string level_bin;         // FILES.BIN name of the chosen level (mission or multiplayer arena)
    std::uint32_t level_id = 0;    // GameState level id (the number in the bin name)
    int difficulty = 0;            // 0 Agent, 1 Secret Agent, 2 00 Agent
    std::optional<MpLaunch> launch;  // multiplayer: mode, map, humans, bots and rules (MpSettings) + participants
};

enum class FrontendMode { MainMenu, Pause };

// What the pause menu shows that only the running game knows.
struct PauseObjective {
    std::uint32_t label = 0;   // label hash of the objective text
    bool complete = false;
};
struct PauseScoreRow {
    std::string name;
    std::string value;
};
struct PauseInfo {
    bool multiplayer = false;                   // MP pause: no OBJECTIVES tab, the SCORE tab lists the players
    std::vector<PauseObjective> objectives;     // OBJECTIVES tab (Mission_ObjectiveState)
    std::vector<PauseScoreRow> score;           // SCORE tab
};
// Per-player options the CONTROLS tab edits (PlayerSetting: controller style, Y-axis inversion).
struct PlayerOptions {
    int controller_style = 7;
    bool invert_y = false;
};

// The original front end run on the parsed menu script (docs/ui.md "Front-end runtime"). `open` starts
// MenuManager_Create on the first page of the mode (P_START or P_PAUSE), `update` runs one 30 Hz frame with
// the pad, `draw` renders the 640x448 canvas.
class Frontend {
public:
    // `menu` must outlive the Frontend (the front end's `MenuFile`, or the one of the level for Pause).
    // `mp` / `sp` may be null when the multiplayer / single player pages are not needed; both must outlive the Frontend.
    Frontend(const UiAssets& assets, const MenuFile& menu, const MpData* mp = nullptr, const SpMenuData* sp = nullptr);
    ~Frontend();
    Frontend(const Frontend&) = delete;
    Frontend& operator=(const Frontend&) = delete;

    // `page` overrides the first page (nfui menu --page).
    void open(FrontendMode mode, std::optional<std::uint32_t> page = std::nullopt);
    void update(const PadHistory& pad);                       // controller 0
    void update(const std::array<PadHistory, 4>& pads);       // all four controllers (multiplayer join)
    // Which controllers are plugged in (controller 0 always is); absent ones cannot join a match.
    void set_controller_present(std::size_t controller, bool present);
    void draw(ui::Renderer& renderer, ui::TextRenderer& text);
    bool wants_close() const;
    const FrontendResult& result() const;
    std::uint32_t page_id() const;                 // the current page
    std::vector<ui::MenuSound> take_sounds();

    void set_pause_info(PauseInfo info);
    PlayerOptions& player_options();

    // The menu runtime and multiplayer model, for tools and tests.
    ui::MenuManager* manager();
    MpSetup* mp_setup();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace nf

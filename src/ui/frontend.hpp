#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "assets/menu_file.hpp"
#include "assets/mp_data.hpp"
#include "assets/profile.hpp"
#include "assets/sp_menu.hpp"
#include "assets/tweak_data.hpp"
#include "assets/credit_data.hpp"
#include "assets/ui_assets.hpp"
#include "game/input.hpp"
#include "ui/input_devices.hpp"
#include "ui/menu.hpp"
#include "ui/mp_setup.hpp"

namespace nf {

// What the front end (or the in-game pause menu) asks the application to do when it closes.
struct FrontendResult {
    enum class Action {
        None,
        StartMultiplayer,  // P_MPCONFIRM: local split-screen multiplayer
        StartOnlineJoin,   // the multiplayer main-menu choice: open the online server browser
        StartListenServer, // the multiplayer setup finished in host mode
        OpenSettings,      // main menu Square: the Settings screen (display, audio, accessibility)
        StartMission,      // P_NFSELECT/P_NFMAP: `level_bin` + `difficulty`
        Resume,           // the pause menu was closed (start button / "Resume")
        RestartMission,   // pause menu "Restart", or P_ENDMISSION "retry"
        QuitToMenu,       // pause menu "Quit", P_ENDMISSION "quit", debriefing "Continue"
        MpRematch,        // P_MPDEBRIEFING "Replay": start `launch` again
        MissionDone,      // P_NFRESULTS accepted (the results chain ran its course)
        Quit,
    };
    Action action = Action::None;
    std::string level_bin;         // FILES.BIN name of the chosen level (mission or multiplayer arena)
    std::uint32_t level_id = 0;    // GameState level id (the number in the bin name)
    int difficulty = 0;            // 0 Agent, 1 Secret Agent, 2 00 Agent
    std::optional<MpLaunch> launch;  // multiplayer: mode, map, humans, bots and rules (MpSettings) + participants
    int end_choice = -1;           // P_ENDMISSION radio: 0 retry mission, 1 retry from the base map
    // The devices that claimed the P_MPJOIN slots (filled by the application's pad feed; all empty when the
    // join page was driven without device claims).
    std::array<SlotDevice, 4> slot_devices{};
};

// Session options edited by the options pages (P_CNOPTIONS 0x2d, P_CNAVOPTIONS 0x31, P_CNCONTROLS 0x22,
// P_CNMPOPTIONS 0x2e, P_SCREENADJUST 0x47). The game reads them after the frontend closes and applies
// them to PlayerSetting / the audio system / the display.
struct GameOptions {
    // P_CNOPTIONS radios, in control order (menu labels verified on the "Advanced Options" page).
    // The comments name the PlayerSetting byte each radio stores (0x4b accept).
    bool vibration = false;        // PlayerSetting[9] ("Vibration")
    bool auto_aim = false;         // PlayerSetting[1] ("Auto Aim")
    bool crosshairs = false;       // PlayerSetting[8] ("Crosshairs")
    bool crouch_toggle = true;     // PlayerSetting[4] ("Crouch", Toggle/Hold rows 0x2a2/0x2a3)
    bool manual_aim = false;       // PlayerSetting[3] ("Manual Aim", Toggle/Hold rows)
    bool weapon_auto_switch = false;  // PlayerSetting[10] ("Weapon Auto Switch")
    bool flashing_objects = false;    // PlayerSetting[0xC] ("Flashing Objects", rows reversed: Off first)
    bool hud_always_on = false;    // PlayerSetting[0xB] ("HUD Always On", rows reversed: Off first)
    // P_CNAVOPTIONS sliders and radios (page 0x31).
    int music_volume = 70;         // slider 0x135, 0..100 step 5 (SFXMusicSetVolume)
    int sfx_volume = 100;          // slider 0x134, 0..100 step 5 (SFXSetVolume)
    bool subtitles = true;         // radio 0x136
    int split_screen = 0;          // radio 0x19a: 0 Horizontal, 1 Vertical
    int speaker = 1;               // radio 0x198: 0 Mono, 1 Stereo, 2 Surround (SFXGetMode)
    bool widescreen = false;       // radio 0x223
    int screen_x = 0, screen_y = 0;  // psi screen position (screen adjust page 0x47)
    // P_CNMPOPTIONS radios ("Multiplayer Options" page): MPSettings+0x40/+0x44 and PlayerSetting[2].
    bool mp_radar = false;         // "Radar"
    int mp_handicap = 0;           // "Health Handicap": row values -75..100
    bool mp_auto_aim = false;      // PlayerSetting[2] ("Auto Aim")
};

// One mission-results screen (P_NFRESULTS 0x36, P_NFSTATS 0x37, P_NFBONUS 0x38): what only the running
// game knows. The handlers fill the labels exactly as the originals do (score line 0x167, name line
// 0x1d1, subtitle 0x2b, next-target 0x168, medal sprite 0x166); the game pre-formats the numbers.
struct MissionResults {
    std::string score_text;        // PlrStat score with thousands separators (label 0x167)
    std::string name_line;         // rank / "new best" line (label 0x1d1)
    std::uint32_t subtitle_label = 0;  // rank subtitle (label 0x2b), 0 = leave the script text
    std::string next_target_text;  // next rank target (label 0x168)
    std::uint32_t medal_sprite = 0;    // medal sprite hash (label 0x166), 0 = no medal
    int bonus_kind = 0;            // Menu_SetLevelBonus kind the results awarded (P_NFBONUS chain)
    std::string bonus_text;        // P_NFBONUS bonus lines (label 0x1d0), empty = none
    bool leads_to_wingame = false;  // P_NFBONUS accept goes to P_WINGAME (else P_NFMAP)
    // P_NFSTATS rows: pre-formatted "label / value" stat lines (list 0x22).
    std::vector<std::pair<std::string, std::string>> stats;
};

// Encyclopedia content for the dossier pages (P_DSWEAPONS 0x3d, P_DSGADGETS 0x3c, P_DSRECORDS 0x3a,
// P_DSREWARDS 0x3b): what only the game knows (upgrade levels, PlrStats totals, reward masks).
struct DossierInfo {
    std::uint32_t weapon_sprite = 0;   // current upgrade level sprite (label 0x173)
    std::uint32_t weapon_name = 0;     // label hash of the weapon name
    std::uint32_t weapon_desc = 0;     // label hash of the weapon description
    std::uint32_t gadget_sprite = 0;
    std::uint32_t gadget_name = 0;
    std::uint32_t gadget_desc = 0;
    std::vector<std::pair<std::string, std::string>> records;  // mission records rows
    std::vector<std::pair<std::string, std::string>> rewards;  // reward rows
};

// One P_MPDEBRIEFING 0x33 table row (MP_SortOutWhoWon order, best first): what only the match knows.
// The handler fills the per-row labels (name 0x156, portrait 0x157, short name 0x158, numbers
// 0x159/0x240/0x15a, score 0x15c) and the banner (label 0x23d, e.g. "X Won" / "A Draw").
struct DebriefRow {
    std::string name;
    int character = 0;             // mp_characters row (portrait sprite + short name)
    int kills = 0, deaths = 0;
    float points = 0;
    int score = 0;                 // Menu_GetMPScore
    bool is_bot = false;
    int slot = -1;                 // participant slot (Extended per-player colour), -1 unknown
};
struct DebriefInfo {
    std::vector<DebriefRow> rows;  // ranked; up to 4 on the original page, more (Extended) in a table
    std::string banner;            // winner line, pre-formatted
    std::size_t slot_count = 8;    // rule-set capacity: the PS2 page shows the top four, larger sets a full table
    bool table() const { return slot_count > 8 && rows.size() > 4; }
};

enum class FrontendMode { MainMenu, Pause };

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
    // `mp` / `sp` / `tweaks` may be null when those pages are not needed; all must outlive the Frontend.
    Frontend(const UiAssets& assets, const MenuFile& menu, const MpData* mp = nullptr,
             const SpMenuData* sp = nullptr, const TweakData* tweaks = nullptr);
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
    void set_dossier(DossierInfo info);
    PlayerOptions& player_options();
    void set_mission_results(MissionResults results);
    void set_debriefing(DebriefInfo info);
    GameOptions& game_options();
    const std::string& profile_name() const;
    // Memory-card profiles (files under the user config dir): the active codename save.
    // set_profile applies name, options, unlock-relevant bonus and controller setup at once.
    void set_profile(Profile profile);
    const Profile& profile() const;
    // Records a finished mission (score/medal) into the active profile.
    void complete_mission(std::uint32_t level_id, int score, int medal);
    // Snapshots options/unlocks/bonuses/cheats into the active profile file. False = unwritable.
    bool save_profile();
    // Session cheat flags (P_TWEAKS/P_TWEAKS2, C_CH*): nonzero = armed. Scroll cheats
    // (e.g. C_CHCHHEALTH) store their level, button cheats toggle 1/0.
    bool tweak(std::uint32_t control) const;
    int tweak_value(std::uint32_t control) const;
    void set_tweak(std::uint32_t control, int value);
    // Live tuning values (P_TWEAKS/P_TWEAKS2 scrolls): the game seeds them (boot defaults come
    // from TweakData) and applies them to the damage globals; accept stores shown / scale.
    const std::map<std::uint32_t, float>& tweak_vars() const;
    // C_NIS accept requests a script play (script hash, 0 = header row): the game plays it.
    std::uint32_t take_nis_request();
    // Movie pages request their PSS id (take_movie_request clears it and hands the transition to
    // the game); movie_finished runs the post-movie transition when playback ends.
    std::uint32_t take_movie_request();
    void movie_finished();
    // Credits roll rows for P_CREDITS (load_credits); empty = the script's static text.
    void set_credits(std::vector<CreditRow> rows);

    // The menu runtime and multiplayer model, for tools and tests.
    ui::MenuManager* manager();
    MpSetup* mp_setup();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace nf

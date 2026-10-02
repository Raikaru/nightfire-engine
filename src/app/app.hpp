// Shared context of the `nightfire` application: the disc data loaded once at
// boot (menu script, MP/SP tables, HUD data, music table) plus the persisted
// settings. Sessions (SP / MP / driving, see session_*.hpp) borrow it.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "assets/elf.hpp"
#include "assets/game_files.hpp"
#include "assets/hud_data.hpp"
#include "assets/level_music.hpp"
#include "assets/menu_file.hpp"
#include "assets/mp_data.hpp"
#include "assets/sp_menu.hpp"
#include "assets/ui_assets.hpp"
#include "core/math.hpp"
#include "render/level_renderer.hpp"  // Camera

namespace nf::app {

// Persisted settings (docs/ui.md "Front end": the P_CNOPTIONS / P_CNAVOPTIONS /
// P_CNCONTROLS / P_CNMPOPTIONS pages). Stored as key=value in
// ~/.config/nightfire/nightfire.cfg (see config.cpp); the options pages edit
// the live GameOptions/PlayerOptions and the app writes them back on change.
struct AppConfig {
    // Audio (P_CNAVOPTIONS sliders 0x134/0x135, applied via SFXSetVolume/SFXMusicSetVolume).
    int sfx_volume = 100;
    int music_volume = 70;
    // Controls (P_CNCONTROLS per-player: style + Y inversion).
    int controller_style = 7;
    bool invert_y = false;
    // Session options (P_CNOPTIONS radios, PlayerSetting bytes; see GameOptions).
    bool vibration = false;
    bool auto_aim = false;
    bool crosshairs = true;
    bool crouch_toggle = true;
    bool manual_aim = false;
    bool weapon_auto_switch = false;
    bool hud_always_on = false;
    // Display (P_CNAVOPTIONS speaker/widescreen + screen adjust).
    int speaker = 1;
    bool widescreen = false;
    int screen_x = 0, screen_y = 0;
    // Last used profile / SP difficulty (convenience, not on the disc).
    std::string profile;
    int difficulty = 1;  // 0 Agent, 1 Secret Agent, 2 00 Agent
};

std::filesystem::path config_path();
bool load_config(const std::filesystem::path& path, AppConfig& cfg);
bool save_config(const std::filesystem::path& path, const AppConfig& cfg);

// Everything loaded from the disc at boot. `assets` carries the frontend
// sprites + strings; sessions add their level's sprites to it.
struct AppContext {
    std::string gamedir;
    GameFiles files;
    Elf32 action_elf;
    UiAssets assets;
    MenuFile menu;
    MpData mp_data;
    SpMenuData sp_data;
    HudData hud_data;
    LevelMusicTable music;

    AppContext(const std::string& gamedir, GameFiles files, Elf32 elf, UiAssets assets, MenuFile menu, MpData mp,
               SpMenuData sp, HudData hud, LevelMusicTable music);
};

std::unique_ptr<AppContext> load_context(const std::string& gamedir);

// Camera helpers (nfgame convention: Camera looks down -Z at yaw 0, the
// player down +Z, so the camera yaw is the player yaw + pi).
Camera camera_for_eye_yaw_pitch(const Vec3& eye, float yaw, float pitch);
float camera_aspect(int width, int height);
// Game viewport (Camera_CalcViewAngles decomp): widescreen (AppConfig.widescreen,
// dword_2A37C4) renders 16:9, otherwise 4:3 pillarboxed into the window.
float game_aspect(const AppConfig& cfg);
struct GameView {
    int x = 0, y = 0, w = 0, h = 0;  // window pixels, origin top left
    float aspect = 4.0f / 3.0f;
};
GameView game_view(const AppConfig& cfg, int width, int height);

}  // namespace nf::app

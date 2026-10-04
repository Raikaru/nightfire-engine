// `nightfire` settings persistence + boot-time disc loading.
#include "app/app.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <algorithm>

#include "assets/menu_validate.hpp"
#include "assets/sp_menu.hpp"
#include "ui/input_devices.hpp"

namespace nf::app {

std::filesystem::path config_path() {
#if defined(_WIN32)
    if (const char* appdata = std::getenv("APPDATA"); appdata && *appdata)
        return std::filesystem::path(appdata) / "Nightfire" / "nightfire.cfg";
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME"); home && *home)
        return std::filesystem::path(home) / "Library" / "Application Support" / "Nightfire" / "nightfire.cfg";
#else
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        return std::filesystem::path(xdg) / "nightfire" / "nightfire.cfg";
    if (const char* home = std::getenv("HOME"); home && *home)
        return std::filesystem::path(home) / ".config" / "nightfire" / "nightfire.cfg";
#endif
    return std::filesystem::path("nightfire.cfg");
}

std::filesystem::path user_data_path() {
#if defined(_WIN32)
    if (const char* appdata = std::getenv("APPDATA"); appdata && *appdata)
        return std::filesystem::path(appdata) / "Nightfire" / "data";
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME"); home && *home)
        return std::filesystem::path(home) / "Library" / "Application Support" / "Nightfire" / "data";
#else
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg)
        return std::filesystem::path(xdg) / "nightfire";
    if (const char* home = std::getenv("HOME"); home && *home)
        return std::filesystem::path(home) / ".local" / "share" / "nightfire";
#endif
    return std::filesystem::path("nightfire-data");
}

namespace {

void set_key(AppConfig& c, const std::string& key, const std::string& value) {
    const int n = std::atoi(value.c_str());
    const bool b = n != 0;
    if (key == "game_dir") c.game_dir = value;
    else if (key == "sfx_volume") c.sfx_volume = std::clamp(n, 0, 100);
    else if (key == "music_volume") c.music_volume = std::clamp(n, 0, 100);
    else if (key == "controller_style") c.controller_style = n;
    else if (key == "invert_y") c.invert_y = b;
    else if (key == "vibration") c.vibration = b;
    else if (key == "auto_aim") c.auto_aim = b;
    else if (key == "crosshairs") c.crosshairs = b;
    else if (key == "crouch_toggle") c.crouch_toggle = b;
    else if (key == "manual_aim") c.manual_aim = b;
    else if (key == "weapon_auto_switch") c.weapon_auto_switch = b;
    else if (key == "hud_always_on") c.hud_always_on = b;
    else if (key == "speaker") c.speaker = std::clamp(n, 0, 2);
    else if (key == "widescreen") c.widescreen = b;
    else if (key == "pillarbox") c.pillarbox = b;
    else if (key == "screen_x") c.screen_x = n;
    else if (key == "screen_y") c.screen_y = n;
    else if (key == "fullscreen") c.fullscreen = b;
    else if (key == "vsync") c.vsync = b;
    else if (key == "crosshair_style") c.crosshair_style = std::clamp(n, 0, 4);
    else if (key == "high_contrast") c.high_contrast = b;
    else if (key == "colorblind_teams") c.colorblind_teams = b;
    else if (key == "favourite_server") {
        if (!value.empty() && std::find(c.favourite_servers.begin(), c.favourite_servers.end(), value) ==
                                  c.favourite_servers.end())
            c.favourite_servers.push_back(value);
    }
    else if (key == "profile") c.profile = value;
    else if (key == "difficulty") c.difficulty = std::clamp(n, 0, 2);
    else input_bindings().set(key, value);   // bind_<context>_<button> / pad_<context>_<button>
}

}  // namespace

bool load_config(const std::filesystem::path& path, AppConfig& cfg) {
    std::ifstream in(path);
    if (!in) return false;
    std::string line;
    while (std::getline(in, line)) {
        if (const auto hash = line.find('#'); hash != std::string::npos) line.resize(hash);
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        set_key(cfg, line.substr(0, eq), line.substr(eq + 1));
    }
    return true;
}

bool save_config(const std::filesystem::path& path, const AppConfig& c) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path);
    if (!out) return false;
    out << "game_dir=" << c.game_dir << "\n";
    out << "# nightfire settings (options pages write this back on change)\n";
    out << "sfx_volume=" << c.sfx_volume << "\nmusic_volume=" << c.music_volume << "\n";
    out << "controller_style=" << c.controller_style << "\ninvert_y=" << (c.invert_y ? 1 : 0) << "\n";
    out << "vibration=" << (c.vibration ? 1 : 0) << "\nauto_aim=" << (c.auto_aim ? 1 : 0) << "\n";
    out << "crosshairs=" << (c.crosshairs ? 1 : 0) << "\ncrouch_toggle=" << (c.crouch_toggle ? 1 : 0) << "\n";
    out << "manual_aim=" << (c.manual_aim ? 1 : 0) << "\nweapon_auto_switch=" << (c.weapon_auto_switch ? 1 : 0) << "\n";
    out << "hud_always_on=" << (c.hud_always_on ? 1 : 0) << "\n";
    out << "speaker=" << c.speaker << "\nwidescreen=" << (c.widescreen ? 1 : 0) << "\n";
    out << "pillarbox=" << (c.pillarbox ? 1 : 0) << "\n";
    out << "screen_x=" << c.screen_x << "\nscreen_y=" << c.screen_y << "\n";
    out << "fullscreen=" << (c.fullscreen ? 1 : 0) << "\nvsync=" << (c.vsync ? 1 : 0) << "\n";
    out << "crosshair_style=" << c.crosshair_style << "\nhigh_contrast=" << (c.high_contrast ? 1 : 0) << "\n";
    out << "colorblind_teams=" << (c.colorblind_teams ? 1 : 0) << "\n";
    for (const std::string& server : c.favourite_servers) out << "favourite_server=" << server << "\n";
    out << "profile=" << c.profile << "\ndifficulty=" << c.difficulty << "\n";
    out << "# button bindings per context (docs/ui.md \"Button prompts\")\n";
    input_bindings().save(out);
    return bool(out);
}

AppContext::AppContext(const std::string& dir, GameFiles f, Elf32 elf, UiAssets a, MenuFile m, MpData mp, SpMenuData sp,
                       HudData hud, LevelMusicTable mus)
    : gamedir(dir),
      files(std::move(f)),
      action_elf(std::move(elf)),
      assets(std::move(a)),
      menu(std::move(m)),
      mp_data(std::move(mp)),
      sp_data(std::move(sp)),
      hud_data(std::move(hud)),
      music(std::move(mus)) {}

std::unique_ptr<AppContext> load_context(const std::string& gamedir) {
    GameFiles files(gamedir);
    Elf32 elf(read_file(std::filesystem::path(gamedir) / "ACTION.ELF"));
    UiAssets assets = load_ui_assets(gamedir, files);
    const GameFile* frontend = files.find(std::string(kFrontEndBin));
    if (!frontend) throw std::runtime_error("FILES.BIN has no " + std::string(kFrontEndBin));
    MenuFile menu = load_menu_from_bin(Bytes(files.read(*frontend)));
    MpData mp = load_mp_data(files, gamedir, assets.strings);
    SpMenuData sp = load_sp_menu(elf);
    HudData hud = load_hud_data(elf);
    LevelMusicTable music(gamedir);
    return std::make_unique<AppContext>(gamedir, std::move(files), std::move(elf), std::move(assets), std::move(menu),
                                        std::move(mp), std::move(sp), std::move(hud), std::move(music));
}

namespace {
// The original's frame is also our view convention here: Camera looks down
// -Z at yaw 0 while the player looks down +Z (see nfgame's camera_for).
constexpr float kPi = 3.14159265358979f;
}  // namespace

Camera camera_for_eye_yaw_pitch(const Vec3& eye, float yaw, float pitch) {
    Camera cam;
    cam.eye = eye;
    cam.yaw = yaw + kPi;
    cam.pitch = pitch;
    return cam;
}

float camera_aspect(int width, int height) { return float(width) / float(std::max(height, 1)); }

float game_aspect(const AppConfig& cfg) { return cfg.pillarbox ? 4.0f / 3.0f : 16.0f / 9.0f; }

GameView game_view(const AppConfig& cfg, int width, int height) {
    const float aspect = cfg.pillarbox ? 4.0f / 3.0f : camera_aspect(width, height);
    int view_w = width, view_h = height;
    if (float(width) / float(std::max(height, 1)) > aspect) view_w = int(float(height) * aspect + 0.5f);
    else view_h = int(float(width) / aspect + 0.5f);
    return {(width - view_w) / 2, (height - view_h) / 2, view_w, view_h, aspect};
}

}  // namespace nf::app

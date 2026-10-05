// nightfire: playable game and first-run user-data setup. The first-launch
// wizard validates the user's own Nightfire PS2 USA disc and installs required
// files to the platform per-user data directory. Settings persist in the
// platform config directory; headless --shot/--frames/--inputs verify sessions.
#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "app/frontend_pads.hpp"
#include "app/menu_background.hpp"
#include "app/app.hpp"
#include "app/movie.hpp"
#include "app/session_drive.hpp"
#include "app/session_mp.hpp"
#include "app/session_sp.hpp"
#include "app/server_browser.hpp"
#include "core/rng.hpp"
#include "net/server_runtime.hpp"
#include "app/online_browser.hpp"
#include "app/settings_screen.hpp"
#include "app/net_client.hpp"
#include "audio/audio.hpp"
#include "render/window.hpp"
#include "ui/art_sheet.hpp"
#include "ui/frontend.hpp"
#include "ui/input_devices.hpp"
#include "ui/menu_audio.hpp"
#include "ui/prompts.hpp"
#include "driving/driving_level.hpp"
#include "ui/renderer.hpp"
#include "ui/text.hpp"
#include "app/setup_wizard.hpp"

namespace nf::app {

namespace {

constexpr int kWindowW = 1280, kWindowH = 720;

const std::map<std::string, std::uint16_t> kPressButtons = {
    {"up", kPadUp},       {"down", kPadDown},     {"left", kPadLeft},   {"right", kPadRight},
    {"cross", kPadCross}, {"circle", kPadCircle}, {"square", kPadSquare}, {"triangle", kPadTriangle},
    {"start", kPadStart}, {"select", kPadSelect}, {"l1", kPadL1},       {"l2", kPadL2},
    {"r1", kPadR1},       {"r2", kPadR2},         {"l3", kPadL3},       {"r3", kPadR3}};

// Virtual gamepads (--virtual-pads): real SDL devices the replay can press, for multi-device captures.
std::vector<SDL_Joystick*> g_virtual_pads;

void attach_virtual_pads(const std::string& list) {
    // Headless captures have no focused window; SDL drops joystick input of background apps by default.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    for (std::size_t p = 0; p < list.size();) {
        const std::size_t e = list.find(',', p);
        const std::string kind = list.substr(p, e == std::string::npos ? e : e - p);
        p = e == std::string::npos ? list.size() : e + 1;
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
        desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        if (kind == "xbox") desc.vendor_id = 0x045E, desc.product_id = 0x02EA, desc.name = "Virtual Xbox pad";
        else if (kind == "ps") desc.vendor_id = 0x054C, desc.product_id = 0x09CC, desc.name = "Virtual PlayStation pad";
        else throw std::runtime_error("--virtual-pads takes xbox and ps entries");
        const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
        SDL_Joystick* joystick = id ? SDL_OpenJoystick(id) : nullptr;
        if (!joystick) throw std::runtime_error(std::string("virtual gamepad: ") + SDL_GetError());
        std::printf("virtual pad %zu: %s, gamepad=%d type=%s\n", g_virtual_pads.size(), desc.name, int(SDL_IsGamepad(id)),
                    SDL_GetGamepadStringForType(SDL_GetGamepadTypeForID(id)));
        g_virtual_pads.push_back(joystick);
    }
    SDL_UpdateJoysticks();
}

void set_virtual_pad(std::size_t index, std::uint16_t buttons) {
    if (index >= g_virtual_pads.size()) throw std::runtime_error("no virtual pad " + std::to_string(index));
    static constexpr std::pair<std::uint16_t, SDL_GamepadButton> kMap[] = {
        {kPadCross, SDL_GAMEPAD_BUTTON_SOUTH},      {kPadCircle, SDL_GAMEPAD_BUTTON_EAST},
        {kPadSquare, SDL_GAMEPAD_BUTTON_WEST},      {kPadTriangle, SDL_GAMEPAD_BUTTON_NORTH},
        {kPadUp, SDL_GAMEPAD_BUTTON_DPAD_UP},       {kPadDown, SDL_GAMEPAD_BUTTON_DPAD_DOWN},
        {kPadLeft, SDL_GAMEPAD_BUTTON_DPAD_LEFT},   {kPadRight, SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
        {kPadStart, SDL_GAMEPAD_BUTTON_START},      {kPadSelect, SDL_GAMEPAD_BUTTON_BACK},
        {kPadL1, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER}, {kPadR1, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER}};
    for (const auto& [bit, button] : kMap) SDL_SetJoystickVirtualButton(g_virtual_pads[index], button, (buttons & bit) != 0);
    SDL_SetJoystickVirtualAxis(g_virtual_pads[index], SDL_GAMEPAD_AXIS_LEFT_TRIGGER, (buttons & kPadL2) ? 32767 : -32768);
    SDL_SetJoystickVirtualAxis(g_virtual_pads[index], SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, (buttons & kPadR2) ? 32767 : -32768);
    SDL_UpdateJoysticks();
}

// --press replay (nfui token format): one 30 Hz frame per token. Plain tokens press slot 0 directly;
// `kb:<buttons>` presses through the keyboard bindings and `pad<N>:<buttons>` virtual gamepad N, so the
// multiplayer join page sees separate devices.
template <typename AdvanceBackground>
void replay_press(Frontend& frontend, FrontendPads& pads, const std::string& script, AdvanceBackground&& advance_background) {
    for (std::size_t p = 0; p < script.size();) {
        const std::size_t e = script.find(',', p);
        std::string tok = script.substr(p, e == std::string::npos ? e : e - p);
        p = e == std::string::npos ? script.size() : e + 1;
        // Reserved for the Online server-browser and Settings screens.
        if (tok.rfind("net-", 0) == 0 || tok.rfind("set-", 0) == 0) continue;
        if (tok.rfind("wait", 0) == 0) {
            const int n = tok.size() > 4 ? std::atoi(tok.c_str() + 4) : 1;
            for (int i = 0; i < n; ++i) {
                pads.sample(frontend);
                advance_background();
            }
            continue;
        }
        std::string device;
        if (const std::size_t colon = tok.find(':'); colon != std::string::npos) {
            device = tok.substr(0, colon);
            tok = tok.substr(colon + 1);
        }
        std::uint16_t buttons = 0;
        for (std::size_t q = 0; q < tok.size();) {
            const std::size_t plus = tok.find('+', q);
            const std::string name = tok.substr(q, plus == std::string::npos ? plus : plus - q);
            q = plus == std::string::npos ? tok.size() : plus + 1;
            const auto it = kPressButtons.find(name);
            if (it == kPressButtons.end()) throw std::runtime_error("unknown button " + name);
            buttons |= it->second;
        }
        if (device.empty()) {
            pads.push_slot0(frontend, buttons);
            advance_background();
            pads.push_slot0(frontend, 0);
        } else if (device == "kb") {
            pads.sample(frontend, buttons);
            advance_background();
            pads.sample(frontend);
        } else if (device.rfind("pad", 0) == 0) {
            const std::size_t index = std::size_t(std::atoi(device.c_str() + 3));
            set_virtual_pad(index, buttons);
            pads.sample(frontend);
            advance_background();
            set_virtual_pad(index, 0);
            pads.sample(frontend);
        } else {
            throw std::runtime_error("unknown device " + device);
        }
        advance_background();
    }
}

void sync_config_to_frontend(const AppConfig& cfg, Frontend& frontend) {
    GameOptions& o = frontend.game_options();
    o.music_volume = cfg.music_volume;
    o.sfx_volume = cfg.sfx_volume;
    o.vibration = cfg.vibration;
    o.auto_aim = cfg.auto_aim;
    o.mp_auto_aim = cfg.mp_auto_aim;
    o.crosshairs = cfg.crosshairs;
    o.crouch_toggle = cfg.crouch_toggle;
    o.manual_aim = cfg.manual_aim;
    o.weapon_auto_switch = cfg.weapon_auto_switch;
    o.hud_always_on = cfg.hud_always_on;
    o.speaker = cfg.speaker;
    o.widescreen = !cfg.pillarbox;
    o.screen_x = cfg.screen_x;
    o.screen_y = cfg.screen_y;
    PlayerOptions& p = frontend.player_options();
    p.controller_style = cfg.controller_style;
    p.invert_y = cfg.invert_y;
}

void report_result(const FrontendResult& r) {
    static const char* const names[] = {"none", "start-multiplayer", "start-online-join", "start-listen-server",
                                        "open-settings", "start-mission", "resume", "restart-mission", "quit-to-menu",
                                        "rematch", "mission-done", "quit"};
    std::printf("frontend: %s level=%s difficulty=%d", names[int(r.action)], r.level_bin.c_str(), r.difficulty);
    if (r.launch) std::printf(" mp mode=0x%08x humans=%u bots=%u", r.launch->settings.mode,
                              r.launch->settings.human_count, r.launch->settings.bot_count);
    std::printf("\n");
}

// One frontend screen (main menu at boot, or after a session ends). Returns
// false when the application should exit.
bool run_frontend(AppContext& ctx, Window& window, ui::Renderer& ui, ui::TextRenderer& text, AppConfig& cfg,
                  audio::AudioSystem* audio, const std::string& press, const std::string& shot,
                  const std::optional<std::uint32_t>& first_page, FrontendResult& out, bool match_takes_shot = false) {
    Frontend frontend(ctx.assets, ctx.menu, &ctx.mp_data, &ctx.sp_data);
    sync_config_to_frontend(cfg, frontend);
    frontend.open(FrontendMode::MainMenu, first_page);
    MenuBackground background(ctx.gamedir);
    background.advance();
    FrontendPads pads;
    ui::select_prompts(0, InputContext::Menu);
    bool trace = true;
    std::uint32_t last_page = 0;
    auto play_sounds = [&] {
        if (!audio) return;
        for (ui::MenuSound s : frontend.take_sounds()) {
            const std::uint32_t id = menu_sound_sfx(s);
            if (id != 0) audio->play_sfx(id);
        }
        audio->update();
    };
    if (!press.empty() || !shot.empty()) {
        // Headless verification: replay the button script, serve a pending movie
        // request into the shot when the script lands on a movie page, else screenshot.
        if (!press.empty()) replay_press(frontend, pads, press, [&] { background.advance(); });
        play_sounds();
        report_result(frontend.result());
        out = frontend.result();
        out.slot_devices = pads.slot_devices();
        if (std::uint32_t movie = frontend.take_movie_request()) {
            if (shot.empty()) {
                // Probe run: note the request and take the fallback transition (no playback).
                std::printf("frontend movie %08X requested (use --shot to capture it)\n", movie);
            } else {
                MovieScreen screen(window, ui, text, audio, nullptr, ctx.gamedir);
                screen.play(movie, shot, 90);
            }
            frontend.movie_finished();
            return false;
        }
        // With --frames the match the replay starts takes the screenshot instead (see run()).
        if (!shot.empty() && !(match_takes_shot && out.action == FrontendResult::Action::StartMultiplayer)) {
            int w, h;
            window.begin_frame(w, h);
            ui.begin(w, h);
            background.draw(ui);
            frontend.draw(ui, text);
            ui.end();
            if (!window.save_bmp(shot)) throw std::runtime_error("could not save frontend screenshot");
            std::printf("frontend page 0x%08x -> %s\n", frontend.page_id(), shot.c_str());
            window.swap();
        }
        return false;
    }
    SDL_Gamepad* gamepad = input_devices().gamepads().empty() ? nullptr : input_devices().gamepads().front();
    double accumulator = 0;
    Uint64 last = SDL_GetTicksNS();
    bool quit_app = false;
    while (!frontend.wants_close() && !quit_app) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) quit_app = true;
        }
        const Uint64 now = SDL_GetTicksNS();
        accumulator = std::min(accumulator + double(now - last) * 1e-9, 0.25);
        last = now;
        while (accumulator >= 1.0 / 30.0) {
            pads.sample(frontend);
            background.advance();
            // Movie pages hand their PSS id to the game (fallback transition when missing).
            if (std::uint32_t movie = frontend.take_movie_request()) {
                MovieScreen screen(window, ui, text, audio, gamepad, ctx.gamedir);
                screen.play(movie);
                frontend.movie_finished();
            }
            accumulator -= 1.0 / 30.0;
        }
        if (trace && frontend.page_id() != last_page) {
            last_page = frontend.page_id();
            std::printf("frontend page 0x%08x\n", last_page);
        }
        int w, h;
        window.begin_frame(w, h);
        ui::select_prompts(0, InputContext::Menu);
        ui.begin(w, h);
        background.draw(ui);
        frontend.draw(ui, text);
        ui.end();
        window.swap();
    }
    if (quit_app) {
        out = FrontendResult{};
        out.action = FrontendResult::Action::Quit;
        return false;
    }
    // Options pages write back into the frontend's live options; persist them.
    const GameOptions& o = frontend.game_options();
    cfg.music_volume = o.music_volume;
    cfg.sfx_volume = o.sfx_volume;
    cfg.vibration = o.vibration;
    cfg.auto_aim = o.auto_aim;
    cfg.mp_auto_aim = o.mp_auto_aim;
    cfg.crosshairs = o.crosshairs;
    cfg.crouch_toggle = o.crouch_toggle;
    cfg.manual_aim = o.manual_aim;
    cfg.weapon_auto_switch = o.weapon_auto_switch;
    cfg.hud_always_on = o.hud_always_on;
    cfg.widescreen = o.widescreen;
    cfg.pillarbox = !o.widescreen;
    cfg.screen_x = o.screen_x;
    cfg.screen_y = o.screen_y;
    const PlayerOptions& p = frontend.player_options();
    cfg.controller_style = p.controller_style;
    cfg.invert_y = p.invert_y;
    save_config(config_path(), cfg);
    ui.set_pillarbox(cfg.pillarbox);
    report_result(frontend.result());
    out = frontend.result();
    out.slot_devices = pads.slot_devices();
    return true;
}

struct Args {
    std::string gamedir;
    bool help = false;
    // Session selection (empty = boot to the frontend).
    std::string mission;
    int difficulty = -1;
    std::vector<std::pair<int, int>> channels;  // --channel CH=VAL presets (nfgame debug hook)
    bool mp = false;
    std::vector<std::string> mp_args;
    std::string drive;
    std::string car;
    std::string movie;  // direct PSS test: hex id or .PSS path
    long frames = -1;
    int logic_hz = 60;
    std::string shot;
    std::string inputs;
    std::string press;
    std::optional<std::uint32_t> first_page;  // start on a menu page, skipping title/movie navigation
    bool mute = false;
    std::string connect;
    std::string password;
    std::string master;
    std::string online_master;
    std::string direct_lookup;
    bool browse_lan = false, browse_master = false;
    std::string map = "07000024.bin";
    std::string player_name = "Player";
    std::string chat;
    int net_sim_loss = 0, net_sim_latency = 0;
    bool net_sim_jitter = false;
    int listen_port = 27500;
    float host_time_limit = -1.0f;  // optional minutes; useful for short rotation tests
    int give = -1;
    int local_players = 1;
    int width = kWindowW, height = kWindowH;
    // Accessibility overrides for this run (the Settings screen's options; not saved to nightfire.cfg).
    std::optional<int> crosshair_style;
    bool high_contrast = false, colorblind_teams = false;
    std::optional<InputDevice> prompts;   // --prompts: fixed glyph set instead of the last used device
    std::string virtual_pads;             // --virtual-pads xbox,ps: SDL virtual gamepads (pad<N>: --press tokens)
    std::string hold;                     // --hold pad0:r1,pad1:l1: virtual pad buttons held during the match
};
bool parse_args(int argc, char** argv, Args& a) {
    // --help works without a positional game directory.
    for (int i = 1; i < argc; ++i) {
        const std::string v = argv[i];
        if (v == "--help" || v == "-h") {
            a.help = true;
            return true;
        }
    }
    int first_option = 1;
    if (argc > 1 && argv[1][0] != '-') {
        a.gamedir = argv[1];
        first_option = 2;
    }
    for (int i = first_option; i < argc; ++i) {
        const std::string v = argv[i];
        auto need = [&](std::string& dst) {
            if (i + 1 >= argc) throw std::runtime_error(v + " needs a value");
            dst = argv[++i];
        };
        if (v == "--help" || v == "-h") a.help = true;
        else if (v == "--mission") need(a.mission);
        else if (v == "--difficulty" && i + 1 < argc) a.difficulty = std::atoi(argv[++i]);
        else if (v == "--channel" && i + 1 < argc) {  // debug: preset a mission switch channel (CH=VAL)
            const std::string spec = argv[++i];
            const auto eq = spec.find('=');
            if (eq == std::string::npos) throw std::runtime_error("--channel needs CH=VAL");
            a.channels.emplace_back(std::atoi(spec.substr(0, eq).c_str()), std::atoi(spec.substr(eq + 1).c_str()));
        }
        else if (v == "--logic-hz" && i + 1 < argc) {
            a.logic_hz = std::atoi(argv[++i]);
            if (a.logic_hz != 30 && a.logic_hz != 60)
                throw std::runtime_error("--logic-hz must be 30 or 60");
        }
        else if (v == "--mp") a.mp = true;
        else if (v == "--local-players" && i + 1 < argc) {
            a.local_players = std::atoi(argv[++i]);
            if (a.local_players < 1 || a.local_players > int(nf::net::kMaxLocalPlayers))
                throw std::runtime_error("--local-players must be 1..4");
        }
        else if (v == "--connect") need(a.connect);
        else if (v == "--password") need(a.password);
        else if (v == "--browse-lan") a.browse_lan = true;
        else if (v == "--browse-master") { a.browse_master = true; need(a.master); }
        else if (v == "--online-master") need(a.online_master);
        else if (v == "--browse-ip") need(a.direct_lookup);
        else if (v == "--map") need(a.map);
        else if (v == "--listen-port" && i + 1 < argc) {
            const long port = std::strtol(argv[++i], nullptr, 10);
            if (port < 1 || port > 65535) throw std::runtime_error("--listen-port must be 1..65535");
            a.listen_port = int(port);
        } else if (v == "--host-time-limit" && i + 1 < argc) {
            char* end = nullptr;
            a.host_time_limit = std::strtof(argv[++i], &end);
            if (!end || *end || !(a.host_time_limit > 0.0f))
                throw std::runtime_error("--host-time-limit must be positive minutes");
        }
        else if (v == "--chat") need(a.chat);
        else if (v == "--name") need(a.player_name);
        else if (v == "--net-sim-latency" && i + 1 < argc) a.net_sim_latency = std::clamp(std::atoi(argv[++i]), 0, 2000);
        else if (v == "--net-sim-loss" && i + 1 < argc) a.net_sim_loss = std::clamp(std::atoi(argv[++i]), 0, 100);
        else if (v == "--net-sim-jitter") a.net_sim_jitter = true;
        else if (v == "--drive") need(a.drive);
        else if (v == "--car") need(a.car);
        else if (v == "--movie") need(a.movie);
        else if (v == "--size" && i + 1 < argc) {
            const std::string size = argv[++i];
            const std::size_t separator = size.find_first_of("xX,");
            if (separator == std::string::npos) throw std::runtime_error("--size must be WIDTHxHEIGHT");
            a.width = std::stoi(size.substr(0, separator));
            a.height = std::stoi(size.substr(separator + 1));
            if (a.width < 320 || a.height < 240 || a.width > 8192 || a.height > 8192)
                throw std::runtime_error("--size must be between 320x240 and 8192x8192");
        }
        else if (v == "--frames" && i + 1 < argc) a.frames = std::atol(argv[++i]);
        else if (v == "--shot") need(a.shot);
        else if (v == "--inputs") need(a.inputs);
        else if (v == "--press") need(a.press);
        else if (v == "--page" && i + 1 < argc)
            a.first_page = static_cast<std::uint32_t>(std::stoul(argv[++i], nullptr, 0));
        else if (v == "--mute") a.mute = true;
        else if (v == "--virtual-pads") need(a.virtual_pads);
        else if (v == "--hold") need(a.hold);
        else if (v == "--prompts" && i + 1 < argc) {   // force the prompt glyph set (screenshots)
            const std::string d = argv[++i];
            if (d == "ps") a.prompts = InputDevice::PlayStation;
            else if (d == "xbox") a.prompts = InputDevice::Xbox;
            else if (d == "keyboard") a.prompts = InputDevice::KeyboardMouse;
            else throw std::runtime_error("--prompts takes ps, xbox or keyboard");
        }
        else if (v == "--give" && i + 1 < argc) a.give = std::atoi(argv[++i]);  // debug equip
        else if (v == "--crosshair" && i + 1 < argc) a.crosshair_style = std::clamp(std::atoi(argv[++i]), 0, 4);
        else if (v == "--high-contrast") a.high_contrast = true;
        else if (v == "--colorblind-teams") a.colorblind_teams = true;
        else if (a.mp && v.rfind("--", 0) == 0) {
            // Match options (nfgame --mp set: --mode/--players/--bots/...): forwarded with values.
            a.mp_args.push_back(v);
            const std::string& last = a.mp_args.back();
            auto takes_value = [&](const char* f) { return last == f; };
            if ((takes_value("--mode") || takes_value("--players") || takes_value("--bots") ||
                 takes_value("--frag-limit") || takes_value("--time-limit") || takes_value("--weapons") ||
                 takes_value("--spawn") || takes_value("--handicap") || takes_value("--seed") ||
                 takes_value("--bot-char") || takes_value("--cam") || takes_value("--follow-bot") ||
                 takes_value("--inputs2") || takes_value("--inputs3") || takes_value("--inputs4") ||
                 takes_value("--ruleset")) &&
                i + 1 < argc) {
                a.mp_args.push_back(argv[++i]);
                if (last == "--cam") {
                    for (int k = 0; k < 4 && i + 1 < argc; ++k) a.mp_args.push_back(argv[++i]);
                }
            }
        } else if (a.mp && v.size() > 4 && v.ends_with(".bin")) {
            a.mp_args.push_back(v);
        } else {
            std::fprintf(stderr, "nightfire: unknown option %s\n", v.c_str());
            return false;
        }
    }
    return true;
}

std::vector<nf::net::MatchConfig> make_host_rotation(const AppContext& ctx, const MpDirect& initial,
                                                      bool repeat_current) {
    std::vector<nf::net::MatchConfig> rotation;
    MatchOptions match = initial.options;
    match.enabled = true;
    match.humans = 0; // Runtime matches start empty; connected clients activate only the slots they own.
    if (repeat_current) rotation.push_back({initial.level_bin, match});
    const auto map_it = std::find_if(ctx.mp_data.maps.begin(), ctx.mp_data.maps.end(),
                                     [&](const nf::MpMap& map) { return map.bin_name == initial.level_bin; });
    if (ctx.mp_data.maps.size() > 1) {
        std::size_t index = map_it == ctx.mp_data.maps.end()
                                ? 0
                                : (std::size_t(map_it - ctx.mp_data.maps.begin()) + 1) % ctx.mp_data.maps.size();
        for (std::size_t attempt = 0; attempt < ctx.mp_data.maps.size(); ++attempt) {
            const nf::MpMap& map = ctx.mp_data.maps[index];
            if (map.bin_name != initial.level_bin) {
                rotation.push_back({map.bin_name, match});
                break;
            }
            index = (index + 1) % ctx.mp_data.maps.size();
        }
    }
    if (ctx.mp_data.scenarios.size() > 1) {
        const auto scenario_it =
            std::find_if(ctx.mp_data.scenarios.begin() + 1, ctx.mp_data.scenarios.end(),
                         [&](const nf::MpScenario& scenario) { return scenario.item.value == match.mode; });
        std::size_t index = scenario_it == ctx.mp_data.scenarios.end()
                                ? 1
                                : (std::size_t(scenario_it - ctx.mp_data.scenarios.begin()) + 1) %
                                      ctx.mp_data.scenarios.size();
        if (index == 0) index = 1;
        for (std::size_t attempt = 0; attempt < ctx.mp_data.scenarios.size() - 1; ++attempt) {
            const std::uint32_t mode = ctx.mp_data.scenarios[index].item.value;
            if (mode != match.mode) {
                MatchOptions changed = match;
                changed.mode = mode;
                rotation.push_back({initial.level_bin, changed});
                break;
            }
            index = (index + 1) % ctx.mp_data.scenarios.size();
            if (index == 0) index = 1;
        }
    }
    return rotation;
}

void usage(const char* prog) {
    std::fprintf(stderr,
                 "usage: %s [<gamedir>] [--mission level.bin [--difficulty 0|1|2] [--channel CH=VAL]] [--mp MAP_OPTS] "
                 "[--drive name [--car name]] [--movie hexid] [--logic-hz 30|60] [--frames N] [--shot out.bmp] "
                 "[--inputs file] [--press a,b,...] [--page 0x40000002] [--size WIDTHxHEIGHT] [--mute] [--give ID] "
                 "[--prompts ps|xbox|keyboard] [--virtual-pads xbox,ps,...] [--hold pad0:r1,...]\n"
                 "[--local-players 1..4] [--net-sim-loss 0..100] [--net-sim-latency ms] [--net-sim-jitter] joins a network server.\n"
                 "  --browse-lan, --browse-master IPv4[:port], or --browse-ip IPv4[:port] query server lists/info; "
                 "the main-menu Multiplayer entry opens Host/Join.\n"
                 "  --online-master IPv4[:port] adds the registry to that join screen; --listen-port and "
                 "--host-time-limit configure a listen host.\n"
                 "  --mp options: nfgame set (--mode/--players/--bots/--frag-limit/--time-limit/--weapons/...).\n"
                 "  --give ID: debug equip (scoped-capture hook): give + select the weapon at session start.\n",
                 prog);
}

}  // namespace

int run(int argc, char** argv) {
    Args args;
    if (!parse_args(argc, argv, args) || args.help) {
        usage(argv[0]);
        return args.help ? 0 : 2;
    }
    AppConfig cfg;
    load_config(config_path(), cfg);
    if (args.gamedir.empty()) args.gamedir = cfg.game_dir;
    if (!args.gamedir.empty()) args.gamedir = std::filesystem::absolute(args.gamedir).lexically_normal().string();
    auto has_game_files = [](const std::string& path) {
        if (path.empty()) return false;
        const std::filesystem::path root(path);
        std::error_code ec;
        return std::filesystem::is_regular_file(root / "ACTION.ELF", ec) &&
               std::filesystem::is_regular_file(root / "FILES.BIN", ec);
    };
    if (!has_game_files(args.gamedir)) {
        const SetupResult setup = run_setup_wizard(cfg, args.shot);
        if (setup == SetupResult::Screenshot) return 0;
        if (setup != SetupResult::Configured) return 1;
        args.gamedir = cfg.game_dir;
    } else if (args.gamedir != cfg.game_dir) {
        cfg.game_dir = args.gamedir;
        save_config(config_path(), cfg);
    }
    cfg.logic_hz = args.logic_hz;
    if (args.password.size() > nf::net::kMaxPasswordBytes)
        throw std::runtime_error("--password is limited to 64 bytes");
    if (args.browse_lan || args.browse_master || !args.direct_lookup.empty()) {
        ServerBrowser browser;
        std::vector<ServerBrowserEntry> entries;
        if (args.browse_lan) entries = browser.lan();
        if (!args.direct_lookup.empty()) {
            for (auto& entry : browser.direct(args.direct_lookup)) {
                const auto found = std::find_if(entries.begin(), entries.end(), [&](const ServerBrowserEntry& item) {
                    return item.endpoint == entry.endpoint;
                });
                if (found == entries.end()) entries.push_back(std::move(entry));
                else *found = std::move(entry);
            }
        }
        if (args.browse_master) {
            const std::size_t colon = args.master.rfind(':');
            const std::string host = args.master.substr(0, colon);
            std::uint16_t port = 27501;
            if (colon != std::string::npos) {
                char* end = nullptr;
                const long value = std::strtol(args.master.c_str() + colon + 1, &end, 10);
                if (!end || *end || value < 1 || value > 65535)
                    throw std::runtime_error("--browse-master has an invalid port");
                port = std::uint16_t(value);
            }
            for (auto& entry : browser.master(host, port)) {
                const auto found = std::find_if(entries.begin(), entries.end(), [&](const ServerBrowserEntry& item) {
                    return item.endpoint == entry.endpoint;
                });
                if (found == entries.end()) entries.push_back(std::move(entry));
                else *found = std::move(entry);
            }
        }
        for (const ServerBrowserEntry& entry : entries)
            std::printf("%s  %s  %s  %u/%u  %ums%s\n", entry.endpoint.c_str(), entry.name.c_str(), entry.map.c_str(),
                        unsigned(entry.players), unsigned(entry.max_players), unsigned(entry.ping_ms),
                        entry.password_required ? "  password" : "");
        std::printf("nightfire: %zu server(s)\n", entries.size());
        return 0;
    }
    std::unique_ptr<AppContext> ctx = load_context(args.gamedir);
    ui::register_art_sheets(ctx->assets.sprites);
    if (!args.connect.empty() && args.frames >= 0 && args.shot.empty()) {
        NetworkClientOptions options;
        options.endpoint = args.connect;
        options.map = args.map;
        options.password = args.password;
        options.name = args.player_name;
        options.press = args.press;
        options.chat = args.chat;
        options.frames = args.frames;
        options.loss_percent = args.net_sim_loss;
        options.latency_ms = args.net_sim_latency;
        options.jitter_ms = args.net_sim_jitter ? nf::net::kDefaultNetSimJitterMs : 0;
        options.local_players = std::uint8_t(args.local_players);
        return run_network_client(*ctx, options);
    }
    // Window first: it owns SDL/GL and must die last (teardown order: menu audio and the
    // UI renderers hold live SDL/GL objects and are destroyed before it).
    const bool direct = !args.mission.empty() || args.mp || !args.drive.empty() ||
                        !args.movie.empty() || !args.connect.empty();
    const bool headless_opts = args.frames >= 0 || !args.shot.empty() || !args.press.empty() || !args.inputs.empty();
    const bool hidden = (direct && headless_opts) || (!direct && (!args.press.empty() || !args.shot.empty()));
    Window window("nightfire", args.width, args.height, hidden);

    // Frontend menu audio (best effort: the shared SFX ids, volumes from the settings).
    SoundArchive archive(args.gamedir);
    audio::AudioSystem menu_audio(archive);
    menu_audio.set_sfx_volume(cfg.sfx_volume);
    menu_audio.set_music_volume(cfg.music_volume);
    audio::AudioSystem* menu_audio_ptr = args.mute ? nullptr : &menu_audio;
    if (menu_audio_ptr && !menu_audio_ptr->open_device()) menu_audio_ptr = nullptr;
    ui::Renderer ui(ctx->assets.sprites);
    apply_settings(cfg, window, ui, menu_audio_ptr);
    {
        ui::Accessibility access = accessibility_from_config(cfg);
        if (args.crosshair_style) access.crosshair = ui::CrosshairStyle(*args.crosshair_style);
        access.high_contrast = access.high_contrast || args.high_contrast;
        access.colorblind_teams = access.colorblind_teams || args.colorblind_teams;
        ui::set_accessibility(access);
    }
    ui::TextRenderer text(ui, ctx->assets.fonts);
    // Button prompts follow each player's last used device (docs/ui.md "Button prompts").
    if (!args.virtual_pads.empty()) attach_virtual_pads(args.virtual_pads);
    input_devices().install();
    input_devices().force(args.prompts);
    ui::PromptGlyphs prompts(ctx->assets.fonts);
    ui::TextRenderer::set_prompts(&prompts);
    struct PromptsReset {   // before the Window (SDL_Quit) goes
        ~PromptsReset() {
            ui::TextRenderer::set_prompts(nullptr);
            input_devices().shutdown();
        }
    } prompts_reset;
    if (!args.connect.empty()) {
        NetworkClientOptions options;
        options.endpoint = args.connect;
        options.map = args.map;
        options.password = args.password;
        options.name = args.player_name;
        options.chat = args.chat;
        options.local_players = std::uint8_t(args.local_players);
        options.auto_aim[0] = cfg.mp_auto_aim;
        options.loss_percent = args.net_sim_loss;
        options.latency_ms = args.net_sim_latency;
        options.jitter_ms = args.net_sim_jitter ? nf::net::kDefaultNetSimJitterMs : 0;
        const auto server_info = ServerBrowser().direct(options.endpoint);
        MpDirect direct_mp;
        direct_mp.level_bin = args.map;
        direct_mp.inputs[0] = args.inputs;
        direct_mp.auto_aim = options.auto_aim;
        direct_mp.options.enabled = true;
        direct_mp.options.humans = args.local_players;
        if (!server_info.empty()) {
            direct_mp.options.bots = 0; // The server snapshot owns every remote bot.
            direct_mp.options.rules = server_info.front().slot_count == 10 ? nf::MpRuleSet::GcXbox :
                                      server_info.front().slot_count == 16 ? nf::MpRuleSet::Extended :
                                                                           nf::MpRuleSet::Ps2;
        }
        NetworkSession network(*ctx, std::move(options));
        MpSession session(*ctx, window, ui, text, direct_mp, cfg);
        if (!session.ready()) return 1;
        const long frames = args.frames >= 0 ? args.frames : (!args.shot.empty() ? 300 : -1);
        session.run_network_interactive(network, frames, args.shot);
        return 0;
    }

    // ---- direct session launches (headless verification / debug) ----
    if (!args.movie.empty()) {
        // Hex id (0x73B0048) or .PSS path; headless with --shot/--frames, skippable windowed.
        std::uint32_t id = 0;
        if (args.movie.size() > 4 &&
            (args.movie.compare(args.movie.size() - 4, 4, ".PSS") == 0 || args.movie.compare(args.movie.size() - 4, 4, ".pss") == 0)) {
            std::fprintf(stderr, "nightfire: --movie takes a hex id (paths play via nfui movie)\n");
            return 2;
        }
        id = std::uint32_t(std::stoul(args.movie, nullptr, 16));
        MovieScreen screen(window, ui, text, menu_audio_ptr, nullptr, args.gamedir);
        screen.play(id, args.shot, args.frames);
        return 0;
    }
    if (!args.mission.empty()) {
        SpLaunch launch{args.mission, args.difficulty >= 0 ? args.difficulty : cfg.difficulty + 1, args.channels,
                        args.give};
        SpSession session(*ctx, window, ui, text, launch, cfg);
        if (!session.ready()) return 1;
        if (args.frames >= 0 || !args.shot.empty() || !args.inputs.empty()) {
            SpHeadless h;
            h.frames = args.frames >= 0 ? args.frames : 300;
            h.shot = args.shot;
            h.inputs = args.inputs;
            session.run_headless(h);
            return 0;
        }
        while (true) {
            const SpResult r = session.run_interactive();
            if (r.exit == SpExit::Restart) continue;
            break;
        }
        return 0;
    }
    if (args.mp) {
        MpDirect direct_mp;
        direct_mp.options.enabled = true;
        direct_mp.auto_aim[0] = cfg.mp_auto_aim;
        for (std::size_t i = 0; i < args.mp_args.size(); ++i) {
            const std::string& m = args.mp_args[i];
            if (direct_mp.options.parse(args.mp_args, i)) continue;
            if (m == "--shot" && i + 1 < args.mp_args.size()) direct_mp.shot = args.mp_args[++i];
            else if (m == "--frames" && i + 1 < args.mp_args.size()) direct_mp.frames = std::atol(args.mp_args[++i].c_str());
            else if (m == "--bot-char" && i + 1 < args.mp_args.size()) direct_mp.bot_characters = args.mp_args[++i];
            else if ((m == "--inputs" || m == "--inputs2" || m == "--inputs3" || m == "--inputs4") &&
                     i + 1 < args.mp_args.size())
                direct_mp.inputs[m == "--inputs" ? 0 : std::size_t(m.back() - '1')] = args.mp_args[++i];
            else if ((m == "--cam" || m == "--follow-bot") && i + 1 < args.mp_args.size()) {
                // Screenshot camera (nfgame --mp set): not kept for the integrated session.
                const int skip = m == "--cam" ? 5 : 1;
                for (int k = 0; k < skip && i + 1 < args.mp_args.size(); ++k) ++i;
            } else if (m == "--bot-log" || m == "--bot-log-states" || m == "--coll") {
            } else if (m.ends_with(".bin")) {
                direct_mp.level_bin = m;
            }
        }
        // Top-level --shot/--frames/--inputs also apply to the direct MP run.
        if (!args.shot.empty() && direct_mp.shot.empty()) direct_mp.shot = args.shot;
        if (args.frames >= 0 && direct_mp.frames < 0) direct_mp.frames = args.frames;
        if (!args.inputs.empty() && direct_mp.inputs[0].empty()) direct_mp.inputs[0] = args.inputs;
        direct_mp.give = args.give;
        MpSession session(*ctx, window, ui, text, direct_mp, cfg);
        if (!session.ready()) return 1;
        if (direct_mp.frames >= 0 || !direct_mp.shot.empty()) {
            session.run_headless();
            return 0;
        }
        while (true) {
            const MpResult r = session.run_interactive();
            if (r.exit == MpExit::Rematch) continue;
            break;
        }
        return 0;
    }
    if (!args.drive.empty()) {
        DriveSessionApp session(*ctx, window, ui, text, args.drive, args.car, cfg);
        if (!session.ready()) return 1;
        if (args.frames >= 0 || !args.shot.empty() || !args.inputs.empty()) {
            DriveHeadless h;
            h.frames = args.frames >= 0 ? args.frames : 600;
            h.shot = args.shot;
            h.inputs = args.inputs;
            session.run_headless(h);
            return 0;
        }
        while (true) {
            const DriveResult r = session.run_interactive();
            if (r.exit == DriveExit::Restart) continue;
            break;
        }
        return 0;
    }

    // ---- full game loop: frontend -> session -> results -> frontend ----
    // A mission win chains into the next sp_level row (ACTION level or driving mission).
    std::uint32_t pending_level = 0;  // nonzero: skip the frontend and launch this sp_level id
    while (true) {
        if (pending_level == 0) {
            FrontendResult result;
            const std::string menu_press = args.press, menu_shot = args.shot;
            // --press ... --frames N: a split-screen match the replay starts runs N ticks and takes the shot.
            const bool headless_match = args.frames >= 0 && !args.press.empty();
            const bool frontend_live = run_frontend(*ctx, window, ui, text, cfg, menu_audio_ptr,
                                                     args.press, args.shot, args.first_page, result, headless_match);
            if (!frontend_live && result.action != FrontendResult::Action::StartOnlineJoin &&
                result.action != FrontendResult::Action::StartListenServer &&
                result.action != FrontendResult::Action::OpenSettings &&
                !(headless_match && result.action == FrontendResult::Action::StartMultiplayer)) {
                if (!args.press.empty() || !args.shot.empty()) return 0;  // headless menu run done
                if (result.action == FrontendResult::Action::Quit) return 0;
                return 0;
            }
            args.press.clear();
            args.first_page.reset();
            args.shot.clear();
            if (result.action == FrontendResult::Action::Quit) return 0;
            if (result.action == FrontendResult::Action::OpenSettings) {
                run_settings(*ctx, window, ui, text, cfg, menu_audio_ptr, menu_press, menu_shot);
                if (!frontend_live) return 0;
                args.first_page = 0x40000002;   // back on the main menu
                continue;
            }
            if (result.action == FrontendResult::Action::StartOnlineJoin) {
                auto options =
                    run_online_browser(*ctx, window, ui, text, cfg, menu_press, menu_shot, args.player_name,
                                       args.online_master);
                if (!options) {
                    if (!frontend_live) return 0;
                    continue;
                }
                NetworkClientOptions connection = std::move(*options);
                connection.auto_aim[0] = cfg.mp_auto_aim;
                connection.loss_percent = args.net_sim_loss;
                connection.latency_ms = args.net_sim_latency;
                connection.jitter_ms = args.net_sim_jitter ? nf::net::kDefaultNetSimJitterMs : 0;
                const std::vector<ServerBrowserEntry> initial_info = ServerBrowser().direct(connection.endpoint);
                std::uint32_t mode = initial_info.empty() ? nf::mp_mode::kArena : initial_info.front().mode;
                ServerBrowser browser;
                while (true) {
                    const std::string map = connection.map;
                    const std::vector<ServerBrowserEntry> server_info = ServerBrowser().direct(connection.endpoint);
                    NetworkSession network(*ctx, connection);
                    MpDirect direct_mp;
                    direct_mp.level_bin = map;
                    direct_mp.auto_aim = connection.auto_aim;
                    direct_mp.options.enabled = true;
                    direct_mp.options.mode = mode;
                    direct_mp.options.humans = int(connection.local_players);
                    direct_mp.options.bots = 0; // Network snapshots provide all remote actors, including bots.
                    if (!server_info.empty()) {
                        direct_mp.options.rules = server_info.front().slot_count == 10 ? nf::MpRuleSet::GcXbox :
                                                  server_info.front().slot_count == 16 ? nf::MpRuleSet::Extended :
                                                                                       nf::MpRuleSet::Ps2;
                    }
                    nf::GameRng client_rng;
                    nf::ScopedGameRng client_rng_binding(client_rng);
                    MpSession session(*ctx, window, ui, text, direct_mp, cfg);
                    if (!session.ready()) break;
                    const MpResult match_result = session.run_network_interactive(network, args.frames);
                    const std::uint64_t completed_revision = match_result.match_revision;
                    if (!match_result.match_over || args.frames >= 0) break;
                    bool advanced = false;
                    for (int attempt = 0; attempt < 120; ++attempt) {
                        SDL_Delay(250);
                        const auto next = browser.direct(connection.endpoint);
                        if (!next.empty() && next.front().match_revision > completed_revision) {
                            connection.map = next.front().map;
                            mode = next.front().mode;
                            std::printf("online client: joining next match %s mode=0x%08x\n", connection.map.c_str(),
                                        mode);
                            advanced = true;
                            break;
                        }
                    }
                    if (!advanced) break;
                }
                if (!frontend_live || args.frames >= 0 || !menu_press.empty()) return 0;
            }
            if (result.action == FrontendResult::Action::StartListenServer && result.launch) {
                MpDirect mp = MpSession::from_launch(*result.launch);
                mp.options.enabled = true;
                mp.options.humans = int(nf::kMpMaxLocalHumans);
                if (args.host_time_limit > 0.0f) mp.options.time_limit = args.host_time_limit * 60.0f;
                nf::net::ServerConfig server_config;
                server_config.logic_hz = cfg.logic_hz;
                server_config.data_dir = args.gamedir;
                server_config.map = mp.level_bin;
                server_config.match = mp.options;
                server_config.match.humans = 0;
                server_config.port = std::uint16_t(args.listen_port);
                server_config.name = args.player_name;
                server_config.password = args.password;
                server_config.net_sim.loss_percent = args.net_sim_loss;
                server_config.net_sim.latency_ms = args.net_sim_latency;
                server_config.net_sim.jitter_ms = args.net_sim_jitter ? nf::net::kDefaultNetSimJitterMs : 0;
                if (!args.online_master.empty()) {
                    const std::size_t colon = args.online_master.rfind(':');
                    server_config.master_host = args.online_master.substr(0, colon);
                    if (colon != std::string::npos) {
                        char* end = nullptr;
                        const long port = std::strtol(args.online_master.c_str() + colon + 1, &end, 10);
                        if (!end || *end || port < 1 || port > 65535)
                            throw std::runtime_error("--online-master has an invalid port");
                        server_config.master_port = std::uint16_t(port);
                    }
                }
                server_config.rotation = make_host_rotation(*ctx, mp, args.host_time_limit > 0.0f);
                nf::net::ServerRuntime server(std::move(server_config));
                std::string error;
                if (!server.start(&error)) {
                    std::fprintf(stderr, "nightfire: listen server failed: %s\n", error.c_str());
                    continue;
                }
                std::size_t matches_played = 0;
                while (server.running()) {
                    const nf::net::ServerStatus before = server.current_match();
                    NetworkClientOptions client_options;
                    client_options.endpoint = "127.0.0.1:" + std::to_string(args.listen_port);
                    client_options.map = before.map;
                    client_options.password = args.password;
                    client_options.name = args.player_name;
                    client_options.local_players = std::uint8_t(args.local_players);
                    client_options.auto_aim = mp.auto_aim;
                    client_options.loss_percent = args.net_sim_loss;
                    client_options.latency_ms = args.net_sim_latency;
                    client_options.jitter_ms = args.net_sim_jitter ? nf::net::kDefaultNetSimJitterMs : 0;
                    MpDirect client_mp;
                    client_mp.level_bin = before.map;
                    client_mp.auto_aim = mp.auto_aim;
                    client_mp.options = mp.options;
                    client_mp.options.mode = before.mode;
                    client_mp.options.humans = args.local_players;
                    nf::GameRng client_rng;
                    nf::ScopedGameRng client_rng_binding(client_rng);
                    NetworkSession network(*ctx, std::move(client_options));
                    MpSession session(*ctx, window, ui, text, client_mp, cfg);
                    if (!session.ready()) break;
                    const MpResult match_result = session.run_network_interactive(network, args.frames);
                    if (!match_result.match_over) break;
                    bool advanced = false;
                    while (true) {
                        const nf::net::ServerStatus current = server.current_match();
                        if (current.revision > before.revision && !current.match_over) {
                            if (!current.running) {
                                SDL_Delay(50);
                                continue;
                            }
                            std::printf("listen server: next match %s mode=0x%08x\n", current.map.c_str(),
                                        current.mode);
                            advanced = true;
                            break;
                        }
                        if (!current.running && !server.error().empty()) break;
                        SDL_Delay(50);
                    }
                    ++matches_played;
                    if (!advanced || (args.frames >= 0 && matches_played >= 2)) break;
                }
                server.stop();
                continue;
            }
            if (result.action == FrontendResult::Action::StartMission) {
                // The mission map can also name a driving mission: launch it on the DRIVING side.
                if (driving::find_level(result.level_bin)) {
                    DriveSessionApp drive(*ctx, window, ui, text, result.level_bin, "", cfg);
                    if (drive.ready()) {
                        while (true) {
                            const DriveResult dr = drive.run_interactive();
                            if (dr.exit != DriveExit::Restart) break;
                        }
                    }
                    continue;
                }
                SpLaunch launch{result.level_bin, result.difficulty, {}};
                while (true) {
                    SpSession session(*ctx, window, ui, text, launch, cfg);
                    if (!session.ready()) break;
                    const SpResult r = session.run_interactive();
                    if (r.exit == SpExit::Restart) continue;
                    if (r.exit == SpExit::NextMission) pending_level = r.next_level;
                    break;
                }
            } else if (result.action == FrontendResult::Action::StartMultiplayer && result.launch) {
                MpDirect mp = MpSession::from_launch(*result.launch);
                mp.devices = result.slot_devices;
                if (headless_match) {
                    // Verification run: hold the --hold buttons on the virtual pads, play N ticks, shoot.
                    for (std::size_t p = 0; p < args.hold.size();) {
                        const std::size_t e = args.hold.find(',', p);
                        const std::string tok = args.hold.substr(p, e == std::string::npos ? e : e - p);
                        p = e == std::string::npos ? args.hold.size() : e + 1;
                        const std::size_t colon = tok.find(':');
                        if (tok.rfind("pad", 0) != 0 || colon == std::string::npos)
                            throw std::runtime_error("--hold takes pad<N>:<buttons>");
                        std::uint16_t buttons = 0;
                        for (std::size_t q = colon + 1; q < tok.size();) {
                            const std::size_t plus = tok.find('+', q);
                            const std::string name = tok.substr(q, plus == std::string::npos ? plus : plus - q);
                            q = plus == std::string::npos ? tok.size() : plus + 1;
                            const auto it = kPressButtons.find(name);
                            if (it == kPressButtons.end()) throw std::runtime_error("unknown button " + name);
                            buttons |= it->second;
                        }
                        set_virtual_pad(std::size_t(std::atoi(tok.c_str() + 3)), buttons);
                    }
                    mp.frames = args.frames;
                    mp.shot = menu_shot;
                    MpSession session(*ctx, window, ui, text, mp, cfg);
                    if (session.ready()) session.run_interactive();
                    return 0;
                }
                while (true) {
                    MpSession session(*ctx, window, ui, text, mp, cfg);
                    if (!session.ready()) break;
                    const MpResult r = session.run_interactive();
                    if (r.exit != MpExit::Rematch) break;
                }
            }
            // Resume / RestartMission / MissionDone / None from the main menu: show it again.
            continue;
        }
        // Mission chain: an ACTION row continues on foot, a driving row on the DRIVING side.
        const std::uint32_t chained = pending_level;
        pending_level = 0;
        if ((chained & 0x0F000000) == 0x09000000) {
            static const std::pair<std::uint32_t, const char*> kDriveMissions[] = {
                {0x09000001, "paris"},      // MIS01
                {0x09000002, "alps"},       // MIS3 SnowMobile (docs/driving-missions.md)
                {0x09000003, "alps2"},      // MIS4 Alps chase
                {0x09000005, "underwater"},  // MIS11
                {0x09000006, "jungle1"}};   // MIS13A (chains to 13B/13C internally)
            const char* name = nullptr;
            for (const auto& [id, n] : kDriveMissions) {
                if (id == chained) name = n;
            }
            if (!name) continue;  // unmapped driving row: back to the frontend
            DriveSessionApp drive(*ctx, window, ui, text, name, "", cfg);
            if (!drive.ready()) continue;
            while (true) {
                const DriveResult dr = drive.run_interactive();
                if (dr.exit != DriveExit::Restart) break;
            }
            continue;
        }
        char bin[16];
        std::snprintf(bin, sizeof(bin), "%08x.bin", chained);
        SpLaunch launch{bin, cfg.difficulty + 1, {}};
        while (true) {
            SpSession session(*ctx, window, ui, text, launch, cfg);
            if (!session.ready()) break;
            const SpResult r = session.run_interactive();
            if (r.exit == SpExit::Restart) continue;
            if (r.exit == SpExit::NextMission) pending_level = r.next_level;
            break;
        }
    }
}

}  // namespace nf::app

int main(int argc, char** argv) {
    try {
        return nf::app::run(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "nightfire: %s\n", e.what());
        return 1;
    }
}

// nightfire: the playable game executable. Boots the frontend (UserInterface)
// on the original menu script and launches ACTION single-player missions
// (Scripting mission flow is Scripting's slice; the session runs the world,
// NPCs, HUD and audio until death/fail/quit), multiplayer arena matches with
// bots (Arena + Bots), or DRIVING.ELF driving missions (Driving), with HUD,
// AudioSystem + MusicDirector, CharacterRenderer bodies, pause menus and
// mission/match results back to the frontend. Settings persist in
// ~/.config/nightfire/nightfire.cfg. Headless --shot/--frames/--inputs runs
// verify each path without a display.
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
#include "app/net_client.hpp"
#include "audio/audio.hpp"
#include "render/window.hpp"
#include "ui/frontend.hpp"
#include "ui/menu_audio.hpp"
#include "driving/driving_level.hpp"
#include "ui/renderer.hpp"
#include "ui/text.hpp"

namespace nf::app {

namespace {

constexpr int kWindowW = 1280, kWindowH = 720;

const std::map<std::string, std::uint16_t> kPressButtons = {
    {"up", kPadUp},       {"down", kPadDown},     {"left", kPadLeft},   {"right", kPadRight},
    {"cross", kPadCross}, {"circle", kPadCircle}, {"square", kPadSquare}, {"triangle", kPadTriangle},
    {"start", kPadStart}, {"select", kPadSelect}, {"l1", kPadL1},       {"l2", kPadL2},
    {"r1", kPadR1},       {"r2", kPadR2},         {"l3", kPadL3},       {"r3", kPadR3}};

// --press replay (nfui token format): one 30 Hz frame per token.
template <typename AdvanceBackground>
void replay_press(Frontend& frontend, PadHistory& pad, const std::string& script, AdvanceBackground&& advance_background) {
    for (std::size_t p = 0; p < script.size();) {
        const std::size_t e = script.find(',', p);
        const std::string tok = script.substr(p, e == std::string::npos ? e : e - p);
        p = e == std::string::npos ? script.size() : e + 1;
        if (tok.rfind("net-", 0) == 0) continue;  // Reserved for the Online server-browser screen.
        if (tok.rfind("wait", 0) == 0) {
            const int n = tok.size() > 4 ? std::atoi(tok.c_str() + 4) : 1;
            for (int i = 0; i < n; ++i) {
                pad.push({});
                frontend.update(pad);
                advance_background();
            }
            continue;
        }
        PadState s;
        for (std::size_t q = 0; q < tok.size();) {
            const std::size_t plus = tok.find('+', q);
            const std::string name = tok.substr(q, plus == std::string::npos ? plus : plus - q);
            q = plus == std::string::npos ? tok.size() : plus + 1;
            const auto it = kPressButtons.find(name);
            if (it == kPressButtons.end()) throw std::runtime_error("unknown button " + name);
            s.buttons |= it->second;
        }
        pad.push(s);
        frontend.update(pad);
        advance_background();
        pad.push({});
        frontend.update(pad);
        advance_background();
    }
}

PadState live_menu_pad(SDL_Gamepad* gamepad) {
    PadState s;
    const bool* k = SDL_GetKeyboardState(nullptr);
    if (k[SDL_SCANCODE_UP]) s.buttons |= kPadUp;
    if (k[SDL_SCANCODE_DOWN]) s.buttons |= kPadDown;
    if (k[SDL_SCANCODE_LEFT]) s.buttons |= kPadLeft;
    if (k[SDL_SCANCODE_RIGHT]) s.buttons |= kPadRight;
    if (k[SDL_SCANCODE_Z] || k[SDL_SCANCODE_RETURN]) s.buttons |= kPadCross;
    if (k[SDL_SCANCODE_X]) s.buttons |= kPadCircle;
    if (k[SDL_SCANCODE_A]) s.buttons |= kPadSquare;
    if (k[SDL_SCANCODE_S]) s.buttons |= kPadTriangle;
    if (k[SDL_SCANCODE_SPACE]) s.buttons |= kPadStart;
    if (k[SDL_SCANCODE_BACKSPACE]) s.buttons |= kPadSelect;
    if (gamepad) {
        if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_UP)) s.buttons |= kPadUp;
        if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_DOWN)) s.buttons |= kPadDown;
        if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_LEFT)) s.buttons |= kPadLeft;
        if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) s.buttons |= kPadRight;
        if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_SOUTH)) s.buttons |= kPadCross;
        if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_EAST)) s.buttons |= kPadCircle;
        if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_WEST)) s.buttons |= kPadSquare;
        if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_NORTH)) s.buttons |= kPadTriangle;
        if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_START)) s.buttons |= kPadStart;
        if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_BACK)) s.buttons |= kPadSelect;
    }
    return s;
}

void sync_config_to_frontend(const AppConfig& cfg, Frontend& frontend) {
    GameOptions& o = frontend.game_options();
    o.music_volume = cfg.music_volume;
    o.sfx_volume = cfg.sfx_volume;
    o.vibration = cfg.vibration;
    o.auto_aim = cfg.auto_aim;
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
                                        "start-mission", "resume", "restart-mission", "quit-to-menu", "rematch",
                                        "mission-done", "quit"};
    std::printf("frontend: %s level=%s difficulty=%d", names[int(r.action)], r.level_bin.c_str(), r.difficulty);
    if (r.launch) std::printf(" mp mode=0x%08x humans=%u bots=%u", r.launch->settings.mode,
                              r.launch->settings.human_count, r.launch->settings.bot_count);
    std::printf("\n");
}

// One frontend screen (main menu at boot, or after a session ends). Returns
// false when the application should exit.
bool run_frontend(AppContext& ctx, Window& window, ui::Renderer& ui, ui::TextRenderer& text, AppConfig& cfg,
                  audio::AudioSystem* audio, const std::string& press, const std::string& shot,
                  const std::optional<std::uint32_t>& first_page, FrontendResult& out) {
    Frontend frontend(ctx.assets, ctx.menu, &ctx.mp_data, &ctx.sp_data);
    sync_config_to_frontend(cfg, frontend);
    frontend.open(FrontendMode::MainMenu, first_page);
    MenuBackground background(ctx.gamedir);
    background.advance();
    PadHistory pad;
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
        if (!press.empty()) replay_press(frontend, pad, press, [&] { background.advance(); });
        play_sounds();
        report_result(frontend.result());
        out = frontend.result();
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
        if (!shot.empty()) {
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
    SDL_Gamepad* gamepad = nullptr;
    int count = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
        if (count > 0) gamepad = SDL_OpenGamepad(ids[0]);
        SDL_free(ids);
    }
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
            pad.push(live_menu_pad(gamepad));
            frontend.update(pad);
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
        ui.begin(w, h);
        background.draw(ui);
        frontend.draw(ui, text);
        ui.end();
        window.swap();
    }
    if (gamepad) SDL_CloseGamepad(gamepad);
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
    // Headless.
    long frames = -1;
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
    int listen_port = 27500;
    float host_time_limit = -1.0f;  // optional minutes; useful for short rotation tests
    int give = -1;
    int local_players = 1;
    int width = kWindowW, height = kWindowH;
};
bool parse_args(int argc, char** argv, Args& a) {
    if (argc < 2) return false;
    // --help anywhere (including argv[1]) prints usage without a gamedir.
    for (int i = 1; i < argc; ++i) {
        const std::string v = argv[i];
        if (v == "--help" || v == "-h") {
            a.help = true;
            return true;
        }
    }
    a.gamedir = argv[1];
    for (int i = 2; i < argc; ++i) {
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
        else if (v == "--mp") a.mp = true;
        else if (v == "--connect") need(a.connect);
        else if (v == "--local-players" && i + 1 < argc) {
            a.local_players = std::atoi(argv[++i]);
            if (a.local_players < 1 || a.local_players > int(nf::net::kMaxLocalPlayers))
                throw std::runtime_error("--local-players must be 1..4");
        }
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
        else if (v == "--give" && i + 1 < argc) a.give = std::atoi(argv[++i]);  // debug equip
        else if (a.mp && v.rfind("--", 0) == 0) {
            // Match options (nfgame --mp set: --mode/--players/--bots/...): forwarded with values.
            a.mp_args.push_back(v);
            const std::string& last = a.mp_args.back();
            auto takes_value = [&](const char* f) { return last == f; };
            if ((takes_value("--mode") || takes_value("--players") || takes_value("--bots") ||
                 takes_value("--frag-limit") || takes_value("--time-limit") || takes_value("--weapons") ||
                 takes_value("--spawn") || takes_value("--handicap") || takes_value("--seed") ||
                 takes_value("--bot-char") || takes_value("--cam") || takes_value("--follow-bot") ||
                 takes_value("--inputs2") || takes_value("--inputs3") || takes_value("--inputs4")) &&
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
    match.humans = int(nf::kMpMaxHumans);
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
                 "usage: %s <gamedir> [--mission level.bin [--difficulty 0|1|2] [--channel CH=VAL]] [--mp MAP_OPTS] "
                 "[--drive name [--car name]] [--movie hexid] [--frames N] [--shot out.bmp] [--inputs file] "
                 "[--press a,b,...] [--page 0x40000002] [--size WIDTHxHEIGHT] [--mute] [--give ID]\n"
                 "  --connect IPv4[:port] [--password text] [--map file.bin] [--name name] [--chat message] [--frames N] "
                 "[--net-sim-loss 0..100] [--net-sim-latency ms] joins a network server.\n"
                 "  --browse-lan, --browse-master IPv4[:port], or --browse-ip IPv4[:port] query server lists/info; the main-menu Multiplayer entry opens Host/Join.\n"
                 "  --online-master IPv4[:port] adds the registry to that join screen; --listen-port and --host-time-limit configure a listen host.\n"
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
    ui.set_pillarbox(cfg.pillarbox);
    ui::TextRenderer text(ui, ctx->assets.fonts);
    if (!args.connect.empty()) {
        NetworkClientOptions options;
        options.endpoint = args.connect;
        options.map = args.map;
        options.password = args.password;
        options.name = args.player_name;
        options.chat = args.chat;
        options.local_players = std::uint8_t(args.local_players);
        options.loss_percent = args.net_sim_loss;
        options.latency_ms = args.net_sim_latency;
        const auto server_info = ServerBrowser().direct(options.endpoint);
        MpDirect direct_mp;
        direct_mp.level_bin = args.map;
        direct_mp.inputs[0] = args.inputs;
        direct_mp.options.enabled = true;
        direct_mp.options.humans = int(nf::kMpMaxHumans);
        if (!server_info.empty()) {
            direct_mp.options.bots = server_info.front().bots;
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
            const bool frontend_live = run_frontend(*ctx, window, ui, text, cfg, menu_audio_ptr,
                                                     args.press, args.shot, args.first_page, result);
            if (!frontend_live && result.action != FrontendResult::Action::StartOnlineJoin &&
                result.action != FrontendResult::Action::StartListenServer) {
                if (!args.press.empty() || !args.shot.empty()) return 0;  // headless menu run done
                if (result.action == FrontendResult::Action::Quit) return 0;
                return 0;
            }
            args.press.clear();
            args.first_page.reset();
            args.shot.clear();
            if (result.action == FrontendResult::Action::Quit) return 0;
            if (result.action == FrontendResult::Action::StartOnlineJoin) {
                auto options =
                    run_online_browser(*ctx, window, ui, text, menu_press, menu_shot, args.player_name,
                                       args.online_master);
                if (!options) {
                    if (!frontend_live) return 0;
                    continue;
                }
                NetworkClientOptions connection = std::move(*options);
                const std::vector<ServerBrowserEntry> initial_info = ServerBrowser().direct(connection.endpoint);
                std::uint32_t mode = initial_info.empty() ? nf::mp_mode::kArena : initial_info.front().mode;
                ServerBrowser browser;
                while (true) {
                    const std::string map = connection.map;
                    const std::vector<ServerBrowserEntry> server_info = ServerBrowser().direct(connection.endpoint);
                    NetworkSession network(*ctx, connection);
                    MpDirect direct_mp;
                    direct_mp.level_bin = map;
                    direct_mp.options.enabled = true;
                    direct_mp.options.mode = mode;
                    direct_mp.options.humans = int(nf::kMpMaxHumans);
                    if (!server_info.empty()) {
                        direct_mp.options.bots = server_info.front().bots;
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
                mp.options.humans = int(nf::kMpMaxHumans);
                if (args.host_time_limit > 0.0f) mp.options.time_limit = args.host_time_limit * 60.0f;
                nf::net::ServerConfig server_config;
                server_config.data_dir = args.gamedir;
                server_config.map = mp.level_bin;
                server_config.match = mp.options;
                server_config.port = std::uint16_t(args.listen_port);
                server_config.name = args.player_name;
                server_config.password = args.password;
                server_config.net_sim.loss_percent = args.net_sim_loss;
                server_config.net_sim.latency_ms = args.net_sim_latency;
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
                    client_options.loss_percent = args.net_sim_loss;
                    client_options.latency_ms = args.net_sim_latency;
                    MpDirect client_mp;
                    client_mp.level_bin = before.map;
                    client_mp.options = mp.options;
                    client_mp.options.mode = before.mode;
                    client_mp.options.humans = int(nf::kMpMaxHumans);
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

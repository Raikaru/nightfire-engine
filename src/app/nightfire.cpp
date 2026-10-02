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
#include <string>
#include <vector>

#include "app/app.hpp"
#include "app/movie.hpp"
#include "app/session_drive.hpp"
#include "app/session_mp.hpp"
#include "app/session_sp.hpp"
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
void replay_press(Frontend& frontend, PadHistory& pad, const std::string& script) {
    for (std::size_t p = 0; p < script.size();) {
        const std::size_t e = script.find(',', p);
        const std::string tok = script.substr(p, e == std::string::npos ? e : e - p);
        p = e == std::string::npos ? script.size() : e + 1;
        if (tok.rfind("wait", 0) == 0) {
            const int n = tok.size() > 4 ? std::atoi(tok.c_str() + 4) : 1;
            for (int i = 0; i < n; ++i) pad.push({}), frontend.update(pad);
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
        pad.push(s), frontend.update(pad);
        pad.push({}), frontend.update(pad);
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
    o.widescreen = cfg.widescreen;
    o.screen_x = cfg.screen_x;
    o.screen_y = cfg.screen_y;
    PlayerOptions& p = frontend.player_options();
    p.controller_style = cfg.controller_style;
    p.invert_y = cfg.invert_y;
}

void report_result(const FrontendResult& r) {
    static const char* const names[] = {"none", "start-multiplayer", "start-mission", "resume",      "restart-mission",
                                        "quit-to-menu", "rematch", "mission-done", "quit"};
    std::printf("frontend: %s level=%s difficulty=%d", names[int(r.action)], r.level_bin.c_str(), r.difficulty);
    if (r.launch) std::printf(" mp mode=0x%08x humans=%u bots=%u", r.launch->settings.mode,
                              r.launch->settings.human_count, r.launch->settings.bot_count);
    std::printf("\n");
}

// One frontend screen (main menu at boot, or after a session ends). Returns
// false when the application should exit.
bool run_frontend(AppContext& ctx, Window& window, ui::Renderer& ui, ui::TextRenderer& text, AppConfig& cfg,
                  audio::AudioSystem* audio, const std::string& press, const std::string& shot, FrontendResult& out) {
    Frontend frontend(ctx.assets, ctx.menu, &ctx.mp_data, &ctx.sp_data);
    sync_config_to_frontend(cfg, frontend);
    frontend.open(FrontendMode::MainMenu);
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
        if (!press.empty()) replay_press(frontend, pad, press);
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
            frontend.draw(ui, text);
            ui.end();
            if (!window.save_bmp(shot)) throw std::runtime_error("cannot write shot");
            std::printf("frontend page 0x%08x -> %s\n", frontend.page_id(), shot.c_str());
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
            play_sounds();
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
    cfg.speaker = o.speaker;
    cfg.widescreen = o.widescreen;
    cfg.screen_x = o.screen_x;
    cfg.screen_y = o.screen_y;
    const PlayerOptions& p = frontend.player_options();
    cfg.controller_style = p.controller_style;
    cfg.invert_y = p.invert_y;
    save_config(config_path(), cfg);
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
    bool mute = false;
};

bool parse_args(int argc, char** argv, Args& a) {
    if (argc < 2) return false;
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
        else if (v == "--drive") need(a.drive);
        else if (v == "--car") need(a.car);
        else if (v == "--movie") need(a.movie);
        else if (v == "--frames" && i + 1 < argc) a.frames = std::atol(argv[++i]);
        else if (v == "--shot") need(a.shot);
        else if (v == "--inputs") need(a.inputs);
        else if (v == "--press") need(a.press);
        else if (v == "--mute") a.mute = true;
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

void usage(const char* prog) {
    std::fprintf(stderr,
                 "usage: %s <gamedir> [--mission level.bin [--difficulty 0|1|2] [--channel CH=VAL]] [--mp MAP_OPTS] "
                 "[--drive name [--car name]] [--movie hexid] [--frames N] [--shot out.bmp] [--inputs file] "
                 "[--press a,b,...] [--mute]\n"
                 "  no session flags: boot to the frontend (title -> main menu -> mission / arena / driving).\n"
                 "  --mp options: nfgame set (--mode/--players/--bots/--frag-limit/--time-limit/--weapons/...).\n",
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

    std::unique_ptr<AppContext> ctx = load_context(args.gamedir);

    // Window first: it owns SDL/GL and must die last (teardown order: menu audio and the
    // UI renderers hold live SDL/GL objects and are destroyed before it).
    const bool direct = !args.mission.empty() || args.mp || !args.drive.empty() || !args.movie.empty();
    const bool headless_opts = args.frames >= 0 || !args.shot.empty() || !args.press.empty() || !args.inputs.empty();
    const bool hidden = (direct && headless_opts) || (!direct && (!args.press.empty() || !args.shot.empty()));
    Window window("nightfire", kWindowW, kWindowH, hidden);

    // Frontend menu audio (best effort: the shared SFX ids, volumes from the settings).
    SoundArchive archive(args.gamedir);
    audio::AudioSystem menu_audio(archive);
    menu_audio.set_sfx_volume(cfg.sfx_volume);
    menu_audio.set_music_volume(cfg.music_volume);
    audio::AudioSystem* menu_audio_ptr = args.mute ? nullptr : &menu_audio;
    if (menu_audio_ptr && !menu_audio_ptr->open_device()) menu_audio_ptr = nullptr;
    ui::Renderer ui(ctx->assets.sprites);
    ui::TextRenderer text(ui, ctx->assets.fonts);

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
        SpLaunch launch{args.mission, args.difficulty >= 0 ? args.difficulty : cfg.difficulty + 1, args.channels};
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
            if (!run_frontend(*ctx, window, ui, text, cfg, menu_audio_ptr, args.press, args.shot, result)) {
                if (!args.press.empty() || !args.shot.empty()) return 0;  // headless menu run done
                if (result.action == FrontendResult::Action::Quit) return 0;
                return 0;
            }
            args.press.clear();
            args.shot.clear();
            if (result.action == FrontendResult::Action::Quit) return 0;
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
                {0x09000001, "paris"}, {0x09000003, "alps"}, {0x09000005, "underwater"}, {0x09000006, "jungle1"}};
            const char* name = nullptr;
            for (const auto& [id, n] : kDriveMissions) {
                if (id == chained) name = n;
            }
            if (!name) continue;  // unmapped driving row (e.g. SnowMobile): back to the frontend
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

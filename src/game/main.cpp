// nfgame: play a level with the original's player movement and collision.
//   nfgame <gamedir> [level.bin] [--coll] [--shot out.bmp] [--frames N] [--inputs file] [--trace out.jsonl]
//
// Interactive controls (docs/gameplay.md has the full table):
//   keyboard + mouse   WASD move/strafe, mouse look (click to capture, Esc releases/quits),
//                      Space jump, C / Left Ctrl crouch, K collision wireframe; left mouse fire, right mouse aim/zoom
//                      (wheel zooms while aiming, else cycles guns), R reload, Tab fire mode, E / Q next / previous gadget
//   gamepad (SDL)      left stick move/strafe, right stick look, A jump, B crouch, right trigger fire, left trigger aim,
//                      X reload, Y next gadget, left bumper fire mode, right bumper next gun
// Headless runs (no window) replay an --inputs file (see tools/oracle/compare.py --make-inputs) and
// write one JSON line per frame to --trace, in the schema of tools/oracle/trace.py.
#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "assets/elf.hpp"
#include "assets/game_files.hpp"
#include "assets/level.hpp"
#include "assets/sound_archive.hpp"
#include "audio/audio.hpp"
#include "game/actions.hpp"
#include "game/drone_cli.hpp"
#include "game/drone_render.hpp"
#include "game/weapon_script.hpp"
#include "game/nfgame_mp.hpp"
#include "game/weapons.hpp"
#include "game/world.hpp"
#include "render/gl.hpp"
#include "render/level_renderer.hpp"
#include "render/window.hpp"

using namespace nf;

namespace {

constexpr float kPi = 3.14159265358979f;

struct ReplayFrame {
    long frame;
    PadState pad;   // already in game form (post psiInput_PollDevices)
    float rate;
    std::optional<Vec3> sync_pos;        // recorded position before this frame (--sync)
    std::optional<float> stand_height;   // recorded collbody+0xCC (animated foot height), if given
};

struct Replay {
    std::optional<Vec3> start_pos;
    float start_yaw = 0, start_pitch = 0, start_ground_normal_y = 1;
    std::vector<ReplayFrame> frames;
};

// Text input file: `start x y z yaw [pitch [ground_normal_y]]`, then `frame sony_word_hex rx ry lx ly
// [frame_rate [stand_height [x y z]]]` per line (x y z: the recorded position before the frame,
// applied with --sync).
Replay read_replay(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open inputs file " + path);
    Replay r;
    std::string line;
    while (std::getline(in, line)) {
        if (const auto hash = line.find('#'); hash != std::string::npos) line.resize(hash);
        std::istringstream s(line);
        std::string head;
        if (!(s >> head)) continue;
        if (head == "start") {
            Vec3 p;
            if (!(s >> p[0] >> p[1] >> p[2] >> r.start_yaw)) throw std::runtime_error("bad start line in " + path);
            if (s >> r.start_pitch) s >> r.start_ground_normal_y;
            r.start_pos = p;
            continue;
        }
        ReplayFrame f{};
        unsigned word, rx, ry, lx, ly;
        f.frame = std::strtol(head.c_str(), nullptr, 10);
        if (!(s >> std::hex >> word >> std::dec >> rx >> ry >> lx >> ly)) throw std::runtime_error("bad input line: " + line);
        f.pad.buttons = buttons_from_sony_pad_word(std::uint16_t(word));
        f.pad.rx = std::uint8_t(rx), f.pad.ry = std::uint8_t(ry), f.pad.lx = std::uint8_t(lx), f.pad.ly = std::uint8_t(ly);
        f.rate = World::kTickHz;
        s >> f.rate;
        float height;
        if (s >> height) f.stand_height = height;
        Vec3 sp;
        if (s >> sp[0] >> sp[1] >> sp[2]) f.sync_pos = sp;
        r.frames.push_back(f);
    }
    return r;
}

std::string hex_bytes(const std::uint8_t* p, std::size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (std::size_t i = 0; i < n; ++i) s += {d[p[i] >> 4], d[p[i] & 15]};
    return s;
}

void write_trace_line(std::FILE* out, long frame, const World& world, const PadState& pad) {
    const Player& p = *world.player(0);
    const ActionInput& in = world.input(0);
    std::fprintf(out, "{\"frame\": %ld, \"pos\": [%.9g, %.9g, %.9g], \"yaw\": %.9g, \"pitch\": %.9g, ", frame, p.pos[0],
                 p.pos[1], p.pos[2], p.yaw, p.pitch);
    std::fprintf(out, "\"pad\": {\"w\": %u, \"s\": [%u, %u, %u, %u]}, \"act\": [", sony_pad_word(pad.buttons), pad.rx, pad.ry,
                 pad.lx, pad.ly);
    for (int a = 0; a < kActionCount; ++a) std::fprintf(out, a ? ", %.9g" : "%.9g", in.actionf(a));
    std::fprintf(out, "], \"flg\": \"%s\", ", hex_bytes(in.flags().data(), in.flags().size()).c_str());
    std::fprintf(out, "\"vel\": [%.9g, %.9g, %.9g], \"fall\": [%.9g, %.9g, %.9g], \"body\": %u, \"gny\": %.9g, ",
                 p.velocity[0], p.velocity[1], p.velocity[2], p.fall_velocity[0], p.fall_velocity[1], p.fall_velocity[2],
                 p.body_flags, p.ground_normal_y);
    const Vec3 eye = p.eye();
    std::fprintf(out, "\"jump\": %u, \"sub\": %d, \"eye\": [%.9g, %.9g, %.9g]}\n", p.jump_state, int(p.substate), eye[0], eye[1],
                 eye[2]);
}

// Keyboard + mouse + gamepad -> one PadState per logic frame.
class HumanInput {
public:
    void handle_event(const SDL_Event& e) {
        if (e.type == SDL_EVENT_MOUSE_MOTION && captured_) {
            mouse_dx_ += e.motion.xrel;
            mouse_dy_ += e.motion.yrel;
        } else if (e.type == SDL_EVENT_MOUSE_WHEEL) {
            wheel_ = e.wheel.y > 0 ? 1 : e.wheel.y < 0 ? -1 : 0;
        } else if (e.type == SDL_EVENT_GAMEPAD_ADDED && !pad_) {
            pad_ = SDL_OpenGamepad(e.gdevice.which);
        } else if (e.type == SDL_EVENT_GAMEPAD_REMOVED && pad_ && SDL_GetGamepadID(pad_) == e.gdevice.which) {
            SDL_CloseGamepad(pad_);
            pad_ = nullptr;
        }
    }
    void set_captured(bool c) { captured_ = c; }
    void open_gamepads() {
        int count = 0;
        if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
            if (count > 0 && !pad_) pad_ = SDL_OpenGamepad(ids[0]);
            SDL_free(ids);
        }
    }

    // Samples the devices for one tick; sticks are returned in game form (after the dead zone).
    PadState sample() {
        PadState raw;
        const bool* k = SDL_GetKeyboardState(nullptr);
        auto axis = [](bool neg, bool pos) { return std::uint8_t(neg == pos ? 0x80 : neg ? 0x00 : 0xFF); };
        // DS2 default layout: left stick Y walks, right stick X strafes, left stick X turns, right stick Y looks.
        raw.ly = axis(k[SDL_SCANCODE_W] || k[SDL_SCANCODE_UP], k[SDL_SCANCODE_S] || k[SDL_SCANCODE_DOWN]);
        raw.rx = axis(k[SDL_SCANCODE_A], k[SDL_SCANCODE_D]);
        raw.lx = axis(k[SDL_SCANCODE_LEFT], k[SDL_SCANCODE_RIGHT]);
        raw.ry = axis(k[SDL_SCANCODE_PAGEUP], k[SDL_SCANCODE_PAGEDOWN]);
        if (k[SDL_SCANCODE_SPACE]) raw.buttons |= kPadTriangle;
        if (k[SDL_SCANCODE_C] || k[SDL_SCANCODE_LCTRL]) raw.buttons |= kPadL2;
        // Weapons (psiInput_MapInputs style 7): R1 fire, L1 aim/zoom, Cross reload/use, Square fire mode,
        // Circle / Left next / previous gadget, R2 / Up next / previous gun (Up / Down also zoom while aiming).
        if (k[SDL_SCANCODE_R]) raw.buttons |= kPadCross;
        if (k[SDL_SCANCODE_TAB]) raw.buttons |= kPadSquare;
        if (k[SDL_SCANCODE_E]) raw.buttons |= kPadCircle;
        if (k[SDL_SCANCODE_Q]) raw.buttons |= kPadLeft;
        if (wheel_ != 0) {
            raw.buttons |= wheel_ > 0 ? kPadUp : kPadR2;
            wheel_ = 0;
        }
        if (pad_) apply_gamepad(raw);
        PadState out = compensate_sticks(raw);
        // Mouse look bypasses the dead zone: pixels per tick map straight onto stick deflection.
        if (mouse_dx_ != 0 || mouse_dy_ != 0) {
            out.lx = stick_from_delta(mouse_dx_, out.lx);
            out.ry = stick_from_delta(mouse_dy_, out.ry);
        }
        if (captured_) {
            const Uint32 mb = SDL_GetMouseState(nullptr, nullptr);
            if (mb & SDL_BUTTON_LMASK) out.buttons |= kPadR1;
            if (mb & SDL_BUTTON_RMASK) out.buttons |= kPadL1;
        }
        mouse_dx_ = mouse_dy_ = 0;
        return out;
    }

private:
    static std::uint8_t stick_from_delta(float delta, std::uint8_t current) {
        constexpr float kUnitsPerPixel = 4.0f;
        const int v = int(std::lround(float(current) + delta * kUnitsPerPixel));
        return std::uint8_t(std::clamp(v, 0, 255));
    }

    void apply_gamepad(PadState& raw) {
        auto stick = [this](SDL_GamepadAxis a) { return std::uint8_t(std::clamp(int(std::lround(SDL_GetGamepadAxis(pad_, a) / 32767.0 * 127.0)) + 0x80, 0, 255)); };
        auto pressed = [this](SDL_GamepadButton b) { return SDL_GetGamepadButton(pad_, b); };
        // Left stick walks/strafes, right stick turns/looks (the DS2 layout swaps X between them).
        const auto lx = stick(SDL_GAMEPAD_AXIS_LEFTX), ly = stick(SDL_GAMEPAD_AXIS_LEFTY);
        const auto rx = stick(SDL_GAMEPAD_AXIS_RIGHTX), ry = stick(SDL_GAMEPAD_AXIS_RIGHTY);
        if (lx != 0x80 || ly != 0x80 || rx != 0x80 || ry != 0x80) {
            raw.rx = lx;
            raw.ly = ly;
            raw.lx = rx;
            raw.ry = ry;
        }
        if (pressed(SDL_GAMEPAD_BUTTON_SOUTH)) raw.buttons |= kPadTriangle;
        if (pressed(SDL_GAMEPAD_BUTTON_EAST)) raw.buttons |= kPadL2;
        if (SDL_GetGamepadAxis(pad_, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) raw.buttons |= kPadL1;    // aim
        if (SDL_GetGamepadAxis(pad_, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) raw.buttons |= kPadR1;  // fire
        if (pressed(SDL_GAMEPAD_BUTTON_WEST)) raw.buttons |= kPadCross;                               // reload / use
        if (pressed(SDL_GAMEPAD_BUTTON_NORTH)) raw.buttons |= kPadCircle;
        if (pressed(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) raw.buttons |= kPadSquare;                     // fire mode
        if (pressed(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) raw.buttons |= kPadR2;                        // next gun
        if (pressed(SDL_GAMEPAD_BUTTON_START)) raw.buttons |= kPadStart;
        if (pressed(SDL_GAMEPAD_BUTTON_BACK)) raw.buttons |= kPadSelect;
        if (pressed(SDL_GAMEPAD_BUTTON_DPAD_UP)) raw.buttons |= kPadUp;
        if (pressed(SDL_GAMEPAD_BUTTON_DPAD_DOWN)) raw.buttons |= kPadDown;
        if (pressed(SDL_GAMEPAD_BUTTON_DPAD_LEFT)) raw.buttons |= kPadLeft;
        if (pressed(SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) raw.buttons |= kPadRight;
    }

    SDL_Gamepad* pad_ = nullptr;
    bool captured_ = false;
    float mouse_dx_ = 0, mouse_dy_ = 0;
    int wheel_ = 0;   // mouse wheel notch since the last tick: +1 up, -1 down
};

// Sound output of the local player (player 0): the level's bank, the listener at the camera, and the WeaponSystem's
// sound events (Sound_Play / Sound_Play3D calls of the weapon code).
class GameAudio {
public:
    GameAudio(const std::filesystem::path& gamedir, const std::string& bin_name) {
        archive_ = std::make_unique<SoundArchive>(gamedir);
        audio_ = std::make_unique<audio::AudioSystem>(*archive_);
        level_ = audio_->enter_level(std::uint32_t(std::strtoul(bin_name.c_str(), nullptr, 16)));
        device_ = audio_->open_device();
    }
    bool level_bank() const { return level_; }
    bool device() const { return device_; }
    void frame(const Player& p, WeaponEvents& events) {
        for (const SoundEvent& e : events.sounds) {
            if (e.exclude == 0 || (e.listener >= 0 && e.listener != 0)) continue;
            audio::PlayOptions o;
            if (e.positional) o.position = e.position;
            audio_->play_sfx(std::uint32_t(e.id), o);
        }
        events.sounds.clear();
        const float c = std::cos(p.view_pitch());
        audio::Listener l;
        l.position = p.eye();
        l.dir = {std::sin(p.yaw) * c, std::sin(p.view_pitch()), std::cos(p.yaw) * c};
        l.norm = {std::cos(p.yaw), 0.0f, -std::sin(p.yaw)};   // Mat_GetNorm: the listener's left
        audio_->set_listener(l);
        audio_->update();   // SFXUpdate runs at 60 Hz; one tick is two frames
        audio_->update();
    }

private:
    std::unique_ptr<SoundArchive> archive_;
    std::unique_ptr<audio::AudioSystem> audio_;
    bool level_ = false, device_ = false;
};

struct View {
    Vec3 eye;
    float yaw, pitch;
};

View view_of(const Player& p) { return {p.eye(), p.yaw, p.view_pitch()}; }

Camera camera_for(const View& prev, const View& cur, float alpha) {
    auto lerp = [alpha](float a, float b) { return a + (b - a) * alpha; };
    Camera cam;
    cam.eye = {lerp(prev.eye[0], cur.eye[0]), lerp(prev.eye[1], cur.eye[1]), lerp(prev.eye[2], cur.eye[2])};
    cam.yaw = lerp(prev.yaw, cur.yaw) + kPi;   // Camera looks down -Z at yaw 0, the player down +Z
    cam.pitch = lerp(prev.pitch, cur.pitch);
    return cam;
}

int run(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: %s <gamedir> [level.bin] [--coll] [--shot out.bmp] [--frames N] [--inputs file [--sync]] [--trace out.jsonl]\n",
                     argv[0]);
        return 2;
    }

    // Multiplayer (--mp / --mode ...): an arena match with split screen, see nfgame_mp.cpp.
    {
        const std::vector<std::string> args(argv + 2, argv + argc);
        const auto has = [&](const char* flag) { return std::find(args.begin(), args.end(), flag) != args.end(); };
        if (has("--mp") || has("--mode") || has("--bots")) {
            MatchLaunch launch;
            launch.gamedir = argv[1];
            for (std::size_t i = 0; i < args.size(); ++i) {
                const std::string& a = args[i];
                if (launch.options.parse(args, i)) continue;
                if (a == "--shot" && i + 1 < args.size()) launch.shot = args[++i];
                else if (a == "--frames" && i + 1 < args.size()) launch.frames = std::atol(args[++i].c_str());
                else if (a == "--coll") launch.collision_wireframe = true;
                else if (a == "--bot-char" && i + 1 < args.size()) launch.bot_characters = args[++i];
                else if (a == "--bot-log") launch.bot_log = true;
                else if (a == "--bot-log-states") launch.bot_log = launch.bot_log_states = true;
                else if (a == "--follow-bot" && i + 1 < args.size()) launch.follow_bot = std::atoi(args[++i].c_str());
                else if (a == "--cam" && i + 5 < args.size()) {
                    launch.has_cam = true;
                    for (float& c : launch.cam) c = float(std::atof(args[++i].c_str()));
                }
                else if ((a == "--inputs" || a == "--inputs2" || a == "--inputs3" || a == "--inputs4") && i + 1 < args.size())
                    launch.inputs[a == "--inputs" ? 0 : std::size_t(a.back() - '1')] = args[++i];
                else if (a.rfind("--", 0) != 0) launch.level_bin = a;
            }
            return run_match(launch);
        }
    }
    std::string bin_name, shot, inputs_path, trace_path, script_path;
    long weapon_seed = 1;
    int player_count = 1;
    bool print_events = false, mute = false;
    long frames = -1;
    bool show_collision = false, sync = false;
    drone::DroneCli drone_cli;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--shot" && i + 1 < argc) shot = argv[++i];
        else if (a == "--frames" && i + 1 < argc) frames = std::atol(argv[++i]);
        else if (a == "--inputs" && i + 1 < argc) inputs_path = argv[++i];
        else if (a == "--trace" && i + 1 < argc) trace_path = argv[++i];
        else if (a == "--script" && i + 1 < argc) script_path = argv[++i];
        else if (a == "--seed" && i + 1 < argc) weapon_seed = std::atol(argv[++i]);
        else if (a == "--players" && i + 1 < argc) player_count = std::clamp(std::atoi(argv[++i]), 1, World::kMaxPlayers);
        else if (a == "--events") print_events = true;
        else if (a == "--mute") mute = true;
        else if (a == "--coll") show_collision = true;
        else if (a == "--sync") sync = true;
        else if (drone_cli.parse(argc, argv, i)) {}   // --drone / --cam / --follow-drone ... (drone_cli.hpp)
        else bin_name = a;
    }

    const std::filesystem::path dir = argv[1];
    GameFiles gf(dir);
    std::vector<std::uint8_t> bin = read_level_bin(gf, bin_name);
    if (bin.empty()) {
        std::fprintf(stderr, "no such level .bin: %s\n", bin_name.c_str());
        return 1;
    }
    Level level(std::move(bin));
    if (!level.map()) {
        std::fprintf(stderr, "%s has no Map entry\n", bin_name.c_str());
        return 1;
    }
    const Elf32 action_elf(read_file(dir / "ACTION.ELF"));
    const std::vector<SpawnPoint> spawns = find_spawn_points(level);
    if (spawns.empty()) {
        std::fprintf(stderr, "%s has no player start markers\n", bin_name.c_str());
        return 1;
    }
    // Arenas use TuningVars.txt [MULTIPLAYER] (Plr_DMod_*), the single-player levels their own section (not modelled: [GLOBAL]).
    const bool multiplayer = spawns.front().kind == SpawnPoint::Kind::Multiplayer;
    PlayerParams params;
    if (const GameFile* tuning = gf.find("TuningVars.txt")) {
        const auto bytes = gf.read(*tuning);
        params = player_params_from_tuning(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()),
                                           multiplayer ? "MULTIPLAYER" : "");
    }
    params.health.damage.mode = multiplayer ? GameMode::Multiplayer : GameMode::SinglePlayer;

    World world(level, InputTables::from_elf(action_elf), params);
    Player& player = world.spawn_player(0, spawns.front());
    // Weapons: weapon_data / ammo_data from the ELF's static initializer, skins and anim scripts from the level's
    // animation bank; damage to players goes through Player::hurt (params.health.damage).
    std::unique_ptr<CharacterBank> weapon_bank = open_character_bank(gf, bin_name);
    auto weapon_system = std::make_unique<WeaponSystem>(WeaponTable::from_elf(action_elf), params.health.damage, std::uint32_t(weapon_seed));
    WeaponSystem& weapons = *weapon_system;
    weapons.set_bank(weapon_bank.get());
    world.add_system(std::move(weapon_system));
    if (drone_cli.enabled()) drone_cli.setup(world, level, *weapon_bank, action_elf, gf, weapons, bin_name);
    for (int i = 1; i < player_count; ++i) world.spawn_player(i, spawns[std::min<std::size_t>(std::size_t(i) * 3, spawns.size() - 1)]);
    std::optional<WeaponScript> script;
    std::unique_ptr<GameAudio> game_audio;
    if (!script_path.empty()) script = WeaponScript::parse(script_path);
    std::printf("%s: %zu placements, %zu solid, spawn '%s' at %.2f,%.2f,%.2f\n", bin_name.c_str(), level.placements().size(),
                world.collision().solid_count(), spawns.front().model.c_str(), player.pos[0], player.pos[1], player.pos[2]);

    Replay replay;
    if (!inputs_path.empty()) {
        replay = read_replay(inputs_path);
        if (replay.start_pos)
            player.place_at_rest(*replay.start_pos, replay.start_yaw, replay.start_pitch, replay.start_ground_normal_y);
    }

    std::FILE* trace = trace_path.empty() ? nullptr : std::fopen(trace_path.c_str(), "w");
    if (!trace_path.empty() && !trace) throw std::runtime_error("cannot write " + trace_path);

    // Fixed-step simulation of a scripted (or idle) input stream.
    const long scripted = frames >= 0 ? frames : std::max(long(replay.frames.size()), script ? script->last_frame() + 1 : 0L);
    for (long i = 0; i < scripted; ++i) {
        PadInputs pads{};
        const ReplayFrame* f = i < long(replay.frames.size()) ? &replay.frames[std::size_t(i)] : nullptr;
        if (f) pads[0] = f->pad;
        if (f && f->stand_height) player.stand_height = *f->stand_height;
        if (sync && f && f->sync_pos) player.pos = *f->sync_pos;
        if (script) script->apply(i, world, weapons, pads);
        world.tick(pads, FrameTiming{f ? f->rate : World::kTickHz});
        drone_cli.after_tick(world);
        if (print_events) WeaponScript::dump_events(i, weapons.events());
        weapons.events().clear();
        if (trace) write_trace_line(trace, f ? f->frame : long(world.frame()), world, pads[0]);
    }
    const bool headless = shot.empty() && (trace || script || drone_cli.enabled()) && (!inputs_path.empty() || frames >= 0 || script);
    if (headless) {
        if (trace) std::fclose(trace);
        std::printf("final pos %.4f,%.4f,%.4f yaw %.4f\n", player.pos[0], player.pos[1], player.pos[2], player.yaw);
        return 0;   // headless: no window
    }

    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");   // pads keep working when the window is not focused
    Window window("nfgame - " + bin_name, 1280, 720, !shot.empty());
    LevelRenderer renderer(level);
    std::optional<drone::DroneRenderer> drone_renderer;
    if (drone_cli.system()) drone_renderer.emplace(*weapon_bank);
    glEnable(GL_DEPTH_TEST);
    // --cam / --follow-drone: camera override (game yaw convention, see drone_cli.hpp).
    auto apply_drone_camera = [&](Camera& cam) {
        Vec3 eye;
        float yaw, pitch;
        if (drone_cli.camera(eye, yaw, pitch)) cam.eye = eye, cam.yaw = yaw + kPi, cam.pitch = pitch;
    };
    auto draw = [&](const Camera& cam) {
        int width, height;
        window.begin_frame(width, height);
        glClearColor(0.25f, 0.3f, 0.4f, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        renderer.draw(cam, float(width) / float(std::max(height, 1)), show_collision);
        if (drone_renderer) drone_renderer->draw(cam, float(width) / float(std::max(height, 1)), *drone_cli.system());
    };

    if (!shot.empty()) {
        if (trace) std::fclose(trace);
        const View v = view_of(player);
        Camera cam = camera_for(v, v, 0.0f);
        apply_drone_camera(cam);
        draw(cam);
        const bool ok = window.save_bmp(shot);
        std::printf("pos %.2f,%.2f,%.2f yaw %.3f pitch %.3f -> %s\n", player.pos[0], player.pos[1], player.pos[2], player.yaw,
                    player.pitch, ok ? shot.c_str() : SDL_GetError());
        return ok ? 0 : 1;
    }

    if (!mute) {
        game_audio = std::make_unique<GameAudio>(dir, bin_name);
        std::printf("audio: level bank %s, output device %s\n", game_audio->level_bank() ? "loaded" : "none", game_audio->device() ? "open" : "none");
    }
    HumanInput human;
    human.open_gamepads();
    bool running = true, captured = false;
    View prev = view_of(player);
    double accumulator = 0;
    Uint64 last = SDL_GetTicksNS();
    constexpr double kStep = 1.0 / World::kTickHz;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            human.handle_event(e);
            if (e.type == SDL_EVENT_QUIT) running = false;
            else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && !captured) {
                captured = SDL_SetWindowRelativeMouseMode(window.sdl(), true);
                human.set_captured(captured);
            } else if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) {
                if (captured) {
                    captured = !SDL_SetWindowRelativeMouseMode(window.sdl(), false);
                    human.set_captured(captured);
                } else {
                    running = false;
                }
            } else if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_K) {
                show_collision = !show_collision;
            }
        }
        const Uint64 now = SDL_GetTicksNS();
        accumulator = std::min(accumulator + double(now - last) * 1e-9, 0.25);
        last = now;
        while (accumulator >= kStep) {
            prev = view_of(player);
            PadInputs pads{};
            pads[0] = human.sample();
            world.tick(pads);
            drone_cli.after_tick(world);
            if (game_audio) game_audio->frame(player, weapons.events());
            weapons.events().clear();
            if (trace) write_trace_line(trace, long(world.frame()), world, pads[0]);
            accumulator -= kStep;
        }
        Camera cam = camera_for(prev, view_of(player), float(accumulator / kStep));
        apply_drone_camera(cam);
        draw(cam);
        window.swap();
    }
    if (trace) std::fclose(trace);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "nfgame: %s\n", e.what());
        return 1;
    }
}

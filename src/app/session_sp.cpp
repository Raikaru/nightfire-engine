// Single-player ACTION session: build, tick, draw, pause and end-mission.
#include "app/session_sp.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "assets/character.hpp"
#include "assets/level.hpp"
#include "assets/menu_validate.hpp"
#include "audio/audio.hpp"
#include "audio/music_director.hpp"
#include "game/drone_render.hpp"
#include "game/drone_system.hpp"
#include "game/local_pad.hpp"
#include "game/nav.hpp"
#include "game/nfgame_effects.hpp"
#include "game/nfgame_weapon_view.hpp"
#include "game/sp_common.hpp"
#include "game/sp_placement.hpp"
#include "game/sp_tables.hpp"
#include "game/weapons.hpp"
#include "game/world.hpp"
#include "render/character_renderer.hpp"
#include "render/gl.hpp"
#include "render/level_renderer.hpp"

namespace nf::app {

namespace {

// P_ENDMISSION lives in the level-bin menu script (frontend_level.cpp).
constexpr std::uint32_t kPageEndMission = 0x40000042;

// Menu pad from the keyboard (nfui mapping) plus the first gamepad's buttons.
PadState menu_pad(SDL_Gamepad* gamepad) {
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
    if (k[SDL_SCANCODE_Q]) s.buttons |= kPadL1;
    if (k[SDL_SCANCODE_E]) s.buttons |= kPadR1;
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

}  // namespace

struct SpSession::Impl {
    Impl(AppContext& c, Window& w, ui::Renderer& u, ui::TextRenderer& t, const SpLaunch& l, const AppConfig& cfg)
        : ctx(c), window(w), ui(u), text(t), launch(l), config(cfg) {}

    AppContext& ctx;
    Window& window;
    ui::Renderer& ui;
    ui::TextRenderer& text;
    SpLaunch launch;
    AppConfig config;

    // Declared in dependency order (Level outlives World, bank outlives renderers).
    std::unique_ptr<Level> level;
    std::uint32_t level_id = 0;
    std::unique_ptr<World> world;
    std::unique_ptr<CharacterBank> bank;
    WeaponSystem* weapons = nullptr;  // owned by World
    drone::DroneSystem* drones = nullptr;
    sp::SpSystem* spsys = nullptr;
    std::unique_ptr<NavNetwork> nav;
    SpriteLibrary fx_sprites;  // level chunk sprites for WeaponEffects (must outlive `effects`)
    std::unique_ptr<WeaponEffects> effects;
    std::unique_ptr<Hud> hud;
    std::unique_ptr<LevelRenderer> renderer;
    std::unique_ptr<CharacterRenderer> chars;
    std::unique_ptr<WeaponView> weapon_view;
    std::unique_ptr<drone::DroneRenderer> drone_renderer;
    MenuFile level_menu;
    bool has_level_menu = false;

    std::unique_ptr<SoundArchive> archive;
    std::unique_ptr<audio::AudioSystem> audio;
    std::unique_ptr<audio::MusicDirector> music;
    SDL_Gamepad* gamepad = nullptr;
    LocalPad pad{true, nullptr};

    int death_frames = 0;  // ticks since the player died / mission failed (0 = running)
    bool end_shown = false;

    bool build() {
        std::string bin = launch.bin;
        std::vector<std::uint8_t> bytes = read_level_bin(ctx.files, bin);
        if (bytes.empty()) {
            std::fprintf(stderr, "nightfire: no such level .bin: %s\n", launch.bin.c_str());
            return false;
        }
        level = std::make_unique<Level>(std::move(bytes));
        if (!level->map()) {
            std::fprintf(stderr, "nightfire: %s has no Map entry\n", launch.bin.c_str());
            return false;
        }
        level_id = level_id_from_name(launch.bin);
        const std::vector<SpawnPoint> spawns = find_spawn_points(*level);
        if (spawns.empty()) {
            std::fprintf(stderr, "nightfire: %s has no player start markers\n", launch.bin.c_str());
            return false;
        }
        std::string tuning_text;
        PlayerParams params;
        if (const GameFile* tuning = ctx.files.find("TuningVars.txt")) {
            const auto raw = ctx.files.read(*tuning);
            tuning_text.assign(reinterpret_cast<const char*>(raw.data()), raw.size());
            params = player_params_from_tuning(tuning_text, "");
        }
        params.health.damage.mode = GameMode::SinglePlayer;

        world = std::make_unique<World>(*level, InputTables::from_elf(ctx.action_elf), params);
        world->spawn_player(0, spawns.front());

        bank = open_character_bank(ctx.files, launch.bin);
        auto weapons_owned =
            std::make_unique<WeaponSystem>(WeaponTable::from_elf(ctx.action_elf), params.health.damage, 1u);
        weapons_owned->set_bank(bank.get());
        weapons = weapons_owned.get();
        world->add_system(std::move(weapons_owned));

        // NPCs: nav + drones + the single-player layer (mirrors DroneCli::setup --sp).
        drone::DroneConfig dcfg;
        dcfg.elf = &ctx.action_elf;
        dcfg.level_id = level_id;
        dcfg.difficulty = launch.difficulty;
        dcfg.tuning = drone::DroneTuning::load(tuning_text, level_id);
        nav = std::make_unique<NavNetwork>(*level, world->collision(), NavLimits::for_level(level_id));
        auto sys = std::make_unique<drone::DroneSystem>(*world, *bank, dcfg);
        drones = sys.get();
        drones->set_nav(nav->empty() ? nullptr : nav.get());
        drones->set_weapons(weapons);
        drones->callbacks().player_alive = [this](int slot) { return weapons->alive(slot); };
        sp::SpConfig spcfg;
        spcfg.level_id = level_id;
        spcfg.difficulty = launch.difficulty;
        auto sp = std::make_unique<sp::SpSystem>(*drones, sp::parse_sp_level(*level, level_id),
                                                 sp::SpTables::load(ctx.action_elf), spcfg);
        spsys = sp.get();
        world->add_system(std::move(sp));
        world->add_system(std::move(sys));
        const std::size_t placed = spsys->spawn_placed();

        if (level->map()) fx_sprites.add(level->map()->chunk);
        effects = std::make_unique<WeaponEffects>(weapons->table(), *bank, &fx_sprites);
        effects->set_map_lights(&bank->lights());

        add_level_sprites(ctx.assets.sprites, Bytes(read_level_bin(ctx.files, bin)));
        HudConfig hcfg;
        hcfg.frame_rate = 30.0f;
        hud = std::make_unique<Hud>(ctx.assets, ctx.hud_data, hcfg);

        renderer = std::make_unique<LevelRenderer>(*level);
        renderer->set_level(level_id);
        chars = std::make_unique<CharacterRenderer>(*bank);
        weapon_view = std::make_unique<WeaponView>(*bank, *chars);
        drone_renderer = std::make_unique<drone::DroneRenderer>(*bank);

        if (const GameFile* f = ctx.files.find(launch.bin)) {
            level_menu = load_menu_from_bin(Bytes(ctx.files.read(*f)));
            has_level_menu = true;
        }
        // Audio: level bank, volumes from the persisted settings, music script.
        archive = std::make_unique<SoundArchive>(std::filesystem::path(ctx.gamedir));
        audio = std::make_unique<audio::AudioSystem>(*archive);
        audio->enter_level(level_id);
        audio->set_sfx_volume(config.sfx_volume);
        audio->set_music_volume(config.music_volume);
        music = std::make_unique<audio::MusicDirector>(*audio, ctx.music);
        music->start_level(level_id);

        // NPC -> app hooks: text, music, speech/SFX and mission fail.
        spsys->on_text = [this](std::uint32_t label) { hud->add_message(HudMessage{HudMsgType::Info, label, {}, 180}); };
        spsys->on_music_event = [this](int event, int arg) { music->event(std::uint32_t(event), arg); };
        spsys->on_combat_music = [this] { music->event(2, 5); };
        spsys->on_sfx = [this](drone::Drone&, int sfx_id) { return audio->play_sfx(std::uint32_t(sfx_id)); };
        spsys->sfx_finished = [this](int handle) { return !audio->is_playing(handle); };
        spsys->on_mission_fail = [this](drone::Drone&, int, std::uint32_t label) {
            if (label != 0) hud->add_message(HudMessage{HudMsgType::Mission, label, {}, 300});
        };

        const Player& p = *world->player(0);
        std::printf("%s: %zu placements, %zu npcs, spawn at %.2f,%.2f,%.2f\n", launch.bin.c_str(),
                    level->placements().size(), placed, p.pos[0], p.pos[1], p.pos[2]);
        return true;
    }

    void audio_frame() {
        for (const SoundEvent& e : weapons->events().sounds) {
            if (e.exclude == 0 || (e.listener >= 0 && e.listener != 0)) continue;
            audio::PlayOptions o;
            if (e.positional) o.position = e.position;
            audio->play_sfx(std::uint32_t(e.id), o);
        }
        weapons->events().sounds.clear();
        const Player& p = *world->player(0);
        const float c = std::cos(p.view_pitch());
        audio::Listener l;
        l.position = p.eye();
        l.dir = {std::sin(p.yaw) * c, std::sin(p.view_pitch()), std::cos(p.yaw) * c};
        l.norm = {std::cos(p.yaw), 0.0f, -std::sin(p.yaw)};  // Mat_GetNorm: the listener's left
        audio->set_listener(l);
        music->update();
        audio->update();
        audio->update();
    }

    void tick_world(const PadState& pad) {
        PadInputs pads{};
        pads[0] = pad;
        world->tick(pads);
        effects->consume(weapons->events());
        effects->tick(FrameTiming{}.mul());
        weapons->events().clear();
        if (!world->player(0)->alive() || spsys->mission_fail_reason != 0) ++death_frames;
    }

    void draw_frame(const Camera& cam) {
        int width = 0, height = 0;
        window.begin_frame(width, height);
        const float aspect = camera_aspect(width, height);
        Camera wc = cam;
        wc.fovy = 1.1f / std::max(1.0f, weapons->zoom(0));
        glClearColor(0.25f, 0.3f, 0.4f, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        renderer->draw(wc, aspect, false);
        drone_renderer->draw(wc, aspect, *drones);
        effects->draw(wc, aspect, *chars, weapons->projectiles());
        glClear(GL_DEPTH_BUFFER_BIT);
        const ViewModel vm = weapons->viewmodel(0);
        if (vm.visible && vm.skin && vm.anim) {
            const Vec3 muzzle =
                weapon_view->draw(wc, aspect, vm, weapons->table().weapon(vm.weapon), effects->lighting_at(wc.eye, 2.0f));
            if (vm.muzzle_flash > 0.0f && muzzle != Vec3{0, 0, 0}) effects->add_light(muzzle, vm.flash_color, 5.0f, 2.0f);
        }
        glEnable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        // HUD overlay (the ui canvas letterboxes over the 3D frame; no clear).
        HudState hs;
        weapons->fill_hud(0, hs);
        hud->update(hs);
        ui.begin(width, height, false);
        hud->draw(ui, text);
        ui.end();
        glDisable(GL_BLEND);
        glEnable(GL_DEPTH_TEST);
    }

    // Pause / end-mission menu on the shared window. Returns Resume (false) or the session exit.
    std::optional<SpResult> run_menu(std::optional<std::uint32_t> page) {
        if (!has_level_menu) return std::nullopt;
        Frontend menu(ctx.assets, level_menu, &ctx.mp_data, &ctx.sp_data);
        PauseInfo info;
        menu.set_pause_info(info);
        menu.open(FrontendMode::Pause, page);
        PadHistory hist;
        bool wait_release = true;
        while (!menu.wants_close()) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_EVENT_QUIT) return SpResult{SpExit::QuitToMenu};
                if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE && !page) return std::nullopt;  // Esc resumes pause
            }
            const PadState s = menu_pad(gamepad);
            if (wait_release) {
                if (s.buttons == 0) wait_release = false;
                hist.push({});
            } else {
                hist.push(s);
            }
            menu.update(hist);
            int w, h;
            window.begin_frame(w, h);
            ui.begin(w, h);
            menu.draw(ui, text);
            ui.end();
            window.swap();
            SDL_Delay(33);
        }
        const FrontendResult& r = menu.result();
        if (r.action == FrontendResult::Action::Resume) return std::nullopt;
        if (r.action == FrontendResult::Action::RestartMission) return SpResult{SpExit::Restart};
        return SpResult{SpExit::QuitToMenu};
    }
};

SpSession::SpSession(AppContext& ctx, Window& window, ui::Renderer& ui, ui::TextRenderer& text, const SpLaunch& launch,
                     const AppConfig& cfg)
    : impl_(std::make_unique<Impl>(ctx, window, ui, text, launch, cfg)) {
    ready_ = impl_->build();
}

SpSession::~SpSession() = default;

SpResult SpSession::run_interactive() {
    Impl& s = *impl_;
    int count = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
        if (count > 0) {
            s.gamepad = SDL_OpenGamepad(ids[0]);
            s.pad = LocalPad(true, s.gamepad);
        }
        SDL_free(ids);
    }
    const bool has_audio = s.audio->open_device();
    std::printf("audio: output device %s\n", has_audio ? "open" : "none");

    bool running = true, captured = false;
    double accumulator = 0;
    Uint64 last = SDL_GetTicksNS();
    constexpr double kStep = 1.0 / 30.0;
    SpResult done{SpExit::QuitToMenu};
    bool finished = false;
    while (running && !finished) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            s.pad.handle_event(e);
            if (e.type == SDL_EVENT_QUIT) running = false;
            else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && !captured) {
                captured = SDL_SetWindowRelativeMouseMode(s.window.sdl(), true);
                s.pad.set_captured(captured);
            } else if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) {
                if (captured) {
                    captured = !SDL_SetWindowRelativeMouseMode(s.window.sdl(), false);
                    s.pad.set_captured(captured);
                }
                if (auto r = s.run_menu(std::nullopt)) {
                    done = *r;
                    finished = true;
                }
                last = SDL_GetTicksNS();
            }
        }
        const Uint64 now = SDL_GetTicksNS();
        accumulator = std::min(accumulator + double(now - last) * 1e-9, 0.25);
        last = now;
        while (accumulator >= kStep) {
            PadState pad = s.pad.sample();
            if (pad.buttons & kPadStart) {
                if (auto r = s.run_menu(std::nullopt)) {
                    done = *r;
                    finished = true;
                } else {
                    last = SDL_GetTicksNS();
                    accumulator = 0;
                }
                break;
            }
            s.tick_world(pad);
            if (has_audio) s.audio_frame();
            accumulator -= kStep;
            if (s.death_frames > 90 && !s.end_shown) {
                s.end_shown = true;
                if (auto r = s.run_menu(kPageEndMission)) {
                    done = *r;
                    done.failed = true;
                    finished = true;
                } else {
                    done = SpResult{SpExit::QuitToMenu, true};
                    finished = true;
                }
                break;
            }
        }
        if (finished) break;
        const Player& p = *s.world->player(0);
        s.draw_frame(camera_for_eye_yaw_pitch(p.eye(), p.yaw, p.view_pitch()));
        s.window.swap();
    }
    if (s.gamepad) SDL_CloseGamepad(s.gamepad);
    return done;
}

SpResult SpSession::run_headless(const SpHeadless& headless) {
    Impl& s = *impl_;
    // Optional nfgame-format replay (start line + `frame word rx ry lx ly` lines).
    struct Frame {
        PadState pad;
    };
    std::vector<Frame> script;
    Vec3 start_pos{};
    bool has_start = false;
    float start_yaw = 0, start_pitch = 0;
    if (!headless.inputs.empty()) {
        std::ifstream in(headless.inputs);
        if (!in) throw std::runtime_error("cannot open inputs file " + headless.inputs);
        std::string line;
        while (std::getline(in, line)) {
            if (const auto hash = line.find('#'); hash != std::string::npos) line.resize(hash);
            std::istringstream ls(line);
            std::string head;
            if (!(ls >> head)) continue;
            if (head == "start") {
                if (!(ls >> start_pos[0] >> start_pos[1] >> start_pos[2] >> start_yaw)) throw std::runtime_error("bad start line");
                ls >> start_pitch;
                has_start = true;
                continue;
            }
            Frame f;
            unsigned word, rx, ry, lx, ly;
            const long n = std::strtol(head.c_str(), nullptr, 10);
            if (!(ls >> std::hex >> word >> std::dec >> rx >> ry >> lx >> ly)) throw std::runtime_error("bad input line");
            f.pad.buttons = buttons_from_sony_pad_word(std::uint16_t(word));
            f.pad.rx = std::uint8_t(rx), f.pad.ry = std::uint8_t(ry), f.pad.lx = std::uint8_t(lx),
            f.pad.ly = std::uint8_t(ly);
            if (n >= long(script.size())) script.resize(std::size_t(n) + 1);
            script[std::size_t(n)] = f;
        }
    }
    if (has_start) s.world->player(0)->place_at_rest(start_pos, start_yaw, start_pitch, 1.0f);
    for (long i = 0; i < headless.frames; ++i) {
        const PadState pad = i < long(script.size()) ? script[std::size_t(i)].pad : PadState{};
        s.tick_world(pad);
    }
    const Player& p = *s.world->player(0);
    std::printf("%s: %ld frames, pos %.2f,%.2f,%.2f yaw %.3f alive %d npcs %zu\n", s.launch.bin.c_str(), headless.frames,
                p.pos[0], p.pos[1], p.pos[2], p.yaw, int(p.alive()), s.spsys->spawned());
    if (!headless.shot.empty()) {
        s.draw_frame(camera_for_eye_yaw_pitch(p.eye(), p.yaw, p.view_pitch()));
        const bool ok = s.window.save_bmp(headless.shot);
        std::printf("shot -> %s\n", ok ? headless.shot.c_str() : SDL_GetError());
        if (!ok) throw std::runtime_error("cannot write shot");
    }
    return SpResult{SpExit::QuitToMenu, !p.alive() || s.spsys->mission_fail_reason != 0};
}

}  // namespace nf::app

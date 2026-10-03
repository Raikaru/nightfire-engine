// Single-player ACTION session: build, tick, draw, pause and end-mission.
#include "app/session_sp.hpp"

#include "app/movie.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "assets/bin_archive.hpp"
#include "assets/character.hpp"
#include "assets/cutscene.hpp"
#include "assets/level.hpp"
#include "assets/menu_validate.hpp"
#include "assets/mission_data.hpp"
#include "assets/strings.hpp"
#include "audio/audio.hpp"
#include "audio/music_director.hpp"
#include "game/arena_view.hpp"   // kViewFovY (Camera_CalcViewAngles 1.0471976)
#include "game/drone_render.hpp"
#include "game/drone_system.hpp"
#include "game/local_pad.hpp"
#include "game/mission.hpp"
#include "game/nav.hpp"
#include "game/nfgame_effects.hpp"
#include "game/nfgame_weapon_view.hpp"
#include "game/plr_stats.hpp"
#include "game/sp_common.hpp"
#include "game/sp_placement.hpp"
#include "game/sp_tables.hpp"
#include "game/weapons.hpp"
#include "game/world.hpp"
#include "render/character_renderer.hpp"
#include "render/gl.hpp"
#include "render/level_renderer.hpp"
#include "render/weather_renderer.hpp"

namespace nf::app {

namespace {

// P_ENDMISSION lives in the level-bin menu script (frontend_level.cpp); P_NFRESULTS in the frontend script.
constexpr std::uint32_t kPageEndMission = 0x40000042, kPageResults = 0x40000036;

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
    std::vector<std::uint8_t> bin_bytes;  // raw level .bin (cutscenes, menu script)
    std::unique_ptr<Level> level;
    std::uint32_t level_id = 0;
    std::unique_ptr<World> world;
    std::unique_ptr<CharacterBank> bank;
    WeaponSystem* weapons = nullptr;  // owned by World
    drone::DroneSystem* drones = nullptr;
    sp::SpSystem* spsys = nullptr;
    MissionSystem* mission_sys = nullptr;  // owned by World (absent when the level has no MissionData entry)
    std::unique_ptr<NavNetwork> nav;
    SpriteLibrary fx_sprites;  // level chunk sprites for WeaponEffects (must outlive `effects`)
    std::unique_ptr<WeaponEffects> effects;
    std::unique_ptr<Hud> hud;
    std::unique_ptr<LevelRenderer> renderer;
    std::unique_ptr<WeatherRenderer> weather;
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

    int death_frames = 0;  // ticks since the end began (Done, death or fail signal)
    bool end_shown = false;
    bool mission_won = false;  // the state machine reported Succeeded before Done
    PlrStats plr_;              // mission counters feeding the results score
    PlrScoreTables score_tables_;
    bool score_tables_ok_ = false;
    bool give_given = false, give_done = false;  // --give ID debug equip progress
    int prev_muzzle_ = 0;              // muzzle_frames edge = one trigger pull
    std::uint64_t prev_deaths_ = 0;    // SpSystem::stats.deaths edge = kills
    std::uint64_t prev_mframes_ = 0;   // mission frames edge = elapsed time
    bool prev_use_ = false;            // Cross edge for pre_tick (held would flap switches)

    bool build() {
        std::string bin = launch.bin;
        bin_bytes = read_level_bin(ctx.files, bin);
        if (bin_bytes.empty()) {
            std::fprintf(stderr, "nightfire: no such level .bin: %s\n", launch.bin.c_str());
            return false;
        }
        level = std::make_unique<Level>(bin_bytes);  // copy: entries below still need the raw .bin
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
            std::make_unique<WeaponSystem>(WeaponTable::from_elf(ctx.action_elf), params.health.damage);
        weapons_owned->set_autoaim(AutoaimTuning::load(tuning_text, ""));
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
        weapons->set_drone_system(drones);   // idle-fidget threat gate (any_visible_threat)
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

        // Mission flow (Scripting): objectives, movers, cutscenes and success/failure.
        for (const MissionEntry& entry : load_mission_data(ctx.action_elf)) {
            if (entry.level != level_id) continue;
            auto mission = std::make_unique<MissionSystem>(*level, level_id, entry, *weapons, spsys);
            for (const BinEntry& e : parse_bin_archive(Bytes(bin_bytes))) {
                if (e.type != EntryType::Script) continue;
                try {
                    mission->add_scripts({{e.hash, parse_cutscene_bin(e.data)}});
                } catch (const std::exception& ex) {
                    std::fprintf(stderr, "nightfire: %s: cutscene %08x skipped: %s\n", launch.bin.c_str(), e.hash,
                                 ex.what());
                }
            }
            mission_sys = mission.get();
            world->add_system(std::move(mission));
            mission_sys->apply_loadout(0);
            for (const auto& [ch, val] : launch.channels) {
                mission_sys->set_channel(std::uint16_t(ch), std::uint8_t(val));
                std::printf("mission: debug preset channel %d = %d\n", ch, val);
            }
            if (launch.give >= 0)  // --give ID: applied in tick_world once the slot state exists
                std::printf("mission: debug give weapon %d (armed, applies when idle)\n", launch.give);
            break;
        }

        if (level->map()) fx_sprites.add(level->map()->chunk);
        effects = std::make_unique<WeaponEffects>(weapons->table(), *bank, &fx_sprites);
        effects->set_map_lights(bank->lights());
        effects->set_level(level.get());
        for (const BinEntry& e : parse_bin_archive(Bytes(bin_bytes))) {
            if (e.type != EntryType::Script || (e.hash != 0x06000052 && e.hash != 0x060007C4)) continue;
            try {
                effects->set_explosion_script(std::uint32_t(e.hash), e.data);
            } catch (const std::exception& ex) {
                std::fprintf(stderr, "nightfire: %s: effect script %08x skipped: %s\n", launch.bin.c_str(),
                             e.hash, ex.what());
            }
        }

        add_level_sprites(ctx.assets.sprites, Bytes(bin_bytes));
        HudConfig hcfg;
        hcfg.frame_rate = 30.0f;
        hud = std::make_unique<Hud>(ctx.assets, ctx.hud_data, hcfg);

        renderer = std::make_unique<LevelRenderer>(*level);
        renderer->set_level(level_id);
        weather = std::make_unique<WeatherRenderer>(*level);
        weather->set_level(level_id);
        chars = std::make_unique<CharacterRenderer>(*bank);
        weapon_view = std::make_unique<WeaponView>(*bank, *chars);
        drone_renderer = std::make_unique<drone::DroneRenderer>(*bank);

        level_menu = load_menu_from_bin(Bytes(bin_bytes));
        has_level_menu = true;
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
            if (mission_sys) mission_sys->fail_mission(label);  // drone reason fails the mission, not just the message
        };
        // Scoring tables + per-mission counter reset (results score source).
        try {
            score_tables_ = load_plr_score_tables(ctx.action_elf);
            score_tables_ok_ = true;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "nightfire: score tables unavailable: %s\n", e.what());
        }
        plr_.reset_for_mission();
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
        for (const SoundEvent& e : effects->take_blast_sounds()) {
            audio::PlayOptions o;
            if (e.positional) o.position = e.position;
            audio->play_sfx(std::uint32_t(e.id), o);
        }
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

    // --give ID debug equip: slot state is spawned lazily on the first tick and select
    // only adopts from Idle, so give once the state exists, then select once idle.
    void maybe_give() {
        if (launch.give < 0 || give_done) return;
        if (weapons->state(0) == nullptr) return;
        if (!give_given) {
            if (!weapons->give_weapon(0, launch.give, 999)) return;
            give_given = true;
        }
        if (weapons->anim_state(0) != WeaponAnim::Idle) return;
        weapons->select_weapon(0, launch.give);
        give_done = true;
        std::printf("mission: debug give weapon %d equipped\n", launch.give);
    }

    void tick_world(const PadState& pad) {
        PadInputs pads{};
        pads[0] = pad;
        // Movers publish before the players collide (MissionSystem::pre_tick). Cross is an
        // edge here: the switch use-headers toggle on press, and held-Cross would flap them.
        const bool use = pad.held(kPadCross) && !prev_use_;
        prev_use_ = pad.held(kPadCross);
        if (mission_sys) mission_sys->pre_tick(*world, {use, false, false, false});
        world->tick(pads);
        if (weather)
            weather->update(world->player(0)->eye(),
                            [this](int ch) { return spsys->channels.on(ch); });
        feed_stats();
        poll_mission();
        effects->consume(weapons->events());
        effects->tick(FrameTiming{}.mul());
        weapons->events().clear();
        maybe_give();
        // End of mission: the state machine settles on Done (success shows results, failure the
        // end-mission page); without a mission entry the legacy death/fail signals end it.
        if (mission_sys) {
            if (mission_sys->state() == MissionSystem::State::Succeeded) mission_won = true;
            if (mission_sys->state() == MissionSystem::State::Done) ++death_frames;
        } else if (!world->player(0)->alive() || spsys->mission_fail_reason != 0) {
            ++death_frames;
        }
    }

    // PlrStat counter feed (Log* mirrors): trigger pulls from the muzzle edge, hits and
    // scenery from the local player's impacts, kills from drone deaths, elapsed from frames.
    // Detections, disabled/surrender detail, health and bond ids have no observable source
    // yet and stay 0 (documented in plr_stats.hpp).
    void feed_stats() {
        if (const PlayerWeapons* p = weapons->state(0)) {
            if (p->muzzle_frames > prev_muzzle_) plr_.log_shot_fired();
            prev_muzzle_ = p->muzzle_frames;
        }
        for (const ImpactEvent& e : weapons->events().impacts) {
            if (e.shooter != 0) continue;
            if (e.on_body) plr_.log_shot_hit_enemy();
            else plr_.log_shot_hit_scenery();
        }
        const std::uint64_t deaths = spsys->stats.deaths;
        while (prev_deaths_ < deaths) {
            plr_.log_kill();
            ++prev_deaths_;
        }
        if (mission_sys) plr_.set_elapsed_100ths(std::uint32_t(mission_sys->stats().frames * 100u / 30u));
    }
    void poll_mission() {
        if (!mission_sys) return;
        for (const MissionSystem::Sound& s : mission_sys->take_sounds()) {
            audio::PlayOptions o;
            if (s.positional) o.position = s.pos;
            audio->play_sfx(s.id, o);
        }
        for (const MissionSystem::Text& t : mission_sys->take_texts()) {
            HudMsgType type = HudMsgType::Info;
            if (t.type == 2) type = HudMsgType::Objective;
            else if (t.type == 3) type = HudMsgType::Mission;
            else if (t.type == 4) type = HudMsgType::Subtitle;
            hud->add_message(HudMessage{type, t.label, {}, t.frames});
        }
        for (const MissionSystem::Music& m : mission_sys->take_music()) music->event(std::uint32_t(m.event), m.value);
        // Coder spawns (Bots): zeroed-DIVars default drones at the event feet (DMODE 0, Idle).
        // Log lines match DroneCli::drain_coder_spawns for cross-tool greppability.
        for (const MissionSystem::Spawn& s : mission_sys->take_spawns()) {
            std::printf("mission spawn: %.1f,%.1f,%.1f\n", s.pos[0], s.pos[1], s.pos[2]);
            if (drone::Drone* d = spsys->spawn_scripted({s.pos[0], s.pos[1], s.pos[2]}, s.args))
                std::printf("sp coder spawn: drone %d\n", d->id);
        }
    }

    void draw_frame(const Camera& cam) {
        int width = 0, height = 0;
        window.begin_frame(width, height);
        // Game viewport: 4:3 pillarbox (black bars) unless widescreen. All 3D below
        // uses the game aspect; the HUD canvas pillarboxes itself in ui.begin.
        const GameView gv = game_view(config, width, height);
        const int gl_y = height - gv.y - gv.h;
        const float aspect = gv.aspect;
        Camera wc = cam;
        wc.fovy = kViewFovY / std::max(1.0f, weapons->zoom(0));
        // Cutscene camera override (NIS): eye + forward + degree FOV [INFERENCE: degrees].
        if (mission_sys) {
            if (const auto sc = mission_sys->script_camera()) {
                const auto& f = sc->forward;
                const float yaw = std::atan2(f[0], f[2]);
                const float pitch = std::asin(std::clamp(f[1], -1.0f, 1.0f));
                wc = camera_for_eye_yaw_pitch({sc->eye[0], sc->eye[1], sc->eye[2]}, yaw, pitch);
                wc.fovy = sc->fov * 3.14159265f / 180.0f;
            }
        }
        glDisable(GL_SCISSOR_TEST);
        glViewport(0, 0, width, height);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_SCISSOR_TEST);
        glViewport(gv.x, gl_y, gv.w, gv.h);
        glScissor(gv.x, gl_y, gv.w, gv.h);
        glClearColor(0.25f, 0.3f, 0.4f, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        renderer->draw(wc, aspect, false);
        if (weather && weather->active())
            weather->draw(wc, renderer->view_projection(wc, aspect));
        drone_renderer->draw(wc, aspect, *drones);
        renderer->draw_objects(wc, aspect, effects->take_blast_draws());
        glClear(GL_DEPTH_BUFFER_BIT);
        const ViewModel vm = weapons->viewmodel(0);
        if (vm.visible && vm.skin && vm.anim) {
            const WeaponDef& def = weapons->table().weapon(vm.weapon);
            const Vec3 muzzle =
                weapon_view->draw(wc, aspect, vm, def, effects->lighting_at(wc.eye, 2.0f), &world->collision());
            if (vm.muzzle_flash > 0.0f && muzzle != Vec3{0, 0, 0}) effects->muzzle_flash(muzzle, def);
        }
        glEnable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        // HUD overlay (the ui canvas letterboxes over the 3D frame; no clear).
        HudState hs;
        weapons->fill_hud(0, hs);
        hud->update(hs);
        ui.begin(width, height, false);
        hud->draw(ui, text);
        // Cutscene fade overlay (MissionSystem::fade 0..1 black).
        if (mission_sys && mission_sys->fade() > 0.001f) {
            const auto a = std::uint8_t(std::clamp(mission_sys->fade(), 0.0f, 1.0f) * 128.0f);
            ui.fill({0, 0, ui::kScreenW, ui::kScreenH}, {0x00, 0x00, 0x00, a});
        }
        ui.end();
        glDisable(GL_BLEND);
        glEnable(GL_DEPTH_TEST);
    }

    // Pause / end-mission menu on the shared window. Returns Resume (false) or the session exit.
    std::optional<SpResult> run_menu(std::optional<std::uint32_t> page) {
        if (!has_level_menu) return std::nullopt;
        Frontend menu(ctx.assets, level_menu, &ctx.mp_data, &ctx.sp_data);
        PauseInfo info;
        if (mission_sys) {
            for (const MissionSystem::ObjectiveInfo& o : mission_sys->objectives()) {
                PauseObjective po;
                po.label = o.def.label;
                po.complete = o.state == ObjectiveState::Done || o.state == ObjectiveState::DoneShown;
                info.objectives.push_back(po);
            }
        }
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
            // C_NIS accept: play the requested level cutscene and resume into it.
            if (mission_sys) {
                if (const std::uint32_t nis = menu.take_nis_request()) {
                    mission_sys->play_nis(nis);
                    return std::nullopt;
                }
            }
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

    // sp_level id after this mission (pending destination, else the next row in sp_level order, else 0).
    std::uint32_t next_level_id() const {
        if (mission_sys && mission_sys->pending_level() != 0) return mission_sys->pending_level();
        const std::vector<SpLevelRow> rows = load_sp_level_order(ctx.action_elf);
        for (std::size_t i = 0; i < rows.size(); ++i) {
            if (rows[i].level != level_id) continue;
            if (i + 1 < rows.size()) return rows[i + 1].level;
            return 0;  // last mission: the bonus chain leads to wingame, then the frontend
        }
        return 0;
    }

    // Results content for P_NFRESULTS (stats + next-mission name).
    std::pair<MissionResults, std::uint32_t> build_results() {
        MissionResults results;
        if (mission_sys) {
            const MissionSystem::Stats& st = mission_sys->stats();
            const long secs = long(st.frames) / 30;
            char time[32];
            std::snprintf(time, sizeof(time), "%ld:%02ld", secs / 60, secs % 60);
            // Difficulty arrives as 1/2/3 (GameState+0x28) or 0/1/2 (menu/CLI index); normalize.
            const int d = launch.difficulty;
            const int difficulty = (d >= 1 && d <= 3) ? d : (d >= 0 && d <= 2) ? d + 1 : 2;
            PlrStats::Score score{};
            if (score_tables_ok_) score = plr_.compute(score_tables_, level_id, difficulty, mission_won);
            if (score.valid) results.score_text = separate_number(score.total);
            results.stats = {{"Time", time},
                             {"Shots", std::to_string(st.shots)},
                             {"Hits", std::to_string(st.hits)},
                             {"Kills", std::to_string(mission_sys->kills())}};
            if (score.valid && score.done_better) results.stats.emplace_back("Best", "New!");
        }
        const std::uint32_t next = next_level_id();
        if (next != 0) {
            for (const SpLevelRow& row : load_sp_level_order(ctx.action_elf)) {
                if (row.level != next) continue;
                const std::string_view name = ctx.assets.strings.label(row.name);
                if (!name.empty()) results.next_target_text = std::string(name);
                break;
            }
        }
        return {results, next};
    }

    // Headless results shot (the interactive run_results drove the same page with live input).
    void draw_results_shot(const std::string& path) {
        const auto [results, next] = build_results();
        (void)next;
        Frontend menu(ctx.assets, ctx.menu, &ctx.mp_data, &ctx.sp_data);
        menu.set_mission_results(results);
        menu.open(FrontendMode::MainMenu, kPageResults);
        PadHistory hist;
        for (int i = 0; i < 30; ++i) {
            hist.push({});
            menu.update(hist);
        }
        int w, h;
        window.begin_frame(w, h);
        ui.begin(w, h);
        menu.draw(ui, text);
        ui.end();
        if (!window.save_bmp(path)) throw std::runtime_error("cannot write shot");
        std::printf("results -> %s\n", path.c_str());
    }

    // Mission-complete results (P_NFRESULTS) on the frontend script. MissionDone continues to next_level_id().
    std::optional<SpResult> run_results() {
        Frontend menu(ctx.assets, ctx.menu, &ctx.mp_data, &ctx.sp_data);
        const auto [results, next] = build_results();
        menu.set_mission_results(results);
        menu.open(FrontendMode::MainMenu, kPageResults);
        PadHistory hist;
        bool wait_release = true;
        while (!menu.wants_close()) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_EVENT_QUIT) return SpResult{SpExit::QuitToMenu};
            }
            const PadState s = menu_pad(gamepad);
            if (wait_release) {
                if (s.buttons == 0) wait_release = false;
                hist.push({});
            } else {
                hist.push(s);
            }
            menu.update(hist);
            // Results chain movies (wingame): play fullscreen, then run the post-movie transition.
            if (std::uint32_t movie = menu.take_movie_request()) {
                MovieScreen screen(window, ui, text, audio.get(), gamepad, ctx.gamedir);
                screen.play(movie);
                menu.movie_finished();
            }
            int w, h;
            window.begin_frame(w, h);
            ui.begin(w, h);
            menu.draw(ui, text);
            ui.end();
            window.swap();
            SDL_Delay(33);
        }
        if (menu.result().action == FrontendResult::Action::MissionDone)
            return SpResult{SpExit::NextMission, false, true, next};
        return SpResult{SpExit::QuitToMenu, false, true, 0};
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
                if (s.mission_won) {
                    // Mission complete -> results chain, then the next mission.
                    if (auto r = s.run_results()) {
                        done = *r;
                        finished = true;
                    } else {
                        done = SpResult{SpExit::QuitToMenu, false, true, 0};
                        finished = true;
                    }
                } else if (auto r = s.run_menu(kPageEndMission)) {
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
        s.draw_frame(camera_for_eye_yaw_pitch(p.shaken_eye(), p.yaw, p.view_pitch()));
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
    long ran = 0;
    for (long i = 0; i < headless.frames; ++i) {
        // Headless neutral: raw 0x80 sticks read as +1/128 strafe/turn (axis()), so a still
        // mission would drift. Live pads pass compensate_sticks (0x80 -> 0x7F -> exact 0);
        // script pads from --inputs already contain compensated bytes (see actions.hpp).
        const PadState pad =
            i < long(script.size()) ? script[std::size_t(i)].pad : compensate_sticks(PadState{});
        s.tick_world(pad);
        ran = i + 1;
        if (s.mission_sys && s.mission_sys->state() == MissionSystem::State::Done) break;
    }
    const Player& p = *s.world->player(0);
    const char* mstate = s.mission_sys
                             ? (s.mission_sys->state() == MissionSystem::State::Done
                                    ? (s.mission_won ? "mission-done-won" : "mission-done")
                                    : "mission-playing")
                             : "no-mission";
    std::printf("%s: %ld frames, pos %.2f,%.2f,%.2f yaw %.3f alive %d npcs %zu %s\n", s.launch.bin.c_str(), ran,
                p.pos[0], p.pos[1], p.pos[2], p.yaw, int(p.alive()), s.spsys->spawned(), mstate);
    if (!headless.shot.empty()) {
        s.draw_frame(camera_for_eye_yaw_pitch(p.shaken_eye(), p.yaw, p.view_pitch()));
        const bool ok = s.window.save_bmp(headless.shot);
        std::printf("shot -> %s\n", ok ? headless.shot.c_str() : SDL_GetError());
        if (!ok) throw std::runtime_error("cannot write shot");
        if (s.mission_won) {
            std::string results = headless.shot;
            if (const auto dot = results.find_last_of('.'); dot != std::string::npos)
                results = results.substr(0, dot) + "_results" + results.substr(dot);
            else
                results += "_results.bmp";
            s.draw_results_shot(results);
        }
    }
    const bool failed = !p.alive() || s.spsys->mission_fail_reason != 0 ||
                        (s.mission_sys && s.mission_sys->state() == MissionSystem::State::Done && !s.mission_won);
    return SpResult{SpExit::QuitToMenu, failed, s.mission_won, 0};
}

}  // namespace nf::app

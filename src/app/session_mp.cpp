// Multiplayer arena session: build, tick, split-screen draw, HUD, pause, debrief.
#include "app/session_mp.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "assets/character.hpp"
#include "assets/level.hpp"
#include "audio/audio.hpp"
#include "audio/music_director.hpp"
#include "game/arena_view.hpp"
#include "game/bot_match.hpp"
#include "game/drone_render.hpp"
#include "game/local_pad.hpp"
#include "game/nfgame_effects.hpp"
#include "game/nfgame_weapon_view.hpp"
#include "game/player_anim.hpp"
#include "game/weapons.hpp"
#include "game/world.hpp"
#include "render/character_renderer.hpp"
#include "render/gl.hpp"
#include "ui/mp_setup.hpp"
#include "render/level_renderer.hpp"
#include "render/weather_renderer.hpp"

namespace nf::app {

namespace {

// P_MPDEBRIEFING in the frontend script (frontend_mp.cpp).
constexpr std::uint32_t kPageDebriefing = 0x40000033;

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

// Player body matrix (object -> world) in the drone convention (feet + yaw).
Mat4 player_model_matrix(const Player& p) {
    const float s = std::sin(p.yaw), c = std::cos(p.yaw);
    return {c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, p.pos[0], p.pos[1], p.pos[2], 1};
}

}  // namespace

MpDirect MpSession::from_launch(const MpLaunch& launch) {
    MpDirect d;
    d.level_bin = launch.level_bin;
    d.options.enabled = true;
    d.options.mode = launch.settings.mode;
    d.options.humans = std::clamp<int>(int(launch.settings.human_count), 1, 4);
    d.options.bots = std::clamp<int>(int(launch.settings.bot_count), 0, 4);
    d.options.score_limit = launch.settings.score_limit;
    d.options.time_limit = float(launch.settings.duration);
    d.options.friendly_fire = launch.settings.friendly_fire != 0;
    d.options.weapon_set = launch.settings.weapon_set;
    // Bot characters: the P_MPCONFIRM participant names, in slot order.
    std::string chars;
    for (const MpParticipant& p : launch.participants) {
        if (!p.bot) continue;
        if (!chars.empty()) chars += ',';
        chars += p.name;
    }
    d.bot_characters = chars;
    return d;
}

struct MpSession::Impl {
    Impl(AppContext& c, Window& w, ui::Renderer& u, ui::TextRenderer& t, const MpDirect& d, const AppConfig& cfg)
        : ctx(c), window(w), ui(u), text(t), direct(d), config(cfg) {}

    AppContext& ctx;
    Window& window;
    ui::Renderer& ui;
    ui::TextRenderer& text;
    MpDirect direct;
    AppConfig config;

    std::unique_ptr<Level> level;
    std::uint32_t level_id = 0;
    std::string tuning_text;
    std::unique_ptr<World> world;
    std::unique_ptr<bots::BotMatch> bot_match;
    std::unique_ptr<ArenaSession> session;
    CharacterBank* bank = nullptr;  // owned by BotMatch, else owned_bank
    std::unique_ptr<CharacterBank> owned_bank;
    SpriteLibrary fx_sprites;
    std::unique_ptr<WeaponEffects> effects;
    std::unique_ptr<LevelRenderer> renderer;
    std::unique_ptr<WeatherRenderer> weather;
    std::unique_ptr<CharacterRenderer> chars;
    std::unique_ptr<WeaponView> weapon_view;
    std::unique_ptr<drone::DroneRenderer> drone_renderer;
    std::vector<std::unique_ptr<Hud>> huds;
    // Remote-player bodies: one animated character per human slot (PlayerAnimator
    // over the participant skin), drawn in every other viewer's split view.
    std::vector<std::unique_ptr<PlayerAnimator>> bodies;

    std::unique_ptr<SoundArchive> archive;
    std::unique_ptr<audio::AudioSystem> audio;
    std::unique_ptr<audio::MusicDirector> music;
    std::vector<LocalPad> pads;
    SDL_Gamepad* menu_pad_handle = nullptr;

    bool build() {
        std::string bin = direct.level_bin.empty() ? "07000024.bin" : direct.level_bin;
        std::vector<std::uint8_t> bytes = read_level_bin(ctx.files, bin);
        if (bytes.empty()) {
            std::fprintf(stderr, "nightfire: no such level .bin: %s\n", bin.c_str());
            return false;
        }
        direct.level_bin = bin;
        level = std::make_unique<Level>(std::move(bytes));
        if (!level->map()) {
            std::fprintf(stderr, "nightfire: %s has no Map entry\n", bin.c_str());
            return false;
        }
        level_id = std::uint32_t(std::strtoul(bin.c_str(), nullptr, 16));
        PlayerParams params;
        if (const GameFile* tuning = ctx.files.find("TuningVars.txt")) {
            const auto raw = ctx.files.read(*tuning);
            tuning_text.assign(reinterpret_cast<const char*>(raw.data()), raw.size());
            params = player_params_from_tuning(tuning_text);
        }
        std::optional<StringTable> strings;
        if (const GameFile* txt = ctx.files.find("USATxt.dat")) strings = StringTable::parse(ctx.files.read(*txt), false);

        world = std::make_unique<World>(*level, InputTables::from_elf(ctx.action_elf), params);
        MatchOptions options = direct.options;
        options.enabled = true;
        if (options.bots > 0) {
            bots::BotMatchOptions bo;
            bo.count = options.bots;
            bo.characters = direct.bot_characters;
            bot_match = std::make_unique<bots::BotMatch>(ctx.files, ctx.gamedir, bin, *level, *world, ctx.action_elf,
                                                         tuning_text, strings ? &*strings : nullptr, bo);
            bank = &bot_match->bank();
        } else {
            owned_bank = open_character_bank(ctx.files, bin);
            bank = owned_bank.get();
        }
        session = std::make_unique<ArenaSession>(
            *world, WeaponTable::from_elf(ctx.action_elf), options, strings ? &*strings : nullptr, tuning_text,
            [this](ArenaSession& s) {
                if (bot_match) bot_match->install(s);
            });
        if (bot_match) bot_match->start();
        // Weapon viewmodels/animations need the level's bank (as in SP); without it
        // viewmodel skins are null and the gun never draws.
        session->weapons().set_bank(bank);
        if (bot_match) session->weapons().set_drone_system(&bot_match->drones());   // idle-fidget threat gate
        if (direct.give >= 0) {  // --give ID: debug equip for scoped-capture verification
            session->weapons().give_weapon(0, direct.give, 999);
            session->weapons().select_weapon(0, direct.give);
            std::printf("match: debug give weapon %d\n", direct.give);
        }
        effects = std::make_unique<WeaponEffects>(session->weapons().table(), *bank, &fx_sprites);
        effects->set_map_lights(bank->lights());

        add_level_sprites(ctx.assets.sprites, Bytes(read_level_bin(ctx.files, bin)));
        for (int i = 0; i < options.humans; ++i) {
            HudConfig hcfg;
            hcfg.multiplayer = true;
            hcfg.players = options.humans;
            hcfg.player = i;
            hcfg.side_by_side = options.side_by_side;
            hcfg.frame_rate = 30.0f;
            huds.push_back(std::make_unique<Hud>(ctx.assets, ctx.hud_data, hcfg));
        }

        renderer = std::make_unique<LevelRenderer>(*level);
        renderer->set_level(level_id);
        weather = std::make_unique<WeatherRenderer>(*level);
        weather->set_level(level_id);
        chars = std::make_unique<CharacterRenderer>(*bank);
        weapon_view = std::make_unique<WeaponView>(*bank, *chars);
        if (bot_match) drone_renderer = std::make_unique<drone::DroneRenderer>(*bank);

        // Remote bodies over the MP skins of the arena setup (mp_characters value = character index).
        const std::vector<AnimSet> sets = read_anim_sets(ctx.action_elf);
        anim_sets = std::make_unique<std::vector<AnimSet>>(sets);
        for (int i = 0; i < options.humans; ++i) {
            // First MP skin of the bank (the arena setup's skins resolve through the same bank).
            const SkinDef* skin = nullptr;
            for (const auto& [hash, def] : bank->skins()) {
                (void)hash;
                const std::string name = bank->skin_name(def);
                if (name.size() > 3 && (name[0] == 'M' || name[0] == 'm') && (name[1] == 'p' || name[1] == 'P') &&
                    name[2] == '_') {
                    skin = &def;
                    break;
                }
            }
            if (!skin && !bank->skins().empty()) skin = &bank->skins().begin()->second;
            if (!skin) {
                bodies.push_back(nullptr);
                continue;
            }
            bodies.push_back(std::make_unique<PlayerAnimator>(*bank, *skin, *anim_sets, 1));
        }

        archive = std::make_unique<SoundArchive>(std::filesystem::path(ctx.gamedir));
        audio = std::make_unique<audio::AudioSystem>(*archive);
        audio->enter_level(level_id);
        audio->set_sfx_volume(config.sfx_volume);
        audio->set_music_volume(config.music_volume);
        music = std::make_unique<audio::MusicDirector>(*audio, ctx.music);
        music->start_level(level_id);

        const ArenaSystem& arena = session->arena();
        std::printf("%s: mode %08x, %d players, %d bots, %zu pickups\n", bin.c_str(), arena.settings().mode,
                    options.humans, options.bots, arena.pickups().all().size());
        return true;
    }

    std::unique_ptr<std::vector<AnimSet>> anim_sets;  // must outlive `bodies`

    void audio_frame() {
        for (const MatchSound& s : session->sounds()) {
            audio::PlayOptions o;
            if (s.at) o.position = *s.at;
            audio->play_sfx(std::uint32_t(s.id), o);
        }
        session->sounds().clear();
        for (const SoundEvent& e : session->weapons().events().sounds) {
            if (e.exclude == 0 || (e.listener >= 0 && e.listener != 0)) continue;
            audio::PlayOptions o;
            if (e.positional) o.position = e.position;
            audio->play_sfx(std::uint32_t(e.id), o);
        }
        session->weapons().events().sounds.clear();
        const Player& p = *world->player(0);
        const float c = std::cos(p.view_pitch());
        audio::Listener l;
        l.position = p.eye();
        l.dir = {std::sin(p.yaw) * c, std::sin(p.view_pitch()), std::cos(p.yaw) * c};
        l.norm = {std::cos(p.yaw), 0.0f, -std::sin(p.yaw)};
        audio->set_listener(l);
        music->update();
        audio->update();
        audio->update();
    }

    HudState hud_state(int slot, const Camera& cam, float vw, float vh) {
        HudState hs;
        session->weapons().fill_hud(slot, hs);
        const ArenaHud ah = session->hud(slot);
        hs.mp.mode = static_cast<HudMpMode>(ah.mode);
        hs.mp.teams = ah.teams;
        hs.mp.objective = ah.objective;
        hs.mp.team = ah.team;
        hs.mp.team_score = ah.team_score;
        hs.mp.points = ah.points;
        hs.mp.kills = ah.kills;
        hs.mp.deaths = ah.deaths;
        hs.mp.has_flag = ah.has_flag;
        hs.mp.has_espionage = ah.has_espionage;
        hs.mp.is_assassin = ah.is_assassin;
        hs.mp.is_target = ah.is_target;
        hs.mp.team_has_golden_gun = ah.team_has_golden_gun;
        for (int i = 0; i < 3 && i < ah.uplink_count; ++i) hs.mp.uplink[std::size_t(i)] = ah.uplink[std::size_t(i)];
        hs.mp.health_bonus = ah.health_bonus;
        for (const ArenaHud::Blip& b : ah.blips)
            hs.mp.blips.push_back(HudBlip{b.x, b.y, b.z, b.color, b.kind});
        // Name tags float over heads in screen space (projected, y up). The raw
        // camera-space blip coords feed the radar, never the labels: unprojected
        // tags land anywhere on the canvas (e.g. over the health bar).
        const Mat4 vp = renderer->view_projection(cam, vw / std::max(1.0f, vh));
        for (const ArenaHud::Blip& b : ah.blips) {
            if (b.name.empty()) continue;
            const Vec3 hp{b.world[0], b.world[1] + 1.8f, b.world[2]};
            const float cx = vp[0] * hp[0] + vp[4] * hp[1] + vp[8] * hp[2] + vp[12];
            const float cy = vp[1] * hp[0] + vp[5] * hp[1] + vp[9] * hp[2] + vp[13];
            const float cw = vp[3] * hp[0] + vp[7] * hp[1] + vp[11] * hp[2] + vp[15];
            if (cw <= 0) continue;  // behind the camera
            const float nx = cx / cw, ny = cy / cw;
            if (nx < -1.0f || nx > 1.0f || ny < -1.0f || ny > 1.0f) continue;
            HudNameTag tag;
            tag.name = b.name;
            tag.x = (nx * 0.5f + 0.5f) * vw;
            tag.y = (ny * 0.5f + 0.5f) * vh;
            tag.same_team = b.same_team;
            hs.mp.name_tags.push_back(tag);
        }
        return hs;
    }

    // Match feed drained once per frame so every viewer sees the same lines.
    struct PendingMessage {
        MatchMessage message;
    };
    std::vector<PendingMessage> pending_messages;

    void drain_messages() {
        for (const MatchMessage& m : session->messages()) pending_messages.push_back({m});
        session->messages().clear();
    }

    void draw_views() {
        drain_messages();
        const int humans = session->humans();
        int width = 0, height = 0;
        window.begin_frame(width, height);
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        // Game viewport first (4:3 pillarbox unless widescreen; the full-window black
        // clear above is the bar color); viewer rects subdivide the game rect, so every
        // 3D view projects with the game aspect, not the window aspect.
        const GameView gv = game_view(config, width, height);
        const auto rects = split_screen_layout(humans, gv.w, gv.h, session->options().side_by_side);
        glEnable(GL_SCISSOR_TEST);
        for (int i = 0; i < humans; ++i) {
            ViewRect r = rects[std::size_t(i)];
            r.x += gv.x;
            r.y += gv.y;
            const int gl_y = height - r.y - r.h;
            const Player& p = *world->player(i);
            Camera cam = camera_for_eye_yaw_pitch(p.eye(), p.yaw, p.view_pitch());
            cam.fovy = kViewFovY / std::max(1.0f, session->weapons().zoom(i));
            glViewport(r.x, gl_y, r.w, r.h);
            glScissor(r.x, gl_y, r.w, r.h);
            const auto clear = renderer->clear_color();
            glClearColor(clear[0], clear[1], clear[2], 1);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            renderer->draw(cam, r.aspect(), false);
            if (weather && weather->active()) weather->draw(cam, renderer->view_projection(cam, r.aspect()));
            // Bots, remote players, then effects and the viewer's own gun.
            if (drone_renderer && bot_match) drone_renderer->draw(cam, r.aspect(), bot_match->drones());
            for (int j = 0; j < humans; ++j) {
                if (j == i || !bodies[std::size_t(j)]) continue;
                const Player& q = *world->player(j);
                if (!q.alive()) continue;
                const auto& anim = bodies[std::size_t(j)]->character();
                chars->draw(cam, r.aspect(), anim.skin(), anim.palette(), player_model_matrix(q), 0, anim.facial(), {});
            }
            effects->draw(cam, r.aspect(), *chars, session->weapons().projectiles());
            glClear(GL_DEPTH_BUFFER_BIT);
            const ViewModel vm = session->weapons().viewmodel(i);
            if (vm.visible && vm.skin && vm.anim) {
                const WeaponDef& def = session->weapons().table().weapon(vm.weapon);
                const Vec3 muzzle =
                    weapon_view->draw(cam, r.aspect(), vm, def, effects->lighting_at(cam.eye, 2.0f));
                if (vm.muzzle_flash > 0.0f && muzzle != Vec3{0, 0, 0}) effects->muzzle_flash(muzzle, def);
            }
            glEnable(GL_DEPTH_TEST);
            glDisable(GL_BLEND);
            // HUD for this viewer, clipped to its rectangle.
            HudState hs = hud_state(i, cam, float(r.w), float(r.h));
            for (const PendingMessage& pm : pending_messages) {
                if (pm.message.slot != -1 && pm.message.slot != i) continue;
                huds[std::size_t(i)]->add_message(HudMessage{static_cast<HudMsgType>(int(pm.message.type)), 0xFFFFFFFF,
                                                            pm.message.text, pm.message.frames});
            }
            huds[std::size_t(i)]->update(hs);
            ui.begin(r.w, r.h, false);
            glViewport(r.x, gl_y, r.w, r.h);
            glScissor(r.x, gl_y, r.w, r.h);
            huds[std::size_t(i)]->draw(ui, text);
            ui.end();
        }
        pending_messages.clear();
        glDisable(GL_SCISSOR_TEST);
        glViewport(0, 0, width, height);
        glDisable(GL_BLEND);
        glEnable(GL_DEPTH_TEST);
    }

    // Remote-player bodies: the same PlayerAnimator inputs the local player gets (stance category,
    // crouch, per-tick body-space velocity), plus anim-script sound events served positional.
    // Footstep SFX need the surface->sound table (Audio owns it); only Sound events play for now.
    void tick_bodies() {
        if (weather && world->player(0)) weather->update(world->player(0)->eye());
        for (int j = 0; j < session->humans(); ++j) {
            if (!bodies[std::size_t(j)]) continue;
            PlayerAnimator& body = *bodies[std::size_t(j)];
            const Player& q = *world->player(j);
            const auto* st = session->weapons().state(j);
            int category = 1;
            if (st) category = int(session->weapons().table().weapon(st->current).category);
            body.set_category(category);
            body.update(q.substate == SubState::Crouch, q.velocity, 2.0f);
            // Event drain: character() is exposed const (Movement's local-player accessor); the
            // animator object itself is ours and mutable, so the cast only recovers that.
            CharacterInstance& character = const_cast<CharacterInstance&>(body.character());
            for (const AnimEvent& e : character.take_events()) {
                if (e.kind != AnimEventKind::Sound || e.arg == 0) continue;
                audio::PlayOptions o;
                o.position = q.pos;
                audio->play_sfx(e.arg, o);
            }
        }
    }

    DebriefInfo debrief_info() const {
        const ArenaSystem& arena = session->arena();
        const MatchResult& res = arena.result();
        const ArenaSettings& settings = arena.settings();
        DebriefInfo info;
        for (const ScoreRow& row : res.ranking) {
            DebriefRow d;
            d.name = row.name;
            d.score = row.score;
            d.kills = row.kills;
            d.deaths = row.deaths;
            d.points = row.points;
            d.is_bot = row.bot;
            if (row.slot >= 0 && std::size_t(row.slot) < settings.slots.size())
                d.character = settings.slots[std::size_t(row.slot)].character;
            info.rows.push_back(d);
        }
        info.banner = res.banner;
        return info;
    }

    void report() const {
        const ArenaSystem& arena = session->arena();
        std::printf("frame %llu: %.1f s, phase %d\n", static_cast<unsigned long long>(world->frame()), arena.elapsed(),
                    int(arena.phase()));
        for (const ScoreRow& r : arena.scoreboard())
            std::printf("  %-10s score %3d  kills %2d deaths %2d points %5.2f%s\n", r.name.c_str(), r.score, r.kills,
                        r.deaths, r.points, r.out ? "  (out)" : "");
    }

    // Pause menu (frontend script) or the debriefing page. Returns the session exit when the menu closed.
    std::optional<MpResult> run_menu(bool debrief) {
        Frontend menu(ctx.assets, ctx.menu, &ctx.mp_data, &ctx.sp_data);
        if (debrief) {
            menu.set_debriefing(debrief_info());
            menu.open(FrontendMode::MainMenu, kPageDebriefing);
        } else {
            PauseInfo info;
            info.multiplayer = true;
            for (const ScoreRow& r : session->arena().scoreboard()) {
                PauseScoreRow row;
                row.name = r.name;
                row.value = std::to_string(r.score);
                info.score.push_back(row);
            }
            menu.set_pause_info(info);
            menu.open(FrontendMode::Pause);
        }
        PadHistory hist;
        bool wait_release = true;
        while (!menu.wants_close()) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_EVENT_QUIT) return MpResult{MpExit::QuitToMenu};
            }
            const PadState s = menu_pad(menu_pad_handle);
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
        if (r.action == FrontendResult::Action::MpRematch ||
            r.action == FrontendResult::Action::RestartMission)
            return MpResult{MpExit::Rematch};
        if (debrief) return MpResult{MpExit::QuitToMenu};
        if (r.action == FrontendResult::Action::Resume) return std::nullopt;
        return MpResult{MpExit::QuitToMenu};
    }
};

MpSession::MpSession(AppContext& ctx, Window& window, ui::Renderer& ui, ui::TextRenderer& text, const MpDirect& direct,
                     const AppConfig& cfg)
    : impl_(std::make_unique<Impl>(ctx, window, ui, text, direct, cfg)) {
    ready_ = impl_->build();
}

MpSession::~MpSession() = default;

MpResult MpSession::run_interactive() {
    Impl& s = *impl_;
    s.pads = open_local_pads(s.session->humans());
    int count = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
        if (count > 0) s.menu_pad_handle = SDL_OpenGamepad(ids[0]);
        SDL_free(ids);
    }
    const bool has_audio = s.audio->open_device();
    std::printf("audio: output device %s\n", has_audio ? "open" : "none");
    bool running = true, captured = false;
    double accumulator = 0;
    Uint64 last = SDL_GetTicksNS();
    constexpr double kStep = 1.0 / 30.0;
    MpResult done{MpExit::QuitToMenu};
    bool finished = false, debrief_shown = false;
    while (running && !finished) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            for (auto& p : s.pads) p.handle_event(e);
            if (e.type == SDL_EVENT_QUIT) running = false;
            else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && !captured) {
                captured = SDL_SetWindowRelativeMouseMode(s.window.sdl(), true);
                for (auto& p : s.pads) p.set_captured(captured);
            } else if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) {
                if (captured) {
                    captured = !SDL_SetWindowRelativeMouseMode(s.window.sdl(), false);
                    for (auto& p : s.pads) p.set_captured(captured);
                }
                if (auto r = s.run_menu(false)) {
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
            PadInputs pads{};
            for (int i = 0; i < s.session->humans(); ++i) pads[std::size_t(i)] = s.pads[std::size_t(i)].sample();
            if (pads[0].buttons & kPadStart) {
                if (auto r = s.run_menu(false)) {
                    done = *r;
                    finished = true;
                } else {
                    last = SDL_GetTicksNS();
                    accumulator = 0;
                }
                break;
            }
            s.session->tick(pads);
            s.tick_bodies();
            if (has_audio) s.audio_frame();
            s.effects->consume(s.session->weapons().events());
            s.effects->tick(FrameTiming{}.mul());
            s.session->weapons().events().clear();
            accumulator -= kStep;
            if (s.session->arena().over() && !debrief_shown) {
                debrief_shown = true;
                s.report();
                if (auto r = s.run_menu(true)) {
                    done = *r;
                    done.played = true;
                    finished = true;
                } else {
                    done = MpResult{MpExit::QuitToMenu, true};
                    finished = true;
                }
                break;
            }
        }
        if (finished) break;
        s.draw_views();
        s.window.swap();
    }
    if (s.menu_pad_handle) SDL_CloseGamepad(s.menu_pad_handle);
    return done;
}

MpResult MpSession::run_headless() {
    Impl& s = *impl_;
    std::array<std::vector<std::pair<long, PadState>>, 4> scripts;
    for (int i = 0; i < s.session->humans(); ++i) {
        if (s.direct.inputs[std::size_t(i)].empty()) continue;
        std::ifstream in(s.direct.inputs[std::size_t(i)]);
        if (!in) throw std::runtime_error("cannot open inputs file " + s.direct.inputs[std::size_t(i)]);
        std::string line;
        while (std::getline(in, line)) {
            if (const auto hash = line.find('#'); hash != std::string::npos) line.resize(hash);
            std::istringstream ls(line);
            std::string head;
            if (!(ls >> head)) continue;
            if (head == "start") continue;
            PadState pad;
            unsigned word, rx, ry, lx, ly;
            if (!(ls >> std::hex >> word >> std::dec >> rx >> ry >> lx >> ly)) throw std::runtime_error("bad input line");
            pad.buttons = buttons_from_sony_pad_word(std::uint16_t(word));
            pad.rx = std::uint8_t(rx), pad.ry = std::uint8_t(ry), pad.lx = std::uint8_t(lx), pad.ly = std::uint8_t(ly);
            scripts[std::size_t(i)].emplace_back(std::strtol(head.c_str(), nullptr, 10), pad);
        }
    }
    const long frames = s.direct.frames >= 0 ? s.direct.frames : 300;
    auto at = [&](int player, long frame) {
        for (const auto& [f, p] : scripts[std::size_t(player)])
            if (f == frame) return p;
        return compensate_sticks(PadState{});   // headless neutral (see session_sp.cpp)
    };
    for (long f = 0; f < frames; ++f) {
        PadInputs pads{};
        for (int i = 0; i < s.session->humans(); ++i) pads[std::size_t(i)] = at(i, f);
        s.session->tick(pads);
        s.tick_bodies();
        // Like the interactive loop: stop ticking once the match is over (post-Over
        // ticks would keep the bots fighting after the debrief snapshot).
        if (s.session->arena().over()) break;
    }
    s.report();
    if (s.bot_match) std::printf("bots:\n%s", s.bot_match->summary().c_str());
    const bool over = s.session->arena().over();
    std::printf("match %s\n", over ? "over" : "running");
    if (!s.direct.shot.empty()) {
        if (over) {
            // Gameplay frame first, then the debriefing page (results) as a second shot.
            s.draw_views();
            const bool ok = s.window.save_bmp(s.direct.shot);
            std::printf("shot -> %s\n", ok ? s.direct.shot.c_str() : SDL_GetError());
            Frontend menu(s.ctx.assets, s.ctx.menu, &s.ctx.mp_data, &s.ctx.sp_data);
            menu.set_debriefing(s.debrief_info());
            menu.open(FrontendMode::MainMenu, kPageDebriefing);
            PadHistory hist;
            for (int i = 0; i < 30; ++i) {
                hist.push({});
                menu.update(hist);
            }
            int w, h;
            s.window.begin_frame(w, h);
            s.ui.begin(w, h);
            menu.draw(s.ui, s.text);
            s.ui.end();
            std::string debrief = s.direct.shot;
            if (const auto dot = debrief.find_last_of('.'); dot != std::string::npos)
                debrief = debrief.substr(0, dot) + "_debrief" + debrief.substr(dot);
            else
                debrief += "_debrief.bmp";
            const bool ok2 = s.window.save_bmp(debrief);
            std::printf("debrief -> %s\n", ok2 ? debrief.c_str() : SDL_GetError());
            if (!ok || !ok2) throw std::runtime_error("cannot write shot");
        } else {
            s.draw_views();
            const bool ok = s.window.save_bmp(s.direct.shot);
            std::printf("shot -> %s\n", ok ? s.direct.shot.c_str() : SDL_GetError());
            if (!ok) throw std::runtime_error("cannot write shot");
        }
    }
    return MpResult{MpExit::QuitToMenu, over};
}

}  // namespace nf::app

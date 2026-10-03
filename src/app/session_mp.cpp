// Multiplayer arena session: build, tick, split-screen draw, HUD, pause, debrief.
#include "app/session_mp.hpp"
#include "app/menu_background.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <optional>
#include <deque>
#include "app/net_client.hpp"
#include "app/net_prediction.hpp"

#include "assets/character.hpp"
#include "assets/bin_archive.hpp"
#include "assets/cutscene.hpp"
#include "assets/level.hpp"
#include "assets/menu_validate.hpp"
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
#include "ui/hud_overlay.hpp"
#include "ui/mp_feed.hpp"
#include "ui/prompts.hpp"
#include "render/level_renderer.hpp"
#include "render/weather_renderer.hpp"

namespace nf::app {

namespace {

// P_MPDEBRIEFING in the frontend script (frontend_mp.cpp).
constexpr std::uint32_t kPageDebriefing = 0x40000033;

PadState menu_pad(SDL_Gamepad* gamepad) { return input_bindings().sample(InputContext::Menu, gamepad); }

// Local players' devices for the prompt glyphs of their HUD views.
void assign_prompt_devices(const std::vector<LocalPad>& pads) {
    std::vector<SlotDevice> slots;
    for (const LocalPad& p : pads) slots.push_back(p.device());
    input_devices().assign(slots);
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
    d.options.rules = launch.settings.rules;
    d.options.mode = launch.settings.mode;
    d.options.humans = std::clamp<int>(int(launch.settings.human_count), 1, int(kMpMaxLocalHumans));
    const int capacity = int(mp_rule_slot_limit(d.options.rules));
    const int reserved = d.options.rules == MpRuleSet::Extended ? d.options.humans : int(kMpMaxLocalHumans);
    d.options.bots = std::clamp<int>(int(launch.settings.bot_count), 0, std::max(0, capacity - reserved));
    d.options.roster_override = true;
    std::string chars;
    for (const MpParticipant& p : launch.participants) {
        if (p.slot >= d.options.roster.size()) continue;
        ArenaSettings::Slot& slot = d.options.roster[p.slot];
        slot.present = true;
        slot.bot = p.bot;
        slot.name = p.name;
        slot.team = int(p.team);
        slot.character = int(p.character);
        slot.health_bonus = p.handicap;
        slot.hud = p.hud;
        if (!p.bot) continue;
        if (!chars.empty()) chars += ',';
        chars += std::to_string(p.character);
    }
    d.bot_characters = chars;
    d.options.time_limit = float(launch.settings.duration);
    d.options.friendly_fire = launch.settings.friendly_fire != 0;
    d.options.weapon_set = launch.settings.weapon_set;
    return d;
}

struct MpSession::Impl {
    Impl(AppContext& c, Window& w, ui::Renderer& u, ui::TextRenderer& t, const MpDirect& d, AppConfig& cfg)
        : ctx(c), window(w), ui(u), text(t), direct(d), config(cfg), timing(float(cfg.logic_hz)), user_config(cfg) {}

    AppContext& ctx;
    Window& window;
    ui::Renderer& ui;
    ui::TextRenderer& text;
    MpDirect direct;
    AppConfig config;
    nf::FrameTiming timing;
    AppConfig& user_config;   // player 1's controller style / Y-inversion (the active profile's) persist here
    MenuFile level_menu;      // the level bin's menu script (the pause page)
    bool has_level_menu = false;

    std::unique_ptr<Level> level;
    std::uint32_t level_id = 0;
    std::string tuning_text;
    std::optional<StringTable> strings;  // outlives the BotMatch and ArenaSession that borrow it
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
    std::vector<HudOverlay> overlays;   // per viewer: kill feed, name plates, scoreboard, chat (ui/hud_overlay.hpp)
    std::vector<std::string> pending_chat;   // network chat received since the last overlay tick
    // Remote-player bodies: one animated character per human slot (PlayerAnimator
    // over the participant skin), drawn in every other viewer's split view.
    std::vector<std::unique_ptr<PlayerAnimator>> bodies;
    std::array<bool, nf::kMpSlots> network_body_visible = [] {
        std::array<bool, nf::kMpSlots> visible{};
        visible.fill(true);
        return visible;
    }();
    NetworkSession* network = nullptr;
    std::uint32_t network_projectiles_tick = 0;
    std::vector<Projectile> network_projectiles;
    std::vector<Projectile> previous_network_projectiles;
    std::array<ViewModel, nf::net::kMaxLocalPlayers> previous_viewmodels{};
    std::array<bool, nf::net::kMaxLocalPlayers> has_previous_viewmodel{};

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
        if (const GameFile* txt = ctx.files.find("USATxt.dat")) strings = StringTable::parse(ctx.files.read(*txt), false);

        world = std::make_unique<World>(*level, InputTables::from_elf(ctx.action_elf), params);
        // Player 1 plays with the active profile's controller style and Y-axis inversion; the other local players
        // start on the defaults (Classic Bond) and change theirs in their own pause menu.
        world->settings(0).controller_style = std::clamp(config.controller_style, 0, 7);
        world->settings(0).invert_look = config.invert_y;
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
        effects->set_level(level.get());
        effects->set_multiplayer(true);
        const std::vector<std::uint8_t> effect_bin = read_level_bin(ctx.files, bin);
        for (const BinEntry& e : parse_bin_archive(Bytes(effect_bin))) {
            if (e.type != EntryType::Script || (e.hash != 0x06000052 && e.hash != 0x060007C4)) continue;
            try {
                effects->set_explosion_script(std::uint32_t(e.hash), e.data);
            } catch (const std::exception&) {
            }
        }

        add_level_sprites(ctx.assets.sprites, Bytes(effect_bin));
        // The pause page (P_PAUSE) lives in the level's menu script, not the front end's.
        try {
            level_menu = load_menu_from_bin(Bytes(effect_bin));
            has_level_menu = true;
        } catch (const std::exception&) {
        }
        for (int i = 0; i < options.humans; ++i) {
            HudConfig hcfg;
            hcfg.multiplayer = true;
            hcfg.players = options.humans;
            hcfg.player = i;
            hcfg.side_by_side = options.side_by_side;
            hcfg.frame_rate = float(config.logic_hz);
            hcfg.slot_count = session->arena().settings().slot_count;
            huds.push_back(std::make_unique<Hud>(ctx.assets, ctx.hud_data, hcfg));
            overlays.emplace_back();
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
        for (std::size_t i = 0; i < session->arena().settings().slot_count; ++i) {
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
        l.norm = {std::cos(p.yaw), 0.0f, -std::sin(p.yaw)};
        audio->set_listener(l);
        music->update();
        audio->update();
        audio->update();
    }

    HudState hud_state(int slot, const Camera& cam, float vw, float vh) {
        HudState hs;
        session->weapons().fill_hud(slot, hs);
        hs.controller_style = world->settings(slot).controller_style;
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
        hs.mp.match_clock = format_match_clock(ah);
        if (network) {
            const nf::net::Snapshot* snapshot = network->latest_snapshot();
            if (snapshot) {
                hs.mp.team_score = {int(snapshot->team_score[0]), int(snapshot->team_score[1])};
                const auto it = std::find_if(snapshot->players.begin(), snapshot->players.end(),
                                             [slot](const nf::net::PlayerSnapshot& p) { return p.slot == slot; });
                if (it != snapshot->players.end()) {
                    hs.mp.points = it->points;
                    hs.mp.kills = it->kills;
                    hs.mp.deaths = it->deaths;
                    hs.mp.team = it->team;
                    hs.health = it->health;
                    hs.armor = it->armor;
                    hs.player_state = it->substate;
                    hs.aiming = it->aiming;
                    hs.clip = it->weapon_clip;
                    hs.reserve = it->weapon_ammo;
                    hs.weapon.id = it->weapon;
                    hs.weapon.base = session->weapons().table().weapon(it->weapon).base;
                }
            }
        }
        for (int i = 0; i < 3 && i < ah.uplink_count; ++i) hs.mp.uplink[std::size_t(i)] = ah.uplink[std::size_t(i)];
        hs.mp.health_bonus = ah.health_bonus;
        for (const ArenaHud::Blip& b : ah.blips)
            hs.mp.blips.push_back(HudBlip{b.x, b.y, b.z, b.color, b.kind, b.slot});
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
            tag.slot = b.slot;
            hs.mp.name_tags.push_back(tag);
        }
        return hs;
    }

    // HudOverlay inputs, once per game tick: scoreboard rows (the network snapshot's when connected), Select held,
    // received chat.
    void tick_overlays(const PadInputs& pads) {
        const ArenaSettings& settings = session->arena().settings();
        std::vector<HudParticipant> rows;
        rows.reserve(settings.slot_count);
        if (network) {
            if (const nf::net::Snapshot* snapshot = network->latest_snapshot()) {
                for (const nf::net::PlayerSnapshot& p : snapshot->players)
                    if (p.present)
                        rows.push_back({p.slot, p.name, p.team, p.bot, p.kills, p.deaths, p.score});
            }
        } else {
            for (const ScoreRow& r : session->arena().scoreboard())
                rows.push_back({r.slot, r.name, r.team, r.bot, r.kills, r.deaths, r.score});
        }
        const std::vector<std::string> chat = std::move(pending_chat);
        pending_chat.clear();
        // Overlay k serves global slot base + k: the network client's first local slot, or slot k in local play.
        const int base = network ? int(network->slot()) : 0;
        for (std::size_t k = 0; k < overlays.size(); ++k) {
            const int slot = base + int(k);
            HudOverlayState st;
            st.slot_count = settings.slot_count;
            st.teams = settings.uses_teams();
            st.viewer_slot = slot;
            st.participants = rows;
            st.scoreboard = std::size_t(slot) < pads.size() && (pads[std::size_t(slot)].buttons & kPadSelect);
            st.chat = chat;
            overlays[k].update(st);
        }
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

    void update_network_replication() {
        if (!network) return;
        for (const nf::net::WorldState& state : network->take_world_states()) {
            auto& pickups = session->arena().pickups().all();
            for (const nf::net::PickupSnapshot& pickup : state.pickups) {
                if (pickup.id >= pickups.size()) continue;
                pickups[pickup.id].state = Pickup::State(pickup.state);
                pickups[pickup.id].spin = pickup.spin;
            }
            auto& objectives = session->arena().mutable_objectives();
            for (const nf::net::ObjectiveSnapshot& objective : state.objectives) {
                if (objective.id >= objectives.size()) continue;
                MpObjective& local = objectives[objective.id];
                local.state = objective.state;
                local.team = objective.team;
                local.carrier = objective.carrier;
                local.visible = objective.visible;
                local.pos = {objective.x, objective.y, objective.z};
                local.hit_points = objective.hit_points;
            }
        }
        if (network->projectiles_tick() > network_projectiles_tick) {
            network_projectiles_tick = network->projectiles_tick();
            previous_network_projectiles.swap(network_projectiles);
            network_projectiles.clear();
            network_projectiles.reserve(network->projectiles().size());
            for (const nf::net::ProjectileSnapshot& state : network->projectiles()) {
                Projectile projectile;
                projectile.network_id = state.id;
                projectile.weapon = state.weapon;
                projectile.owner = state.owner;
                projectile.pos = {state.position[0], state.position[1], state.position[2]};
                projectile.previous_pos = projectile.pos;
                const auto previous = std::lower_bound(
                    previous_network_projectiles.begin(), previous_network_projectiles.end(), state.id,
                    [](const Projectile& p, std::uint16_t id) { return p.network_id < id; });
                if (previous != previous_network_projectiles.end() && previous->network_id == state.id)
                    projectile.previous_pos = previous->pos;
                projectile.dir = {state.direction[0], state.direction[1], state.direction[2]};
                projectile.state = Projectile::State(state.state);
                projectile.resting = state.resting;
                projectile.age = state.age;
                network_projectiles.push_back(projectile);
            }
        }
        WeaponEvents effects_to_play;
        for (const nf::net::ReplicationEvent& event : network->take_events()) {
            if (event.kind == nf::net::EventKind::Sound) {
                if (event.code < 0 || (event.actor >= 0 && event.actor != network->slot()) ||
                    event.target == network->slot())
                    continue;
                audio::PlayOptions options;
                if (event.positional) options.position = Vec3{event.position[0], event.position[1], event.position[2]};
                audio->play_sfx(std::uint32_t(event.code), options);
            } else if (event.kind == nf::net::EventKind::Impact) {
                effects_to_play.impacts.push_back({Vec3{event.position[0], event.position[1], event.position[2]},
                                                   Vec3{event.normal[0], event.normal[1], event.normal[2]},
                                                   event.code, event.weapon, event.on_body, event.actor});
            } else if (event.kind == nf::net::EventKind::Explosion) {
                effects_to_play.explosions.push_back({Vec3{event.position[0], event.position[1], event.position[2]},
                                                      event.radius, event.weapon, event.script, event.yaw});
            } else if (event.kind == nf::net::EventKind::Message || event.kind == nf::net::EventKind::Kill) {
                if (event.actor >= 0 && event.actor != network->slot()) continue;
                const int type = event.code >= 1 && event.code <= 6 ? event.code : 1;
                pending_messages.push_back({MatchMessage{event.actor, MatchMessage::Type(type), event.text,
                                                         event.frames != 0 ? event.frames : 45}});
            }
        }
        effects->consume(effects_to_play);
        for (const SoundEvent& sound : effects->take_blast_sounds()) {
            audio::PlayOptions options;
            if (sound.positional) options.position = sound.position;
            audio->play_sfx(std::uint32_t(sound.id), options);
        }
    }
    void draw_views(int viewer_only = -1, int viewer_count = 0, float interpolation = 1.0f) {
        drain_messages();
        update_network_replication();
        const int humans = session->humans();
        int width = 0, height = 0;
        window.begin_frame(width, height);
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        const GameView gv = game_view(config, width, height);
        const int view_count = viewer_count > 0 ? viewer_count : (viewer_only < 0 ? humans : 1);
        const auto rects = split_screen_layout(view_count, gv.w, gv.h, session->options().side_by_side);
        glEnable(GL_SCISSOR_TEST);
        for (int view = 0; view < view_count; ++view) {
            const int i = viewer_count > 0 ? viewer_only + view : (viewer_only < 0 ? view : viewer_only);
            ViewRect r = rects[std::size_t(view)];
            r.x += gv.x;
            r.y += gv.y;
            const int gl_y = height - r.y - r.h;
            const Player* view_player = world->player(i);
            if (!view_player) continue;
            const Player& p = *view_player;
            Camera cam = camera_for_eye_yaw_pitch(p.eye(), p.yaw, p.view_pitch());
            float zoom = session->weapons().zoom(i);
            if (network && i >= int(network->slot()) &&
                i < int(network->slot() + network->local_players())) {
                const std::size_t local = std::size_t(i - int(network->slot()));
                if (local < previous_viewmodels.size() && has_previous_viewmodel[local])
                    zoom = previous_viewmodels[local].zoom + (zoom - previous_viewmodels[local].zoom) * interpolation;
            }
            cam.fovy = kViewFovY / std::max(1.0f, zoom);
            glViewport(r.x, gl_y, r.w, r.h);
            glScissor(r.x, gl_y, r.w, r.h);
            const auto clear = renderer->clear_color();
            glClearColor(clear[0], clear[1], clear[2], 1);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            renderer->draw(cam, r.aspect(), false);
            if (weather && weather->active()) weather->draw(cam, renderer->view_projection(cam, r.aspect()));
            // Bots, remote players, then effects and the viewer's own gun.
            if (drone_renderer && bot_match)
                drone_renderer->draw(cam, r.aspect(), bot_match->drones(), interpolation);
            for (int j = 0; j < int(session->arena().settings().slot_count); ++j) {
                if (j == i || !network_body_visible[std::size_t(j)] || !bodies[std::size_t(j)]) continue;
                const Player* remote = world->player(j);
                if (!remote || !remote->alive()) continue;
                const Player& q = *remote;
                const auto& anim = bodies[std::size_t(j)]->character();
                chars->draw(cam, r.aspect(), anim.skin(), anim.palette(), player_model_matrix(q), 0, anim.facial(), {});
            }
            effects->draw(cam, r.aspect(), *chars,
                          network ? network_projectiles : session->weapons().projectiles(), interpolation);
            renderer->draw_objects(cam, r.aspect(), effects->take_blast_draws(interpolation));
            glClear(GL_DEPTH_BUFFER_BIT);
            ViewModel vm = session->weapons().viewmodel(i);
            const std::size_t local = network && i >= int(network->slot())
                                          ? std::size_t(i - int(network->slot()))
                                          : std::size_t(i);
            if (local < previous_viewmodels.size() && has_previous_viewmodel[local] &&
                previous_viewmodels[local].weapon == vm.weapon) {
                const ViewModel& previous = previous_viewmodels[local];
                vm.offset = previous.offset + (vm.offset - previous.offset) * interpolation;
                vm.zoom = previous.zoom + (vm.zoom - previous.zoom) * interpolation;
                vm.muzzle_flash = previous.muzzle_flash +
                                  (vm.muzzle_flash - previous.muzzle_flash) * interpolation;
            }
            if (vm.visible && vm.skin && vm.anim) {
                const WeaponDef& def = session->weapons().table().weapon(vm.weapon);
                const Vec3 muzzle =
                    weapon_view->draw(cam, r.aspect(), vm, def, effects->lighting_at(cam.eye, 2.0f),
                                      &world->collision());
                if (vm.muzzle_flash > 0.0f && muzzle != Vec3{0, 0, 0}) effects->muzzle_flash(muzzle, def, i);
            }
            glEnable(GL_DEPTH_TEST);
            glDisable(GL_BLEND);
            // HUD for this viewer, clipped to its rectangle.
            HudState hs = hud_state(i, cam, float(r.w), float(r.h));
            // Local play has a HUD per human slot; a network client one per local player (its views are the
            // consecutive global slots from network->slot()).
            const std::size_t h = network ? std::size_t(view) : std::size_t(i);
            if (HudOverlay::extended(session->arena().settings().slot_count)) {
                // Extended rule set: name tags become plates drawn by the overlay.
                overlays[h].set_name_tags(std::move(hs.mp.name_tags), float(r.w), float(r.h));
                hs.mp.name_tags.clear();
            }
            for (const PendingMessage& pm : pending_messages) {
                if (pm.message.slot != -1 && pm.message.slot != i) continue;
                huds[h]->add_message(HudMessage{static_cast<HudMsgType>(int(pm.message.type)), 0xFFFFFFFF,
                                                pm.message.text, pm.message.frames});
            }
            huds[h]->update(hs);
            ui.begin(r.w, r.h, false);
            glViewport(r.x, gl_y, r.w, r.h);
            glScissor(r.x, gl_y, r.w, r.h);
            ui::select_prompts(i, InputContext::OnFoot);
            huds[h]->draw(ui, text);
            overlays[h].draw(ui, text, huds[h]->geometry(ui.canvas_width()));
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
        if (weather && world->player(0)) {
            weather->update(world->player(0)->eye(), timing.mul(), timing.rec(),
                            [this](int ch) { return world->objects().channel(unsigned(ch)); });
        }
        renderer->set_time(double(world->frame()) / double(timing.FRAME_RATE));
        for (int j = 0; j < int(session->arena().settings().slot_count); ++j) {
            if (!bodies[std::size_t(j)]) continue;
            PlayerAnimator& body = *bodies[std::size_t(j)];
            const Player* remote = world->player(j);
            if (!remote) continue;
            const Player& q = *remote;
            const auto* st = session->weapons().state(j);
            int category = 1;
            const nf::net::PlayerSnapshot* replicated = nullptr;
            if (network) {
                if (const nf::net::Snapshot* snapshot = network->latest_snapshot()) {
                    const auto it = std::find_if(snapshot->players.begin(), snapshot->players.end(),
                                                 [j](const nf::net::PlayerSnapshot& p) { return p.slot == j; });
                    if (it != snapshot->players.end()) replicated = &*it;
                }
            }
            else if (st) category = int(session->weapons().table().weapon(st->current).category);
            body.set_category(category);
            if (replicated) {
                const Vec3 velocity{replicated->velocity[0], replicated->velocity[1], replicated->velocity[2]};
                body.update(replicated->substate == std::uint8_t(SubState::Crouch), velocity, timing.mul());
            } else {
                body.update(q.substate == SubState::Crouch, q.velocity, timing.mul());
            }
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
            d.slot = row.slot;
            if (row.slot >= 0 && std::size_t(row.slot) < settings.slots.size())
                d.character = settings.slots[std::size_t(row.slot)].character;
            info.rows.push_back(d);
        }
        info.banner = res.banner;
        info.slot_count = settings.slot_count;
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

    // Pause menu (frontend script) or the debriefing page. Returns the session exit when the menu closed. The pause
    // belongs to local player `pauser`: it reads that player's devices, shows their prompts and edits their controller
    // style and Y-axis inversion (player 1's are the active profile's and persist in nightfire.cfg).
    std::optional<MpResult> run_menu(bool debrief, int pauser = 0) {
        Frontend menu(ctx.assets, !debrief && has_level_menu ? level_menu : ctx.menu, &ctx.mp_data, &ctx.sp_data);
        PlayerSettings& settings = world->settings(pauser);
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
            menu.player_options().controller_style = settings.controller_style;
            menu.player_options().invert_y = settings.invert_look;
            menu.open(FrontendMode::Pause);
        }
        struct KeepOptions {
            Impl& s;
            Frontend& menu;
            PlayerSettings& settings;
            int pauser;
            bool active;
            ~KeepOptions() {
                const PlayerOptions& o = menu.player_options();
                if (!active || (settings.controller_style == o.controller_style && settings.invert_look == o.invert_y))
                    return;
                settings.controller_style = std::clamp(o.controller_style, 0, 7);
                settings.invert_look = o.invert_y;
                if (pauser != 0) return;
                s.user_config.controller_style = s.config.controller_style = settings.controller_style;
                s.user_config.invert_y = s.config.invert_y = settings.invert_look;
                save_config(config_path(), s.user_config);
            }
        } keep{*this, menu, settings, pauser, !debrief};
        std::optional<MenuBackground> background;
        if (debrief) {
            background.emplace(ctx.gamedir);
            background->advance();
        }
        PadHistory hist;
        bool wait_release = true;
        long menu_frames = 0;   // MpDirect::frames >= 0 (verification): the menu is shot and left after 3 s
        while (!menu.wants_close()) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_EVENT_QUIT) return MpResult{MpExit::QuitToMenu};
            }
            const PadState s = std::size_t(pauser) < pads.size() ? pads[std::size_t(pauser)].sample_menu()
                                                                  : menu_pad(menu_pad_handle);
            if (wait_release) {
                if (s.buttons == 0) wait_release = false;
                hist.push({});
            } else {
                hist.push(s);
            }
            menu.update(hist);
            if (background) background->advance();
            int w, h;
            window.begin_frame(w, h);
            ui.begin(w, h);
            if (background) background->draw(ui);
            ui::select_prompts(pauser, InputContext::Menu);
            menu.draw(ui, text);
            ui.end();
            if (direct.frames >= 0 && ++menu_frames >= 90) {
                if (!direct.shot.empty() && !window.save_bmp(direct.shot))
                    throw std::runtime_error("could not save menu screenshot");
                if (!direct.shot.empty()) std::printf("menu of player %d -> %s\n", pauser + 1, direct.shot.c_str());
                return MpResult{MpExit::QuitToMenu};
            }
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
                     AppConfig& cfg)
    : impl_(std::make_unique<Impl>(ctx, window, ui, text, direct, cfg)) {
    ready_ = impl_->build();
}

MpSession::~MpSession() = default;

MpResult MpSession::run_interactive() {
    Impl& s = *impl_;
    const bool claimed = std::any_of(s.direct.devices.begin(), s.direct.devices.end(),
                                     [](const SlotDevice& d) { return !d.empty(); });
    s.pads = claimed ? open_local_pads(s.direct.devices, s.session->humans()) : open_local_pads(s.session->humans());
    assign_prompt_devices(s.pads);
    for (std::size_t i = 0; i < s.pads.size(); ++i) {
        const SlotDevice d = s.pads[i].device();
        const InputDevice kind = input_devices().device(int(i));
        std::printf("local player %zu: %s%s%s (%s glyphs)\n", i + 1, d.keyboard_mouse ? "keyboard+mouse" : "",
                    d.keyboard_mouse && d.gamepad ? " + " : "", d.gamepad ? SDL_GetGamepadNameForID(d.gamepad) : "",
                    kind == InputDevice::PlayStation ? "PlayStation" : kind == InputDevice::Xbox ? "Xbox" : "keyboard");
    }
    int count = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
        if (count > 0) s.menu_pad_handle = SDL_OpenGamepad(ids[0]);
        SDL_free(ids);
    }
    const bool has_audio = s.audio->open_device();
    std::printf("audio: output device %s\n", has_audio ? "open" : "none");
    bool running = true, captured = false;
    const FrameTiming timing{float(s.config.logic_hz)};
    double accumulator = 0;
    Uint64 last = SDL_GetTicksNS();
    const double kStep = timing.rec();
    MpResult done{MpExit::QuitToMenu};
    bool finished = false, debrief_shown = false;
    long ticks = 0;   // MpDirect::frames >= 0: stop after that many ticks (headless verification, --frames)
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
                const int keyboard_player = std::max(0, input_devices().keyboard_player());
                if (auto r = s.run_menu(false, keyboard_player)) {
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
            int pauser = -1;   // the first local player whose start button is down opens their pause menu
            for (int i = 0; i < s.session->humans() && pauser < 0; ++i)
                if (pads[std::size_t(i)].buttons & kPadStart) pauser = i;
            if (pauser >= 0) {
                if (auto r = s.run_menu(false, pauser)) {
                    done = *r;
                    finished = true;
                } else {
                    last = SDL_GetTicksNS();
                    accumulator = 0;
                }
                break;
            }
            if (s.drone_renderer && s.bot_match) s.drone_renderer->capture_previous(s.bot_match->drones());
            s.session->tick(pads, timing);
            s.tick_overlays(pads);
            s.tick_bodies();
            if (has_audio) s.audio_frame();
            s.effects->consume(s.session->weapons().events());
            s.effects->tick(timing.mul(), s.session->weapons().projectiles());
            s.session->weapons().events().clear();
            accumulator -= kStep;
            ++ticks;
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
        s.draw_views(-1, 0, float(std::clamp(accumulator / kStep, 0.0, 1.0)));
        if (s.direct.frames >= 0 && ticks >= s.direct.frames) {
            if (!s.direct.shot.empty() && !s.window.save_bmp(s.direct.shot))
                throw std::runtime_error("could not save match screenshot");
            if (!s.direct.shot.empty()) std::printf("match tick %ld -> %s\n", ticks, s.direct.shot.c_str());
            break;
        }
        s.window.swap();
    }
    if (s.menu_pad_handle) SDL_CloseGamepad(s.menu_pad_handle);
    input_devices().assign({});
    return done;
}

MpResult MpSession::run_network_interactive(NetworkSession& network, long frames, const std::string& shot) {
    Impl& s = *impl_;
    s.network = &network;
    s.pads = open_local_pads(nf::net::kMaxLocalPlayers);
    assign_prompt_devices(s.pads);
    int gamepad_count = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&gamepad_count)) {
        if (gamepad_count > 0) s.menu_pad_handle = SDL_OpenGamepad(ids[0]);
        SDL_free(ids);
    }
    const bool has_audio = s.audio->open_device();
    std::printf("audio: output device %s\n", has_audio ? "open" : "none");
    struct StartPose {
        nf::Vec3 position;
        float yaw = 0.0f, pitch = 0.0f, ground_normal_y = 1.0f;
    };
    struct HeldInput {
        long first, last;
        PadState pad;
    };
    struct ScheduledGive {
        long frame;
        int weapon, rounds;
    };
    std::vector<ScheduledGive> scheduled_gives;
    std::optional<StartPose> start_pose;
    std::vector<std::pair<long, std::string>> scheduled_shots;
    std::vector<std::pair<long, PadState>> scripted_inputs;
    std::vector<HeldInput> held_inputs;
    const bool use_script = !s.direct.inputs[0].empty();
    if (use_script) {
        std::ifstream in(s.direct.inputs[0]);
        if (!in) throw std::runtime_error("cannot open inputs file " + s.direct.inputs[0]);
        std::string line;
        while (std::getline(in, line)) {
            if (const auto hash = line.find('#'); hash != std::string::npos) line.resize(hash);
            std::istringstream ls(line);
            std::string frame_text;
            if (!(ls >> frame_text)) continue;
            if (frame_text == "hold") {
                HeldInput held{};
                unsigned word, rx, ry, lx, ly;
                if (!(ls >> held.first >> held.last >> std::hex >> word >> std::dec >> rx >> ry >> lx >> ly) ||
                    held.first < 0 || held.last < held.first)
                    throw std::runtime_error("bad hold line in " + s.direct.inputs[0]);
                held.pad.buttons = buttons_from_sony_pad_word(std::uint16_t(word));
                held.pad.rx = std::uint8_t(rx), held.pad.ry = std::uint8_t(ry), held.pad.lx = std::uint8_t(lx),
                held.pad.ly = std::uint8_t(ly);
                held_inputs.push_back(held);
                continue;
            }
            if (frame_text == "shot") {
                long frame = -1;
                std::string path;
                if (!(ls >> frame >> path) || frame < 0) throw std::runtime_error("bad shot line in " + s.direct.inputs[0]);
                scheduled_shots.emplace_back(frame, std::move(path));
                continue;
            }
            if (frame_text == "give") {
                ScheduledGive give{};
                if (!(ls >> give.frame >> give.weapon >> give.rounds) || give.frame < 0)
                    throw std::runtime_error("bad give line in " + s.direct.inputs[0]);
                scheduled_gives.push_back(give);
                continue;
            }
            if (frame_text == "start") {
                StartPose pose;
                if (!(ls >> pose.position[0] >> pose.position[1] >> pose.position[2] >> pose.yaw))
                    throw std::runtime_error("bad start line in " + s.direct.inputs[0]);
                if (ls >> pose.pitch) ls >> pose.ground_normal_y;
                start_pose = pose;
                continue;
            }
            PadState pad;
            unsigned word, rx, ry, lx, ly;
            if (!(ls >> std::hex >> word >> std::dec >> rx >> ry >> lx >> ly))
                throw std::runtime_error("bad input line in " + s.direct.inputs[0]);
            pad.buttons = buttons_from_sony_pad_word(std::uint16_t(word));
            pad.rx = std::uint8_t(rx), pad.ry = std::uint8_t(ry), pad.lx = std::uint8_t(lx), pad.ly = std::uint8_t(ly);
            scripted_inputs.emplace_back(std::strtol(frame_text.c_str(), nullptr, 10), pad);
        }
    }
    auto scripted_pad = [&](long frame) {
        for (const auto& [at, pad] : scripted_inputs)
            if (at == frame) return pad;
        for (const HeldInput& held : held_inputs)
            if (held.first <= frame && frame <= held.last) return held.pad;
        return compensate_sticks(PadState{});
    };
    auto input_for_frame = [&](long frame, std::size_t local) {
        if (use_script) return local == 0 ? scripted_pad(frame) : PadState{};
        return local < s.pads.size() ? s.pads[local].sample() : PadState{};
    };
    std::uint8_t viewer = 0;
    const double kStep = s.timing.rec();
    bool running = true, captured = false, match_over = false;
    long input_frames = 0;
    double accumulator = 0;
    Uint64 last = SDL_GetTicksNS(), results_until = 0;
    struct UnackedInput {
        std::uint32_t tick;
        nf::ActionInput mapped;
    };
    std::deque<nf::net::Snapshot> snapshot_history;
    std::array<std::deque<UnackedInput>, nf::net::kMaxLocalPlayers> unacked_inputs;
    std::array<nf::Vec3, nf::kMpSlots> previous_remote_render{};
    std::array<bool, nf::kMpSlots> has_previous_remote_render{};
    std::array<nf::Vec3, nf::kMpSlots> previous_local_position{};
    std::array<float, nf::kMpSlots> previous_local_yaw{}, previous_local_pitch{};
    std::array<bool, nf::kMpSlots> has_previous_local_pose{};
    std::vector<float> correction_magnitudes;
    std::vector<float> remote_position_jitter_cm;
    std::array<bool, nf::net::kMaxLocalPlayers> have_prediction_baseline{};
    double previous_render_tick = 0.0;
    bool have_previous_render_tick = false;
    std::uint32_t local_input_tick = 0;
    bool input_tick_initialized = false;
    Uint64 latest_snapshot_time = SDL_GetTicksNS();
    std::uint32_t latest_snapshot_tick = 0;
    double rendered_view_tick = 0.0;
    std::unique_ptr<Frontend> results_frontend;
    PadHistory results_pad_history;
    Uint64 last_results_update = 0;
    auto apply_snapshot = [&](const nf::net::Snapshot& snapshot) {
        for (const nf::net::PlayerSnapshot& state : snapshot.players) {
            if (state.slot >= nf::World::kMaxPlayers) continue;
            const std::size_t slot = state.slot;
            if (state.present && !s.world->player(int(slot)))
                s.session->ensure_human_actor(int(slot), state.name, true);
            nf::ArenaSettings::Slot& settings = s.session->arena().mutable_settings().slots[slot];
            settings.present = state.present;
            settings.bot = state.bot;
            settings.name = state.name;
            settings.team = state.team;
            settings.character = state.character;
            const bool is_viewer = state.slot >= viewer && state.slot < viewer + network.local_players();
            const std::size_t local = is_viewer ? std::size_t(state.slot - viewer) : 0;
            s.network_body_visible[slot] = state.present && state.alive && (state.visible || is_viewer);
            if (!state.present) continue;
            nf::Player* player = s.world->player(int(slot));
            if (!player) continue;
            if (is_viewer) {
                const nf::Vec3 authoritative{state.x, state.y, state.z};
                const nf::Vec3 predicted_before = player->pos;
                const float server_state_delta = nf::length(authoritative - predicted_before);
                if (!state.owner_movement && !have_prediction_baseline[local] &&
                    state.substate == static_cast<std::uint8_t>(nf::SubState::Walk))
                    player->place_at_rest(authoritative, state.yaw, state.pitch, 1.0f);
                if (state.owner_movement) {
                    restore_owner_movement_state(*player, state);
                } else {
                    player->pos = authoritative;
                    player->yaw = state.yaw;
                    player->pitch = state.pitch;
                    player->velocity = nf::Vec3{state.velocity[0], state.velocity[1], state.velocity[2]};
                    player->substate = static_cast<nf::SubState>(state.substate);
                }
                player->vitals.health = state.health;
                player->vitals.armour = state.armor;
                player->life = state.alive ? nf::LifeState::Alive : nf::LifeState::Dead;
                auto& pending = unacked_inputs[local];
                while (!pending.empty() && pending.front().tick <= snapshot.ack_input_tick) pending.pop_front();
                for (const UnackedInput& input : pending)
                    s.world->replay_player(int(state.slot), input.mapped);
                if (!have_prediction_baseline[local]) {
                    have_prediction_baseline[local] = true;
                    std::printf("net prediction baseline slot=%u tick=%u snapshot delta=%.2f cm\n", state.slot,
                                snapshot.tick, server_state_delta * 100.0f);
                } else {
                    const float correction = nf::length(player->pos - predicted_before);
                    correction_magnitudes.push_back(correction);
                    if (correction > 0.01f)
                        std::printf("net prediction correction slot=%u tick=%u ack=%u pending=%zu %.2f cm\n",
                                    state.slot, snapshot.tick, snapshot.ack_input_tick, pending.size(),
                                    correction * 100.0f);
                }
            } else {
                player->pos = {state.x, state.y, state.z};
                player->yaw = state.yaw;
                player->pitch = state.pitch;
                player->velocity = {state.velocity[0], state.velocity[1], state.velocity[2]};
                player->substate = static_cast<nf::SubState>(state.substate);
                player->vitals.health = state.health;
                player->vitals.armour = state.armor;
                player->life = state.alive ? nf::LifeState::Alive : nf::LifeState::Dead;
            }
        }
    };
    auto sample_player = [&](std::uint8_t slot, double render_tick, nf::net::PlayerSnapshot& result) {
        const nf::net::PlayerSnapshot* before = nullptr;
        const nf::net::PlayerSnapshot* after = nullptr;
        std::uint32_t before_tick = 0, after_tick = 0;
        for (const nf::net::Snapshot& snapshot : snapshot_history) {
            for (const nf::net::PlayerSnapshot& state : snapshot.players) {
                if (state.slot != slot || !state.present || !state.alive || !state.visible) continue;
                if (double(snapshot.tick) <= render_tick && (!before || snapshot.tick >= before_tick)) {
                    before = &state;
                    before_tick = snapshot.tick;
                }
                if (double(snapshot.tick) >= render_tick && (!after || snapshot.tick <= after_tick)) {
                    after = &state;
                    after_tick = snapshot.tick;
                }
            }
        }
        if (!before && !after) return false;
        if (!before) { result = *after; return true; }
        if (!after) {
            result = *before;
            if (snapshot_history.size() >= 2 && render_tick > double(before_tick)) {
                const nf::net::Snapshot& previous = snapshot_history[snapshot_history.size() - 2];
                const nf::net::PlayerSnapshot* old = nullptr;
                for (const auto& state : previous.players)
                    if (state.slot == slot && state.present && state.alive && state.visible) old = &state;
                const double dt = double(before_tick) - double(previous.tick);
                const float extrapolate = float(std::clamp(render_tick - double(before_tick), 0.0, 1.5));
                if (old && dt > 0.0) {
                    const float scale = extrapolate / float(dt);
                    result.x += (before->x - old->x) * scale;
                    result.y += (before->y - old->y) * scale;
                    result.z += (before->z - old->z) * scale;
                }
            }
            return true;
        }
        result = *before;
        const double span = double(after_tick - before_tick);
        const float alpha = span > 0.0 ? float(std::clamp((render_tick - double(before_tick)) / span, 0.0, 1.0)) : 0.0f;
        result.x += (after->x - before->x) * alpha;
        result.y += (after->y - before->y) * alpha;
        result.z += (after->z - before->z) * alpha;
        const float yaw_delta = std::atan2(std::sin(after->yaw - before->yaw), std::cos(after->yaw - before->yaw));
        result.yaw = before->yaw + yaw_delta * alpha;
        return true;
    };
    auto draw_interpolated = [&]() {
        std::array<nf::Player*, nf::World::kMaxPlayers> players{};
        std::array<nf::Vec3, nf::World::kMaxPlayers> saved_positions{};
        std::array<float, nf::World::kMaxPlayers> saved_yaw{}, saved_pitch{};
        const Uint64 render_now = SDL_GetTicksNS();
        const double rate = double(s.config.logic_hz);
        const double extrapolated =
            std::min(rate * 0.15, double(render_now - latest_snapshot_time) * rate * 1.0e-9);
        const double render_tick = double(latest_snapshot_tick) + extrapolated - rate / 10.0;
        const float interpolation =
            float(std::clamp(render_tick - std::floor(render_tick), 0.0, 1.0));
        for (std::uint8_t slot = 0; slot < nf::World::kMaxPlayers; ++slot) {
            if (slot >= network.slot() && slot < std::uint8_t(network.slot() + network.local_players())) continue;
            nf::net::PlayerSnapshot state;
            if (!sample_player(slot, render_tick, state)) {
                has_previous_remote_render[slot] = false;
                continue;
            }
            nf::Player* player = s.world->player(int(slot));
            if (!player) continue;
            const nf::Vec3 state_position{state.x, state.y, state.z};
            players[slot] = player;
            saved_positions[slot] = player->pos;
            if (has_previous_remote_render[slot] && have_previous_render_tick) {
                nf::net::PlayerSnapshot previous;
                if (sample_player(slot, previous_render_tick, previous)) {
                    const nf::Vec3 expected{state.x - previous.x, state.y - previous.y, state.z - previous.z};
                    const nf::Vec3 actual = state_position - previous_remote_render[slot];
                    remote_position_jitter_cm.push_back(nf::length(actual - expected) * 100.0f);
                }
            }
            previous_remote_render[slot] = state_position;
            has_previous_remote_render[slot] = true;
            saved_yaw[slot] = player->yaw;
            saved_pitch[slot] = player->pitch;
            player->pos = state_position;
            player->yaw = state.yaw;
            player->pitch = state.pitch;
        }
        for (std::uint8_t slot = std::uint8_t(network.slot());
             slot < std::uint8_t(network.slot() + network.local_players()); ++slot) {
            nf::Player* player = s.world->player(int(slot));
            if (!player || !has_previous_local_pose[slot]) continue;
            if (!players[slot]) {
                players[slot] = player;
                saved_positions[slot] = player->pos;
                saved_yaw[slot] = player->yaw;
                saved_pitch[slot] = player->pitch;
            }
            player->pos = previous_local_position[slot] +
                          (player->pos - previous_local_position[slot]) * interpolation;
            const float yaw_delta = std::atan2(std::sin(player->yaw - previous_local_yaw[slot]),
                                               std::cos(player->yaw - previous_local_yaw[slot]));
            player->yaw = previous_local_yaw[slot] + yaw_delta * interpolation;
            player->pitch = previous_local_pitch[slot] +
                            (player->pitch - previous_local_pitch[slot]) * interpolation;
        }
        std::array<nf::drone::Drone*, nf::kMpSlots> bot_drones{};
        std::array<nf::Vec3, nf::kMpSlots> bot_positions{};
        std::array<float, nf::kMpSlots> bot_yaws{};
        std::array<bool, nf::kMpSlots> bot_hidden{};
        if (s.bot_match) {
            for (std::size_t slot = 0; slot < s.session->arena().settings().slot_count; ++slot) {
                auto* bot = s.bot_match->bots().bot_at_slot(int(slot));
                if (!bot || !bot->drone) continue;
                nf::drone::Drone& drone = *bot->drone;
                bot_drones[slot] = &drone;
                bot_positions[slot] = drone.pos;
                bot_yaws[slot] = drone.yaw;
                bot_hidden[slot] = drone.hidden;
                nf::net::PlayerSnapshot state;
                if (!sample_player(std::uint8_t(slot), render_tick, state)) {
                    has_previous_remote_render[slot] = false;
                    drone.hidden = true;
                    continue;
                }
                const nf::Vec3 state_position{state.x, state.y, state.z};
                if (has_previous_remote_render[slot] && have_previous_render_tick) {
                    nf::net::PlayerSnapshot previous;
                    if (sample_player(std::uint8_t(slot), previous_render_tick, previous)) {
                        const nf::Vec3 expected{state.x - previous.x, state.y - previous.y, state.z - previous.z};
                        const nf::Vec3 actual = state_position - previous_remote_render[slot];
                        remote_position_jitter_cm.push_back(nf::length(actual - expected) * 100.0f);
                    }
                }
                previous_remote_render[slot] = state_position;
                has_previous_remote_render[slot] = true;
                drone.pos = state_position;
                drone.yaw = state.yaw;
                drone.hidden = !state.visible || !state.alive;
            }
        }
        previous_render_tick = render_tick;
        have_previous_render_tick = true;
        s.draw_views(network.connected() ? int(network.slot()) : int(viewer),
                     network.connected() ? int(network.local_players()) : 0, interpolation);
        for (std::size_t i = 0; i < players.size(); ++i) {
            if (!players[i]) continue;
            players[i]->pos = saved_positions[i];
            players[i]->yaw = saved_yaw[i];
            players[i]->pitch = saved_pitch[i];
        }
        for (std::size_t i = 0; i < bot_drones.size(); ++i) {
            if (!bot_drones[i]) continue;
            bot_drones[i]->pos = bot_positions[i];
            bot_drones[i]->yaw = bot_yaws[i];
            bot_drones[i]->hidden = bot_hidden[i];
        }
    };
    bool running_match = true;
    auto draw_results = [&] {
        const Uint64 now = SDL_GetTicksNS();
        if (results_frontend && now - last_results_update >= 33333333ull) {
            results_pad_history.push({});
            results_frontend->update(results_pad_history);
            last_results_update = now;
        }
        int width = 0, height = 0;
        s.window.begin_frame(width, height);
        s.ui.begin(width, height);
        if (results_frontend) results_frontend->draw(s.ui, s.text);
        s.ui.end();
    };
    bool start_pose_applied = false;
    while (running && running_match && (frames < 0 || input_frames < frames)) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            for (auto& pad : s.pads) pad.handle_event(event);
            if (event.type == SDL_EVENT_QUIT) running = false;
            else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && !captured) {
                captured = SDL_SetWindowRelativeMouseMode(s.window.sdl(), true);
                for (auto& pad : s.pads) pad.set_captured(captured);
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
                if (captured) {
                    captured = !SDL_SetWindowRelativeMouseMode(s.window.sdl(), false);
                    for (auto& pad : s.pads) pad.set_captured(captured);
                }
                running = false;
            }
        }
        network.poll();
        if (start_pose && !start_pose_applied && network.connected() && network.slot() < nf::World::kMaxPlayers) {
            if (nf::Player* player = s.world->player(network.slot()))
                player->place_at_rest(start_pose->position, start_pose->yaw, start_pose->pitch,
                                      start_pose->ground_normal_y);
            start_pose_applied = true;
        }
        for (const nf::net::Snapshot& snapshot : network.take_snapshots()) {
            if (!network.connected()) continue;
            latest_snapshot_tick = snapshot.tick;
            latest_snapshot_time = SDL_GetTicksNS();
            if (snapshot_history.size() == 32) snapshot_history.pop_front();
            snapshot_history.push_back(snapshot);
            apply_snapshot(snapshot);
            if (snapshot.match_phase == std::uint8_t(nf::MatchPhase::Over)) {
                if (!match_over) {
                    match_over = true;
                    results_until = latest_snapshot_time + 3000000000ull;
                    nf::DebriefInfo info;
                    for (const nf::net::PlayerSnapshot& player : snapshot.players) {
                        if (!player.present) continue;
                        nf::DebriefRow row;
                        row.name = player.name;
                        row.character = player.character;
                        row.kills = player.kills;
                        row.deaths = player.deaths;
                        row.points = player.points;
                        row.score = player.score;
                        row.is_bot = player.bot;
                        row.slot = int(player.slot);
                        info.rows.push_back(std::move(row));
                    }
                    std::stable_sort(info.rows.begin(), info.rows.end(), [](const nf::DebriefRow& a, const nf::DebriefRow& b) {
                        if (a.score != b.score) return a.score > b.score;
                        if (a.points != b.points) return a.points > b.points;
                        return a.kills > b.kills;
                    });
                    info.banner = "Match Results";
                    info.slot_count = s.session->arena().settings().slot_count;
                    results_frontend = std::make_unique<Frontend>(s.ctx.assets, s.ctx.menu, &s.ctx.mp_data, &s.ctx.sp_data);
                    results_frontend->set_debriefing(std::move(info));
                    results_frontend->open(FrontendMode::MainMenu, kPageDebriefing);
                }
            }
        }
        for (std::string& message : network.take_chat()) {
            std::printf("chat: %s\n", message.c_str());
            s.pending_chat.push_back(std::move(message));   // shown by the HUD overlay's chat lines
        }
        const Uint64 now = SDL_GetTicksNS();
        accumulator = std::min(accumulator + double(now - last) * 1e-9, 0.25);
        last = now;
        while (network.connected() && !match_over && accumulator >= kStep) {
            viewer = network.slot();
            PadInputs pads{};
            std::array<PadState, nf::net::kMaxLocalPlayers> local_pads{};
            for (const ScheduledGive& give : scheduled_gives) {
                if (give.frame == input_frames &&
                    !s.session->weapons().give_weapon(network.slot(), give.weapon, give.rounds))
                    throw std::runtime_error("scripted network weapon grant was refused");
            }
            for (std::size_t local = 0; local < network.local_players(); ++local) {
                local_pads[local] = input_for_frame(input_frames, local);
                pads[std::size_t(viewer) + local] = local_pads[local];
            }
            if (!input_tick_initialized) {
                local_input_tick = network.server_tick();
                input_tick_initialized = true;
            }
            const std::uint32_t input_tick = ++local_input_tick;
            const std::uint32_t view_tick = std::uint32_t(std::max(0.0, std::floor(rendered_view_tick)));
            network.send_inputs(std::span<const PadState>(local_pads.data(), network.local_players()), view_tick);
            for (std::size_t local = 0; local < network.local_players(); ++local) {
                const std::size_t slot = std::size_t(viewer) + local;
                const nf::Player* player = s.world->player(int(slot));
                if (!player) continue;
                previous_local_position[slot] = player->pos;
                previous_local_yaw[slot] = player->yaw;
                previous_local_pitch[slot] = player->pitch;
                has_previous_local_pose[slot] = true;
            }
            for (std::size_t local = 0; local < network.local_players(); ++local) {
                const std::size_t slot = std::size_t(viewer) + local;
                s.previous_viewmodels[local] = s.session->weapons().viewmodel(int(slot));
                s.has_previous_viewmodel[local] = true;
            }
            if (s.drone_renderer && s.bot_match) s.drone_renderer->capture_previous(s.bot_match->drones());
            s.session->tick(pads, s.timing);
            s.tick_overlays(pads);
            for (std::size_t local = 0; local < network.local_players(); ++local) {
                auto& pending = unacked_inputs[local];
                pending.push_back({input_tick, s.world->input(int(viewer) + int(local))});
                if (pending.size() > 64) pending.pop_front();
            }
            s.tick_bodies();
            if (has_audio) s.audio_frame();
            s.effects->consume(s.session->weapons().events());
            s.effects->tick(s.timing.mul(), s.session->weapons().projectiles());
            s.session->weapons().events().clear();
            accumulator -= kStep;
            ++input_frames;
        }
        if (!running) break;
        if (match_over) {
            if (SDL_GetTicksNS() >= results_until) running_match = false;
            draw_results();
        } else {
            draw_interpolated();
        }
        s.window.swap();
        for (auto shot_it = scheduled_shots.begin(); shot_it != scheduled_shots.end();) {
            if (shot_it->first <= input_frames) {
                if (match_over && results_frontend) draw_results();
                else draw_interpolated();
                if (!s.window.save_bmp(shot_it->second))
                    throw std::runtime_error(std::string("cannot write scheduled network screenshot: ") + SDL_GetError());
                std::printf("network match screenshot -> %s (frame %ld)\n", shot_it->second.c_str(), input_frames);
                shot_it = scheduled_shots.erase(shot_it);
            } else {
                ++shot_it;
            }
        }
    }
    if (!shot.empty()) {
        if (match_over && results_frontend) draw_results();
        else draw_interpolated();
        if (!s.window.save_bmp(shot)) throw std::runtime_error(std::string("cannot write network match screenshot: ") + SDL_GetError());
        std::printf("network match screenshot -> %s\n", shot.c_str());
    }
    if (!correction_magnitudes.empty()) {
        std::vector<float> sorted = correction_magnitudes;
        std::sort(sorted.begin(), sorted.end());
        double sum = 0.0;
        for (float value : sorted) sum += value;
        const std::size_t p99_index = (sorted.size() * 99 + 99) / 100 - 1;
        std::printf("net prediction corrections=%zu mean=%.2f cm p99=%.2f cm\n", sorted.size(),
                    sum * 100.0 / double(sorted.size()), double(sorted[p99_index]) * 100.0);
    }
    if (!remote_position_jitter_cm.empty()) {
        std::sort(remote_position_jitter_cm.begin(), remote_position_jitter_cm.end());
        const std::size_t p99_index = (remote_position_jitter_cm.size() * 99 + 99) / 100 - 1;
        std::printf("net remote render-position jitter samples=%zu p99=%.3f cm\n",
                    remote_position_jitter_cm.size(), remote_position_jitter_cm[p99_index]);
    }
    s.network = nullptr;
    input_devices().assign({});
    if (s.menu_pad_handle) {
        SDL_CloseGamepad(s.menu_pad_handle);
        s.menu_pad_handle = nullptr;
    }
    const auto* latest = network.latest_snapshot();
    return MpResult{MpExit::QuitToMenu, input_frames > 0, match_over, latest ? latest->match_revision : 0};
}

MpResult MpSession::run_headless() {
    Impl& s = *impl_;
    struct StartPose {
        Vec3 position;
        float yaw = 0.0f, pitch = 0.0f, ground_normal_y = 1.0f;
    };
    std::array<std::vector<std::pair<long, PadState>>, 4> scripts;
    std::array<std::optional<StartPose>, 4> start_poses;
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
            if (head == "start") {
                StartPose pose;
                if (!(ls >> pose.position[0] >> pose.position[1] >> pose.position[2] >> pose.yaw))
                    throw std::runtime_error("bad start line in " + s.direct.inputs[std::size_t(i)]);
                if (ls >> pose.pitch) ls >> pose.ground_normal_y;
                start_poses[std::size_t(i)] = pose;
                continue;
            }
            PadState pad;
            unsigned word, rx, ry, lx, ly;
            if (!(ls >> std::hex >> word >> std::dec >> rx >> ry >> lx >> ly)) throw std::runtime_error("bad input line");
            pad.buttons = buttons_from_sony_pad_word(std::uint16_t(word));
            pad.rx = std::uint8_t(rx), pad.ry = std::uint8_t(ry), pad.lx = std::uint8_t(lx), pad.ly = std::uint8_t(ly);
            scripts[std::size_t(i)].emplace_back(std::strtol(head.c_str(), nullptr, 10), pad);
        }
    }
    for (int i = 0; i < s.session->humans(); ++i) {
        if (const auto& pose = start_poses[std::size_t(i)]; pose)
            s.world->player(i)->place_at_rest(pose->position, pose->yaw, pose->pitch, pose->ground_normal_y);
    }
    const long frames = s.direct.frames >= 0 ? s.direct.frames : 300;
    auto at = [&](int player, long frame) {
        for (const auto& [f, p] : scripts[std::size_t(player)])
            if (f == frame) return p;
        return compensate_sticks(PadState{});   // headless neutral (see session_sp.cpp)
    };
    const FrameTiming timing{float(s.config.logic_hz)};
    for (long f = 0; f < frames; ++f) {
        PadInputs pads{};
        for (int i = 0; i < s.session->humans(); ++i) pads[std::size_t(i)] = at(i, f);
        s.session->tick(pads, timing);
        s.tick_overlays(pads);
        s.tick_bodies();
        s.effects->consume(s.session->weapons().events());
        s.effects->tick(timing.mul(), s.session->weapons().projectiles());
        s.session->weapons().events().clear();
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
            MenuBackground background(s.ctx.gamedir);
            background.advance();
            PadHistory hist;
            for (int i = 0; i < 30; ++i) {
                hist.push({});
                menu.update(hist);
                background.advance();
            }
            int w, h;
            s.window.begin_frame(w, h);
            s.ui.begin(w, h);
            background.draw(s.ui);
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

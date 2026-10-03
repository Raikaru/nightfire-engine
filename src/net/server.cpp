#include <condition_variable>
#include <stop_token>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <iostream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "assets/character.hpp"
#include "assets/level.hpp"
#include "assets/strings.hpp"
#include "app/app.hpp"
#include "game/arena_session.hpp"
#include "game/bot_match.hpp"
#include "game/input.hpp"
#include "game/world.hpp"
#include "game/drone_vision.hpp"
#include "net/net.hpp"
#include "core/rng.hpp"
#include "net/server_runtime.hpp"

namespace {
void usage() {
    std::puts("usage: nfserver <gamedir> [--config file] [--map file.bin] [--mode arena] [--ruleset ps2|gc-xbox|extended]"
              " [--bots N] [--port 27500] [--name server] [--password text] [--master host:port]"
              " [--frag-limit N] [--time-limit minutes] [--net-sim-loss percent] [--net-sim-latency ms]"
              " [--visibility-culling|--no-visibility-culling] [--logic-hz 30|60] [--ticks N]");
}
using Clock = std::chrono::steady_clock;
constexpr std::size_t kPendingInputCapacity = 64;
struct Peer {
    bool active = false;
    std::string host, name;
    std::uint16_t port = 0;
    nf::net::Reliability reliability;
    std::uint8_t slot = 0, local_players = 1;
    std::array<std::uint32_t, nf::net::kMaxLocalPlayers> last_input_tick{};
    std::array<std::uint32_t, nf::net::kMaxLocalPlayers> last_received_input_tick{};
    std::array<std::uint32_t, nf::net::kMaxLocalPlayers> view_tick{};
    std::array<nf::net::PadInput, kPendingInputCapacity> pending_inputs{};
    std::size_t pending_input_head = 0, pending_input_count = 0;
    Clock::time_point last_seen{};
};
struct AdminQueue {
    std::mutex mutex;
    std::deque<std::string> lines;
};

struct HitHistoryFrame {
    std::uint32_t tick = 0;
    nf::WeaponSystem::LagCompVolumes volumes;
};

HitHistoryFrame capture_hit_history(std::uint32_t tick, nf::World& world, const nf::ArenaSystem& arena,
                                    nf::bots::BotMatch* bots) {
    HitHistoryFrame frame;
    frame.tick = tick;
    for (int slot = 0; slot < int(arena.settings().slot_count); ++slot) {
        const auto& settings = arena.settings().slots[std::size_t(slot)];
        if (!settings.present) continue;
        auto& volume = frame.volumes.values[frame.volumes.count++];
        volume.id = slot;
        if (settings.bot) {
            auto* bot = bots ? bots->bots().bot_at_slot(slot) : nullptr;
            if (!bot || !bot->arena_body || !bot->drone) {
                --frame.volumes.count;
                continue;
            }
            const nf::Vec3 center = bot->drone->pos;
            const float radius = bot->drone->radius;
            const float half_height = std::max(0.0f, bot->drone->stand_height - radius + 0.4f);
            volume.alive = bot->drone->alive();
            volume.a = nf::Vec3{center[0], center[1] + half_height, center[2]};
            volume.b = nf::Vec3{center[0], center[1] - half_height, center[2]};
            volume.blast_ref = center;
            volume.radius = radius;
            continue;
        }
        const nf::Player* player = world.player(slot);
        if (!player) {
            --frame.volumes.count;
            continue;
        }
        volume.alive = player->alive();
        volume.blast_ref = player->eye();
        if (player->substate == nf::SubState::Crouch) {
            volume.a = player->capsule_a;
            volume.b = player->capsule_b;
            volume.radius = player->capsule_radius;
        } else {
            volume.a = nf::Vec3{player->pos[0], player->pos[1] + 0.275f, player->pos[2]};
            volume.b = nf::Vec3{player->pos[0], player->pos[1] + 0.55f - player->stand_height, player->pos[2]};
            volume.radius = 0.55f;
        }
    }
    return frame;
}


bool parse_int(const std::string& text, int& value) {
    char* end = nullptr;
    const long v = std::strtol(text.c_str(), &end, 10);
    if (!end || *end || v < 0 || v > 1000000) return false;
    value = int(v);
    return true;
}

void put32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) out.push_back(std::uint8_t(value >> (i * 8)));
}
void put16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(std::uint8_t(value));
    out.push_back(std::uint8_t(value >> 8));
}

nf::net::Packet master_message(std::uint8_t type, std::uint16_t game_port, const std::string& name = {}) {
    nf::net::Packet packet;
    packet.payload = {'N', 'F', 'M', 'R', 1, type};
    put16(packet.payload, game_port);
    if (type == 1) {
        const std::size_t count = std::min<std::size_t>(name.size(), 64);
        packet.payload.push_back(std::uint8_t(count));
        packet.payload.insert(packet.payload.end(), name.begin(), name.begin() + std::ptrdiff_t(count));
    }
    return packet;
}

bool same_endpoint(const Peer& peer, const nf::net::Received& packet) {
    return peer.active && peer.host == packet.address && peer.port == packet.port;
}

nf::net::OwnerMovementState owner_movement_state(const nf::PlayerPredictionState& source) {
    nf::net::OwnerMovementState state;
    for (std::size_t i = 0; i < 3; ++i) {
        state.fall_velocity[i] = source.fall_velocity[i];
        state.settled_pos[i] = source.settled_pos[i];
        state.prev_pos[i] = source.prev_pos[i];
        state.prev_velocity[i] = source.prev_velocity[i];
    }
    state.body_flags = source.body_flags;
    state.ground_normal_y = source.ground_normal_y;
    state.jump_state = source.jump_state;
    state.ground_history = source.ground_history;
    state.anim_random_timer = source.anim_random_timer;
    state.stand_height = source.stand_height;
    state.applied_height = source.applied_height;
    state.yaw_step = source.yaw_step;
    state.fall_timer = source.fall_timer;
    state.enabled = source.enabled;
    state.input_frozen = source.input_frozen;
    state.movement_frozen = source.movement_frozen;
    state.jump_delay = source.jump_delay;
    state.crouch_timer = source.crouch_timer;
    state.turn_speed = source.turn_speed;
    state.pitch_speed = source.pitch_speed;
    state.pitch_target = source.pitch_target;
    state.aim_yaw = source.aim_yaw;
    state.scope_aiming = source.scope_aiming;
    state.zoom = source.zoom;
    state.aim_state = {source.aim.cursor_x, source.aim.cursor_y, source.aim.turn_x, source.aim.turn_y,
                       source.aim.scope_x, source.aim.scope_y};
    state.timing_rate = source.timing.FRAME_RATE;
    state.body_basis = source.body_basis;
    state.look_state = source.look_state;
    state.walk_class = source.walk_class;
    state.water_room = std::int32_t(source.water.room);
    for (std::size_t axis = 0; axis < 3; ++axis)
        state.water_room_from[axis] = source.water.room_from[axis];
    state.water_air = source.water.air;
    state.water_surfaced = source.water.surfaced;
    state.water_meter_alpha = source.water.meter_alpha;
    state.water_meter_flags = source.water.meter_flags;
    state.water_meter_enabled = source.water.meter_enabled;
    state.water_frame = source.water.frame;
    return state;
}

nf::net::Snapshot make_snapshot(std::uint32_t tick, nf::World& world, nf::ArenaSession& session,
                                nf::bots::BotMatch* bots) {
    nf::net::Snapshot snapshot;
    const nf::ArenaSystem& arena = session.arena();
    snapshot.tick = tick;
    snapshot.slot_count = std::uint8_t(arena.settings().slot_count);
    snapshot.match_phase = std::uint8_t(arena.phase());
    snapshot.state_code = std::uint8_t(arena.state_code());
    snapshot.score_limit = std::int16_t(std::clamp(arena.settings().score_limit, -1, 32767));
    snapshot.elapsed = arena.elapsed();
    snapshot.time_left = arena.time_left();
    snapshot.team_score = arena.team_score();
    const auto scoreboard = arena.scoreboard();
    snapshot.players.reserve(arena.settings().slot_count);
    for (std::uint8_t slot = 0; slot < snapshot.slot_count; ++slot) {
        nf::net::PlayerSnapshot state;
        const auto& settings = arena.settings().slots[slot];
        state.slot = slot;
        state.present = settings.present;
        state.bot = settings.bot;
        state.name = settings.name;
        state.team = std::int8_t(settings.team);
        state.character = std::uint8_t(settings.character);
        state.kills = std::int16_t(std::clamp(arena.kills(slot), -32768, 32767));
        state.deaths = std::int16_t(std::clamp(arena.deaths(slot), -32768, 32767));
        state.out = arena.participant_out(slot);
        if (const auto it = std::find_if(scoreboard.begin(), scoreboard.end(), [slot](const nf::ScoreRow& row) {
                return row.slot == slot;
            }); it != scoreboard.end()) {
            state.score = std::int16_t(std::clamp(it->score, -32768, 32767));
            state.points = it->points;
        }
        if (!settings.bot && slot < nf::World::kMaxPlayers) {
            if (const nf::Player* player = world.player(slot)) {
                state.alive = player->alive();
                state.visible = settings.present;
                state.x = player->pos[0]; state.y = player->pos[1]; state.z = player->pos[2];
                state.yaw = player->view_yaw(); state.pitch = player->view_pitch();
                state.velocity = {player->velocity[0], player->velocity[1], player->velocity[2]};
                state.health = player->health(); state.armor = player->armor();
                state.substate = std::uint8_t(player->substate);
                const int weapon_id = session.weapons().current_weapon(slot);
                state.weapon = std::uint8_t(std::clamp(weapon_id, 0, 255));
                state.weapon_anim = std::uint8_t(session.weapons().anim_state(slot));
                state.weapon_clip = std::int16_t(std::clamp(
                    session.weapons().clip(slot, weapon_id), -32768, 32767));
                const int ammo_type = weapon_id >= 0 && weapon_id < nf::WeaponTable::kWeaponCount
                                          ? session.weapons().table().weapon(weapon_id).ammo_type
                                          : 0;
                state.weapon_ammo = std::int16_t(std::clamp(
                    ammo_type > 0 && ammo_type < nf::WeaponTable::kAmmoCount
                        ? session.weapons().ammo_pool(slot, ammo_type)
                        : 0,
                    -32768, 32767));
                state.aiming = session.weapons().aiming(slot);
            }
        } else if (settings.bot && bots) {
            if (auto* bot = bots->bots().bot_at_slot(slot); bot && bot->arena_body) {
                const nf::Vec3 pos = bot->arena_body->position();
                state.alive = bot->arena_body->alive();
                state.visible = settings.present;
                state.x = pos[0]; state.y = pos[1]; state.z = pos[2];
                state.health = bot->drone ? bot->drone->health : 0.0f;
                state.yaw = bot->drone ? bot->drone->yaw : 0.0f;
            }
        }
        state.radar_x = state.x; state.radar_y = state.y; state.radar_z = state.z;
        snapshot.players.push_back(std::move(state));
    }
    return snapshot;
}
nf::net::WorldState make_world_state(std::uint32_t tick, const nf::ArenaSystem& arena) {
    nf::net::WorldState state;
    state.tick = tick;
    const auto& pickups = arena.pickups().all();
    const auto& objectives = arena.objectives();
    const std::size_t objective_count = std::min<std::size_t>(objectives.size(), 32);
    const std::size_t payload_limit = nf::net::kMaxDatagramBytes - 20;
    const std::size_t pickup_limit = std::min<std::size_t>(
        128, (payload_limit - 7 - objective_count * 22) / 7);
    state.pickups.reserve(std::min(pickups.size(), pickup_limit));
    for (std::size_t i = 0; i < pickups.size() && i < pickup_limit; ++i) {
        const nf::Pickup& pickup = pickups[i];
        state.pickups.push_back({std::uint16_t(i), std::uint8_t(pickup.state), pickup.spin});
    }
    state.objectives.reserve(objective_count);
    for (std::size_t i = 0; i < objective_count; ++i) {
        const nf::MpObjective& objective = objectives[i];
        state.objectives.push_back({std::uint8_t(i), std::uint8_t(objective.kind),
                                    std::int8_t(objective.team), std::int8_t(objective.carrier),
                                    std::uint8_t(objective.state), objective.visible,
                                    objective.pos[0], objective.pos[1], objective.pos[2], objective.hit_points});
    }
    return state;
}

std::vector<nf::net::ProjectileSnapshot> make_projectiles(const nf::WeaponSystem& weapons) {
    std::vector<nf::net::ProjectileSnapshot> projectiles;
    const auto& source = weapons.projectiles();
    projectiles.reserve(source.size());
    for (std::size_t i = 0; i < source.size() && i <= 65535; ++i) {
        const nf::Projectile& projectile = source[i];
        if (projectile.delete_me) continue;
        nf::net::ProjectileSnapshot state;
        state.id = std::uint16_t(i);
        state.weapon = std::uint16_t(projectile.weapon);
        state.owner = std::int16_t(projectile.owner);
        state.state = std::uint8_t(projectile.state);
        state.resting = projectile.resting;
        state.position = {projectile.pos[0], projectile.pos[1], projectile.pos[2]};
        state.direction = {projectile.dir[0], projectile.dir[1], projectile.dir[2]};
        state.age = projectile.age;
        projectiles.push_back(state);
    }
    return projectiles;
}

std::vector<nf::net::ReplicationEvent> make_events(std::uint32_t tick, nf::ArenaSession& session) {
    std::vector<nf::net::ReplicationEvent> events;
    auto append = [&](nf::net::ReplicationEvent event) {
        if (events.size() < 255) {
            event.id = (tick << 8) | std::uint32_t(events.size());
            event.tick = tick;
            events.push_back(std::move(event));
        }
    };
    auto& weapon_events = session.weapons().events();
    for (const nf::SoundEvent& sound : weapon_events.sounds) {
        nf::net::ReplicationEvent event;
        event.actor = std::int16_t(sound.listener);
        event.target = std::int16_t(sound.exclude);
        event.code = std::int16_t(sound.id);
        event.position = {sound.position[0], sound.position[1], sound.position[2]};
        append(std::move(event));
    }
    for (const nf::MatchSound& sound : session.sounds()) {
        nf::net::ReplicationEvent event;
        event.kind = nf::net::EventKind::Sound;
        event.actor = std::int16_t(sound.slot);
        event.code = std::int16_t(sound.id);
        event.positional = sound.at.has_value();
        if (sound.at) event.position = {(*sound.at)[0], (*sound.at)[1], (*sound.at)[2]};
        append(std::move(event));
    }
    for (const nf::ImpactEvent& impact : weapon_events.impacts) {
        nf::net::ReplicationEvent event;
        event.kind = nf::net::EventKind::Impact;
        event.actor = std::int16_t(impact.shooter);
        event.code = std::int16_t(impact.surface);
        event.weapon = std::int16_t(impact.weapon);
        event.on_body = impact.on_body;
        event.position = {impact.point[0], impact.point[1], impact.point[2]};
        event.normal = {impact.normal[0], impact.normal[1], impact.normal[2]};
        append(std::move(event));
    }
    for (const nf::ExplosionEvent& explosion : weapon_events.explosions) {
        nf::net::ReplicationEvent event;
        event.kind = nf::net::EventKind::Explosion;
        event.weapon = std::uint16_t(explosion.weapon);
        event.script = explosion.script;
        event.position = {explosion.position[0], explosion.position[1], explosion.position[2]};
        event.radius = explosion.radius;
        event.yaw = explosion.yaw;
        append(std::move(event));
    }
    for (const nf::MatchMessage& message : session.messages()) {
        nf::net::ReplicationEvent event;
        event.kind = nf::net::EventKind::Message;
        event.actor = std::int16_t(message.slot);
        event.code = std::int16_t(message.type);
        event.frames = std::uint16_t(std::clamp(message.frames, 0, 65535));
        event.text = message.text;
        append(std::move(event));
    }
    for (const nf::PickupEvent& pickup : session.arena().pickup_events()) {
        nf::net::ReplicationEvent event;
        event.kind = nf::net::EventKind::Pickup;
        event.actor = std::int16_t(pickup.slot);
        event.code = std::int16_t(pickup.sound);
        event.label = pickup.label;
        event.arg_label = pickup.arg_label;
        event.count = std::int16_t(std::clamp(pickup.count, 0, 32767));
        event.position = {pickup.pos[0], pickup.pos[1], pickup.pos[2]};
        append(std::move(event));
    }
    weapon_events.clear();
    session.messages().clear();
    session.sounds().clear();
    return events;
}

struct RuntimeState {
    std::mutex mutex;
    std::condition_variable condition;
    nf::net::ServerStatus status;
    std::deque<nf::net::MatchConfig> queued_matches;
    std::string error;
    bool startup_complete = false, starting = false;
};
}  // namespace

int run_server_impl(int argc, char** argv, std::stop_token stop = {}, const nf::net::ServerConfig* runtime_config = nullptr,
                    RuntimeState* runtime_state = nullptr) {
    nf::GameRng server_rng;
    nf::ScopedGameRng rng_binding(server_rng);
    if (!runtime_config && (argc < 2 || std::string(argv[1]) == "--help")) {
        usage();
        return argc < 2 ? 2 : 0;
    }
    const std::filesystem::path game_dir = runtime_config ? runtime_config->data_dir : argv[1];
    std::string map_name = runtime_config ? runtime_config->map : "07000024.bin";
    std::string server_name = runtime_config ? runtime_config->name : "Nightfire";
    std::string password = runtime_config ? runtime_config->password : std::string(), master_host;
    std::uint16_t master_port = runtime_config ? runtime_config->master_port : 27501;
    nf::MatchOptions options;
    options.enabled = true;
    options.humans = 0; // Online humans are admitted only when a peer joins; never reserve ghost players.
    options.log = true;
    int port = 27500, net_loss = 0, net_latency = 0, max_ticks = -1;
    int logic_hz = runtime_config ? runtime_config->logic_hz : int(nf::World::kTickHz);
    bool visibility_culling = true;
    if (runtime_config) {
        options = runtime_config->match;
        options.enabled = true;
        port = runtime_config->port;
        master_host = runtime_config->master_host;
        net_loss = int(runtime_config->net_sim.loss_percent);
        net_latency = int(runtime_config->net_sim.latency_ms);
        visibility_culling = runtime_config->visibility_culling;
    } else {
        try {
            std::vector<std::string> args;
        std::string config_path;
        for (int i = 2; i < argc; ++i) {
            if (std::string(argv[i]) == "--config") {
                if (++i >= argc) throw std::runtime_error("--config needs a value");
                config_path = argv[i];
            } else {
                args.emplace_back(argv[i]);
            }
        }
        if (!config_path.empty()) {
            std::ifstream config(config_path);
            std::vector<std::string> config_args;
            if (!config) throw std::runtime_error("cannot open server config: " + config_path);
            std::string line;
            std::size_t line_number = 0;
            while (std::getline(config, line)) {
                ++line_number;
                const std::size_t comment = line.find('#');
                if (comment != std::string::npos) line.resize(comment);
                const std::size_t first = line.find_first_not_of(" \t\r\n");
                if (first == std::string::npos) continue;
                const std::size_t equals = line.find('=', first);
                if (equals == std::string::npos) throw std::runtime_error("invalid server config line " + std::to_string(line_number));
                const std::size_t key_end = line.find_last_not_of(" \t", equals - 1) + 1;
                const std::size_t value_start = line.find_first_not_of(" \t", equals + 1);
                if (value_start == std::string::npos) throw std::runtime_error("empty server config value on line " + std::to_string(line_number));
                const std::string key = line.substr(first, key_end - first);
                const std::size_t value_end = line.find_last_not_of(" \t\r\n");
                const std::string value_text = line.substr(value_start, value_end - value_start + 1);
                if (key == "visibility-culling") {
                    if (value_text == "true" || value_text == "1") config_args.emplace_back("--visibility-culling");
                    else if (value_text == "false" || value_text == "0") config_args.emplace_back("--no-visibility-culling");
                    else throw std::runtime_error("invalid visibility-culling value on config line " + std::to_string(line_number));
                } else {
                    config_args.push_back("--" + key);
                    config_args.push_back(value_text);
                }
            }
            args.insert(args.begin(), config_args.begin(), config_args.end());
        }
        for (std::size_t i = 0; i < args.size(); ++i) {
            const std::string arg = args[i];
            auto value = [&]() -> std::string {
                if (++i >= args.size()) throw std::runtime_error("missing value for " + arg);
                return args[i];
            };
            if (arg == "--map") map_name = value();
            else if (arg == "--name") server_name = value();
            else if (arg == "--password") password = value();
            else if (arg == "--master") {
                const std::string endpoint = value();
                const std::size_t colon = endpoint.rfind(':');
                master_host = endpoint.substr(0, colon);
                if (colon != std::string::npos) {
                    int v = 0;
                    if (!parse_int(endpoint.substr(colon + 1), v) || v == 0 || v > 65535)
                        throw std::runtime_error("invalid --master port");
                    master_port = std::uint16_t(v);
                }
                if (master_host.empty()) throw std::runtime_error("--master requires a host");
            } else if (arg == "--visibility-culling") visibility_culling = true;
            else if (arg == "--no-visibility-culling") visibility_culling = false;
            else if (arg == "--logic-hz") {
                int v = 0;
                if (!parse_int(value(), v) || (v != 30 && v != 60))
                    throw std::runtime_error("--logic-hz must be 30 or 60");
                logic_hz = v;
            } else if (arg == "--port" || arg == "--net-sim-loss" || arg == "--net-sim-latency" || arg == "--ticks") {
                int v = 0;
                if (!parse_int(value(), v)) throw std::runtime_error("invalid non-negative integer for " + arg);
                if (arg == "--port") { if (v == 0 || v > 65535) throw std::runtime_error("port must be 1..65535"); port = v; }
                else if (arg == "--net-sim-loss") { if (v > 100) throw std::runtime_error("loss must be 0..100"); net_loss = v; }
                else if (arg == "--net-sim-latency") net_latency = v;
                else max_ticks = v;
            } else {
                std::vector<std::string> options_args(args.begin() + std::ptrdiff_t(i), args.end());
                std::size_t at = 0;
                if (!options.parse(options_args, at)) throw std::runtime_error("unknown option: " + arg);
                i += at;
            }
        }
    } catch (const std::exception& e) { std::fprintf(stderr, "nfserver: %s\n", e.what()); return 2; }
    }
    const int match_capacity = int(nf::mp_rule_slot_limit(options.rules));
    const int reserved_humans =
        options.rules == nf::MpRuleSet::Extended ? options.humans : int(nf::kMpMaxLocalHumans);
    options.bots = std::clamp(options.bots, 0, std::max(0, match_capacity - reserved_humans));

    try {
        auto ctx = nf::app::load_context(game_dir.string());
        if (!ctx) throw std::runtime_error("failed to load game data");
        const nf::GameFile* map_file = ctx->files.find(map_name);
        if (!map_file) throw std::runtime_error("map not found in game data: " + map_name);
        auto level_bytes = ctx->files.read(*map_file);
        auto level = std::make_unique<nf::Level>(std::move(level_bytes));
        if (!level->map()) throw std::runtime_error("map data has no Map entry: " + map_name);
        std::string tuning_text;
        if (const nf::GameFile* tuning = ctx->files.find("TuningVars.txt")) {
            const auto raw = ctx->files.read(*tuning);
            tuning_text.assign(reinterpret_cast<const char*>(raw.data()), raw.size());
        }
        std::optional<nf::StringTable> strings;
        if (const nf::GameFile* text = ctx->files.find("USATxt.dat")) strings = nf::StringTable::parse(ctx->files.read(*text), false);
        const nf::PlayerParams params = nf::player_params_from_tuning(tuning_text);
        nf::World world(*level, nf::InputTables::from_elf(ctx->action_elf), params);

        std::unique_ptr<nf::bots::BotMatch> bots;
        if (options.bots > 0) {
            nf::bots::BotMatchOptions bot_options; bot_options.count = options.bots;
            bots = std::make_unique<nf::bots::BotMatch>(ctx->files, game_dir, map_name, *level, world, ctx->action_elf,
                                                         tuning_text, strings ? &*strings : nullptr, bot_options);
        }
        nf::ArenaSession session(world, nf::WeaponTable::from_elf(ctx->action_elf), options,
                                 strings ? &*strings : nullptr, tuning_text,
                                 [&](nf::ArenaSession& arena) { if (bots) bots->install(arena); });
        if (bots) {
            session.weapons().set_bank(&bots->bank());
            session.weapons().set_drone_system(&bots->drones());
            bots->start();
        }

        const auto elf_bytes = nf::read_file(game_dir / "ACTION.ELF");
        const auto data_hash = nf::net::game_data_hash(elf_bytes, ctx->files.read(*map_file));
        nf::net::UdpSocket socket;
        const nf::net::NetSim net_sim =
            runtime_config ? runtime_config->net_sim : nf::net::NetSim{unsigned(net_loss), unsigned(net_latency), 0x4e465345};
        socket.simulate(net_sim);
        std::string socket_error;
        if (!socket.bind(std::uint16_t(port), &socket_error)) throw std::runtime_error(socket_error);
        if (runtime_state) {
            std::lock_guard lock(runtime_state->mutex);
            runtime_state->status.running = true;
            runtime_state->status.match_over = false;
            runtime_state->status.map = map_name;
            runtime_state->status.mode = options.mode;
            runtime_state->startup_complete = true;
            runtime_state->starting = false;
            runtime_state->condition.notify_all();
        }

        std::array<Peer, nf::kMpMaxHumans> peers{};
        std::array<std::array<std::uint32_t, nf::kMpSlots>, nf::kMpSlots> last_visible_tick{};
        nf::PadInputs current_inputs{};
        std::uint32_t tick = 0;
        std::array<std::uint32_t, nf::kMpMaxHumans> shooter_view_ticks{};
        std::array<HitHistoryFrame, 64> hit_history{};
        hit_history[0] = capture_hit_history(0, world, session.arena(), bots.get());
        session.weapons().set_lag_comp_provider([&](int shooter) {
            nf::WeaponSystem::LagCompVolumes empty;
            if (shooter < 0 || shooter >= nf::World::kMaxPlayers) return empty;
            const std::uint32_t requested = shooter_view_ticks[std::size_t(shooter)];
            const std::uint32_t rewind_ticks = std::uint32_t(logic_hz / 5);
            const std::uint32_t rewind_tick = std::clamp(requested, tick > rewind_ticks ? tick - rewind_ticks : 0u, tick);
            const HitHistoryFrame* selected = nullptr;
            for (const HitHistoryFrame& frame : hit_history) {
                if (frame.tick <= rewind_tick && (!selected || frame.tick > selected->tick)) selected = &frame;
            }
            return selected ? selected->volumes : empty;
        });
        auto next_tick = Clock::now();
        std::printf("nfserver: map=%s mode=0x%08x bots=%d UDP=%d culling=%s protocol=%u data=%s\n", map_name.c_str(),
                    options.mode, options.bots, port, visibility_culling ? "on" : "off",
                    unsigned(nf::net::kProtocolVersion), nf::net::hash_hex(data_hash).c_str());
        auto next_master_heartbeat = Clock::now();
        auto admin = std::make_shared<AdminQueue>();
        if (!runtime_config) {
            std::thread([admin] {
                std::string line;
                while (std::getline(std::cin, line)) {
                    std::lock_guard lock(admin->mutex);
                    admin->lines.push_back(std::move(line));
                }
            }).detach();
        }
        std::optional<Clock::time_point> result_deadline;
        int test_give_on_kill_slot = -1, test_give_on_kill_weapon = 0;
        while (!stop.stop_requested() && (max_ticks < 0 || tick < std::uint32_t(max_ticks))) {
            std::deque<std::string> commands;
            {
                std::lock_guard lock(admin->mutex);
                commands.swap(admin->lines);
            }
            for (const std::string& line : commands) {
                std::istringstream command(line);
                std::string name;
                command >> name;
                if (name == "status") {
                    std::printf("nfserver: map=%s mode=0x%08x bots=%d password=%s\n", map_name.c_str(),
                                options.mode, options.bots, password.empty() ? "off" : "on");
                    for (const Peer& peer : peers)
                        if (peer.active) std::printf("  slot %u %s %s:%u\n", unsigned(peer.slot), peer.name.c_str(),
                                                     peer.host.c_str(), unsigned(peer.port));
                } else if (name == "kick") {
                    int slot = -1;
                    if (!(command >> slot) || slot < 0 || slot >= int(nf::kMpMaxHumans)) {
                        std::puts("nfserver: usage: kick <human-slot>");
                        continue;
                    }
                    auto peer_it = std::find_if(peers.begin(), peers.end(), [slot](const Peer& peer) {
                        return peer.active && slot >= peer.slot && slot < peer.slot + peer.local_players;
                    });
                    if (peer_it == peers.end()) {
                        std::puts("nfserver: usage: kick <connected-human-slot>");
                        continue;
                    }
                    nf::net::Packet reject;
                    reject.header.message = nf::net::Message::Reject;
                    const std::string reason = "kicked by server admin";
                    reject.payload.assign(reason.begin(), reason.end());
                    socket.send(peer_it->host, peer_it->port,
                                peer_it->reliability.prepare(std::move(reject), true, Clock::now()));
                    std::printf("nfserver: kicked %s from slots %u..%u\n", peer_it->name.c_str(),
                                unsigned(peer_it->slot), unsigned(peer_it->slot + peer_it->local_players - 1));
                    for (std::size_t local = 0; local < peer_it->local_players; ++local) {
                        const std::size_t human_slot = std::size_t(peer_it->slot) + local;
                        current_inputs[human_slot] = {};
                    }
                    last_visible_tick[peer_it->slot].fill(0);
                    *peer_it = {};
                } else if (name == "say") {
                    std::string text;
                    std::getline(command >> std::ws, text);
                    if (text.empty() || text.size() > 200) { std::puts("nfserver: say requires 1..200 characters"); continue; }
                    nf::net::Packet chat;
                    chat.header.message = nf::net::Message::Chat;
                    const std::string message = "SERVER: " + text;
                    chat.payload.assign(message.begin(), message.end());
                    for (Peer& peer : peers)
                        if (peer.active) socket.send(peer.host, peer.port, peer.reliability.prepare(chat, true, Clock::now()));
                } else if (name == "test-spawns") {
                    const nf::ArenaLevelData data = nf::read_arena_level(*level);
                    for (std::size_t i = 0; i < data.spawns.size(); ++i) {
                        const nf::MpSpawnMarker& spawn = data.spawns[i];
                        std::printf("nfserver: spawn %zu team=%d pos=(%.3f %.3f %.3f) yaw=%.3f\n", i,
                                    spawn.team, spawn.pos[0], spawn.pos[1], spawn.pos[2], spawn.yaw);
                    }
                } else if (name == "test-pose") {
                    int slot = -1;
                    float x = 0.0f, y = 0.0f, z = 0.0f, yaw = 0.0f;
                    if (!(command >> slot >> x >> y >> z >> yaw) || slot < 0 || slot >= nf::World::kMaxPlayers ||
                        !world.player(slot)) {
                        std::puts("nfserver: usage: test-pose <slot> <x> <y> <z> <yaw>");
                        continue;
                    }
                    world.player(slot)->place_at_rest(nf::Vec3{x, y, z}, yaw, 0.0f, 1.0f);
                    std::printf("nfserver: test-pose slot=%d pos=(%.3f %.3f %.3f) yaw=%.3f\n", slot, x, y, z, yaw);
                } else if (name == "test-view") {
                    int from = -1, to = -1;
                    if (!(command >> from >> to) || from < 0 || to < 0 ||
                        from >= nf::World::kMaxPlayers || to >= nf::World::kMaxPlayers ||
                        !world.player(from) || !world.player(to)) {
                        std::puts("nfserver: usage: test-view <from-slot> <to-slot>");
                        continue;
                    }
                    const nf::Player& a = *world.player(from);
                    const nf::Player& b = *world.player(to);
                    const nf::Vec3 eye_a = a.eye(), eye_b = b.eye();
                    std::printf("nfserver: test-view %d=(%.2f %.2f %.2f) yaw=%.3f -> %d=(%.2f %.2f %.2f) yaw=%.3f LOS=%d\n",
                                from, a.pos[0], a.pos[1], a.pos[2], a.yaw, to, b.pos[0], b.pos[1], b.pos[2],
                                b.yaw, world.collision().line_of_sight(eye_a, eye_b));
                } else if (name == "test-give") {
                    int slot = -1, weapon = 0;
                    if (!(command >> slot >> weapon) || slot < 0 || slot >= nf::World::kMaxPlayers ||
                        !session.weapons().give_weapon(slot, weapon, 1)) {
                        std::printf("nfserver: test-give rejected slot=%d weapon=%d\n", slot, weapon);
                        continue;
                    }
                    std::printf("nfserver: test-give slot=%d weapon=%d\n", slot, weapon);
                } else if (name == "test-give-on-kill") {
                    if (!(command >> test_give_on_kill_slot >> test_give_on_kill_weapon) ||
                        test_give_on_kill_slot < 0 || test_give_on_kill_slot >= nf::World::kMaxPlayers) {
                        test_give_on_kill_slot = -1;
                        std::puts("nfserver: usage: test-give-on-kill <slot> <weapon>");
                    } else {
                        std::printf("nfserver: queued weapon %d for slot %d after its next kill\n",
                                    test_give_on_kill_weapon, test_give_on_kill_slot);
                    }
                } else if (name == "password") {
                    command >> password;
                    std::printf("nfserver: password %s\n", password.empty() ? "disabled" : "updated");
                } else if (!name.empty()) {
                    std::printf("nfserver: unknown admin command '%s' (status, kick, say, password)\n", name.c_str());
                }
            }
            const auto master_now = Clock::now();
            if (!master_host.empty() && master_now >= next_master_heartbeat) {
                socket.send(master_host, master_port, master_message(1, std::uint16_t(port), server_name));
                next_master_heartbeat = master_now + std::chrono::seconds(20);
            }
            for (const nf::net::Received& incoming : socket.receive()) {
                if (incoming.packet.header.message == nf::net::Message::ServerQuery) {
                    std::uint32_t query_id = 0;
                    if (!nf::net::decode_server_query(incoming.packet.payload, query_id)) continue;
                    nf::net::ServerInfo info;
                    const nf::ArenaSettings& active_settings = session.arena().settings();
                    info.query_id = query_id;
                    info.name = server_name;
                    info.map = map_name;
                    info.mode = active_settings.mode;
                    if (runtime_state) {
                        std::lock_guard lock(runtime_state->mutex);
                        info.match_revision = runtime_state->status.revision;
                    }
                    info.players = 0;
                    for (const Peer& peer : peers)
                        if (peer.active) info.players = std::uint8_t(info.players + peer.local_players);
                    info.max_players = std::uint8_t(active_settings.slot_count);
                    info.bots = std::uint8_t(active_settings.bot_count());
                    info.slot_count = std::uint8_t(active_settings.slot_count);
                    info.modified_rules = active_settings.score_limit != 10 || active_settings.time_limit != 600.0f ||
                                          active_settings.friendly_fire || active_settings.weapon_set != 0 ||
                                          active_settings.spawn_selection != nf::SpawnSelection::Random;
                    for (int slot = 0; slot < int(nf::kMpMaxHumans); ++slot)
                        info.modified_rules |= active_settings.slots[std::size_t(slot)].health_bonus != 0;
                    info.password_required = !password.empty();
                    nf::net::Packet response;
                    response.header.message = nf::net::Message::ServerInfo;
                    response.payload = nf::net::encode_server_info(info);
                    socket.send(incoming.address, incoming.port, response);
                    continue;
                }
                auto peer_it = std::find_if(peers.begin(), peers.end(), [&](const Peer& p) { return same_endpoint(p, incoming); });
                bool newly_joined = peer_it == peers.end();
                if (incoming.packet.header.message == nf::net::Message::Hello) {
                    std::array<std::uint8_t, nf::net::kDataHashBytes> client_hash{};
                    std::string client_name, client_password;
                    std::uint8_t local_players = 1;
                    if (!nf::net::decode_hello(incoming.packet.payload, client_hash, client_name, client_password,
                                               local_players))
                        continue;
                    nf::net::Packet response;
                    if (client_password != password) {
                        response.header.message = nf::net::Message::Reject;
                        const std::string reason = "incorrect server password";
                        response.payload.assign(reason.begin(), reason.end());
                        socket.send(incoming.address, incoming.port, response);
                        continue;
                    }
                    if (client_hash != data_hash) {
                        response.header.message = nf::net::Message::Reject;
                        const std::string reason = "game data hash mismatch";
                        response.payload.assign(reason.begin(), reason.end());
                        socket.send(incoming.address, incoming.port, response);
                        continue;
                    }
                    if (peer_it != peers.end() && peer_it->local_players != local_players) {
                        response.header.message = nf::net::Message::Reject;
                        const std::string reason = "local player count changed during connection";
                        response.payload.assign(reason.begin(), reason.end());
                        socket.send(incoming.address, incoming.port, response);
                        continue;
                    }
                    if (peer_it == peers.end()) {
                        peer_it = std::find_if(peers.begin(), peers.end(), [](const Peer& p) { return !p.active; });
                        if (peer_it == peers.end()) {
                            response.header.message = nf::net::Message::Reject;
                            const std::string reason = "server connection capacity reached";
                            response.payload.assign(reason.begin(), reason.end());
                            socket.send(incoming.address, incoming.port, response);
                            continue;
                        }
                        std::array<bool, nf::kMpMaxHumans> occupied{};
                        for (const Peer& peer : peers) {
                            if (!peer.active) continue;
                            for (std::size_t local = 0; local < peer.local_players; ++local)
                                occupied[std::size_t(peer.slot) + local] = true;
                        }
                        int base = -1;
                        for (int candidate = 0; candidate + local_players <= int(occupied.size()); ++candidate) {
                            bool free = true;
                            for (int local = 0; local < local_players; ++local)
                                free = free && !occupied[std::size_t(candidate + local)];
                            if (free) { base = candidate; break; }
                        }
                        if (base < 0) {
                            response.header.message = nf::net::Message::Reject;
                            const std::string reason = "not enough free player slots";
                            response.payload.assign(reason.begin(), reason.end());
                            socket.send(incoming.address, incoming.port, response);
                            continue;
                        }
                        bool claimable = true;
                        for (std::size_t local = 0; local < local_players; ++local) {
                            const std::size_t slot = std::size_t(base) + local;
                            if (session.arena().settings().slots[slot].bot &&
                                (!bots || !bots->bots().bot_at_slot(int(slot))))
                                claimable = false;
                        }
                        if (!claimable) {
                            response.header.message = nf::net::Message::Reject;
                            const std::string reason = "a bot slot could not be claimed";
                            response.payload.assign(reason.begin(), reason.end());
                            socket.send(incoming.address, incoming.port, response);
                            continue;
                        }
                        for (std::size_t local = 0; local < local_players; ++local) {
                            const std::size_t slot = std::size_t(base) + local;
                            const std::string suffix = local == 0 ? "" : " " + std::to_string(local + 1);
                            const std::string player_name =
                                client_name.substr(0, 32 - std::min<std::size_t>(suffix.size(), 32)) + suffix;
                            if (session.arena().settings().slots[slot].bot) {
                                if (!bots->replace_bot_with_human(session, int(slot), player_name))
                                    throw std::runtime_error("failed to replace bot participant");
                            } else {
                                session.activate_human(int(slot), player_name);
                            }
                            current_inputs[slot] = {};
                            shooter_view_ticks[slot] = 0;
                            last_visible_tick[slot].fill(0);
                        }
                        peer_it->active = true;
                        peer_it->host = incoming.address;
                        peer_it->port = incoming.port;
                        peer_it->slot = std::uint8_t(base);
                        peer_it->local_players = local_players;
                        peer_it->name = client_name;
                        peer_it->last_input_tick.fill(tick);
                        peer_it->last_received_input_tick.fill(tick);
                        peer_it->view_tick.fill(0);
                        peer_it->pending_input_head = 0;
                        peer_it->pending_input_count = 0;
                        std::printf("nfserver: %s joined slots %u..%u\n", client_name.c_str(), unsigned(base),
                                    unsigned(base + local_players - 1));
                    }
                    peer_it->last_seen = Clock::now();
                    peer_it->reliability.observe(incoming.packet.header);
                    if (!newly_joined) continue;
                    response.header.message = nf::net::Message::Welcome;
                    response.payload = {peer_it->slot, peer_it->local_players};
                    put32(response.payload, tick);
                    socket.send(incoming.address, incoming.port,
                                peer_it->reliability.prepare(std::move(response), true, Clock::now()));
                } else if (peer_it != peers.end()) {
                    peer_it->last_seen = Clock::now();
                    const bool fresh = peer_it->reliability.observe(incoming.packet.header);
                    if (incoming.packet.header.message == nf::net::Message::Input) {
                        nf::net::InputBatch batch;
                        if (!nf::net::decode_input_batch(incoming.packet.payload, batch)) continue;
                        for (std::size_t i = batch.count; i > 0; --i) {
                            const auto& sample = batch.samples[i - 1];
                            if (sample.local_player >= peer_it->local_players) continue;
                            const std::size_t local = sample.local_player;
                            const std::uint32_t input_window = std::uint32_t(logic_hz / 5);
                            if (sample.tick <= peer_it->last_received_input_tick[local] ||
                                sample.tick > tick + input_window || tick > sample.tick + std::uint32_t(logic_hz) ||
                                peer_it->pending_input_count == kPendingInputCapacity)
                                continue;
                            const std::size_t tail =
                                (peer_it->pending_input_head + peer_it->pending_input_count) % kPendingInputCapacity;
                            peer_it->pending_inputs[tail] = sample;
                            ++peer_it->pending_input_count;
                            peer_it->last_received_input_tick[local] = sample.tick;
                        }
                    } else if (fresh && incoming.packet.header.message == nf::net::Message::Chat &&
                               incoming.packet.payload.size() <= 256) {
                        nf::net::Packet chat; chat.header.message = nf::net::Message::Chat;
                        const std::string prefix = peer_it->name + ": ";
                        chat.payload.assign(prefix.begin(), prefix.end());
                        chat.payload.insert(chat.payload.end(), incoming.packet.payload.begin(), incoming.packet.payload.end());
                        for (Peer& peer : peers) {
                            if (!peer.active) continue;
                            socket.send(peer.host, peer.port,
                                        peer.reliability.prepare(chat, true, Clock::now()));
                        }
                    }
                }
            }
            const auto now = Clock::now();
            for (Peer& peer : peers) {
                if (peer.active && now - peer.last_seen > std::chrono::seconds(10)) {
                    const std::uint8_t base = peer.slot;
                    const std::uint8_t local_players = peer.local_players;
                    std::printf("nfserver: %s timed out\n", peer.name.c_str());
                    peer = {};
                    last_visible_tick[base].fill(0);
                    for (std::size_t local = 0; local < local_players; ++local)
                        current_inputs[std::size_t(base) + local] = {};
            }
            }
            for (Peer& peer : peers) {
                if (!peer.active) continue;
                for (const nf::net::Packet& pending : peer.reliability.retransmit_due(now))
                    socket.send(peer.host, peer.port, pending);
            }
            if (now < next_tick) { std::this_thread::sleep_until(next_tick); continue; }
            if (!session.arena().over()) {
                for (Peer& peer : peers) {
                    if (!peer.active) continue;
                    while (peer.pending_input_count != 0 &&
                           peer.pending_inputs[peer.pending_input_head].tick <= tick) {
                        const nf::net::PadInput input = peer.pending_inputs[peer.pending_input_head];
                        peer.pending_input_head = (peer.pending_input_head + 1) % kPendingInputCapacity;
                        --peer.pending_input_count;
                        const std::size_t local = input.local_player;
                        const std::size_t slot = std::size_t(peer.slot) + local;
                        peer.last_input_tick[local] = input.tick;
                        peer.view_tick[local] = input.view_tick;
                        shooter_view_ticks[slot] = input.view_tick;
                        auto& pad = current_inputs[slot];
                        pad.buttons = input.buttons;
                        pad.rx = input.sticks[0]; pad.ry = input.sticks[1];
                        pad.lx = input.sticks[2]; pad.ly = input.sticks[3];
                    }
                }
            }
            nf::PadInputs pads{};
            for (std::size_t i = 0; i < current_inputs.size(); ++i) pads[i] = current_inputs[i];
            if (!session.arena().over()) session.tick(pads, nf::FrameTiming(float(logic_hz)));
            if (test_give_on_kill_slot >= 0) {
                for (const nf::ScoreRow& row : session.arena().scoreboard()) {
                    if (row.slot != test_give_on_kill_slot || row.kills == 0) continue;
                    if (session.weapons().give_weapon(test_give_on_kill_slot, test_give_on_kill_weapon, 6))
                        std::printf("nfserver: test-give-on-kill slot=%d weapon=%d\n", test_give_on_kill_slot,
                                    test_give_on_kill_weapon);
                    else
                        std::printf("nfserver: test-give-on-kill rejected slot=%d weapon=%d\n",
                                    test_give_on_kill_slot, test_give_on_kill_weapon);
                    test_give_on_kill_slot = -1;
                    break;
                }
            }
            ++tick;
            if (runtime_state && session.arena().over()) {
                if (!result_deadline) {
                    result_deadline = Clock::now() + std::chrono::seconds(3);
                    std::lock_guard lock(runtime_state->mutex);
                    runtime_state->status.match_over = true;
                    ++runtime_state->status.revision;
                    runtime_state->condition.notify_all();
                } else if (Clock::now() >= *result_deadline) {
                    return 2;
                }
            }
            hit_history[tick % hit_history.size()] = capture_hit_history(tick, world, session.arena(), bots.get());
            if ((tick % std::uint32_t(std::max(1, logic_hz / 30))) == 0) {
                nf::net::Snapshot snapshot = make_snapshot(tick, world, session, bots.get());
                if (runtime_state) {
                    std::lock_guard lock(runtime_state->mutex);
                    snapshot.match_revision = runtime_state->status.revision;
                }
                for (Peer& peer : peers) {
                    if (!peer.active) continue;
                    nf::net::Snapshot recipient_snapshot = snapshot;
                    recipient_snapshot.ack_input_tick =
                        *std::min_element(peer.last_input_tick.begin(),
                                          peer.last_input_tick.begin() + peer.local_players);
                    for (std::size_t local = 0; local < peer.local_players; ++local) {
                        const std::uint8_t slot = std::uint8_t(peer.slot + local);
                        const nf::Player* owner = world.player(slot);
                        if (!owner) continue;
                        auto owner_record = std::find_if(
                            recipient_snapshot.players.begin(), recipient_snapshot.players.end(),
                            [slot](const nf::net::PlayerSnapshot& player) {
                                return player.slot == slot && player.present && !player.bot;
                            });
                        if (owner_record != recipient_snapshot.players.end())
                            owner_record->owner_movement = owner_movement_state(owner->prediction_state());
                    }
                    if (visibility_culling) {
                        for (nf::net::PlayerSnapshot& target : recipient_snapshot.players) {
                            const bool owner_target =
                                target.slot >= peer.slot && target.slot < peer.slot + peer.local_players;
                            if (!target.present || !target.alive || owner_target) continue;
                            nf::Vec3 position{target.x, target.y, target.z};
                            if (!target.bot) {
                                if (const nf::Player* target_player = world.player(target.slot)) position = target_player->eye();
                            } else if (bots) {
                                if (auto* bot = bots->bots().bot_at_slot(target.slot); bot && bot->drone)
                                    position = nf::drone::head_pos(*bot->drone);
                            }
                            bool visible = false;
                            for (std::size_t local = 0; local < peer.local_players && !visible; ++local) {
                                const nf::Player* observer = world.player(peer.slot + std::uint8_t(local));
                                if (!observer) continue;
                                const nf::Vec3 eye = observer->eye();
                                const float dx = position[0] - eye[0], dy = position[1] - eye[1],
                                            dz = position[2] - eye[2];
                                const bool within_sound_radius = dx * dx + dy * dy + dz * dz <= 50.0f * 50.0f;
                                visible = within_sound_radius || world.collision().line_of_sight(eye, position);
                            }
                            if (visible) last_visible_tick[peer.slot][target.slot] = tick;
                            target.visible = visible ||
                                             (last_visible_tick[peer.slot][target.slot] != 0 &&
                                              tick - last_visible_tick[peer.slot][target.slot] <=
                                                  std::uint32_t(logic_hz * 11 / 30));
                            if (!target.visible) {
                                target.x = target.y = target.z = target.yaw = target.pitch = 0.0f;
                                target.velocity = {};
                                target.health = target.armor = 0.0f;
                                target.substate = target.weapon = target.weapon_anim = target.character = 0;
                                target.weapon_clip = target.weapon_ammo = 0;
                                target.aiming = false;
                            }
                        }
                    }
                    for (const nf::net::Snapshot& page : nf::net::split_snapshot(recipient_snapshot)) {
                        nf::net::Packet packet;
                        packet.header.message = nf::net::Message::Snapshot;
                        packet.payload = nf::net::encode_snapshot(page);
                        if (!packet.payload.empty())
                            socket.send(peer.host, peer.port,
                                        peer.reliability.prepare(std::move(packet), false, Clock::now()));
                    }
                }
                const auto world_payload = nf::net::encode_world_state(make_world_state(tick, session.arena()));
                const auto projectile_pages = nf::net::split_projectiles(tick, make_projectiles(session.weapons()));
                const auto events = make_events(tick, session);
                std::vector<std::vector<std::uint8_t>> projectile_payloads;
                projectile_payloads.reserve(projectile_pages.size());
                for (const nf::net::ProjectilePage& page : projectile_pages)
                    projectile_payloads.push_back(nf::net::encode_projectile_page(page));
                for (Peer& peer : peers) {
                    if (!peer.active) continue;
                    if (!world_payload.empty()) {
                        nf::net::Packet world_packet;
                        world_packet.header.message = nf::net::Message::WorldState;
                        world_packet.payload = world_payload;
                        socket.send(peer.host, peer.port,
                                    peer.reliability.prepare(std::move(world_packet), false, Clock::now()));
                    }
                    for (const auto& payload : projectile_payloads) {
                        if (payload.empty()) continue;
                        nf::net::Packet projectile_packet;
                        projectile_packet.header.message = nf::net::Message::Projectiles;
                        projectile_packet.payload = payload;
                        socket.send(peer.host, peer.port,
                                    peer.reliability.prepare(std::move(projectile_packet), false, Clock::now()));
                    }
                    for (const nf::net::ReplicationEvent& event : events) {
                        if (event.kind == nf::net::EventKind::Sound &&
                            ((event.actor >= 0 && event.actor != peer.slot) || event.target == peer.slot))
                            continue;
                        if (event.kind == nf::net::EventKind::Message &&
                            event.actor >= 0 && event.actor != peer.slot)
                            continue;
                        nf::net::Packet event_packet;
                        event_packet.header.message = nf::net::Message::Event;
                        event_packet.payload = nf::net::encode_event(event);
                        if (!event_packet.payload.empty())
                            socket.send(peer.host, peer.port,
                                        peer.reliability.prepare(std::move(event_packet), true, Clock::now()));
                    }
                }
            }
            next_tick += std::chrono::nanoseconds(1000000000 / logic_hz);
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "nfserver: %s\n", e.what());
        if (runtime_state) {
            std::lock_guard lock(runtime_state->mutex);
            runtime_state->error = e.what();
            runtime_state->status.running = false;
            runtime_state->starting = false;
            runtime_state->startup_complete = true;
            runtime_state->condition.notify_all();
        }
        return 1;
    }
}

namespace nf::net {

struct ServerRuntime::Impl {
    explicit Impl(ServerConfig cfg) : config(std::move(cfg)), state(std::make_shared<RuntimeState>()) {
        state->status.map = config.map;
        state->status.mode = config.match.mode;
    }

    void run(std::stop_token stop) {
        MatchConfig current{config.map, config.match};
        std::size_t rotation_index = 0;
        for (;;) {
            if (stop.stop_requested()) break;
            ServerConfig match_config = config;
            match_config.map = current.map;
            match_config.match = current.match;
            const int result = run_server_impl(0, nullptr, stop, &match_config, state.get());
            if (stop.stop_requested() || result != 2) break;

            {
                std::lock_guard lock(state->mutex);
                if (!state->queued_matches.empty()) {
                    current = std::move(state->queued_matches.front());
                    state->queued_matches.pop_front();
                } else if (!config.rotation.empty()) {
                    const MatchConfig& selected = config.rotation[rotation_index++ % config.rotation.size()];
                    current = selected;
                }
                state->status.running = false;
                state->starting = true;
                ++state->status.match_index;
                ++state->status.revision;
                state->status.map = current.map;
                state->status.mode = current.match.mode;
                state->status.match_over = false;
                state->condition.notify_all();
            }
        }
        std::lock_guard lock(state->mutex);
        state->status.running = false;
        state->starting = false;
        state->condition.notify_all();
    }

    ServerConfig config;
    std::shared_ptr<RuntimeState> state;
    std::jthread worker;
};

ServerRuntime::ServerRuntime(ServerConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}
ServerRuntime::~ServerRuntime() { stop(); }
ServerRuntime::ServerRuntime(ServerRuntime&&) noexcept = default;
ServerRuntime& ServerRuntime::operator=(ServerRuntime&&) noexcept = default;

bool ServerRuntime::start(std::string* error_out) {
    if (!impl_) {
        if (error_out) *error_out = "server runtime has been moved from";
        return false;
    }
    {
        std::lock_guard lock(impl_->state->mutex);
        if (impl_->state->status.running || impl_->state->starting) {
            if (error_out) *error_out = "server runtime is already running";
            return false;
        }
        impl_->state->starting = true;
    }
    if (impl_->worker.joinable()) impl_->worker.join();
    if (impl_->config.data_dir.empty() || impl_->config.map.empty() || impl_->config.port == 0) {
        const std::string why = "server data directory, map, and UDP port are required";
        if (error_out) *error_out = why;
        std::lock_guard lock(impl_->state->mutex);
        impl_->state->error = why;
        impl_->state->starting = false;
        impl_->state->condition.notify_all();
        return false;
    }
    {
        std::lock_guard lock(impl_->state->mutex);
        impl_->state->error.clear();
        impl_->state->startup_complete = false;
        impl_->state->status.running = false;
        impl_->state->status.match_over = false;
        impl_->state->status.match_index = 0;
        impl_->state->status.revision = 0;
        impl_->state->status.map = impl_->config.map;
        impl_->state->queued_matches.clear();
        impl_->state->status.mode = impl_->config.match.mode;
    }
    impl_->worker = std::jthread([runtime = impl_.get()](std::stop_token stop) { runtime->run(stop); });
    std::unique_lock lock(impl_->state->mutex);
    const bool started = impl_->state->condition.wait_for(lock, std::chrono::seconds(60), [&] {
        return impl_->state->startup_complete || !impl_->state->error.empty();
    });
    if (!started || !impl_->state->status.running) {
        const std::string why = !impl_->state->error.empty()
                                    ? impl_->state->error
                                    : "timed out while starting the authoritative server";
        lock.unlock();
        impl_->worker.request_stop();
        impl_->worker.join();
        if (error_out) *error_out = why;
        return false;
    }
    return true;
}

void ServerRuntime::stop() {
    if (impl_ && impl_->worker.joinable()) {
        impl_->worker.request_stop();
        impl_->worker.join();
    }
}

bool ServerRuntime::running() const {
    if (!impl_) return false;
    std::lock_guard lock(impl_->state->mutex);
    return impl_->state->status.running;
}

std::string ServerRuntime::error() const {
    if (!impl_) return "server runtime has been moved from";
    std::lock_guard lock(impl_->state->mutex);
    return impl_->state->error;
}

ServerStatus ServerRuntime::current_match() const {
    if (!impl_) return {};
    std::lock_guard lock(impl_->state->mutex);
    return impl_->state->status;
}

bool ServerRuntime::enqueue_match(MatchConfig match) {
    if (!impl_ || match.map.empty()) return false;
    match.match.enabled = true;
    std::lock_guard lock(impl_->state->mutex);
    if (!impl_->state->status.running) return false;
    impl_->state->queued_matches.push_back(std::move(match));
    return true;
}

}  // namespace nf::net

#ifndef NF_SERVER_EMBEDDED
int main(int argc, char** argv) {
    return run_server_impl(argc, argv);
}
#endif

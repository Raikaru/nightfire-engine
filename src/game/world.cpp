#include "game/world.hpp"
#include "core/rng.hpp"

#include <cstdlib>

namespace nf {

namespace {

constexpr std::uint32_t kEntityPlayerStart = 0x2D, kEntityPlayerStartAlt = 0x24, kEntityMpSpawn = 0x25;
constexpr std::uint32_t kHiddenFlags = 0xA000;  // parsemap_block_map_data_dynamic skips these

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

}  // namespace

std::vector<SpawnPoint> find_spawn_points(const Level& level) {
    std::vector<SpawnPoint> single, multi;
    const ChunkFile* map = level.map();
    if (!map) return {};
    const MapChunk& chunk = map->chunk;
    for (const StaticInstance& s : chunk.statics) {
        if (s.flags & kHiddenFlags) continue;
        SpawnPoint::Kind kind;
        if (s.flags == kEntityPlayerStart || s.flags == kEntityPlayerStartAlt) kind = SpawnPoint::Kind::SinglePlayer;
        else if (s.flags == kEntityMpSpawn) kind = SpawnPoint::Kind::Multiplayer;
        else continue;
        std::string model;
        if (s.hash == -1 && s.model_index < chunk.models.size()) model = chunk.models[s.model_index].name;
        SpawnPoint p{kind, {s.position[0], s.position[1], s.position[2]}, s.euler[1], model, s.param(3)};
        (kind == SpawnPoint::Kind::SinglePlayer ? single : multi).push_back(std::move(p));
    }
    single.insert(single.end(), multi.begin(), multi.end());
    return single;
}

PlayerParams player_params_from_tuning(std::string_view text, std::string_view section) {
    PlayerParams params;
    const std::string_view whole = text;
    bool global = false;
    while (!text.empty()) {
        const auto eol = text.find('\n');
        std::string_view line = trim(text.substr(0, eol));
        text = eol == std::string_view::npos ? std::string_view{} : text.substr(eol + 1);
        if (line.empty() || line.front() == '#') continue;
        if (line.front() == '[') {
            global = line == "[GLOBAL]";
            continue;
        }
        if (!global) continue;
        const auto eq = line.find('=');
        if (eq == std::string_view::npos) continue;
        const std::string_view key = trim(line.substr(0, eq));
        const std::string value(trim(line.substr(eq + 1)));
        const float f = std::strtof(value.c_str(), nullptr);
        if (key == "Plr_NoAimTurnSpeed_X") params.look.turn.speed = f;
        else if (key == "Plr_NoAimTurnSpeed_X_Mul") params.look.turn.mul = f;
        else if (key == "Plr_NoAimTurnSpeed_X_Steps") params.look.turn.steps = f;
        else if (key == "Plr_NoAimTurnSpeed_Y") params.look.pitch.speed = f;
        else if (key == "Plr_NoAimTurnSpeed_Y_Mul") params.look.pitch.mul = f;
        else if (key == "Plr_NoAimTurnSpeed_Y_Steps") params.look.pitch.steps = f;
        else if (key == "Plr_AimSpeed_X") params.look.aim_speed_x = f;
        else if (key == "Plr_AimSpeed_Y") params.look.aim_speed_y = f;
        else if (key == "Plr_AimTurnSpeed_X") params.look.aim_turn_x = f;
        else if (key == "Plr_AimTurnSpeed_Y") params.look.aim_turn_y = f;
        else if (key == "Plr_ScopeSpeed_X") params.look.scope_x.speed = f;
        else if (key == "Plr_ScopeSpeed_X_Mul") params.look.scope_x.mul = f;
        else if (key == "Plr_ScopeSpeed_X_Steps") params.look.scope_x.steps = f;
        else if (key == "Plr_ScopeSpeed_Y") params.look.scope_y.speed = f;
        else if (key == "Plr_ScopeSpeed_Y_Mul") params.look.scope_y.mul = f;
        else if (key == "Plr_ScopeSpeed_Y_Steps") params.look.scope_y.steps = f;
        else if (key == "ContinueHealthBoostEasy") params.health.continue_boost[0] = f;
        else if (key == "ContinueHealthBoostMedium") params.health.continue_boost[1] = f;
        else if (key == "ContinueHealthBoostHard") params.health.continue_boost[2] = f;
    }
    params.health.damage.load(whole, section);
    if (!section.empty()) params.health.damage.mode = section == "MULTIPLAYER" ? GameMode::Multiplayer : GameMode::SinglePlayer;
    return params;
}

World::World(Level& level, InputTables tables, PlayerParams params)
    : level_(level), collision_(level), rope_world_(level), objects_(level), rooms_(level, collision_), tables_(tables),
      params_(params) {}

Player& World::spawn_player(int index, const SpawnPoint& at) {
    auto& slot = players_.at(std::size_t(index));
    slot = std::make_unique<Player>(at.position, at.yaw, params_);
    slot->set_rope_world(&rope_world_);
    slot->set_object_world(&objects_);
    slot->rooms = &rooms_;
    // Player_Init: a single-player marker's parameter 3 picks the substate the player starts in.
    SubState start = SubState::Walk;
    if (params_.health.damage.mode == GameMode::SinglePlayer) {
        if (at.start_type == 1) start = SubState::Swim;
        else if (at.start_type == 2) start = SubState::ZeroG;
        else if (at.start_type == 3) start = SubState::Crouch;
    }
    slot->stand_at(at.position, at.yaw, collision_, start);
    if (start == SubState::Crouch) slot->crouch_dip = 0.45f;   // BL+0x90C: the eye starts lowered
    return *slot;
}

void World::tick(const PadInputs& pads, FrameTiming timing) {
    ++frame_;
    ++timer_frame_;
    // GameFlow_Main advances the source timer before dispatching frame updates.
    game_rng().set_trace_frame(frame_);
    // Env_Update draws Rand_Rand(20000) once for a live world before Player_Update.
    (void)game_rng().rand_int(20000);
    for (int i = 0; i < kMaxPlayers; ++i)
        inputs_[std::size_t(i)].update(pads[std::size_t(i)], tables_, settings_[std::size_t(i)], i);
    for (auto& s : systems_) s->before_player_update(*this, timing);
    const bool multiplayer = params_.health.damage.mode == GameMode::Multiplayer;
    if (!multiplayer) {
        for (auto& p : players_)
            if (p) p->update_camera(timing);
        for (auto& s : systems_) s->tick(*this, timing);
        for (auto& s : systems_) s->after_tick(*this, timing);
        for (auto& s : systems_) s->after_camera_update(*this, timing);
        return;
    }

    // Mission_Update advances players before Game_Run enters MP_Update.
    for (int i = 0; i < kMaxPlayers; ++i)
        if (auto& p = players_[std::size_t(i)])
            p->update(inputs_[std::size_t(i)], settings_[std::size_t(i)], collision_, timing);
    for (auto& p : players_)
        if (p) p->resolve_collisions(collision_);
    for (auto& s : systems_) s->after_player_update(*this, timing);

    // Game_Run calls MP_Update before control_movement_object_handler.
    for (auto& s : systems_)
        if (s->multiplayer_phase() == MultiplayerPhase::MpUpdate) s->tick(*this, timing);
    for (auto& s : systems_) s->before_object_update(*this, timing);
    for (auto& s : systems_)
        if (s->multiplayer_phase() == MultiplayerPhase::ObjectControl) s->tick(*this, timing);
    for (auto& s : systems_) s->after_tick(*this, timing);
    for (auto& p : players_)
        if (p) p->update_camera(timing);
    for (auto& s : systems_) s->after_camera_update(*this, timing);
}

void World::replay_player(int index, const ActionInput& input, FrameTiming timing) {
    Player* p = players_[std::size_t(index)].get();
    if (!p) return;
    inputs_[std::size_t(index)] = input;
    p->update(inputs_[std::size_t(index)], settings_[std::size_t(index)], collision_, timing);
    p->resolve_collisions(collision_);
    // Live tick runs update_camera after resolve: re-simulation must consume the identical
    // RNG draws (active camera shake) and keep the same eye state, or the shared stream desyncs.
    p->update_camera(timing);
}

void World::camera_shake(const Vec3& pos, float radius) {
    for (auto& p : players_)
        if (p) p->camera_shake(pos, radius);
}

}  // namespace nf

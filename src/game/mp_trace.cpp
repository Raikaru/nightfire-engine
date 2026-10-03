#include "game/mp_trace.hpp"

#include "core/rng.hpp"
#include "game/arena.hpp"
#include "game/bot_brain.hpp"
#include "game/bot_system.hpp"
#include "game/drone.hpp"
#include "game/player.hpp"
#include "game/weapons.hpp"
#include "game/world.hpp"

namespace {

constexpr std::size_t kOracleWeaponSlotCount = 0x55;
constexpr std::size_t kOracleAmmoPoolCount = 0x21;
const char* rng_call_name(nf::GameRngCall call) {
    switch (call) {
        case nf::GameRngCall::Random: return "Random";
        case nf::GameRngCall::RandInt: return "RandInt";
        case nf::GameRngCall::FRand: return "FRand";
        case nf::GameRngCall::MVar2: return "MVar2";
    }
    return "Unknown";
}

void write_json_string(std::FILE* out, const char* text) {
    std::fputc('"', out);
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p; ++p) {
        switch (*p) {
            case '"': std::fputs("\\\"", out); break;
            case '\\': std::fputs("\\\\", out); break;
            case '\b': std::fputs("\\b", out); break;
            case '\f': std::fputs("\\f", out); break;
            case '\n': std::fputs("\\n", out); break;
            case '\r': std::fputs("\\r", out); break;
            case '\t': std::fputs("\\t", out); break;
            default:
                if (*p < 0x20) std::fprintf(out, "\\u%04x", unsigned(*p));
                else std::fputc(*p, out);
                break;
        }
    }
    std::fputc('"', out);
}

}  // namespace

namespace nf {

bool MpTraceSink::open(const std::string& path) {
    if (out_) close();
    out_ = std::fopen(path.c_str(), "w");
    if (!out_) return false;
    rng_trace_.reset();
    rng_trace_.set_enabled(true);
    rng_emitted_sequence_ = 0;
    traced_rng_ = &game_rng();
    traced_rng_->attach_trace(&rng_trace_);
    return true;
}

void MpTraceSink::close() {
    if (traced_rng_) traced_rng_->attach_trace(nullptr);
    traced_rng_ = nullptr;
    rng_trace_.set_enabled(false);
    if (out_) std::fclose(out_);
    out_ = nullptr;
}

void MpTraceSink::dump(const World& world, const ArenaSystem& arena, const WeaponSystem& weapons,
                       const PadInputs& pads, bots::BotSystem* bots) {
    if (!out_) return;
    const auto team_score = arena.team_score();
    const ArenaSettings& settings = arena.settings();
    std::fprintf(out_, "{\"frame\":%llu,\"timer_frame\":%llu,\"elapsed\":%.3f,\"total_elapsed\":%.3f,\"time_limit\":%.3f,"
                       "\"time_left\":%.3f,\"mode\":%u,\"map\":%u,\"score_limit\":%d,\"weapon_set\":%d,"
                       "\"state\":%d,\"state_code\":%d,\"teams\":[%.1f,%.1f],"
                       "\"assassin\":%d,\"target\":%d,\"golden_effect_ticks\":%.3f,\"golden_target\":%d,"
                       "\"rng\":[%u,%u]",
                 static_cast<unsigned long long>(world.frame()),
                 static_cast<unsigned long long>(world.timer_frame()), arena.elapsed(), arena.total_elapsed(),
                 settings.time_limit, arena.time_left(), settings.mode, settings.level_id, settings.score_limit,
                 settings.weapon_set, int(arena.phase()), arena.state_code(), team_score[0], team_score[1],
                 arena.assassin().value_or(-1), arena.target().value_or(-1), arena.golden_effect_ticks(),
                 arena.golden_target(), game_rng().seed_x(), game_rng().seed_y());
    std::fprintf(out_, ",\"pad_all\":[");
    for (int s = 0; s < 4; ++s) {
        const ActionInput& input = world.input(s);
        std::fprintf(out_, "%s{\"port\":%d,\"w\":%u,\"s\":[%u,%u,%u,%u],\"act\":[",
                     s ? "," : "", s, sony_pad_word(pads[std::size_t(s)].buttons),
                     pads[std::size_t(s)].rx, pads[std::size_t(s)].ry,
                     pads[std::size_t(s)].lx, pads[std::size_t(s)].ly);
        for (int i = 0; i < kActionCount; ++i)
            std::fprintf(out_, "%s%.6f", i ? "," : "", input.values()[std::size_t(i)]);
        std::fprintf(out_, "],\"flg\":[");
        for (int i = 0; i < kActionCount; ++i)
            std::fprintf(out_, "%s%u", i ? "," : "", input.flags()[std::size_t(i)]);
        std::fprintf(out_, "]}");
    }
    std::fprintf(out_, "]");
    std::fprintf(out_, ",\"pl\":[");
    for (int s = 0; s < 8; ++s) {
        bots::BotSystem::Bot* bot = s >= 4 && bots ? bots->bot_at_slot(s) : nullptr;
        const drone::Drone* botdrone = bot ? bot->drone : nullptr;
        const bool out = arena.participant_out(s);
        const unsigned type = s >= 4 ? (out ? 0x11u : 2u) : (out ? 0x12u : 3u);
        const Player* p = s < 4 ? world.player(s) : nullptr;
        if (!p && !botdrone) {
            std::fprintf(out_, "%snull", s ? "," : "");
            continue;
        }
        if (botdrone) {
            int active_goal = -1;
            int goal_type = -1;
            int goal_kind = -1;
            int goal_target = -1;
            if (bot && bot->brain) {
                active_goal = bot->brain->active_goal();
                if (active_goal >= 0 && active_goal < int(bot->brain->v.goal.size())) {
                    const bots::BotGoal& goal = bot->brain->v.goal[std::size_t(active_goal)];
                    goal_type = goal.type;
                    goal_kind = goal.kind;
                    goal_target = goal.target;
                }
            }
            std::fprintf(out_, "%s{\"pos\":[%.4f,%.4f,%.4f],\"yaw\":%.6f,\"type\":%u,\"hp\":%.3f,\"arm\":0.0,"
                               "\"weap\":%d,\"alive\":%s,\"mp_status\":%u,\"dead\":%s,\"out\":%s,"
                               "\"state\":%d,\"active_goal\":%d,\"goal_type\":%d,\"goal_kind\":%d,"
                               "\"goal_target\":%d,\"vel\":[%.5f,%.5f,%.5f],\"fall_vel\":[%.5f,%.5f,%.5f]}",
                         s ? "," : "", botdrone->pos[0], botdrone->pos[1], botdrone->pos[2], botdrone->yaw,
                         type, botdrone->health, botdrone->weapon, botdrone->health > 0.0f ? "true" : "false",
                         static_cast<unsigned>(arena.status(s)), arena.dead(s) ? "true" : "false",
                         out ? "true" : "false", botdrone->state(), active_goal, goal_type, goal_kind, goal_target,
                         botdrone->velocity[0], botdrone->velocity[1], botdrone->velocity[2],
                         botdrone->fall_velocity[0], botdrone->fall_velocity[1], botdrone->fall_velocity[2]);
        } else {
            const PlayerWeapons* state = weapons.state(s);
            std::fprintf(out_, "%s{\"pos\":[%.4f,%.4f,%.4f],\"yaw\":%.6f,\"type\":%u,\"state\":%d,"
                               "\"hp\":%.3f,\"arm\":%.3f,\"weap\":%d,\"alive\":%s,\"mp_status\":%u,"
                               "\"dead\":%s,\"out\":%s,\"foot\":%.4f,\"pitch\":%.5f,\"substate\":%d,"
                               "\"vel\":[%.5f,%.5f,%.5f],\"fall_vel\":[%.5f,%.5f,%.5f],"
                               "\"zoom\":%.4f,\"aim\":%s,\"lock_victim\":%d,\"lock_yaw\":%.6f,"
                               "\"lock_pitch\":%.6f,\"ammo_pool\":[",
                         s ? "," : "", p->pos[0], p->pos[1], p->pos[2], p->yaw, type, p->alive() ? 1 : 0,
                         p->health(), p->armor(), weapons.current_weapon(s), p->alive() ? "true" : "false",
                         static_cast<unsigned>(arena.status(s)), arena.dead(s) ? "true" : "false",
                         out ? "true" : "false", p->stand_height, p->pitch, int(p->substate),
                         p->velocity[0], p->velocity[1], p->velocity[2],
                         p->fall_velocity[0], p->fall_velocity[1], p->fall_velocity[2],
                         state ? state->zoom : 1.0f, state && state->aim ? "true" : "false",
                         state ? state->lock_victim : -1, state ? state->lock_yaw : 0.0f,
                         state ? state->lock_pitch : 0.0f);
            if (state) {
                for (std::size_t i = 0; i < kOracleAmmoPoolCount && i < state->pool.size(); ++i)
                    std::fprintf(out_, "%s%u", i ? "," : "", state->pool[i]);
            }
            std::fprintf(out_, "],\"weapon_slots\":[");
            if (state) {
                const std::size_t count = kOracleWeaponSlotCount < state->weapon.size()
                                              ? kOracleWeaponSlotCount
                                              : state->weapon.size();
                for (std::size_t i = 0; i < count; ++i) {
                    const auto& slot = state->weapon[i];
                    std::fprintf(out_, "%s{\"clip\":%d,\"owned\":%s,\"mode\":%u,\"upgrade\":%d,\"zoom\":%.4f}",
                                 i ? "," : "", slot.clip, slot.owned ? "true" : "false",
                                 unsigned(slot.mode_index), int(slot.upgrade_off), slot.saved_zoom);
                }
            }
            std::fprintf(out_, "],\"weapon_timers\":{\"fire_cooldown\":%.4f,\"last_gun\":%d,"
                               "\"last_gadget\":%d,\"trigger_remaining\":%d,\"muzzle_timer\":%d,"
                               "\"weapon_anim\":%u}}",
                         state ? state->cooldown : 0.0f, state ? state->last_gun : 0,
                         state ? state->last_gadget : 0, state ? state->shots_left : 0,
                         state ? state->muzzle_frames : 0, state ? unsigned(state->anim_state) : 0u);
        }
    }
    std::fprintf(out_, "],\"respawns\":[");
    for (int s = 0; s < 8; ++s)
        std::fprintf(out_, "%s%.3f", s ? "," : "", arena.respawn_in(s));
    std::fprintf(out_, "],\"bots\":[");
    for (int k = 0; k < 4; ++k) {
        bots::BotSystem::Bot* b = bots ? bots->bot_at_slot(4 + k) : nullptr;
        const drone::Drone* d = b ? b->drone : nullptr;
        if (!d) {
            std::fprintf(out_, "%snull", k ? "," : "");
            continue;
        }
        int goal_kind = -1;
        if (const bots::BotBrain* brain = b->brain) {
            const int g = brain->active_goal();
            if (g >= 0 && g < 2) goal_kind = brain->v.goal[std::size_t(g)].kind;
        }
        std::fprintf(out_, "%s{\"pos\":[%.4f,%.4f,%.4f],\"yaw\":%.6f,\"state\":%d,\"hp\":%.3f,\"goal\":%d}",
                     k ? "," : "", d->pos[0], d->pos[1], d->pos[2], d->yaw, d->state(), d->health, goal_kind);
    }
    std::fprintf(out_, "],\"scores\":[");
    bool first = true;
    for (const ScoreRow& r : arena.scoreboard()) {
        std::fprintf(out_, "%s{\"slot\":%d,\"k\":%d,\"d\":%d,\"p\":%.1f}", first ? "" : ",", r.slot, r.kills,
                     r.deaths, r.points);
        first = false;
    }
    std::fprintf(out_, "],\"pk\":[");
    first = true;
    const auto& all = arena.pickups().all();
    for (std::size_t i = 0; i < all.size(); ++i) {
        const Pickup& pickup = all[i];
        if (pickup.state == Pickup::State::Gone) continue;
        const std::uint64_t age = world.frame() >= pickup.stamp ? world.frame() - pickup.stamp : 0;
        const std::uint32_t lifetime_left =
            pickup.dynamic && age < pickup.lifetime_total_frames ? pickup.lifetime_total_frames - std::uint32_t(age) : 0;
        std::fprintf(out_, "%s{\"idx\":%zu,\"st\":%d,\"cat\":%d,\"item\":%d,\"units\":%d,"
                           "\"remaining_s\":%.3f,\"stamp\":%llu,\"lifetime_frames\":%u,\"pos\":[%.2f,%.2f,%.2f]",
                     first ? "" : ",", i, int(pickup.state), pickup.category, pickup.item, pickup.respawn_units,
                     arena.pickup_respawn_left(pickup), static_cast<unsigned long long>(pickup.stamp), lifetime_left,
                     pickup.pos[0], pickup.pos[1], pickup.pos[2]);
        if (pickup.dynamic)
            std::fprintf(out_, ",\"amount\":%d,\"radar_hidden\":%s", pickup.amount,
                         pickup.radar_hidden ? "true" : "false");
        std::fprintf(out_, ",\"visit_until\":[");
        for (std::size_t k = 0; k < kMpPs2Slots - kMpMaxLocalHumans; ++k)
            std::fprintf(out_, "%s%.6f", k ? "," : "", pickup.visit_until[k]);
        std::fprintf(out_, "]");
        std::fputc('}', out_);
        first = false;
    }
    std::fprintf(out_, "],\"objs\":[");
    const auto& objectives = arena.objectives();
    for (std::size_t i = 0; i < objectives.size(); ++i) {
        const MpObjective& objective = objectives[i];
        std::fprintf(out_, "%s{\"idx\":%zu,\"kind\":%d,\"team\":%d,\"state\":%d,\"carrier\":%d,"
                           "\"hp\":%.1f,\"visible\":%s,\"timer\":%d,\"last_damager\":%d,"
                           "\"capturer\":%d,\"round_over\":%s,\"pos\":[%.6f,%.6f,%.6f]}",
                     i ? "," : "", i, int(objective.kind), objective.team, objective.state, objective.carrier,
                     objective.hit_points, objective.visible ? "true" : "false", arena.objective_timer(i),
                     arena.objective_last_damager(i), arena.objective_capturer(i),
                     arena.objective_round_over(i) ? "true" : "false", objective.pos[0], objective.pos[1],
                     objective.pos[2]);
    }
    std::fprintf(out_, "],\"projectiles_available\":true,\"projectiles\":[");
    const auto& projectiles = weapons.projectiles();
    for (std::size_t i = 0; i < projectiles.size(); ++i) {
        const Projectile& p = projectiles[i];
        std::fprintf(out_, "%s{\"id\":%zu,\"weapon\":%d,\"owner\":%d,\"pos\":[%.4f,%.4f,%.4f],"
                           "\"dir\":[%.6f,%.6f,%.6f],\"speed\":%.5f,\"travelled\":%.5f,"
                           "\"timer\":%.4f,\"state\":%u,\"bounces\":%u,\"delete_me\":%s,"
                           "\"resting\":%s,\"stuck_normal\":[%.5f,%.5f,%.5f],"
                           "\"damage_scale\":%.4f,\"age\":%.4f}",
                     i ? "," : "", i, p.weapon, p.owner, p.pos[0], p.pos[1], p.pos[2], p.dir[0], p.dir[1],
                     p.dir[2], p.speed, p.travelled, p.timer, unsigned(p.state), unsigned(p.bounces),
                     p.delete_me ? "true" : "false", p.resting ? "true" : "false",
                     p.stuck_normal[0], p.stuck_normal[1], p.stuck_normal[2], p.damage_scale, p.age);
    }
    const std::uint64_t call_count = rng_trace_.count();
    const std::uint64_t first_call = rng_trace_.first_sequence();
    const std::uint64_t first_emitted = rng_emitted_sequence_ > first_call ? rng_emitted_sequence_ : first_call;
    const std::uint64_t dropped = first_emitted - rng_emitted_sequence_;
    std::fprintf(out_, "],\"rng_call_count\":%llu,\"rng_call_dropped\":%llu,\"rng_calls\":[",
                 static_cast<unsigned long long>(call_count - first_emitted),
                 static_cast<unsigned long long>(dropped));
    for (std::uint64_t seq = first_emitted; seq < call_count; ++seq) {
        const GameRngTraceRecord& call = rng_trace_.at(seq);
        std::fprintf(out_, "%s{\"sequence\":%llu,\"frame\":%llu,\"call\":",
                     seq == first_emitted ? "" : ",", static_cast<unsigned long long>(seq),
                     static_cast<unsigned long long>(call.frame));
        write_json_string(out_, rng_call_name(call.call));
        std::fputs(",\"file\":", out_);
        write_json_string(out_, call.file);
        std::fputs(",\"function\":", out_);
        write_json_string(out_, call.function);
        std::fprintf(out_, ",\"line\":%u,\"result_bits\":%u}", call.line, call.result_bits);
    }
    rng_emitted_sequence_ = call_count;
    std::fprintf(out_, "]}\n");
}

}  // namespace nf

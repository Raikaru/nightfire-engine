#include "game/mp_trace.hpp"

#include "core/rng.hpp"
#include "game/arena.hpp"
#include "game/bot_brain.hpp"
#include "game/bot_system.hpp"
#include "game/drone.hpp"
#include "game/player.hpp"
#include "game/weapons.hpp"
#include "game/world.hpp"

namespace nf {

bool MpTraceSink::open(const std::string& path) {
    out_ = std::fopen(path.c_str(), "w");
    return out_ != nullptr;
}

void MpTraceSink::close() {
    if (out_) std::fclose(out_);
    out_ = nullptr;
}

void MpTraceSink::dump(const World& world, const ArenaSystem& arena, const WeaponSystem& weapons,
                       bots::BotSystem* bots) {
    if (!out_) return;
    std::fprintf(out_, "{\"frame\":%llu,\"elapsed\":%.3f,\"state\":%d,\"teams\":[%.1f,%.1f],"
                       "\"rng\":[%u,%u]",
                 static_cast<unsigned long long>(world.frame()), arena.elapsed(), int(arena.phase()),
                 arena.team_score()[0], arena.team_score()[1], game_rng().seed_x(), game_rng().seed_y());
    std::fprintf(out_, ",\"pl\":[");
    for (int s = 0; s < 8; ++s) {
        const drone::Drone* botdrone = nullptr;
        if (s >= 4 && bots) {
            if (bots::BotSystem::Bot* b = bots->bot_at_slot(s)) botdrone = b->drone;
        }
        const Player* p = s < 4 ? world.player(s) : nullptr;
        if (!p && !botdrone) {
            std::fprintf(out_, "%snull", s ? "," : "");
            continue;
        }
        if (botdrone) {
            std::fprintf(out_, "%s{\"pos\":[%.4f,%.4f,%.4f],\"yaw\":%.6f,\"hp\":%.3f,\"arm\":0.0,"
                               "\"weap\":-1,\"alive\":%s}",
                         s ? "," : "", botdrone->pos[0], botdrone->pos[1], botdrone->pos[2], botdrone->yaw,
                         botdrone->health, botdrone->health > 0.0f ? "true" : "false");
        } else {
            std::fprintf(out_, "%s{\"pos\":[%.4f,%.4f,%.4f],\"yaw\":%.6f,\"hp\":%.3f,\"arm\":%.3f,"
                               "\"weap\":%d,\"alive\":%s}",
                         s ? "," : "", p->pos[0], p->pos[1], p->pos[2], p->yaw, p->health(), p->armor(),
                         weapons.current_weapon(s), p->alive() ? "true" : "false");
        }
    }
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
        if (!all[i].available()) continue;
        std::fprintf(out_, "%s{\"idx\":%zu,\"st\":1}", first ? "" : ",", i);
        first = false;
    }
    std::fprintf(out_, "]}\n");
}

}  // namespace nf

// Civilian / hostage / alarm helpers (docs/spec-arena-ai.md Part 3 §8).
#include "game/sp_civilian_util.hpp"

#include <cmath>

namespace nf::sp {

using namespace nf::drone;

bool check_surrender(const Drone& d) {
    // NDrone2_CheckSurrender 0x14a060 as used by DroneFunc_FirstSightState: behaviour bit 0x43, an
    // opponent, alertness < 0.66, within 2.0 m with the target facing away (|0x1d4| > 2.094 = 120 deg),
    // visible (bone 5), the target armed, and the target facing the drone (BondIsFacingMe, 45 deg).
    if (!d.has_beh(beh::kMaySurrender)) return false;
    if (!d.opponent.valid()) return false;
    if (d.alertness >= 0.66f) return false;
    if (d.opp_dist >= 2.0f) return false;
    if (std::fabs(d.opp_facing_b) <= 2.0943952f) return false;
    if (!can_see_object(const_cast<Drone&>(d), d.opponent, 5)) return false;
    if (!opponent_armed(d)) return false;
    return opponent_facing_me(d, 45.0f);
}

void mission_fail(Drone& d, int reason, std::uint32_t label) {
    // DroneFunc_SetMissionFailReason 0x13e560.
    SpSystem& sp = sp_of(d);
    sp.mission_fail_reason = reason;
    if (sp.on_mission_fail) sp.on_mission_fail(d, reason, label);
}

bool find_alarm_point(Drone& d) {
    // NDrone2_FindAlarmPoint 0x153008: nearest alarm point of the level's AI points.
    const SpLevel& level = sp_of(d).level();
    if (level.ai_points.empty()) return false;
    const Vec3 f = d.feet();
    const AiPointDef* best = nullptr;
    float best_d2 = 1e18f;
    for (const AiPointDef& p : level.ai_points) {
        const float dx = f[0] - p.pos[0], dz = f[2] - p.pos[2];
        const float d2 = dx * dx + dz * dz;
        if (d2 < best_d2) {
            best_d2 = d2;
            best = &p;
        }
    }
    if (!best) return false;
    set_ai_goal(d, best->pos, best->radius);
    return true;
}

int near_armed_drone(Drone& d) {
    // NDrone2_NearArmedDrone: nearest live armed enemy within 30 m.
    SpExt& e = sx(d);
    e.near_drone = -1;
    float best = 30.0f;
    for (Drone* o : sp_of(d).sp_drones()) {
        if (o == &d || !o->alive() || o->weapon == 0) continue;
        if (o->side != drone::kSideEnemy) continue;
        const Vec3 a = d.feet(), b = o->feet();
        const float dist = std::hypot(a[0] - b[0], a[2] - b[2]);
        if (dist < best) {
            best = dist;
            e.near_drone = o->id;
        }
    }
    return e.near_drone;
}

Drone* sp_drone_by_id(Drone& d, int id) {
    if (id < 0) return nullptr;
    for (Drone* o : sp_of(d).sp_drones())
        if (o->id == id && o->alive()) return o;
    return nullptr;
}

void civilian_scare(Drone& d) {
    // The Civilian TICK reaction to gunfire / alerts (NDrone2_DSTATE_Civilian 0x15d118).
    if (d.opponent.valid()) set_angle_to_obj(d, d.opponent, 0.0f, true);
    talk(d, Speech::CivilianScared);
    d.set_state(kStCivilianScared);
}

Drone* hostage_killer_of(Drone& d) {
    return sp_drone_by_id(d, sx(d).partner);
}

void release_waiting(Drone& d) {
    // WaitSwitch resume (NDrone2_DSTATE_WaitSwitch 0x15b960): enable + TruckDriverInit for DTYPE 0x17,
    // else the initial state, or PlayScript when the drone has a script (DIVars+0x38).
    if (d.smi.cur != kStWaitSwitch) return;
    enable_drone(d, true);
    if (d.dtype == 0x17) d.set_state(kStTruckDriverInit);
    else if (d.script_id == 0 || d.script_id == 0x6000000) d.set_state(d.initial_state);
    else d.set_state(kStPlayScript);
}

bool cover_usable(Drone& d, std::size_t index) {
    const SpSystem& sp = sp_of(d);
    const CoverNodeDef& n = sp.level().cover_nodes[index];
    if (n.require_on != 0 && !sp.channels.on(n.require_on)) return false;
    if (n.require_off != 0 && sp.channels.on(n.require_off)) return false;
    for (Drone* o : sp_of(d).sp_drones()) {
        if (o != &d && sx(*o).cover_node == int(index)) return false;
    }
    return true;
}

bool cover_available(Drone& d) {
    // NDrone2_CoverAvailable: needs an opponent and free cover (Drone+0x4f8 & 0x1000 not set).
    if (!d.opponent.valid()) return false;
    if (d.flags & flag::kCoverClaimed) return false;
    const SpLevel& level = sp_of(d).level();
    for (std::size_t i = 0; i < level.cover_nodes.size(); ++i)
        if (cover_usable(d, i)) return true;
    return false;
}

bool find_cover(Drone& d) {
    SpExt& e = sx(d);
    const SpLevel& level = sp_of(d).level();
    const Vec3 f = d.feet();
    int best = -1;
    float best_d2 = 1e18f;
    for (std::size_t i = 0; i < level.cover_nodes.size(); ++i) {
        if (!cover_usable(d, i)) continue;
        const Vec3& p = level.cover_nodes[i].pos;
        const float dx = f[0] - p[0], dz = f[2] - p[2];
        const float d2 = dx * dx + dz * dz;
        if (d2 < best_d2) {
            best_d2 = d2;
            best = int(i);
        }
    }
    if (best < 0) return false;
    e.cover_node = best;
    d.flags |= flag::kCoverClaimed;
    set_ai_goal(d, level.cover_nodes[std::size_t(best)].pos, 1.0f);
    return true;
}

void release_cover(Drone& d) {
    sx(d).cover_node = -1;
    d.flags &= ~(flag::kCoverClaimed | flag::kCoverClaimed2);
}

void notify_death(Drone& d) {
    SpExt* e = sx_or_null(d);
    if (!e || !e->sp || e->death_channel == 0) return;
    e->sp->channels.set(e->death_channel, true);
}

}  // namespace nf::sp

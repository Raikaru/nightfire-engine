#include "game/sp_common.hpp"

namespace nf::sp {

void set_idle_timeout(Drone& d, int min_seconds, int rand_seconds) {
    // NDrone2_SetIdleTimeOut: Drone+0x104 = now + (Rand_Rand(r) + min) * FRAME_RATE_INT
    const std::uint32_t r = rand_seconds > 0 ? d.sys->rand() % std::uint32_t(rand_seconds) : 0;
    d.idle_timeout = d.now() + d.seconds(float(r + std::uint32_t(min_seconds)));
}

bool skel_impact(Drone& d, const Msg& m, int default_state) {
    bool non_punch;
    switch (m.id) {
    case drone::kMsgPunch:
    case drone::kMsgBullet:
        non_punch = false;
        break;
    case drone::kMsgStunElectric:
    case drone::kMsgExplosive:
    case drone::kMsgGas:
    case drone::kMsgStunGrenade:
    case drone::kMsgStunDart:
        non_punch = true;
        break;
    default:
        return false;
    }
    drone::handle_impact(d, m, default_state, non_punch);
    return true;
}

bool skel_alerts(Drone& d, const Msg& m) {
    switch (m.id) {
    case drone::kMsgShoutFirstSight:
    case drone::kMsgShoutHurt:
    case drone::kMsgSoundAlert:
    case drone::kMsgShoutType5:
    case drone::kMsgShoutAttack:
    case drone::kMsgDroneAlert: {
        // DroneVision_EnemyAlerts head: remember where we were (return state 0x5a0, home position 0x5b0)
        if (SpExt* e = sx_or_null(d); e && !e->heard_noise && !e->scared_hiding) {
            e->return_state = std::uint16_t(d.smi.cur);
            e->home_pos = d.feet();
        }
        const int next = drone::enemy_alerts(d, m);   // DroneVision_EnemyAlerts
        if (next != 0) d.set_state(next);
        return true;
    }
    default:
        return false;
    }
}

bool skel_forced_attack(Drone& d, const Msg& m) {
    if (m.id != drone::kMsgForcedAttack) return false;
    do_forced_attack(d);
    return true;
}

bool skel_explosive(Drone& d, const Msg& m, int default_state) {
    if (m.id != drone::kMsgExplosiveNearby) return false;
    consider_explosive(d, m, default_state);
    return true;
}

bool skel_common(Drone& d, const Msg& m, int impact_default_state) {
    return skel_impact(d, m, impact_default_state) || skel_alerts(d, m) || skel_forced_attack(d, m) ||
           skel_explosive(d, m, kStAttack);
}

void talk(Drone& d, Speech what) {
    if (SpExt* e = sx_or_null(d); e && e->sp->speech) e->sp->speech(d, what);
}

void enable_drone(Drone& d, bool enable) {
    // NDrone2_Enable
    if (!enable) {
        d.ghost = true;
        d.hidden = true;
        d.flags &= 0xffe7fe5fu;
    } else if ((d.flags & 0x200) == 0) {
        d.ghost = false;
        d.hidden = false;
        d.flags |= 0x801a0;
    }
}

void set_as_attacking(Drone& d) {
    // DroneFunc_SetAsAttacking 0x147d70
    d.alt_dmode = 0;
    d.flags |= drone::flag::kFirstAttackDone;
    d.alt_channel = 0;
    if (d.first_attack_time == 0) d.first_attack_time = d.now();
    d.flags |= drone::flag::kAlertedByNoise;
    d.alertness = 1.0f;
    const std::uint32_t lvl = d.sys->config().level_id;
    if (lvl >= 0x7000009 && lvl < 0x700000b) return;   // Tower 1 A/B keep their sight
    if (d.sight_range < d.max_combat_dist) d.sight_range = d.max_combat_dist;
    d.sight_cone = 0;
}

bool do_forced_attack(Drone& d) {
    // DroneFunc_DoForcedAttack 0x148ff0
    if (d.sys->config().multiplayer) return false;
    const SpExt* e = sx_or_null(d);
    const std::uint32_t lvl = d.sys->config().level_id;
    if (lvl == kLevelCastleC && e && e->sp->alarm_raised && d.dtype != 0x13 && d.dtype != 0x16) return false;
    if (!d.alive() || (d.flags & drone::flag::kActive) == 0) return false;
    if ((d.flags & 0x2020000) != 0x20000) return false;
    d.flags &= ~std::uint32_t(0x20000);
    d.flags |= 0x24000000;
    d.set_state(kStAttack);
    return true;
}

}  // namespace nf::sp

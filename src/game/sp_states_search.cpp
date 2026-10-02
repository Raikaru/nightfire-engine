// Noise / search family: HeardNoise{Aware,Suspect,Alert}, SearchArea, AlertToPosition, GoToGoalPosition
// (docs/spec-arena-ai.md Part 3 §4 "Awareness reactions" and "Idle / patrol / guard / investigate").
#include <cmath>

#include "game/sp_civilian_util.hpp"
#include "game/sp_common.hpp"
#include "game/sp_idle.hpp"
#include "game/sp_states.hpp"

namespace nf::sp {

using namespace nf::drone;

namespace {

// EnemyAlerts group without the sound message (the HeardNoise* states handle 0x14 themselves).
bool shouts(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgShoutFirstSight:
    case kMsgShoutHurt:
    case kMsgShoutType5:
    case kMsgShoutAttack:
    case kMsgDroneAlert: {
        const int next = enemy_alerts(d, m);
        if (next != 0) d.set_state(next);
        return true;
    }
    default:
        return false;
    }
}

// impacts (default state Attack), shouts, forced attack, explosives: the tail of every switch below.
int skeleton_tail(Drone& d, const Msg& m) {
    return (skel_impact(d, m, kStAttack) || shouts(d, m) || skel_forced_attack(d, m) || skel_explosive(d, m, kStAttack)) ? 1 : 0;
}

// The `TICK: DroneVision_EnemyLookForOpponent -> SetState` step.
bool look_and_switch(Drone& d) {
    const int next = enemy_look_for_opponent(d);
    if (next == 0) return false;
    d.set_state(next);
    return true;
}

// NDrone2_SetSoundAlertRoute 0x1541c0: the AI goal becomes the alert position; the movement route restarts.
void set_sound_alert_route(Drone& d) {
    set_ai_goal(d, d.alert_pos, 2.0f);
    invalidate_attack_route(d);
}

// NDrone2_GetAngleDifferenceToAlert / NDrone2_SetAngleToAlert
float angle_difference_to_alert(const Drone& d) {
    const Vec3 v = d.alert_pos - d.pos;
    return angle_diff(d.yaw, heading_of(v[0], v[2]));
}
void set_angle_to_alert(Drone& d, float offset) {
    if (d.flags & flag::kSawPlayer) return;
    const Vec3 v = d.alert_pos - d.pos;
    d.mv.dest_angle = offset + heading_of(v[0], v[2]);
}

// ---- HeardNoiseAware 0xa3 (0x16af20) ------------------------------------------------------------------------------
int state_heard_noise_aware(Drone& d, const Msg& m) {
    SpExt& e = sx(d);
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        e.heard_noise = true;
        if (e.noise_level < 2) {
            e.noise_level = 2;
            d.flags |= flag::kAware | flag::kAlertableByDroneSight;
            anim_call(d, 0, kStandAlertLook, 1, e.return_state);
            return 1;
        }
        d.set_state(kStHeardNoiseSuspect);
        return 1;
    case kMsgTick:
        look_and_switch(d);
        return 1;
    case kMsgSoundAlert:
        if (d.noise_delta <= 0.1f) return 1;
        d.set_state(kStHeardNoiseSuspect);
        return 1;
    default:
        return skeleton_tail(d, m);
    }
}

// Face the alert position; when the drone follows up (behaviour 0x21) SearchArea becomes its follow-on state.
// Returns true when the caller must still start the AlertToPosition transition.
bool suspect_face_alert(Drone& d) {
    SpExt& e = sx(d);
    e.alert_turned = true;
    set_angle_to_alert(d, 0.0f);
    if (!d.has_beh(0x21)) {
        d.flags |= flag::kAware;
        anim_call(d, 0, kStandAlertLook, 1, e.return_state);
        return false;
    }
    d.initial_state = kStSearchArea;   // Drone+0x5a2 = 0x65: what AlertToPosition returns to
    e.cfg.initial_state = kStSearchArea;
    return true;
}

// ---- HeardNoiseSuspect 0xa4 (0x16b0e0) -----------------------------------------------------------------------------
int state_heard_noise_suspect(Drone& d, const Msg& m) {
    SpExt& e = sx(d);
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter: {
        if (e.noise_level >= 3) {
            d.set_state(kStHeardNoiseAlert);
            return 1;
        }
        e.noise_level = 3;
        drone::alert_status_set(d, AlertStatus::Alert);
        talk(d, Speech::Other);   // DroneFunc_HeardNoiseTalk
        e.heard_noise = true;
        d.flags |= flag::kAlertableByDroneSight;
        if ((d.flags & flag::kStationary) == 0 && !d.has_beh(beh::kNeverMovesInCombat)) {
            set_sound_alert_route(d);
            if (e.alert_turned && d.has_beh(0x21)) {
                d.set_state(kStAlertToPosition);
                return 1;
            }
            d.flags |= flag::kAware;
            if (std::fabs(angle_difference_to_alert(d)) > 2.3561945f) {
                anim_call(d, 0, k180Aim, 0, 0, 0x23);   // turn round, then msg 0x23
                return 1;
            }
            if (suspect_face_alert(d)) anim_call(d, 0, kStandAlertLook, 0, kStAlertToPosition);
            return 1;
        }
        d.flags |= flag::kAware;
        anim_call(d, 0, kStandAlertLook, 1, e.return_state);
        return 1;
    }
    case kMsgTick:
        look_and_switch(d);
        return 1;
    case kMsgSoundAlert:
        if (d.noise_delta > 0.1f) d.flags |= flag::kAlertedByNoise | flag::kShoutSource;
        d.set_state(kStAttack);
        return 1;
    case 0x23:   // turn finished
        if (e.alert_turned && d.has_beh(0x21)) {
            d.set_state(kStAlertToPosition);
            return 1;
        }
        if (suspect_face_alert(d)) {
            if (d.anim.cur_state == kStandAlert) anim_call(d, 0, kAimStandLook, 0, kStAlertToPosition);
            else d.set_state(kStAlertToPosition);
        }
        return 1;
    default:
        return skeleton_tail(d, m);
    }
}

// ---- HeardNoiseAlert 0xa5 (0x16b540) ---------------------------------------------------------------------------------
int state_heard_noise_alert(Drone& d, const Msg& m) {
    SpExt& e = sx(d);
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        e.noise_level = 4;
        drone::alert_status_set(d, AlertStatus::Alert);
        e.heard_noise = true;
        talk(d, Speech::Other);
        d.flags |= flag::kAware | flag::kAlertableByDroneSight;
        if ((d.flags & flag::kStationary) == 0 && !d.has_beh(beh::kNeverMovesInCombat)) {
            d.timer1 = {d.now() + 60, 0};   // FRAME_RATE_DIV * 60 literal frames
            if (std::fabs(d.opp_facing_b) <= 2.3561945f) {
                e.alert_turned = true;
                set_angle_to_obj(d, d.opponent, 0.0f);
                anim_call(d, 0, kAimStandLook, 0, kStAttack);
                return 1;
            }
            anim_call(d, 0, k180Aim, 0, 0, 0x23);
            return 1;
        }
        anim_call(d, 0, kStandAlertLook, 1, kStAttack);
        return 1;
    case kMsgTick:
        look_and_switch(d);
        return 1;
    case kMsgTimer1:
        d.flags |= flag::kAlertedByNoise;
        d.set_state(kStAttack);
        return 1;
    case 0x23:
        e.alert_turned = true;
        set_angle_to_obj(d, d.opponent, 0.0f);
        anim_call(d, 0, kAimStandLook, 0, kStAttack);
        return 1;
    default:
        return skeleton_tail(d, m);
    }
}

// The sound-alert (msg 0x14) reaction of SearchArea / AlertToPosition.
void sound_while_searching(Drone& d, const Msg& m) {
    if (d.noise_delta > 0.2f && (d.flags & flag::kAlertedByNoise) == 0) {
        d.flags |= flag::kAlertedByNoise | flag::kShoutSource;
        d.set_state(kStAttack);
        return;
    }
    const int next = enemy_alerts(d, m);
    if (next != 0) d.set_state(next);
}

// ---- SearchArea 0x65 (0x165bf0) -----------------------------------------------------------------------------------------
void go_back_home(Drone& d) {
    // AINetwork_SetupGoalPosition(2.0, ..., Drone+0x5b0) then GoToGoalPosition
    set_ai_goal(d, sx(d).home_pos, 2.0f);
    invalidate_attack_route(d);
    d.set_state(kStGoToGoalPosition);
}

int state_search_area(Drone& d, const Msg& m) {
    SpExt& e = sx(d);
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.flags |= flag::kAware | flag::kAlertableByDroneSight;
        drone::alert_status_set(d, AlertStatus::Alert);
        anim_call(d, 0, kStandAlertLook);
        d.timer1 = {d.now() + d.seconds(3.0f), 0};
        return 1;
    case kMsgTick:
        look_and_switch(d);
        return 1;
    case kMsgTimer1:
        anim_call(d, 0, k180Aim, 0, 0, 0x24);
        return 1;
    case 0x24:
        anim_call(d, 0, kAimStandLook, 0, 0, 0x25);
        return 1;
    case 0x25:
        drone::alert_status_set(d, AlertStatus::Relaxed);
        d.alertness = 0;
        e.alert_turned = false;
        d.initial_state = e.return_state;
        go_back_home(d);
        return 1;
    case kMsgPatrolObstructed:
        d.initial_state = e.return_state;
        go_back_home(d);
        return 1;
    case kMsgSoundAlert:
        sound_while_searching(d, m);
        return 1;
    default:
        return skeleton_tail(d, m);
    }
}

// ---- AlertToPosition 0x62 (0x165270) ----------------------------------------------------------------------------------------
int state_alert_to_position(Drone& d, const Msg& m) {
    SpExt& e = sx(d);
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        drone::alert_status_set(d, AlertStatus::Alert);
        d.flags |= flag::kAware | flag::kAlertableByDroneSight;
        anim_call(d, 0, kAimWalk);
        d.timer1 = {d.now() + d.seconds(5.0f), 0};
        return 1;
    case kMsgTick: {
        if (d.alertness >= 1.0f) sp_of(d).note_attacker(d);
        if (look_and_switch(d)) return 1;
        // NDrone2_MoveToAlertPosition = NDrone2_MoveToGoalPosition on the AI goal
        const int status = move_to_ai_goal(d);
        switch (status) {
        case 0: case 1: case 2:
            if (d.mv.route_distance >= 10.0f) {
                anim_call(d, 0, kRun);
            } else if (d.anim.cur_state != kWalkAlert && d.anim.cur_state != kAimWalk) {
                anim_call(d, 0, kWalkAlert);
            }
            return 1;
        case 3:
            d.set_state(d.initial_state);
            return 1;
        case 6:
            d.set_state(kStCombatNoRoute);
            return 1;
        case 10:
            return 1;
        default:
            e.heard_noise = false;
            d.set_state(e.return_state);
            return 1;
        }
    }
    case kMsgTimer1:
        if (ai_goal(d).radius + 8.0f <= distance_to_ai_point(d)) anim_call(d, 0, kRun);
        return 1;
    case kMsgSoundAlert:
        sound_while_searching(d, m);
        return 1;
    case kMsgPatrolObstructed:
        d.set_state(d.initial_state);
        return 1;
    default:
        return skeleton_tail(d, m);
    }
}

// ---- GoToGoalPosition 0x63 (0x165640) ----------------------------------------------------------------------------------------
int state_go_to_goal_position(Drone& d, const Msg& m) {
    SpExt& e = sx(d);
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.flags |= flag::kAware | flag::kDeaf;
        return 1;
    case kMsgLeave:
        d.flags &= ~std::uint32_t(flag::kDeaf);
        return 1;
    case kMsgTick: {
        sp_of(d).note_attacker(d);
        const std::uint32_t lvl = d.sys->config().level_id;
        if (!(lvl >= 0x7000009 && lvl < 0x700000b) && (d.sight_flags & sight::kSeen)) {   // Tower 1 A/B drones just walk
            bool skip_look = false;
            if (d.sight_flags & sight::kFirstSighted) skip_look = d.engage_dist <= d.opp_dist;
            if (!skip_look) skip_look = (d.alert_flags & 0x200000) != 0 || e.running_to_alarm;
            if (!skip_look && look_and_switch(d)) return 1;
        }
        distance_to_ai_point(d);   // NDrone2_DistanceToAIPoint
        const int status = move_to_ai_goal(d);
        switch (status) {
        case 0: case 1: case 2:
            anim_call(d, 0, kRun);
            return 1;
        case 9: case 0xc:
            // creep failed while the player is shooting at us: stand and return fire
            if (d.side != kSideEnemy || !opponent_armed(d)) {
                anim_call(d, 0, kStandAlert);
                return 1;
            }
            set_angle_to_obj(d, d.opponent, 0.0f);
            if ((d.sight_flags & sight::kSeen) == 0) {
                d.fire_requested = false;
                anim_call(d, 0, kStandAlert);
                return 1;
            }
            d.fire_requested = !d.fire_lock;
            anim_call(d, 0, kAimStand);
            if (d.fire_requested) weap::do_firing(d);
            return 1;
        case 10:
            return 1;
        default:   // arrived (3) or the route failed
            if (d.has_beh(0x59)) {
                d.set_state(kStDeleteMe);
                return 1;
            }
            e.heard_noise = false;
            if (d.sys->config().multiplayer || d.dtype != 7) {
                d.set_state(d.initial_state);
                return 1;
            }
            // RunToPoint drones swap between their two behaviour sets at the point
            if (d.active_behaviour == 1) {
                d.active_behaviour = 0;   // DroneFunc_Set1stBehaviour
                return 1;
            }
            d.active_behaviour = 1;       // DroneFunc_Set2ndBehaviour
            d.dtype = d.dtype_base;
            return 1;
        }
    }
    case kMsgPatrolObstructed:
        d.set_state(d.initial_state);
        return 1;
    default:
        if (skel_impact(d, m, d.smi.cur)) return 1;
        if (m.id == kMsgExplosiveNearby) {
            consider_explosive(d, m, d.smi.cur);
            return 1;
        }
        return 0;
    }
}

}  // namespace

void register_search_states() {
    drone::register_state(kStHeardNoiseAware, "HeardNoiseAware", state_heard_noise_aware);
    drone::register_state(kStHeardNoiseSuspect, "HeardNoiseSuspect", state_heard_noise_suspect);
    drone::register_state(kStHeardNoiseAlert, "HeardNoiseAlert", state_heard_noise_alert);
    drone::register_state(kStSearchArea, "SearchArea", state_search_area);
    drone::register_state(kStAlertToPosition, "AlertToPosition", state_alert_to_position);
    drone::register_state(kStGoToGoalPosition, "GoToGoalPosition", state_go_to_goal_position);
}

}  // namespace nf::sp

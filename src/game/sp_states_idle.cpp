// Idle / patrol / investigate family and the framework states (docs/spec-arena-ai.md Part 3 §4, "Framework / scripting"
// and "Idle / patrol / guard / investigate"). Every handler mirrors the dispatch skeleton of its NDrone2_DSTATE_*.
#include "game/sp_common.hpp"
#include "game/sp_idle.hpp"
#include "game/sp_states.hpp"

namespace nf::sp {

using namespace nf::drone;

namespace {

int route_count_zero(Drone& d) { return d.nav && d.nav->has_ai_path() ? 0 : 1; }   // `Drone+0x9f2 == 0` (mission route empty)

// The `default:` of every state's switch: impacts / alerts / forced attack / explosives. Returns 1 if consumed.
int skeleton(Drone& d, const Msg& m, int impact_state = kStAttack) {
    return skel_common(d, m, impact_state) ? 1 : 0;
}

// ---- Idle 0x04 (0x15bf18) ---------------------------------------------------------------------------------------
int state_idle(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter: {
        if (d.sys->config().level_id == 0x700001b) {
            if (d.char_class == 0x14) {
                d.set_state(0xc2);   // SpaceDrake
                return 1;
            }
        }
        d.flags |= flag::kAware | flag::kAlertableByDroneSight;
        if (d.has_beh(0x1d)) assign_ai_path(d, pathflag::kMission);
        if (!route_count_zero(d)) {
            d.set_state(kStEnemyMission);
            return 1;
        }
        d.behaviour[d.active_behaviour].set(0x1d, 0);
        if (d.alertness < 0.66f) {
            stand_idle_anim(d, false);
            if (d.has_beh(beh::kIdleFidget)) set_idle_timeout(d, 0x2d, 5);
            return 1;
        }
        d.set_state(kStAlert);
        return 1;
    }
    case kMsgTick: {
        const int next = enemy_look_for_opponent(d);
        if (next != 0) {
            d.set_state(next);
            return 1;
        }
        stand_idle_anim(d, true);
        return 1;
    }
    case kMsgTimeout:
        if (d.has_beh(beh::kIdleFidget)) {
            if (d.alertness >= 0.66f) anim_call(d, 0, kStandAlertLook);
            else anim_call(d, 0, kIdleAnim, int(d.sys->rand_int(3)));
            set_idle_timeout(d, 0x2d, 5);
        }
        return 1;
    case kMsgAnimResume:
        if (route_count_zero(d)) {
            if (d.alertness >= 0.66f) anim_call(d, 0, kStandAlert);
            else stand_idle_anim(d, false);
            return 1;
        }
        d.set_state(kStEnemyMission);
        return 1;
    default:
        return skeleton(d, m);
    }
}

// ---- Alert 0x05 (0x15c248) ----------------------------------------------------------------------------------------
int state_alert(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.flags |= flag::kAware | flag::kAlertableByDroneSight;
        if (d.has_beh(0x1d)) assign_ai_path(d, pathflag::kMission);
        if (!route_count_zero(d)) {
            d.set_state(kStEnemyMission);
            return 1;
        }
        d.behaviour[d.active_behaviour].set(0x1d, 0);
        anim_call(d, 0, kStandAlert);
        if (d.has_beh(beh::kIdleFidget)) set_idle_timeout(d, 0x2d, 5);
        return 1;
    case kMsgTick: {
        const int next = enemy_look_for_opponent(d);
        if (next != 0) {
            d.set_state(next);
            return 1;
        }
        anim_call(d, 0, kStandAlert);
        return 1;
    }
    case kMsgTimeout:
        if (d.has_beh(beh::kIdleFidget)) {
            anim_call(d, 0, kStandAlertLook);
            set_idle_timeout(d, 0x2d, 5);
        }
        return 1;
    case kMsgAnimResume:
        if (route_count_zero(d)) {
            anim_call(d, 0, kStandAlert);
            return 1;
        }
        d.set_state(kStEnemyMission);
        return 1;
    default:
        return skeleton(d, m);
    }
}

// ---- InitPatrol 0x06 (0x1724f0) --------------------------------------------------------------------------------------
int state_init_patrol(Drone& d, const Msg& m) {
    if (m.id == kMsgNone) return 1;
    if (m.id != kMsgEnter) return 0;
    if (!d.has_beh(beh::kPatrolStart) && !d.has_beh(beh::kPatrolAlert)) {
        d.set_state(kStIdle);
        return 1;
    }
    assign_ai_path(d, pathflag::kPatrol);
    if (route_count_zero(d)) {
        d.set_state(kStIdle);
        return 1;
    }
    d.set_state(kStPatrol);
    return 1;
}

// ---- Patrol 0x07 (0x15c498) -------------------------------------------------------------------------------------------
int state_patrol(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.flags |= flag::kAware | flag::kAlertableByDroneSight;
        if (!d.has_beh(beh::kPatrolAlert) || d.alertness < 0.66f) {
            if (d.has_beh(beh::kIdleFidget)) set_idle_timeout(d, 0x2d, 10);
        } else if (d.has_beh(beh::kIdleFidget)) {
            set_idle_timeout(d, 5, 10);
        }
        return 1;
    case kMsgTick: {
        patrol_talk(d);
        update_patrol_route(d);
        const int next = enemy_look_for_opponent(d);
        if (next != 0) d.set_state(next);
        return 1;
    }
    case kMsgTimeout:
        if (d.has_beh(beh::kIdleFidget)) {
            if (d.has_beh(beh::kPatrolAlert) && d.alertness >= 0.66f) {
                anim_call(d, 0, kWalkAlert);
            } else {
                anim_call(d, 0, kIdleAnim, int(d.sys->rand_int(3)), kStPatrol);
            }
        }
        return 1;
    case kMsgPatrolObstructed:
        d.set_state(kStObstructed);   // NDrone2_ObstructedPatrolPath returns 0x61
        return 1;
    default:
        return skeleton(d, m);
    }
}

// ---- ReturnToPatrolPath 0x15 (0x172810) --------------------------------------------------------------------------------
int state_return_to_patrol_path(Drone& d, const Msg& m) {
    if (m.id == kMsgNone) return 1;
    if (m.id != kMsgEnter) return 0;
    refind_mission_path(d);   // NDrone2_ReFindMissionPath
    sx(d).cfg.initial_state = 0x14;
    d.initial_state = 0x14;
    d.set_state(kStGoToGoalPosition);
    return 1;
}

// ---- StandBlind 0x18 (0x172870) ------------------------------------------------------------------------------------------
int state_stand_blind(Drone& d, const Msg& m) {
    if (m.id == kMsgNone) return 1;
    if (m.id != kMsgEnter) return 0;
    anim_call(d, 0, kStand);   // DASC 0x24 (Crouch/Stand fallback of the anim tables)
    return 1;
}

// ---- WaitSwitch 0x01 (0x15b960) ---------------------------------------------------------------------------------------------
// Resume after the start channel fired: TruckDriverInit for DTYPE 0x17, else the initial state, or PlayScript.
void resume_from_wait(Drone& d, bool truck_check) {
    if (truck_check && d.dtype == 0x17) d.set_state(kStTruckDriverInit);
    else if (d.script_id == 0 || d.script_id == 0x6000000) d.set_state(d.initial_state);
    else d.set_state(kStPlayScript);
}

int state_wait_switch(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        enable_drone(d, false);   // NDrone2_Enable(0, drone): hidden and inert until the channel fires
        return 1;
    case kMsgTick:
        if (!sp_of(d).channels.on(d.start_channel)) return 1;
        enable_drone(d, true);
        resume_from_wait(d, true);
        return 1;
    case kMsgEnable:   // Drone_EnableAll (msg 0x0e)
        enable_drone(d, true);
        resume_from_wait(d, false);
        return 1;
    default:
        return 0;
    }
}

// ---- Disabled 0x02 (0x172440) ---------------------------------------------------------------------------------------------------
int state_disabled(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
    case kMsgEnter:
        return 1;
    case kMsgTick: {
        if ((d.flags & flag::kActive) == 0) return 1;
        if (d.start_channel != 0 && !d.start_channel_snapshot) {
            d.set_state(kStWaitSwitch);
            return 1;
        }
        if (d.dtype == 0x17) {
            d.set_state(kStTruckDriverInit);
        } else if (d.script_id == 0 || d.script_id == 0x6000000) {
            const int back = sx(d).slot<IdleSlot>().saved_state;   // Drone+0xcd8
            d.set_state(back != 0 ? back : d.initial_state);
        } else {
            d.set_state(kStPlayScript);
        }
        return 1;
    }
    default:
        return 0;
    }
}

// ---- PlayScript 0x03 (0x15ba68) -----------------------------------------------------------------------------------------------------
// The script id (DIVars+0x38, 0x06xxxxxx) is a character animation script. Scripts 0x6000168/0x6000169 (hostage
// intro), 0x60007ac and 0x60005e6/0x60008ae have dedicated set-ups; every other id plays as a stand-idle script
// (SetScript(id, 0x24, 0x11a, 0x1e, 0, 5, 0)).
int state_play_script(Drone& d, const Msg& m) {
    IdleSlot& s = sx(d).slot<IdleSlot>();
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter: {
        d.flags |= flag::kAware | flag::kAlertableByDroneSight;
        if (d.dtype == 0x18) {
            d.set_state(kStCastleChatGuard1);
            return 1;
        }
        s.saved_state = 0;   // Drone+0xcd8: impact fall-back state
        s.script_looks = true;       // Drone+0xcdc: watch for the opponent while playing
        const std::uint32_t id = d.script_id;
        switch (id) {
        case 0x60005e6:
        case 0x60008ae:
            s.saved_state = kStAttack;
            play_script(d, id, kStandAlert, /*loop=*/true, 0);
            return 1;
        case 0x6000168:
        case 0x6000169:
            s.saved_state = 0;
            play_script(d, id, id == 0x6000168 ? 10 : 5, false, 5);
            attack_talk(d);
            d.flags |= flag::kDeaf;
            return 1;
        case 0x60007ac:
            s.saved_state = 0;
            s.script_looks = false;
            play_script(d, id, 10, false, 5);
            d.flags |= flag::kDeaf;
            d.behaviour[d.active_behaviour].set(0x3b, 0);
            d.behaviour[d.active_behaviour].set(0x3a, 0);
            sx(d).cfg.impact_mask &= 0xfeff;
            return 1;
        default:
            s.saved_state = kStAttack;
            play_script(d, id, kStandIdle1, false, 5);
            return 1;
        }
    }
    case kMsgTick: {
        if (s.script_looks) {
            const int next = enemy_look_for_opponent(d);
            if (next != 0) {
                d.set_state(next);
                return 1;
            }
        }
        if (d.alertness >= 1.0f) sp_of(d).note_attacker(d);
        if (d.smi.entry_time + 30 < d.now()) {
            if (d.script_id >= 0x6000168 && d.script_id <= 0x6000169) attack_talk(d);
        }
        return 1;
    }
    case kMsgAnimResume: {   // the script ended
        Drone& x = d;
        const std::uint32_t id = x.script_id;
        if (id == 0x6000169 || id == 0x6000168) {
            x.flags &= ~std::uint32_t(flag::kDeaf);
            x.script_id = 0x6000000;
            anim_call(x, 0, kAimStand, 0, 0x68);
            attack_talk(x);
            return 1;
        }
        if (id == 0x60007ac) {
            x.flags &= ~std::uint32_t(flag::kDeaf);
            x.script_id = 0x6000000;
            x.set_state(x.initial_state);
            return 1;
        }
        x.script_id = 0x6000000;
        x.set_state(x.initial_state);
        return 1;
    }
    case kMsgPunch:
    case kMsgBullet:
        drone::handle_impact(d, m, s.saved_state, false);
        return 1;
    case kMsgStunElectric:
    case kMsgExplosive:
    case kMsgGas:
    case kMsgStunGrenade:
    case kMsgStunDart:
        drone::handle_impact(d, m, s.saved_state, true);
        return 1;
    case kMsgShoutFirstSight:
    case kMsgShoutHurt:
    case kMsgSoundAlert:
    case kMsgShoutType5:
    case kMsgShoutAttack:
    case kMsgDroneAlert: {
        const int next = enemy_alerts(d, m);
        if (next != 0) d.set_state(next);
        return 1;
    }
    case kMsgForcedAttack:
        do_forced_attack(d);
        return 1;
    case kMsgExplosiveNearby:
        consider_explosive(d, m, s.saved_state);
        return 1;
    default:
        return 0;
    }
}

// ---- awareness reactions --------------------------------------------------------------------------------------------------------------
int goto_attack_on_enter(Drone& d, const Msg& m) {   // SeenOpponent 0x9d / SeenDroneShot 0xa0
    if (m.id == kMsgNone) return 1;
    if (m.id != kMsgEnter) return 0;
    d.set_state(kStAttack);
    return 1;
}

int state_seen_explosive(Drone&, const Msg& m) {   // 0xa1: ignores everything but ENTER/TICK/none
    return (m.id == kMsgEnter || m.id == kMsgNone || m.id == kMsgTick) ? 1 : 0;
}

int state_heard_noise(Drone& d, const Msg& m) {   // 0xa2 (0x173be8)
    if (m.id == kMsgNone) return 1;
    if (m.id != kMsgEnter) return 0;
    drone::alert_status_set(d, AlertStatus::Alert);
    sx(d).heard_noise = true;   // Drone+0x33
    d.set_state(kStHeardNoiseAware);
    return 1;
}

// ---- Obstructed 0x61 (0x173390) -----------------------------------------------------------------------------------------------------------
int state_obstructed(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.flags |= flag::kAware;
        anim_call(d, 0, kStandAlert);
        d.timer1 = {d.now() + d.seconds(1.0f), 0};   // Drone+0xcc0 armed: msg 0xc after one second
        invalidate_attack_route(d);
        return 1;
    case kMsgTick:
        sp_of(d).note_attacker(d);
        return 1;
    case kMsgTimer1:
        d.set_state(sx(d).return_state);
        return 1;
    default:
        return 0;
    }
}

// ---- DroneStuck 0x7f (0x165ef0) -----------------------------------------------------------------------------------------------------------
int state_drone_stuck(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.flags |= flag::kAware;
        d.set_state(kStCombatNoRoute);
        return 1;
    case kMsgTick:
        sp_of(d).note_attacker(d);
        // Drone+0x868 (route target stamp) < NPCGlobals+0x214 (never written after PostLoad_Init: 0): stays stuck
        return 1;
    case kMsgPunch:
    case kMsgBullet:
        drone::handle_impact(d, m, d.smi.prev, false);
        return 1;
    case kMsgStunElectric:
    case kMsgExplosive:
    case kMsgGas:
    case kMsgStunGrenade:
    case kMsgStunDart:
        drone::handle_impact(d, m, d.smi.prev, true);
        return 1;
    case kMsgExplosiveNearby:
        consider_explosive(d, m, d.smi.prev);
        return 1;
    default:
        return 1;
    }
}

// ---- StandFiddle 0x84 (0x1687c0) -------------------------------------------------------------------------------------------------------------
int state_stand_fiddle(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.flags |= flag::kAware;
        anim_call(d, 0, kStandFiddle);
        d.timer1 = {d.now() + d.seconds(5.0f), 0};
        return 1;
    case kMsgTick:
        sx(d).slot<IdleSlot>().blind_flag = true;   // Drone+0x38
        return 1;
    case kMsgPunch:
    case kMsgBullet:
        drone::handle_impact(d, m, d.smi.prev, false);
        return 1;
    case kMsgStunElectric:
    case kMsgExplosive:
    case kMsgGas:
    case kMsgStunGrenade:
    case kMsgStunDart:
        drone::handle_impact(d, m, d.smi.prev, true);
        return 1;
    case kMsgTimer1:
        d.set_state(sx(d).return_state2);
        return 1;
    case kMsgExplosiveNearby:
        consider_explosive(d, m, d.smi.prev);
        return 1;
    default:
        return 0;
    }
}

// ---- Investigate 0x7e (0x168070) ---------------------------------------------------------------------------------------------------------------
int state_investigate(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.flags |= flag::kAware | flag::kAlertableByDroneSight;
        drone::alert_status_set(d, AlertStatus::Scared);
        anim_call(d, 0, kStandAlert);
        return 1;
    case kMsgTick: {
        int next = enemy_look_for_opponent(d);
        if (next == 0 && d.alertness < 0.66f) next = kStIdle;
        if (next != 0) d.set_state(next);
        return 1;
    }
    default:
        return skeleton(d, m);
    }
}

}  // namespace

void register_idle_states() {
    drone::register_state(kStIdle, "Idle", state_idle);
    drone::register_state(kStAlert, "Alert", state_alert);
    drone::register_state(kStInitPatrol, "InitPatrol", state_init_patrol);
    drone::register_state(kStPatrol, "Patrol", state_patrol);
    drone::register_state(kStReturnToPatrolPath, "ReturnToPatrolPath", state_return_to_patrol_path);
    drone::register_state(kStStandBlind, "StandBlind", state_stand_blind);
    drone::register_state(kStWaitSwitch, "WaitSwitch", state_wait_switch);
    drone::register_state(kStDisabled, "Disabled", state_disabled);
    drone::register_state(kStPlayScript, "PlayScript", state_play_script);
    drone::register_state(kStSeenOpponent, "SeenOpponent", goto_attack_on_enter);
    drone::register_state(kStSeenDroneShot, "SeenDroneShot", goto_attack_on_enter);
    drone::register_state(kStSeenExplosive, "SeenExplosive", state_seen_explosive);
    drone::register_state(kStHeardNoise, "HeardNoise", state_heard_noise);
    drone::register_state(kStObstructed, "Obstructed", state_obstructed);
    drone::register_state(kStDroneStuck, "DroneStuck", state_drone_stuck);
    drone::register_state(kStStandFiddle, "StandFiddle", state_stand_fiddle);
    drone::register_state(kStInvestigate, "Investigate", state_investigate);
    register_search_states();
}

}  // namespace nf::sp

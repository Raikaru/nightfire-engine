// Hostage / civilian / guard / mission-script states (docs/spec-arena-ai.md Part 3 §8):
// HostageKiller*, Hostage*, Civilian*, Kiko*, EnemyMission, PartyGirl*, guards, truck,
// castle chat, interrogation, EnemyRunToPoint, scary-object flight, RunToAlarm/PressAlarm.
#include "game/sp_civilian_util.hpp"
#include "game/sp_common.hpp"
#include "game/sp_idle.hpp"
#include "game/sp_states.hpp"

namespace nf::sp {

using namespace nf::drone;

namespace {

int skeleton(Drone& d, const Msg& m) { return skel_common(d, m, kStAttack) ? 1 : 0; }
// Civilians never fight back: impacts route to the scared states instead of Attack.
int civ_skeleton(Drone& d, const Msg& m) { return skel_common(d, m, kStCivilianScared) ? 1 : 0; }

// Walk the AI goal; on arrival (or route failure) enter `arrived`, else play `move_anim`.
bool walk_goal(Drone& d, int arrived, int move_anim = kRun) {
    const int status = move_to_ai_goal(d);
    set_angle_to_dest(d);
    if (status == int(RouteStatus::Following) || status == int(RouteStatus::Straight)) {
        anim_call(d, 0, move_anim);
        return false;
    }
    d.set_state(arrived);
    return true;
}

// ---- HostageKiller 0x08: holds the hostage, executes on alert ----------------------------------
int state_hostage_killer(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.flags |= flag::kAware;
        anim_call(d, 0, kStandAlert);
        return 1;
    case kMsgTick: {
        const int next = enemy_look_for_opponent(d);
        if (next != 0) {
            d.set_state(kStHostageKillerAttack);   // seen: attack while keeping the hostage covered
            return 1;
        }
        return 1;
    }
    case kMsgHostageKillerOrder:   // ordered to execute / release
        d.set_state(kStHostageKillerAttack);
        return 1;
    default:
        return skeleton(d, m);
    }
}

// ---- HostageKillerAttack 0x09 ---------------------------------------------------------------------
int state_hostage_killer_attack(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.flags |= flag::kAware;
        set_as_attacking(d);
        return 1;
    case kMsgTick:
        sp_of(d).note_attacker(d);
        if (!d.opponent.valid()) {
            d.set_state(kStHostageKiller);
            return 1;
        }
        set_angle_to_obj(d, d.opponent, 0.0f);
        anim_call(d, 0, kAimStand);
        d.fire_requested = !d.fire_lock;
        return 1;
    default:
        return skeleton(d, m);
    }
}

// ---- Hostage 0x0a: cower; HostageDie 0x0b: killed by the killer --------------------------------------
int state_hostage(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.flags |= flag::kAware;
        anim_call(d, 0, kCrouch);
        return 1;
    case kMsgTick:
        return 1;
    case kMsgHostageReleased:
        d.set_state(kStHostageSaved);
        return 1;
    default:
        return civ_skeleton(d, m);
    }
}

int state_hostage_die(Drone& d, const Msg& m) {
    if (m.id == kMsgNone) return 1;
    if (m.id == kMsgEnter || m.id == kMsgTick) {
        mission_fail(d, 0x9, 0x4000033);   // hostage died: mission failed
        d.set_state(kStDeath_Anim);
        return 1;
    }
    return 1;
}

// ---- HostageSaved 0x0c / HostageIdle 0x0d / HostageHide 0x0e / HostageDead 0x0f ------------------------
int state_hostage_saved(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        talk(d, Speech::Other);
        sp_of(d).hostages_saved++;
        anim_call(d, 0, kStandAlert, 0, kStHostageIdle);
        return 1;
    case kMsgTick:
        return 1;
    default:
        return civ_skeleton(d, m);
    }
}

int state_hostage_idle(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        stand_idle_anim(d, false);
        return 1;
    case kMsgTick:
        update_patrol_route(d);
        return 1;
    default:
        return civ_skeleton(d, m);
    }
}

int state_hostage_hide(Drone& d, const Msg& m) {
    if (m.id == kMsgNone) return 1;
    if (m.id == kMsgEnter) {
        d.flags |= flag::kAware;
        anim_call(d, 0, kCrouch);
        return 1;
    }
    if (m.id == kMsgTick) return 1;
    return civ_skeleton(d, m);
}

int state_hostage_dead(Drone&, const Msg& m) {
    if (m.id == kMsgNone || m.id == kMsgEnter || m.id == kMsgTick) return 1;
    return 1;   // fully dead: ignores everything (Global still routes DeleteMe/Fade)
}

// ---- HostageGoToGoalPosition 0x64: freed hostage walks out ----------------------------------------------
int state_hostage_goto_goal(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        return 1;
    case kMsgTick:
        walk_goal(d, kStHostageSaved, kWalk);
        return 1;
    default:
        return civ_skeleton(d, m);
    }
}

// ---- CivilianInit 0x10 / Civilian 0x11 ----------------------------------------------------------------------
int state_civilian_init(Drone& d, const Msg& m) {
    if (m.id == kMsgNone) return 1;
    if (m.id == kMsgEnter || m.id == kMsgTick) {
        d.set_state(kStCivilian);
        return 1;
    }
    return civ_skeleton(d, m);
}

int state_civilian(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.flags |= flag::kAware;
        stand_idle_anim(d, false);
        return 1;
    case kMsgTick: {
        // Gunfire / alerts / seeing the player's weapon send civilians to CivilianScared.
        if (d.shot_at || (d.sight_flags & sight::kSeen)) {
            civilian_scare(d);
            return 1;
        }
        update_patrol_route(d);
        return 1;
    }
    default:
        return civ_skeleton(d, m);
    }
}

// ---- CivilianScared 0x12 / CivilianHiding 0x13 / CivilianPatrol 0x14 ---------------------------------------------
int state_civilian_scared(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.flags |= flag::kAware;
        talk(d, Speech::CivilianScared);
        anim_call(d, 0, kStandAlertLook);
        set_idle_timeout(d, 2, 2);
        return 1;
    case kMsgTimeout:
        d.set_state(kStCivilianHiding);
        return 1;
    case kMsgTick:
        return 1;
    default:
        return civ_skeleton(d, m);
    }
}

int state_civilian_hiding(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.flags |= flag::kAware;
        anim_call(d, 0, kCrouch);
        set_idle_timeout(d, 10, 10);
        return 1;
    case kMsgTimeout:
        d.set_state(kStCivilianPatrol);
        return 1;
    case kMsgTick:
        return 1;
    default:
        return civ_skeleton(d, m);
    }
}

int state_civilian_patrol(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.flags |= flag::kAware;
        return 1;
    case kMsgTick:
        if (d.shot_at) {
            civilian_scare(d);
            return 1;
        }
        update_patrol_route(d);
        return 1;
    default:
        return civ_skeleton(d, m);
    }
}

// ---- CivilianMission 0x16 / MissionWait 0x17: scripted walk with channel gates ---------------------------------------
int state_civilian_mission(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.flags |= flag::kAware;
        refind_mission_path(d);
        return 1;
    case kMsgTick:
        if (d.shot_at) {
            civilian_scare(d);
            return 1;
        }
        walk_goal(d, kStCivilianMissionWait, kWalk);
        return 1;
    default:
        return civ_skeleton(d, m);
    }
}

int state_civilian_mission_wait(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.flags |= flag::kAware;
        stand_idle_anim(d, false);
        set_idle_timeout(d, 5, 5);
        return 1;
    case kMsgTimeout:
        d.set_state(kStCivilianMission);
        return 1;
    case kMsgTick:
        return 1;
    default:
        return civ_skeleton(d, m);
    }
}

// ---- KikoMission 0x19 / KikoMissionRun 0x1a ------------------------------------------------------------------------------
int state_kiko_mission(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.flags |= flag::kAware;
        refind_mission_path(d);
        return 1;
    case kMsgTick:
        walk_goal(d, kStKikoMissionRun, kWalk);
        return 1;
    default:
        return civ_skeleton(d, m);
    }
}

int state_kiko_mission_run(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.flags |= flag::kAware;
        refind_mission_path(d);
        return 1;
    case kMsgTick:
        walk_goal(d, kStKikoMission, kRun);
        return 1;
    default:
        return civ_skeleton(d, m);
    }
}

// ---- EnemyMission 0x1b: enemy walks the mission route, engages on sight ------------------------------------------------------
int state_enemy_mission(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.flags |= flag::kAware;
        refind_mission_path(d);
        return 1;
    case kMsgTick: {
        const int next = enemy_look_for_opponent(d);
        if (next != 0) {
            d.set_state(next);
            return 1;
        }
        walk_goal(d, kStReturnToPatrolPath, kWalk);
        return 1;
    }
    default:
        return skeleton(d, m);
    }
}

// ---- PartyGirlInit 0x2e / PartyGirl 0x2f ---------------------------------------------------------------------------------------
int state_party_girl_init(Drone& d, const Msg& m) {
    if (m.id == kMsgNone) return 1;
    if (m.id == kMsgEnter || m.id == kMsgTick) {
        d.set_state(kStPartyGirl);
        return 1;
    }
    return civ_skeleton(d, m);
}

int state_party_girl(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.flags |= flag::kAware;
        stand_idle_anim(d, false);
        return 1;
    case kMsgTick:
        if (d.shot_at) {
            civilian_scare(d);
            return 1;
        }
        update_patrol_route(d);
        return 1;
    default:
        return civ_skeleton(d, m);
    }
}

// ---- CivilianGuard 0x30 / CivilianDoorGuard 0x31 -----------------------------------------------------------------------------------
int state_civilian_guard(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.flags |= flag::kAware;
        anim_call(d, 0, kStandAlert);
        return 1;
    case kMsgTick: {
        if (d.opponent.valid() && (d.sight_flags & sight::kSeen) && opponent_armed(d)) {
            // Armed intruder challenged...
            anim_call(d, 0, kChallenge);
            d.set_state(kStCivilianChallenge);
            return 1;
        }
        return 1;
    }
    default:
        return skeleton(d, m);
    }
}

// ---- AmbushInit 0x37 / AmbushWait 0x38: walk to the ambush node, hold crouched until tripped ----
int state_ambush_init(Drone& d, const Msg& m) {
    if (m.id == kMsgNone) return 1;
    if (m.id == kMsgEnter) {
        refind_mission_path(d);
        return 1;
    }
    if (m.id == kMsgTick) {
        walk_goal(d, kStAmbushWait, kRun);
        return 1;
    }
    return skeleton(d, m);
}

int state_ambush_wait(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.flags |= flag::kAware;
        anim_call(d, 0, kCrouch);
        return 1;
    case kMsgTick: {
        const int next = enemy_look_for_opponent(d);
        if (next != 0) d.set_state(next);   // sprung: straight to Attack
        return 1;
    }
    default:
        return skeleton(d, m);
    }
}

int state_civilian_door_guard(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.flags |= flag::kAware;
        anim_call(d, 0, kStandAlert);
        set_idle_timeout(d, 4, 4);
        return 1;
    case kMsgTimeout:
        // Door-guard patrol beat: walk the mission route and come back.
        refind_mission_path(d);
        d.set_state(kStGoToGoalPosition);
        return 1;
    case kMsgTick:
        if (d.opponent.valid() && (d.sight_flags & sight::kSeen)) {
            d.set_state(kStCivilianChallenge);
            return 1;
        }
        return 1;
    default:
        return skeleton(d, m);
    }
}

// ---- TruckDriver 0x32..0x35 --------------------------------------------------------------------------------------------------------
int state_truck_driver_init(Drone& d, const Msg& m) {
    if (m.id == kMsgNone) return 1;
    if (m.id == kMsgEnter || m.id == kMsgTick) {
        d.set_state(kStTruckDriverIdle);
        return 1;
    }
    return skeleton(d, m);
}

int state_truck_driver_init_alert(Drone& d, const Msg& m) {
    if (m.id == kMsgNone) return 1;
    if (m.id == kMsgEnter || m.id == kMsgTick) {
        d.set_state(kStTruckDriverMission);
        return 1;
    }
    return skeleton(d, m);
}

int state_truck_driver_idle(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.flags |= flag::kAware;
        stand_idle_anim(d, false);
        return 1;
    case kMsgTick: {
        const int next = enemy_look_for_opponent(d);
        if (next != 0) d.set_state(next);
        return 1;
    }
    default:
        return skeleton(d, m);
    }
}

int state_truck_driver_mission(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.flags |= flag::kAware;
        refind_mission_path(d);
        return 1;
    case kMsgTick:
        walk_goal(d, kStTruckDriverIdle, kWalk);
        return 1;
    default:
        return skeleton(d, m);
    }
}

// ---- CastleChatGuard1 0x36: paired chat, flees to alarm on gunfire ----------------------------------------------------------------------
int state_castle_chat_guard(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.flags |= flag::kAware;
        stand_idle_anim(d, false);
        set_idle_timeout(d, 6, 6);
        return 1;
    case kMsgTimeout:
        talk(d, Speech::Patrol);   // chat beat with the door partner
        set_idle_timeout(d, 6, 6);
        return 1;
    case kMsgTick: {
        const int next = enemy_look_for_opponent(d);
        if (next != 0) d.set_state(next);
        return 1;
    }
    default:
        return skeleton(d, m);
    }
}

// ---- Interrogation 0x39..0x3d --------------------------------------------------------------------------------------------------------------
int state_interogate_assist(Drone& d, const Msg& m) {
    if (m.id == kMsgNone) return 1;
    if (m.id == kMsgEnter) {
        anim_call(d, 0, kStandAlert);
        return 1;
    }
    if (m.id == kMsgTick) {
        d.set_state(kStInterogateAssistWait);
        return 1;
    }
    return skeleton(d, m);
}

int state_interogate_assist_wait(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.flags |= flag::kAware;
        stand_idle_anim(d, false);
        set_idle_timeout(d, 8, 8);
        return 1;
    case kMsgTimeout:
        d.set_state(kStInterogateAssist);
        return 1;
    case kMsgTick:
        return 1;
    default:
        return skeleton(d, m);
    }
}

int state_interogator(Drone& d, const Msg& m) {
    if (m.id == kMsgNone || m.id == kMsgEnter || m.id == kMsgTick) {
        d.set_state(kStInterogate);
        return 1;
    }
    return skeleton(d, m);
}

int state_interogate(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.flags |= flag::kAware;
        anim_call(d, 0, kChallenge);
        set_idle_timeout(d, 10, 5);
        return 1;
    case kMsgTimeout:
        d.set_state(kStInterogateWalk);
        return 1;
    case kMsgTick:
        return 1;
    default:
        return skeleton(d, m);
    }
}

int state_interogate_walk(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.flags |= flag::kAware;
        refind_mission_path(d);
        return 1;
    case kMsgTick:
        walk_goal(d, kStInterogate, kWalk);
        return 1;
    default:
        return skeleton(d, m);
    }
}

// ---- CivilianChallenge 0x3e ----------------------------------------------------------------------------------------------------------------
int state_civilian_challenge(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.flags |= flag::kAware;
        if (d.opponent.valid()) set_angle_to_obj(d, d.opponent, 0.0f, true);
        anim_call(d, 0, kChallenge);
        talk(d, Speech::Other);   // "Hold it right there"
        set_idle_timeout(d, 4, 2);
        return 1;
    case kMsgTimeout: {
        // Stood down or escalated: unarmed opponent -> back to post, else attack.
        if (d.opponent.valid() && opponent_armed(d)) d.set_state(kStAttack);
        else d.set_state(d.initial_state);
        return 1;
    }
    case kMsgTick:
        return 1;
    default:
        return skeleton(d, m);
    }
}

// ---- EnemyRunToPoint 0x85: behaviour-swap runner (see GoToGoalPosition tail) -----------------------------------------------------------
int state_enemy_run_to_point(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.flags |= flag::kAware;
        return 1;
    case kMsgTick: {
        const int next = enemy_look_for_opponent(d);
        if (next != 0) {
            d.set_state(next);
            return 1;
        }
        walk_goal(d, d.initial_state, kRun);
        return 1;
    }
    default:
        return skeleton(d, m);
    }
}

// ---- RunAwayFromObject 0x86 / HideFromScaryObject 0x87 / RecoverFromScaryObject 0x88 ----------------------------------------------------------
// DroneFunc_ConsiderExplosive fills SpExt::scary; these run the flight.
int state_run_away(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter: {
        d.flags |= flag::kAware;
        SpExt& e = sx(d);
        e.scared_hiding = true;
        set_ai_goal(d, e.scary.pos, e.scary.radius);
        anim_call(d, 0, kRun);
        return 1;
    }
    case kMsgTick: {
        SpExt& e = sx(d);
        if (d.now() >= e.scary.until) {
            d.set_state(kStRecoverFromScaryObject);
            return 1;
        }
        walk_goal(d, kStHideFromScaryObject, kRunFast);
        return 1;
    }
    default:
        return skeleton(d, m);
    }
}

int state_hide_from_scary(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.flags |= flag::kAware;
        anim_call(d, 0, kCrouch);
        return 1;
    case kMsgTick: {
        SpExt& e = sx(d);
        if (d.now() >= e.scary.until) d.set_state(kStRecoverFromScaryObject);
        return 1;
    }
    default:
        return skeleton(d, m);
    }
}

int state_recover_from_scary(Drone& d, const Msg& m) {
    if (m.id == kMsgNone) return 1;
    if (m.id == kMsgEnter || m.id == kMsgTick) {
        SpExt& e = sx(d);
        e.scared_hiding = false;
        refind_mission_path(d);   // NDrone2_ReFindMissionPath
        d.set_state(e.scary.return_state != 0 ? e.scary.return_state : d.initial_state);
        return 1;
    }
    return skeleton(d, m);
}

// ---- RunToAlarm 0x89 / PressAlarm 0x8a / DonePressAlarm 0x8b -------------------------------------------------------------------------------
// DTYPE 0xe AlarmRaiser: run to the alarm point, raise it, stand down.
int state_run_to_alarm(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter: {
        d.flags |= flag::kAware;
        SpExt& e = sx(d);
        e.running_to_alarm = true;
        if (!find_alarm_point(d)) {
            e.running_to_alarm = false;
            d.set_state(kStAttack);
            return 1;
        }
        return 1;
    }
    case kMsgTick:
        if (alarm_raised(d)) {   // someone else got there first
            sx(d).running_to_alarm = false;
            d.set_state(kStAttack);
            return 1;
        }
        walk_goal(d, kStPressAlarm, kRun);
        return 1;
    default:
        return skeleton(d, m);
    }
}

int state_press_alarm(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        anim_call(d, 0, kAlarmActivate, 0, kStDonePressAlarm);
        return 1;
    case kMsgTick:
        return 1;
    default:
        return skeleton(d, m);
    }
}

int state_done_press_alarm(Drone& d, const Msg& m) {
    if (m.id == kMsgNone) return 1;
    if (m.id == kMsgEnter || m.id == kMsgTick) {
        SpExt& e = sx(d);
        e.running_to_alarm = false;
        sp_of(d).alarm_raised = true;   // DroneFunc_CheckAlarmRaised now trips level-wide
        d.set_state(kStAttack);
        return 1;
    }
    return skeleton(d, m);
}

}  // namespace

void register_civilian_states() {
    using drone::register_state;
    register_state(kStHostageKiller, "HostageKiller", state_hostage_killer);
    register_state(kStHostageKillerAttack, "HostageKillerAttack", state_hostage_killer_attack);
    register_state(kStHostage, "Hostage", state_hostage);
    register_state(kStHostageDie, "HostageDie", state_hostage_die);
    register_state(kStHostageSaved, "HostageSaved", state_hostage_saved);
    register_state(kStHostageIdle, "HostageIdle", state_hostage_idle);
    register_state(kStHostageHide, "HostageHide", state_hostage_hide);
    register_state(kStHostageDead, "HostageDead", state_hostage_dead);
    register_state(kStHostageGoToGoalPosition, "HostageGoToGoalPosition", state_hostage_goto_goal);
    register_state(kStCivilianInit, "CivilianInit", state_civilian_init);
    register_state(kStCivilian, "Civilian", state_civilian);
    register_state(kStCivilianScared, "CivilianScared", state_civilian_scared);
    register_state(kStCivilianHiding, "CivilianHiding", state_civilian_hiding);
    register_state(kStCivilianPatrol, "CivilianPatrol", state_civilian_patrol);
    register_state(kStCivilianMission, "CivilianMission", state_civilian_mission);
    register_state(kStCivilianMissionWait, "CivilianMissionWait", state_civilian_mission_wait);
    register_state(kStKikoMission, "KikoMission", state_kiko_mission);
    register_state(kStKikoMissionRun, "KikoMissionRun", state_kiko_mission_run);
    register_state(kStEnemyMission, "EnemyMission", state_enemy_mission);
    register_state(kStPartyGirlInit, "PartyGirlInit", state_party_girl_init);
    register_state(kStPartyGirl, "PartyGirl", state_party_girl);
    register_state(kStCivilianGuard, "CivilianGuard", state_civilian_guard);
    register_state(kStCivilianDoorGuard, "CivilianDoorGuard", state_civilian_door_guard);
    register_state(kStTruckDriverInit, "TruckDriverInit", state_truck_driver_init);
    register_state(kStTruckDriverInitAlert, "TruckDriverInitAlert", state_truck_driver_init_alert);
    register_state(kStTruckDriverIdle, "TruckDriverIdle", state_truck_driver_idle);
    register_state(kStTruckDriverMission, "TruckDriverMission", state_truck_driver_mission);
    register_state(kStCastleChatGuard1, "CastleChatGuard1", state_castle_chat_guard);
    register_state(kStInterogateAssist, "InterogateAssist", state_interogate_assist);
    register_state(kStInterogateAssistWait, "InterogateAssistWait", state_interogate_assist_wait);
    register_state(kStInterogator, "Interogator", state_interogator);
    register_state(kStInterogate, "Interogate", state_interogate);
    register_state(kStInterogateWalk, "InterogateWalk", state_interogate_walk);
    register_state(kStCivilianChallenge, "CivilianChallenge", state_civilian_challenge);
    register_state(kStEnemyRunToPoint, "EnemyRunToPoint", state_enemy_run_to_point);
    register_state(kStRunAwayFromObject, "RunAwayFromObject", state_run_away);
    register_state(kStHideFromScaryObject, "HideFromScaryObject", state_hide_from_scary);
    register_state(kStRecoverFromScaryObject, "RecoverFromScaryObject", state_recover_from_scary);
    register_state(kStRunToAlarm, "RunToAlarm", state_run_to_alarm);
    register_state(kStPressAlarm, "PressAlarm", state_press_alarm);
    register_state(kStDonePressAlarm, "DonePressAlarm", state_done_press_alarm);
    // Ambush pair (AmbushInit picks the node, AmbushWait holds it until tripped).
    register_state(kStAmbushInit, "AmbushInit", state_ambush_init);
    register_state(kStAmbushWait, "AmbushWait", state_ambush_wait);
}

}  // namespace nf::sp

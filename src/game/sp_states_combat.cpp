// Attack / combat core, aim sub-states, snipers, grenades (docs/spec-arena-ai.md Part 3 §4
// "Attack / combat core" and "Aim / fire / reload / move", §4.2 combat-state selector).
#include "game/sp_civilian_util.hpp"
#include "game/sp_common.hpp"
#include "game/sp_idle.hpp"
#include "game/sp_states.hpp"

#include "game/drone_weap.hpp"

namespace nf::sp {

using namespace nf::drone;

namespace {

int skeleton(Drone& d, const Msg& m) { return skel_common(d, m, kStAttack) ? 1 : 0; }

// ---- DroneFunc pieces used by the combat family -----------------------------------------------------------------
// NDrone2_CoverAvailable is cover_available() (sp_civilian_util.hpp); NDrone2_ChooseCombatMove is
// evasive_move() (drone_move.hpp).
bool can_throw_grenade(Drone& d) {
    // DroneWeap_CanThrowGrenade: clip loaded, coin flip, opponent at 8..20 m, grenade anim available.
    const SpExt& e = sx(d);
    if (e.ammo < 1 || e.ammo_max < 1) return false;
    if (d.sys->rand() % 2 != 0) return false;
    if (!d.opponent.valid() || d.opp_dist < 8.0f || d.opp_dist > 20.0f) return false;
    return anim_can_do(d, kGrenade);
}

bool castle56(std::uint32_t level) { return level == 0x7000005 || level == 0x7000006; }   // GameState+0xc + 0xf8fffffb < 2

// Face the opponent and flag firing; DroneWeap_HandleFiring shoots post-move (never call do_firing here:
// the original states only set Drone+0x3b).
void aim_flag(Drone& d, int aim_dasc) {
    set_angle_to_obj(d, d.opponent, 0.0f);
    anim_call(d, 0, aim_dasc);
    d.fire_requested = (d.sight_flags & sight::kSeen) != 0;
}

// DroneFunc_CombatState 0x148338: shared per-tick selector of the combat family, called from Combat,
// CombatOutOfRange, CombatNoSight, CombatWait, AimStandFire and AimCrouchFire. Moves toward the opponent
// (closing to min(engage, 6)), then branches by current state. Returns the next state, 0 = stay (the
// original returns 0 after playing a stay-put anim).
int combat_tick(Drone& d) {
    if (!d.opponent.valid()) return kStNoOpponent;
    if (d.anim.req_state != 0) return 0;   // DroneAnim_InTransition: hold the state
    const int cur = d.smi.cur;
    const bool firing_pose = cur == kStAimStandFire || cur == kStAimCrouchFire;
    const int r = move_to_object(d, d.opponent, std::min(d.engage_dist, 6.0f), !firing_pose);
    const bool route_ok = r < 4 || r == 10;
    const bool seen = (d.sight_flags & sight::kSeen) != 0;
    const bool nomove = (d.flags & flag::kStationary) != 0 || d.has_beh(beh::kNeverMovesInCombat);
    const std::uint32_t lvl = d.sys->config().level_id;
    // Arrival / creep-failure facing + firing while staying (shared tail of several branches).
    const auto face_and_hold = [&](int aim_dasc) {
        set_angle_to_obj(d, d.opponent, 0.0f);
        d.fire_requested = seen;
        anim_call(d, 0, aim_dasc);
    };
    // Move-status arrival handling shared by the OutOfRange / NoSight walkers.
    const auto walk_status = [&](int fire_dasc) -> int {
        switch (r) {
        case 0: case 1: case 2:
            if ((d.mv.boundary_flags & 2) != 0) {   // blocked ahead: stand, explaining the hold
                anim_call(d, 0, kStandAlert);
                return 0;
            }
            if (castle56(lvl)) {
                set_combat_move_anim(d, d.mv.route_distance);
                return 0;
            }
            anim_call(d, 0, kRun);
            return 0;
        case 3: case 9: case 0xb: case 0xc:
            face_and_hold(fire_dasc);
            return 0;
        case 5:
            return kStDroneStuck;
        case 10:
            return 0;
        default:
            return kStCombatNoRoute;
        }
    };
    switch (cur) {
    case kStCombat: {
        if (nomove) return kStCombatNoMove;
        if (d.opp_dist >= d.engage_dist) return kStCombatOutOfRange;
        if (!seen) return d.lost_frames > 15 ? kStCombatNoSight : kStCombatNoRoute;
        return kStCombatNewSighting;
    }
    case kStCombatOutOfRange: {
        if (nomove) return kStCombatNoMove;
        if (cover_available(d)) return kStRunForCover;
        if (d.opp_dist < d.engage_dist) return kStCombat;
        return walk_status(kAimStand);
    }
    case kStCombatNoSight: {
        if (nomove) return kStCombatNoMove;
        if (cover_available(d)) return kStRunForCover;
        if (seen) return kStCombatNewSighting;
        if (d.lost_frames > 15) {
            if (r == 0 || r == 1 || r == 2) {
                set_combat_move_anim(d, d.mv.route_distance);
                return 0;
            }
            return walk_status(kAimStand);
        }
        face_and_hold(kAimStand);
        return 0;
    }
    case kStCombatWait: {
        set_angle_to_obj(d, d.opponent, 0.0f);
        if (route_ok) return kStCombat;
        if (seen) {
            d.fire_requested = true;
            anim_call(d, 0, kAimStand);
        } else {
            anim_call(d, 0, kStandAlert);
        }
        return 0;
    }
    case kStAimStandFire: case kStAimCrouchFire: {
        SpExt& e = sx(d);
        if (e.ammo < 1) return kStAimStandReload;   // Drone+0xbbc < 1
        if (route_ok) {
            if (d.engage_dist <= d.opp_dist) return kStCombatOutOfRange;
            if (d.lost_frames > 15) return kStCombatNoSight;
        }
        if (cover_available(d)) return kStRunForCover;
        if (can_throw_grenade(d)) return kStGrenadeThrow;
        // NDrone2_ChooseCombatMove when the current anim allows it (approximated by availability), with
        // the crouch-fire 120-frame cooldown (Drone+0x11c).
        const bool cooled = cur != kStAimCrouchFire || d.now() >= e.last_combat_move + d.seconds(2.0f);
        if (cooled) {
            if (const int evade = evasive_move(d); evade != 0) {
                e.last_combat_move = d.now();
                return evade;
            }
        }
        face_and_hold(cur == kStAimCrouchFire ? kAimCrouch : kAimStand);
        return 0;
    }
    default:
        return kStCombat;
    }
}

// ---- DroneFunc_FirstAlertState 0x147fd0 ---------------------------------------------------------------------------
// Alert-source dispatch of the first attack: the 0x200000 (attack-shout) source on the interrogation levels
// resumes the interrogation assist; every other source falls through to the normal setup (return 0).
int first_alert_state(const Drone& d) {
    const std::uint32_t f = d.alert_flags;
    if ((f & 0x10) || (f & 0x20) || (f & 0x20000) || (f & 0x40000) || (f & 0x80000) || (f & 0x100000)) return 0;
    if (!(f & 0x200000)) return 0;
    const std::uint32_t lvl = d.sys->config().level_id;
    if (lvl == 0x7000005 || lvl == 0x7000006 || lvl == 0x7000007 || lvl == 0x7000041) return kStInterogateAssist;
    return 0;
}

// ---- DroneFunc_FirstSightState 0x147d70-ish --------------------------------------------------------------------------
// Re-attack dispatch: surrender, first-sight shout, interrogation challenges, grenade / combat-move choice.
int first_sight_state(Drone& d) {
    if (check_surrender(d)) {   // behaviour 0x43, close, unaware, target looking away and armed
        d.alertness = 1.0f;
        return kStSurrender_Anim;
    }
    if (d.has_beh(beh::kShoutsOnFirstSight) && !(d.flags & flag::kFirstSightShoutSent)) {
        d.alert_flags |= 0x1;
        alert_others(d, kMsgShoutFirstSight, d.opponent, d.alert_pos);   // broadcast +30 ticks
        d.flags |= flag::kFirstSightShoutSent;
    }
    const std::uint32_t lvl = d.sys->config().level_id;
    if (d.has_beh(beh::kChallengeFar)) {
        if (lvl < 0x700000b) {
            if (lvl > 0x7000008) return kStCivilianChallenge;
            if (lvl == 0x7000007) return kStInterogate;
        } else if (lvl == 0x7000041) {
            return kStInterogate;
        }
    }
    talk(d, Speech::Other);   // NDrone2_SeenPlayerTalk
    if (d.has_beh(beh::kChallengeNear)) {
        if (d.sys->rand() % 2 == 0) return 0;
        if (const int evade = evasive_move(d); evade != 0) return evade;   // NDrone2_ChooseCombatMove
    }
    if (d.has_beh(beh::kAimStandPreferred)) return 0;
    if (d.has_beh(beh::kCombatMoveAlt)) {
        if (const int evade = evasive_move(d); evade != 0) return evade;
        return kStGrenadeThrow;
    }
    if (const int evade = evasive_move(d); evade != 0) return evade;   // NDrone2_ChooseCombatMove
    return 0;
}

// ---- DroneFunc_AttackNoWeapon ------------------------------------------------------------------------------------------
// Fitted weapon kind (collbody+0x62) above 1 fights on; a held variant (DIVars key 10 -> Drone+0x45 above 1)
// draws weapon 6 (DroneWeap_ChangeWeapon); the truly unarmed cower.
int attack_no_weapon(Drone& d) {
    if (d.weapon > 1) return 0;
    if (sx(d).spec.variant > 1) return kStDrawWeapon;
    return kStCivilianScared;
}

// Shared FirstAttack bookkeeping: alt mode cleared, reacted + first-attack flags, first-attack time,
// full alertness, widened tower sight.
void mark_reacted(Drone& d) {
    d.alt_dmode = 0;
    d.flags |= flag::kFirstAttackDone;
    d.alt_channel_snapshot = false;
    if (d.first_attack_time == 0) d.first_attack_time = d.now();
    d.flags |= flag::kAlertedByNoise;
    d.alertness = 1.0f;
}

// ---- DroneFunc_FirstAttack 0x148010 -----------------------------------------------------------------------------------------
// Attack ENTER dispatch: already-reacted or blind drones stay; unseen drones with an alert source run the
// alert dispatch; seen drones run the sight dispatch; otherwise the setup runs and the alt-state swap (or
// AttackNoWeapon) picks the state.
int first_attack(Drone& d) {
    if (d.flags & flag::kFirstAttackDone) return 0;
    if (d.sys->config().blind_drones) return 0;   // switch_BLIND_DRONES
    const std::uint32_t lvl = d.sys->config().level_id;
    if (!(d.flags & flag::kSawPlayer)) {
        if (d.alert_flags & 0x70000000) {
            if (const int s = first_alert_state(d); s != 0) return s;
        }
        mark_reacted(d);
    } else {
        if (const int s = first_sight_state(d); s != 0) {
            mark_reacted(d);
            return s;
        }
        mark_reacted(d);
    }
    if (lvl != 0x7000009 && lvl != 0x700000a) {   // tower snipers keep their long sight
        if (d.sight_range < d.max_combat_dist) d.sight_range = d.max_combat_dist;
        d.sight_cone = 0.0f;
    }
    if (d.side == kSideEnemy && !(d.flags & flag::kFirstSightShoutSent) && d.opponent.valid()) {
        alert_others(d, kMsgShoutAttack, d.opponent, d.alert_pos);   // first-attack attack shout
        d.flags |= flag::kFirstSightShoutSent;
    }
    attack_talk(d);
    if (d.has_beh(beh::kFailsMissionOnFirstSight)) mission_fail(d, 8);
    if (d.alt_state == 0 || d.alt_channel_snapshot) return attack_no_weapon(d);
    d.initial_state = kStAttack;
    if (d.dtype_alt != 0) d.dtype = d.dtype_alt;
    d.dtype_alt = 0;
    d.alt_state = 0;
    if (d.alt_dmode == 100) d.active_behaviour = 1;   // DroneFunc_Set2ndBehaviour
    const int alt = d.alt_state;
    return alt != 0 && alt != 0xb4 ? alt : kStAttack;
}

// ---- Attack 0x56 (0x164210): ENTER-only dispatcher ----------------------------------------------------------------------------
// FirstAttack picks the state (or 0: alertness/attack setup, then AttackNoWeapon -> Combat/AimStand/...).
int state_attack(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter: {
        set_as_attacking(d);   // DroneFunc_SetAsAttacking 0x147d70: alt mode cleared, widened sight
        const int first = first_attack(d);
        if (first != 0) {
            d.set_state(first);
            return 1;
        }
        d.alertness = 1.0f;
        d.set_alert_status(AlertStatus::Alert);
        if (const int nw = attack_no_weapon(d); nw != 0) {
            d.set_state(nw);
            return 1;
        }
        if (d.dtype != kDtypeSniper && d.dtype != kDtypeSniperAlert) {
            d.flags |= flag::kFirstSightShoutSent | flag::kAware;
            invalidate_attack_route(d);
            if (!d.has_beh(beh::kAimStandPreferred) || d.engage_dist <= d.opp_dist) {
                d.set_state(kStCombat);
                return 1;
            }
            if ((d.sight_flags & sight::kSeen) == 0 || !d.opponent.valid()) {
                d.initial_state = kStAimStandFire;
                d.set_state(kStAimStand);
                return 1;
            }
        } else {
            d.set_state(kStSniperAim);
            return 1;
        }
        d.set_state(kStCombat);
        return 1;
    }
    default:
        return skeleton(d, m);
    }
}

// ---- Alerted1stEncounter 0x57: one-tick bridge into Attack ----------------------------------
int state_alerted_1st(Drone& d, const Msg& m) {
    if (m.id == kMsgNone) return 1;
    if (m.id == kMsgEnter || m.id == kMsgTick) {
        d.set_state(kStAttack);
        return 1;
    }
    return skeleton(d, m);
}

// ---- Combat 0x58: thin dispatcher, re-runs the selector every tick --------------------------
int state_combat(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.flags |= flag::kAware;   // lingering states re-arm perception (cleared on transition)
        return 1;
    case kMsgTick:
        sp_of(d).note_attacker(d);
        if (const int next = combat_tick(d); next != 0) d.set_state(next);
        return 1;
    default:
        return skeleton(d, m);
    }
}

// ---- CombatNoMove 0x59: planted firing state -------------------------------------------------
int state_combat_no_move(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.flags |= flag::kAware;
        set_angle_to_obj(d, d.opponent, 0.0f, true);
        [[fallthrough]];
    case kMsgTick: {
        sp_of(d).note_attacker(d);
        const int next = enemy_look_for_opponent(d);
        if (next != 0) {
            d.set_state(next);
            return 1;
        }
        if (!d.opponent.valid() || d.lost_frames > 30) {
            d.set_state(kStNoOpponent);
            return 1;
        }
        aim_flag(d, kAimStand);
        return 1;
    }
    default:
        return skeleton(d, m);
    }
}

// ---- CombatOutOfRange 0x5a: close to max_combat_dist ------------------------------------------
int state_combat_out_of_range(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.flags |= flag::kAware;
        return 1;
    case kMsgTick: {
        sp_of(d).note_attacker(d);
        if (const int next = combat_tick(d); next != 0) d.set_state(next);
        return 1;
    }
    default:
        return skeleton(d, m);
    }
}

// ---- CombatNewSighting 0x5b: aim pause on regained sight ---------------------------------------
int state_combat_new_sighting(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.flags |= flag::kAware;
        set_angle_to_obj(d, d.opponent, 0.0f, true);
        anim_call(d, 0, kAimStand);
        d.timer1 = {d.now() + d.seconds(0.5f), 0};
        return 1;
    case kMsgTimer1:
        d.set_state(kStCombat);
        return 1;
    case kMsgTick:
        aim_flag(d, kAimStand);
        return 1;
    default:
        return skeleton(d, m);
    }
}

// ---- CombatNoSight 0x5c: push to the last known position ----------------------------------------
int state_combat_no_sight(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.flags |= flag::kAware;
        set_ai_goal(d, d.opp_last_known, 2.0f);
        return 1;
    case kMsgTick:
        sp_of(d).note_attacker(d);
        if (const int next = combat_tick(d); next != 0) d.set_state(next);
        return 1;
    default:
        return skeleton(d, m);
    }
}

// ---- CombatTooClose 0x5d -> back off ------------------------------------------------------------
int state_combat_too_close(Drone& d, const Msg& m) {
    if (m.id == kMsgNone) return 1;
    if (m.id == kMsgEnter || m.id == kMsgTick) {
        d.set_state(kStAimBackoff);
        return 1;
    }
    return skeleton(d, m);
}

// ---- CombatWait 0x5e -----------------------------------------------------------------------------
int state_combat_wait(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.flags |= flag::kAware;
        anim_call(d, 0, kStandAlert);
        d.timer1 = {d.now() + d.seconds(1.0f), 0};
        return 1;
    case kMsgTimer1:
        d.set_state(kStCombat);
        return 1;
    case kMsgTick:
        sp_of(d).note_attacker(d);
        if (const int next = combat_tick(d); next != 0) d.set_state(next);
        return 1;
    default:
        return skeleton(d, m);
    }
}

// ---- CombatNoRoute 0x5f: evasive move or hold ------------------------------------------------------
int state_combat_no_route(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter: {
        d.flags |= flag::kAware;
        const int evade = evasive_move(d);   // NDrone2_EvasiveMove: strafe/dodge/roll/step state or 0
        if (evade != 0) d.set_state(evade);
        return 1;
    }
    case kMsgTick:
        d.set_state(kStCombat);
        return 1;
    default:
        return skeleton(d, m);
    }
}


// ---- NoOpponent 0x60 -------------------------------------------------------------------------------
int state_no_opponent(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        anim_call(d, 0, kStandAlertLook);
        set_idle_timeout(d, 3, 3);
        return 1;
    case kMsgTimeout:
        d.set_state(kStSearchArea);
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

// ---- DrawWeapon 0x66 ---------------------------------------------------------------------------------
int state_draw_weapon(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.flags |= flag::kAware;   // NDrone2_DSTATE_DrawWeapon: armed drones perceive from here on
        d.weapon = 6;   // DroneWeap_ChangeWeapon(6): fit the standard gun (kind goes above 1)
        anim_call(d, 0, kDraw, 0, kStAttack);   // end state resumes the fight
        return 1;
    case kMsgTick:
        return 1;
    default:
        return skeleton(d, m);
    }
}

// Aim-state boilerplate: ENTER faces + plays the clip, TICK fires, msg 5 (anim end) advances.
int aim_enter(Drone& d, int dasc, int timer_frames = 0) {
    set_angle_to_obj(d, d.opponent, 0.0f, true);
    anim_call(d, 0, dasc);
    if (timer_frames > 0) d.timer1 = {d.now() + std::uint32_t(timer_frames), 0};
    return 1;
}

// ---- AimStand 0x67 / AimStandFire 0x68 ------------------------------------------------------------------
int state_aim_stand(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.flags |= flag::kAware;
        return aim_enter(d, kAimStand);
    case kMsgTick:
        aim_flag(d, kAimStand);
        if (d.burst_done) {
            d.burst_done = false;
            d.set_state(kStCombat);
        }
        return 1;
    default:
        return skeleton(d, m);
    }
}

int state_aim_stand_fire(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.flags |= flag::kAware;
        return aim_enter(d, kAimStand);
    case kMsgTick:
        sp_of(d).note_attacker(d);
        if (const int next = combat_tick(d); next != 0) d.set_state(next);
        return 1;
    default:
        return skeleton(d, m);
    }
}

// ---- AimStandReload 0x69: refill the clip, then resume ------------------------------------------------------
int state_aim_stand_reload(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        anim_call(d, 0, kReload, 0, kStCombat);
        return 1;
    case kMsgAnimResume: {
        SpExt& e = sx(d);   // Drone+0xbbc clip refill
        e.ammo = e.ammo_max;
        d.set_state(kStCombat);
        return 1;
    }
    case kMsgTick:
        return 1;
    default:
        return skeleton(d, m);
    }
}

// ---- AimStandDiscard 0x6a: drop the weapon, draw the sidearm ----------------------------------------------------
int state_aim_stand_discard(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        anim_call(d, 0, kDiscard, 0, kStDrawWeapon);
        d.weapon = 0;
        return 1;
    case kMsgTick:
        return 1;
    default:
        return skeleton(d, m);
    }
}

// ---- Prone 0x6b / ProneFire 0x6c -------------------------------------------------------------------------------
int state_prone(Drone& d, const Msg& m) {
    if (m.id == kMsgNone) return 1;
    if (m.id == kMsgEnter) {
        anim_call(d, 0, kProne);
        return 1;
    }
    if (m.id == kMsgTick) {
        d.set_state(kStProneFire);
        return 1;
    }
    return skeleton(d, m);
}

int state_prone_fire(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.flags |= flag::kAware;
        return aim_enter(d, kProne);
    case kMsgTick:
        aim_flag(d, kProne);
        if (!d.opponent.valid() || d.lost_frames > 30) d.set_state(kStCombat);
        return 1;
    default:
        return skeleton(d, m);
    }
}

// ---- AimBackoff 0x6d: step away while aiming -----------------------------------------------------------------------
int state_aim_backoff(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        if (!can_backoff(d)) {   // NDrone2_CanBackoff: DASC 4, behaviour 6, 1.8 m
            d.set_state(kStCombatNoMove);
            return 1;
        }
        anim_call(d, 0, kAimBackoff);
        return 1;
    case kMsgAnimResume:
        d.set_state(kStCombat);
        return 1;
    case kMsgTick:
        set_angle_to_obj(d, d.opponent, 0.0f);
        return 1;
    default:
        return skeleton(d, m);
    }
}

// ---- CrouchCover 0x6e / AimCrouch 0x6f / AimCrouchFire 0x70 / AimCrouchReload 0x71 ---------------------------
int state_crouch_cover(Drone& d, const Msg& m) {
    if (m.id == kMsgNone) return 1;
    if (m.id == kMsgEnter) {
        anim_call(d, 0, kCrouchCover);
        return 1;
    }
    if (m.id == kMsgTick) {
        d.set_state(kStAimCrouch);
        return 1;
    }
    return skeleton(d, m);
}

int state_aim_crouch(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.flags |= flag::kAware;
        return aim_enter(d, kAimCrouch);
    case kMsgTick:
        aim_flag(d, kAimCrouch);
        if (d.burst_done) {
            d.burst_done = false;
            d.set_state(kStCombat);
        }
        return 1;
    default:
        return skeleton(d, m);
    }
}

int state_aim_crouch_fire(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.flags |= flag::kAware;
        return aim_enter(d, kAimCrouch);
    case kMsgTick:
        sp_of(d).note_attacker(d);
        if (const int next = combat_tick(d); next != 0) d.set_state(next);
        return 1;
    default:
        return skeleton(d, m);
    }
}

int state_aim_crouch_reload(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        anim_call(d, 0, kReload, 0, kStAimCrouch);
        return 1;
    case kMsgAnimResume: {
        SpExt& e = sx(d);
        e.ammo = e.ammo_max;
        d.set_state(kStAimCrouch);
        return 1;
    }
    case kMsgTick:
        return 1;
    default:
        return skeleton(d, m);
    }
}

// ---- AltAttack 0x72: special attack (flamethrower etc.), fires while the clip plays -------------------------------
int state_alt_attack(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        return aim_enter(d, kAimSpecial);
    case kMsgAnimResume:
        d.set_state(kStCombat);
        return 1;
    case kMsgTick:
        aim_flag(d, kAimSpecial);
        return 1;
    default:
        return skeleton(d, m);
    }
}

// ---- combat moves 0x73..0x7a: step / strafe / dodge / roll -------------------------------------------------------------
int move_state(Drone& d, const Msg& m, int dasc, bool firing) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        set_angle_to_obj(d, d.opponent, 0.0f, true);
        anim_call(d, 0, dasc);
        return 1;
    case kMsgAnimResume:
        d.set_state(kStCombat);
        return 1;
    case kMsgTick:
        if (firing) d.fire_requested = !d.fire_lock;   // HandleFiring shoots; the clip keeps playing
        return 1;
    default:
        return skeleton(d, m);
    }
}

// Named wrappers: register_state takes a plain function pointer, so each move gets one.
int state_step_aim_left(Drone& d, const Msg& m) { return move_state(d, m, kStepLeft, true); }
int state_step_aim_right(Drone& d, const Msg& m) { return move_state(d, m, kStepRight, true); }
int state_strafe_aim_left(Drone& d, const Msg& m) { return move_state(d, m, kAimStrafeLeft, true); }
int state_strafe_aim_right(Drone& d, const Msg& m) { return move_state(d, m, kAimStrafeRight, true); }
int state_strafe_dodge_left(Drone& d, const Msg& m) { return move_state(d, m, kStrafeDodgeLeft, false); }
int state_strafe_dodge_right(Drone& d, const Msg& m) { return move_state(d, m, kStrafeDodgeRight, false); }
int state_roll_left(Drone& d, const Msg& m) { return move_state(d, m, kRollLeft, false); }
int state_roll_right(Drone& d, const Msg& m) { return move_state(d, m, kRollRight, false); }

// ---- GrenadeThrow 0x2d (DroneWeap_ThrowGrenade target) --------------------------------------------------------------------
int state_grenade_throw(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        set_angle_to_obj(d, d.opponent, 0.0f, true);
        anim_call(d, 0, kGrenade, 0, kStCombat);
        return 1;
    case kMsgTick:
        return 1;
    default:
        return skeleton(d, m);
    }
}

// ---- Sniper 0x29..0x2c: stationary aim / fire / reload cycle ------------------------------------------------------------------
int state_sniper_idle(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.flags |= flag::kAware;
        anim_call(d, 0, kStandAlert);
        set_idle_timeout(d, 2, 2);
        return 1;
    case kMsgTimeout:
        d.set_state(kStSniperAim);
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

int state_sniper_aim(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.flags |= flag::kAware;
        set_angle_to_obj(d, d.opponent, 0.0f, true);
        anim_call(d, 0, kAimStand);
        d.timer1 = {d.now() + d.seconds(1.0f), 0};
        return 1;
    case kMsgTimer1:
        d.set_state(kStSniperFire);
        return 1;
    case kMsgTick:
        set_angle_to_obj(d, d.opponent, 0.0f);
        return 1;
    default:
        return skeleton(d, m);
    }
}

int state_sniper_fire(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.flags |= flag::kAware;
        set_angle_to_obj(d, d.opponent, 0.0f, true);
        anim_call(d, 0, kShoot);
        return 1;
    case kMsgAnimResume:
        d.set_state(kStSniperReload);
        return 1;
    case kMsgTick:
        d.fire_requested = !d.fire_lock;   // HandleFiring shoots post-move
        return 1;
    default:
        return skeleton(d, m);
    }
}

int state_sniper_reload(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        anim_call(d, 0, kReload, 0, kStSniperIdle);
        return 1;
    case kMsgTick:
        return 1;
    default:
        return skeleton(d, m);
    }
}

}  // namespace

// DroneFunc_ConsiderExplosive 0x1431c0 (msg 0x21, arg = explosive object): drones that are not
// hiding already fill the scary-object record (Drone+0x324..) and flee to RunAwayFromObject.
void consider_explosive(Drone& d, const Msg& m, int default_state) {
    using namespace nf::drone;
    SpExt& e = sx(d);
    if (e.scared_hiding) return;
    e.scary.until = d.now() + d.seconds(5.0f);
    e.scary.pos = d.alert_pos;   // last reported explosive position (AlertRecord of msg 0x21)
    e.scary.radius = 8.0f;       // safety distance
    e.scary.return_state = default_state;
    d.set_state(kStRunAwayFromObject);
    (void)m;
}

void register_combat_states() {
    using drone::register_state;
    register_state(kStAttack, "Attack", state_attack);
    register_state(kStAlerted1stEncounter, "Alerted1stEncounter", state_alerted_1st);
    register_state(kStCombat, "Combat", state_combat);
    register_state(kStCombatNoMove, "CombatNoMove", state_combat_no_move);
    register_state(kStCombatOutOfRange, "CombatOutOfRange", state_combat_out_of_range);
    register_state(kStCombatNewSighting, "CombatNewSighting", state_combat_new_sighting);
    register_state(kStCombatNoSight, "CombatNoSight", state_combat_no_sight);
    register_state(kStCombatTooClose, "CombatTooClose", state_combat_too_close);
    register_state(kStCombatWait, "CombatWait", state_combat_wait);
    register_state(kStCombatNoRoute, "CombatNoRoute", state_combat_no_route);
    register_state(kStNoOpponent, "NoOpponent", state_no_opponent);
    register_state(kStDrawWeapon, "DrawWeapon", state_draw_weapon);
    register_state(kStAimStand, "AimStand", state_aim_stand);
    register_state(kStAimStandFire, "AimStandFire", state_aim_stand_fire);
    register_state(kStAimStandReload, "AimStandReload", state_aim_stand_reload);
    register_state(kStAimStandDiscard, "AimStandDiscard", state_aim_stand_discard);
    register_state(kStProne, "Prone", state_prone);
    register_state(kStProneFire, "ProneFire", state_prone_fire);
    register_state(kStAimBackoff, "AimBackoff", state_aim_backoff);
    register_state(kStCrouchCover, "CrouchCover", state_crouch_cover);
    register_state(kStAimCrouch, "AimCrouch", state_aim_crouch);
    register_state(kStAimCrouchFire, "AimCrouchFire", state_aim_crouch_fire);
    register_state(kStAimCrouchReload, "AimCrouchReload", state_aim_crouch_reload);
    register_state(kStAltAttack, "AltAttack", state_alt_attack);
    // Step / strafe / dodge / roll (0x73..0x7a) share the move skeleton; firing ones aim while moving.
    register_state(kStStepAimLeft, "StepAimLeft", state_step_aim_left);
    register_state(kStStepAimRight, "StepAimRight", state_step_aim_right);
    register_state(kStStrafeAimLeft, "StrafeAimLeft", state_strafe_aim_left);
    register_state(kStStrafeAimRight, "StrafeAimRight", state_strafe_aim_right);
    register_state(kStStrafeDodgeLeft, "StrafeDodgeLeft", state_strafe_dodge_left);
    register_state(kStStrafeDodgeRight, "StrafeDodgeRight", state_strafe_dodge_right);
    register_state(kStRollLeftCrouch, "RollLeftCrouch", state_roll_left);
    register_state(kStRollRightCrouch, "RollRightCrouch", state_roll_right);
    register_state(kStGrenadeThrow, "GrenadeThrow", state_grenade_throw);
    register_state(kStSniperIdle, "SniperIdle", state_sniper_idle);
    register_state(kStSniperAim, "SniperAim", state_sniper_aim);
    register_state(kStSniperFire, "SniperFire", state_sniper_fire);
    register_state(kStSniperReload, "SniperReload", state_sniper_reload);
}

}  // namespace nf::sp

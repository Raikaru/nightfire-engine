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

// DroneFunc_CombatState 0x148338: per-tick selector of the combat family. Constants from the
// spec: opponent lost > 30 frames -> NoOpponent; geometry/sight/ammo pick the rest.
int combat_state(Drone& d) {
    if (!d.opponent.valid() || d.lost_frames > 30) return kStNoOpponent;
    if ((d.sight_flags & sight::kSeen) == 0) {
        if (d.opp_dist > d.max_combat_dist) return kStCombatOutOfRange;
        return kStCombatNoSight;
    }
    if (d.opp_dist < 3.0f) return kStCombatTooClose;
    if (d.opp_dist > d.max_combat_dist) return kStCombatOutOfRange;
    const SpExt& e = sx(d);
    if (e.ammo_max > 0 && e.ammo == 0) return kStAimStandReload;
    if (d.has_beh(beh::kNeverMovesInCombat)) return kStCombatNoMove;
    return kStCombatNoMove;
}

// Face the opponent and run the firing cadence for one tick (AimStandFire / AimCrouchFire core).
void aim_and_fire(Drone& d) {
    set_angle_to_obj(d, d.opponent, 0.0f);
    d.fire_requested = !d.fire_lock;
    if (d.fire_requested) weap::do_firing(d);
}

// ---- Attack 0x56 (0x164210) ---------------------------------------------------------------
int state_attack(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        set_as_attacking(d);   // DroneFunc_SetAsAttacking 0x147d70: alertness 1, widened sight
        attack_talk(d);
        return 1;
    case kMsgTick:
        sp_of(d).note_attacker(d);   // NDrone2_SeenAndAttacking 0x145150 (music/attacker count)
        d.set_state(combat_state(d));
        return 1;
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
        return 1;
    case kMsgTick:
        sp_of(d).note_attacker(d);
        d.set_state(combat_state(d));
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
        set_angle_to_obj(d, d.opponent, 0.0f, true);
        return 1;
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
        anim_call(d, 0, kAimStand);
        aim_and_fire(d);
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
        return 1;
    case kMsgTick: {
        sp_of(d).note_attacker(d);
        if (!d.opponent.valid()) {
            d.set_state(kStNoOpponent);
            return 1;
        }
        move_to_object(d, d.opponent, d.max_combat_dist);
        set_angle_to_dest(d);
        anim_for_dist(d, d.mv.route_distance);
        if (d.mv.route_status == RouteStatus::Arrived) d.set_state(kStCombat);
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
        set_angle_to_obj(d, d.opponent, 0.0f, true);
        anim_call(d, 0, kAimStand);
        d.timer1 = {d.now() + d.seconds(0.5f), 0};
        return 1;
    case kMsgTimer1:
        d.set_state(kStCombat);
        return 1;
    case kMsgTick:
        aim_and_fire(d);
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
        set_ai_goal(d, d.opp_last_known, 2.0f);
        return 1;
    case kMsgTick: {
        const int next = enemy_look_for_opponent(d);
        if (next != 0) {
            d.set_state(next);
            return 1;
        }
        const int status = move_to_ai_goal(d);
        set_angle_to_dest(d);
        if (status == int(RouteStatus::Following) || status == int(RouteStatus::Straight))
            anim_call(d, 0, kRun);
        else
            d.set_state(kStNoOpponent);
        return 1;
    }
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
        anim_call(d, 0, kStandAlert);
        d.timer1 = {d.now() + d.seconds(1.0f), 0};
        return 1;
    case kMsgTimer1:
        d.set_state(kStCombat);
        return 1;
    case kMsgTick:
        set_angle_to_obj(d, d.opponent, 0.0f);
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
        return aim_enter(d, kAimStand);
    case kMsgTick:
        aim_and_fire(d);
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
        return aim_enter(d, kAimStand);
    case kMsgTick:
        aim_and_fire(d);
        if (d.burst_done || !d.opponent.valid()) {
            d.burst_done = false;
            d.set_state(kStCombat);
        }
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
        return aim_enter(d, kProne);
    case kMsgTick:
        aim_and_fire(d);
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
        return aim_enter(d, kAimCrouch);
    case kMsgTick:
        aim_and_fire(d);
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
        return aim_enter(d, kAimCrouch);
    case kMsgTick:
        aim_and_fire(d);
        if (d.burst_done || !d.opponent.valid()) {
            d.burst_done = false;
            d.set_state(kStCombat);
        }
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
        aim_and_fire(d);
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
        if (firing) aim_and_fire(d);
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
        set_angle_to_obj(d, d.opponent, 0.0f, true);
        anim_call(d, 0, kShoot);
        return 1;
    case kMsgAnimResume:
        d.set_state(kStSniperReload);
        return 1;
    case kMsgTick:
        d.fire_requested = !d.fire_lock;
        if (d.fire_requested) weap::do_firing(d);
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

// Special states (docs/spec-arena-ai.md Part 3 §9/§10): death / fade, taser / stun /
// smoke reactions, impact states, abseil, the ninja boss, the astronaut boss, and the
// framework leftovers (DeleteMe, FailMission, JustStand, testers, doors, ActionAnim,
// seen-body reactions, ElevatorJumper).
#include "game/sp_civilian_util.hpp"
#include "game/sp_common.hpp"
#include "game/sp_idle.hpp"
#include "game/sp_states.hpp"

namespace nf::sp {

using namespace nf::drone;

namespace {

int skeleton(Drone& d, const Msg& m) { return skel_common(d, m, kStAttack) ? 1 : 0; }

// ---- Death_Anim 0x44 / DeathByExplosion 0x45 / SpecialDeath_Anim 0x46 -------------------------------
// NDrone2_BulletImpact routes here at health <= 0 (DroneFunc_OnInitDeath ran first).
int state_death_anim(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        notify_death(d);   // DroneFunc_SetDeathChannel: switch_channels[Drone+0x13c]
        talk(d, Speech::Death);
        return 1;
    case kMsgTick:
        return 1;
    default:
        return 1;   // the dead ignore the world
    }
}

int state_death_by_explosion(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        notify_death(d);
        talk(d, Speech::Death);
        anim_call(d, 0, kDeathExplosive, 0, kStDead);
        return 1;
    case kMsgTick:
        return 1;
    default:
        return 1;
    }
}

int state_special_death_anim(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        notify_death(d);
        talk(d, Speech::Death);
        anim_call(d, 0, kDeathDir, 0, kStDead);
        return 1;
    case kMsgTick:
        return 1;
    default:
        return 1;
    }
}

// ---- Dead 0x47 / Fade 0x48 / FadeFast 0x49 -------------------------------------------------------------------
int state_dead(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        anim_call(d, 0, kDead);
        set_idle_timeout(d, 10, 5);   // corpse lingers, then fades
        return 1;
    case kMsgTimeout:
        d.set_state(kStFade);
        return 1;
    case kMsgTick:
        return 1;
    default:
        return 1;
    }
}

int state_fade(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        set_idle_timeout(d, 3, 0);
        return 1;
    case kMsgTick:
        d.fade -= 1.0f / 90.0f;   // ~3 s fade at 30 Hz
        if (d.fade <= 0) {
            d.fade = 0;
            d.set_state(kStDeleteMe);
        }
        return 1;
    case kMsgTimeout:
        d.set_state(kStDeleteMe);
        return 1;
    default:
        return 1;
    }
}

int state_fade_fast(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        set_idle_timeout(d, 1, 0);
        return 1;
    case kMsgTick:
        d.fade -= 1.0f / 30.0f;
        if (d.fade <= 0) {
            d.fade = 0;
            d.set_state(kStDeleteMe);
        }
        return 1;
    case kMsgTimeout:
        d.set_state(kStDeleteMe);
        return 1;
    default:
        return 1;
    }
}

// ---- Taser 0x4a / Stunned 0x4b / Stunned_Recover 0x4c ------------------------------------------------------------------
int state_taser(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        anim_call(d, 0, d.taser_hits > 2 ? kTaser2 : kTaser1, 0, kStStunned);
        return 1;
    case kMsgTick:
        return 1;
    default:
        return 1;
    }
}

int state_stunned(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        anim_call(d, 0, kStunned1);
        set_idle_timeout(d, 4, 2);
        return 1;
    case kMsgTimeout:
        d.set_state(kStStunned_Recover);
        return 1;
    case kMsgTick:
        return 1;
    default:
        return 1;
    }
}

int state_stunned_recover(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        anim_call(d, 0, kStunned2, 0, kStAttack);
        return 1;
    case kMsgTick:
        return 1;
    default:
        return skeleton(d, m);
    }
}

// ---- StunGrenade 0x4d..0x4f / StunDart 0x50..0x52 -------------------------------------------------------------------------------
// Impact -> loop (eyes covered) -> recover.
int state_stun_grenade_impact(Drone& d, const Msg& m) {
    if (m.id == kMsgNone) return 1;
    if (m.id == kMsgEnter || m.id == kMsgTick) {
        anim_call(d, 0, kStunGrenade1, 0, kStStunGrenadeLoop);
        return 1;
    }
    return 1;
}

int state_stun_grenade_loop(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        anim_call(d, 0, kStunGrenade2);
        set_idle_timeout(d, 5, 3);
        return 1;
    case kMsgTimeout:
        d.set_state(kStStunGrenadeRecover);
        return 1;
    case kMsgTick:
        return 1;
    default:
        return 1;
    }
}

int state_stun_grenade_recover(Drone& d, const Msg& m) {
    if (m.id == kMsgNone) return 1;
    if (m.id == kMsgEnter) {
        anim_call(d, 0, kCStand, 0, kStAttack);
        return 1;
    }
    if (m.id == kMsgTick) return 1;
    return skeleton(d, m);
}

int state_stun_dart_impact(Drone& d, const Msg& m) {
    if (m.id == kMsgNone) return 1;
    if (m.id == kMsgEnter || m.id == kMsgTick) {
        anim_call(d, 0, kStunDart1, 0, kStStunDartLoop);
        return 1;
    }
    return 1;
}

int state_stun_dart_loop(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        anim_call(d, 0, kStunDart2);
        set_idle_timeout(d, 4, 2);
        return 1;
    case kMsgTimeout:
        d.set_state(kStStunDartRecover);
        return 1;
    case kMsgTick:
        return 1;
    default:
        return 1;
    }
}

int state_stun_dart_recover(Drone& d, const Msg& m) {
    if (m.id == kMsgNone) return 1;
    if (m.id == kMsgEnter) {
        anim_call(d, 0, kCStand, 0, kStAttack);
        return 1;
    }
    if (m.id == kMsgTick) return 1;
    return skeleton(d, m);
}

// ---- PunchImpact 0x53 / ExplosiveImpact 0x54 / BulletImpact 0x55 ------------------------------------------------------------------
// DroneFunc_HandleImpact targets: flinch clips, then back to the return state.
int state_punch_impact(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        talk(d, Speech::Pain);
        anim_call(d, 0, kPunched, 0, kStAttack);
        return 1;
    case kMsgTick:
        return 1;
    default:
        return skeleton(d, m);
    }
}

int state_explosive_impact(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        talk(d, Speech::Pain);
        anim_call(d, 0, kExplosive, 0, kStAttack);
        return 1;
    case kMsgTick:
        return 1;
    default:
        return skeleton(d, m);
    }
}

int state_bullet_impact(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        if (!d.alive()) {   // killed by the burst: death anim handles the rest
            d.set_state(kStDeath_Anim);
            return 1;
        }
        talk(d, Speech::Pain);
        anim_call(d, 0, kImpact, 0, kStAttack);
        return 1;
    case kMsgTick:
        return 1;
    default:
        return skeleton(d, m);
    }
}

// ---- SmokedOut 0x7b..0x7d ----------------------------------------------------------------------------------------------------------------
int state_smoked_out(Drone& d, const Msg& m) {
    if (m.id == kMsgNone) return 1;
    if (m.id == kMsgEnter || m.id == kMsgTick) {
        anim_call(d, 0, kSmoked, 0, kStSmokedOut_Loop);
        return 1;
    }
    return 1;
}

int state_smoked_out_loop(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        anim_call(d, 0, kSmoked);
        set_idle_timeout(d, 5, 3);
        return 1;
    case kMsgTimeout:
        d.set_state(kStSmokedOut_Recover);
        return 1;
    case kMsgTick:
        return 1;
    default:
        return 1;
    }
}

int state_smoked_out_recover(Drone& d, const Msg& m) {
    if (m.id == kMsgNone) return 1;
    if (m.id == kMsgEnter) {
        anim_call(d, 0, kCStand, 0, kStAttack);
        return 1;
    }
    if (m.id == kMsgTick) return 1;
    return skeleton(d, m);
}

// ---- SeenDeadBody 0x9e / SeenSurrenderedDrone 0x9f -------------------------------------------------------------------------
// Awareness pings: look at the body, then escalate to SearchArea.
int state_seen_dead_body(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.flags |= flag::kAware;
        set_angle_to_obj(d, d.opponent, 0.0f, true);
        anim_call(d, 0, kStandAlertLook);
        set_idle_timeout(d, 3, 2);
        return 1;
    case kMsgTimeout:
        d.set_state(kStSearchArea);
        return 1;
    case kMsgTick:
        return 1;
    default:
        return skeleton(d, m);
    }
}

int state_seen_surrendered(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.flags |= flag::kAware;
        anim_call(d, 0, kStandAlertLook);
        set_idle_timeout(d, 2, 2);
        return 1;
    case kMsgTimeout:
        d.set_state(d.initial_state);
        return 1;
    case kMsgTick:
        return 1;
    default:
        return skeleton(d, m);
    }
}

// ---- HoldItRightThere 0x80: armed standoff before the fight ------------------------------------------------------------------
int state_hold_it(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.flags |= flag::kAware;
        if (d.opponent.valid()) set_angle_to_obj(d, d.opponent, 0.0f, true);
        anim_call(d, 0, kAimStand);
        talk(d, Speech::Other);
        set_idle_timeout(d, 3, 2);
        return 1;
    case kMsgTimeout:
        d.set_state(kStAttack);
        return 1;
    case kMsgTick:
        return 1;
    default:
        return skeleton(d, m);
    }
}

// ---- OpenDoor 0x81 / KickObject 0x82 / ActionAnim 0x83 ----------------------------------------------------------------------------
int state_open_door(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.flags |= flag::kAware;
        anim_call(d, 0, kCStand, 0, sx(d).return_state2 != 0 ? sx(d).return_state2 : kStPatrol);
        return 1;
    case kMsgTick:
        return 1;
    default:
        return skeleton(d, m);
    }
}

int state_kick_object(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        anim_call(d, 0, kKick, 0, kStPatrol);
        return 1;
    case kMsgTick:
        return 1;
    default:
        return skeleton(d, m);
    }
}

int state_action_anim(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        anim_call(d, 0, kIdleAnim, 0, sx(d).return_state2);
        return 1;
    case kMsgTick:
        return 1;
    default:
        return skeleton(d, m);
    }
}

// ---- ElevatorJumper 0x8d: rides the scripted lift, then resumes ----------------------------------------------------------------------
int state_elevator_jumper(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.flags |= flag::kAware;
        anim_call(d, 0, kStand);
        set_idle_timeout(d, 6, 0);
        return 1;
    case kMsgTimeout:
        d.set_state(d.initial_state);
        return 1;
    case kMsgTick:
        return 1;
    default:
        return skeleton(d, m);
    }
}

// ---- Abseil 0x8e..0x92 ----------------------------------------------------------------------------------------------------------------------------
int state_abseil_init(Drone& d, const Msg& m) {
    if (m.id == kMsgNone) return 1;
    if (m.id == kMsgEnter || m.id == kMsgTick) {
        anim_call(d, 0, kAbseilHang, 0, kStAbseilSlide);
        return 1;
    }
    return 1;
}

int state_abseil_slide(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        anim_call(d, 0, kAbseilSlide);
        set_idle_timeout(d, 4, 0);
        return 1;
    case kMsgTimeout:
        d.set_state(kStAbseilHang);
        return 1;
    case kMsgTick:
        return 1;
    default:
        return 1;
    }
}

int state_abseil_hang(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        anim_call(d, 0, kAbseilHang);
        set_idle_timeout(d, 2, 0);
        return 1;
    case kMsgTimeout:
        d.set_state(kStAbseilStepOff);
        return 1;
    case kMsgTick:
        return 1;
    default:
        return 1;
    }
}

int state_abseil_step_off(Drone& d, const Msg& m) {
    if (m.id == kMsgNone) return 1;
    if (m.id == kMsgEnter) {
        anim_call(d, 0, kCStand, 0, kStAttack);
        return 1;
    }
    if (m.id == kMsgTick) return 1;
    return skeleton(d, m);
}

int state_abseil_death(Drone& d, const Msg& m) {
    if (m.id == kMsgNone || m.id == kMsgEnter || m.id == kMsgTick) {
        if (m.id == kMsgEnter) notify_death(d);
        anim_call(d, 0, kDeathFall, 0, kStDead);
        return 1;
    }
    return 1;
}

// ---- Ninja 0xa6..0xb3 (DTYPE 0xd boss) -------------------------------------------------------------------------------------------------
// Melee boss: closes range with flips, sword at short range, gun at long range.
int state_ninja_stand(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.flags |= flag::kAware;
        anim_call(d, 0, kzNinjaStand);
        return 1;
    case kMsgTick: {
        if (!d.opponent.valid()) return 1;
        if (d.opp_dist > 15.0f) d.set_state(kStNinjaAttackLongRange);
        else if (d.opp_dist > 4.0f) d.set_state(kStNinjaGetCloseToPlayer);
        else d.set_state(kStNinjaAttackShortRange);
        return 1;
    }
    default:
        return skeleton(d, m);
    }
}

int state_ninja_attack(Drone& d, const Msg& m) {
    if (m.id == kMsgNone) return 1;
    if (m.id == kMsgEnter || m.id == kMsgTick) {
        d.set_state(kStNinjaStand);
        return 1;
    }
    return skeleton(d, m);
}

int ninja_close(Drone& d, int arrived_state, int move_dasc) {
    if (!d.opponent.valid()) {
        d.set_state(kStNinjaStand);
        return 1;
    }
    set_angle_to_obj(d, d.opponent, 0.0f);
    const int status = move_to_object(d, d.opponent, 2.0f);
    if (status == int(RouteStatus::Following) || status == int(RouteStatus::Straight))
        anim_call(d, 0, move_dasc);
    else
        d.set_state(arrived_state);
    return 1;
}

int state_ninja_long_range(Drone& d, const Msg& m) {
    if (m.id == kMsgNone) return 1;
    if (m.id == kMsgEnter) {
        anim_call(d, 0, kzNinjaAimStand);
        return 1;
    }
    if (m.id == kMsgTick) {
        if (d.opp_dist <= 15.0f) {
            d.set_state(kStNinjaGetCloseToPlayer);
            return 1;
        }
        d.set_state(kStNinjaStandFire);
        return 1;
    }
    return skeleton(d, m);
}

int state_ninja_mid_range(Drone& d, const Msg& m) {
    if (m.id == kMsgNone) return 1;
    if (m.id == kMsgEnter || m.id == kMsgTick) return ninja_close(d, kStNinjaSword, kzNinjaRun);
    return skeleton(d, m);
}

int state_ninja_short_range(Drone& d, const Msg& m) {
    if (m.id == kMsgNone) return 1;
    if (m.id == kMsgEnter || m.id == kMsgTick) {
        if (!d.opponent.valid()) {
            d.set_state(kStNinjaStand);
            return 1;
        }
        set_angle_to_obj(d, d.opponent, 0.0f, true);
        d.set_state(kStNinjaSword);
        return 1;
    }
    return skeleton(d, m);
}

int state_ninja_get_close(Drone& d, const Msg& m) {
    if (m.id == kMsgNone) return 1;
    if (m.id == kMsgEnter) d.flags |= flag::kAware;
    if (m.id == kMsgEnter || m.id == kMsgTick) return ninja_close(d, kStNinjaSword, kzNinjaStealthRun);
    return skeleton(d, m);
}

int state_ninja_sword(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        set_angle_to_obj(d, d.opponent, 0.0f, true);
        anim_call(d, 0, kzNinjaSwordAttack, 0, kStNinjaStand);
        return 1;
    case kMsgTick:
        return 1;
    default:
        return skeleton(d, m);
    }
}

int state_ninja_somersault(Drone& d, const Msg& m) {
    if (m.id == kMsgNone) return 1;
    if (m.id == kMsgEnter) {
        anim_call(d, 0, kzNinjaSomersault, 0, kStNinjaStand);
        return 1;
    }
    if (m.id == kMsgTick) return 1;
    return skeleton(d, m);
}

int state_ninja_backflip(Drone& d, const Msg& m) {
    if (m.id == kMsgNone) return 1;
    if (m.id == kMsgEnter) {
        anim_call(d, 0, kzNinjaBackflip, 0, kStNinjaStand);
        return 1;
    }
    if (m.id == kMsgTick) return 1;
    return skeleton(d, m);
}

int state_ninja_sideflip(Drone& d, const Msg& m) {
    if (m.id == kMsgNone) return 1;
    if (m.id == kMsgEnter || m.id == kMsgTick) {
        d.set_state(d.sys->rand_int(2) ? kStNinjaSideflipLeft : kStNinjaSideflipRight);
        return 1;
    }
    return skeleton(d, m);
}

int state_ninja_sideflip_left(Drone& d, const Msg& m) {
    if (m.id == kMsgNone) return 1;
    if (m.id == kMsgEnter) {
        anim_call(d, 0, kzNinjaFlipLeft, 0, kStNinjaStand);
        return 1;
    }
    if (m.id == kMsgTick) return 1;
    return skeleton(d, m);
}

int state_ninja_sideflip_right(Drone& d, const Msg& m) {
    if (m.id == kMsgNone) return 1;
    if (m.id == kMsgEnter) {
        anim_call(d, 0, kzNinjaFlipRight, 0, kStNinjaStand);
        return 1;
    }
    if (m.id == kMsgTick) return 1;
    return skeleton(d, m);
}

int state_ninja_stand_fire(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.flags |= flag::kAware;
        set_angle_to_obj(d, d.opponent, 0.0f, true);
        anim_call(d, 0, kzNinjaAimStand);
        return 1;
    case kMsgAnimResume:
        d.set_state(kStNinjaStand);
        return 1;
    case kMsgTick:
        set_angle_to_obj(d, d.opponent, 0.0f);
        d.fire_requested = !d.fire_lock;
        return 1;
    default:
        return skeleton(d, m);
    }
}

int state_ninja_no_route(Drone& d, const Msg& m) {
    if (m.id == kMsgNone) return 1;
    if (m.id == kMsgEnter || m.id == kMsgTick) {
        const int evade = evasive_move(d);
        d.set_state(evade != 0 ? evade : kStNinjaStand);
        return 1;
    }
    return skeleton(d, m);
}

// ---- Astronaut 0xbd..0xc2 (space boss, flight movement) ----------------------------------------------------------------------------
int state_astronaut_launch(Drone& d, const Msg& m) {
    if (m.id == kMsgNone) return 1;
    if (m.id == kMsgEnter) {
        anim_call(d, 0, kAstro_Hover, 0, kStAstronautCombat);
        return 1;
    }
    if (m.id == kMsgTick) return 1;
    return 1;
}

int state_astronaut_hit(Drone& d, const Msg& m) {
    if (m.id == kMsgNone) return 1;
    if (m.id == kMsgEnter) {
        anim_call(d, 0, kImpact, 0, kStAstronautCombat);
        return 1;
    }
    if (m.id == kMsgTick) return 1;
    return 1;
}

int state_astronaut_death(Drone& d, const Msg& m) {
    if (m.id == kMsgNone || m.id == kMsgEnter || m.id == kMsgTick) {
        if (m.id == kMsgEnter) notify_death(d);
        anim_call(d, 0, kAstro_Death1, 0, kStDead);
        return 1;
    }
    return 1;
}

int state_astronaut_combat(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        anim_call(d, 0, kAstro_Hover);
        d.flags |= flag::kAware;
        return 1;
    case kMsgTick:
        if (!d.opponent.valid()) return 1;
        set_angle_to_obj(d, d.opponent, 0.0f);
        if (d.opp_dist > 8.0f) d.set_state(kStAstronautCombatMove);
        else {
            d.fire_requested = !d.fire_lock;
            anim_call(d, 0, kShoot);
        }
        return 1;
    default:
        return skeleton(d, m);
    }
}

int state_astronaut_combat_move(Drone& d, const Msg& m) {
    if (m.id == kMsgNone) return 1;
    if (m.id == kMsgEnter) d.flags |= flag::kAware;
    if (m.id == kMsgEnter || m.id == kMsgTick) {
        if (!d.opponent.valid()) {
            d.set_state(kStAstronautCombat);
            return 1;
        }
        set_angle_to_obj(d, d.opponent, 0.0f);
        move_to_object(d, d.opponent, 6.0f);
        anim_call(d, 0, kAstro_MoveForward);
        if (d.mv.route_status == RouteStatus::Arrived) d.set_state(kStAstronautCombat);
        return 1;
    }
    return skeleton(d, m);
}

int state_space_drake(Drone& d, const Msg& m) {
    if (m.id == kMsgNone || m.id == kMsgEnter || m.id == kMsgTick) {
        d.set_state(kStAstronautCombat);
        return 1;
    }
    return skeleton(d, m);
}

// ---- framework leftovers: DeleteMe / FailMission / JustStand / testers / HangUp / WaitForever ----
int state_delete_me(Drone& d, const Msg& m) {
    if (m.id == kMsgNone || m.id == kMsgEnter || m.id == kMsgTick) {
        d.pending_delete = true;   // Drone_Delete at the end of the tick
        return 1;
    }
    return 1;
}

int state_fail_mission(Drone& d, const Msg& m) {
    if (m.id == kMsgNone || m.id == kMsgEnter || m.id == kMsgTick) {
        mission_fail(d, 0x1, 0x4000034);
        d.set_state(kStWaitForever);
        return 1;
    }
    return 1;
}

int state_just_stand(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        anim_call(d, 0, kStand);
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

int state_tester(Drone& d, const Msg& m, int next) {
    // Tester1..4 walk the mission route and chain into each other (animation test path).
    if (m.id == kMsgNone) return 1;
    if (m.id == kMsgEnter) {
        refind_mission_path(d);
        return 1;
    }
    if (m.id == kMsgTick) {
        const int status = move_to_ai_goal(d);
        if (status == int(RouteStatus::Following)) anim_call(d, 0, kWalk);
        else d.set_state(next);
        return 1;
    }
    return skeleton(d, m);
}
int state_tester1(Drone& d, const Msg& m) { return state_tester(d, m, kStTester2); }
int state_tester2(Drone& d, const Msg& m) { return state_tester(d, m, kStTester3); }
int state_tester3(Drone& d, const Msg& m) { return state_tester(d, m, kStTester4); }
int state_tester4(Drone& d, const Msg& m) { return state_tester(d, m, kStTester1); }

int state_hang_up(Drone& d, const Msg& m) {
    if (m.id == kMsgNone || m.id == kMsgEnter || m.id == kMsgTick) {
        anim_call(d, 0, kAbseilHang);
        return 1;
    }
    return 1;
}

int state_wait_forever(Drone&, const Msg& m) {
    if (m.id == kMsgNone || m.id == kMsgEnter || m.id == kMsgTick) return 1;
    return 0;
}

}  // namespace

void register_special_states() {
    using drone::register_state;
    register_state(kStDeath_Anim, "Death_Anim", state_death_anim);
    register_state(kStDeathByExplosion, "DeathByExplosion", state_death_by_explosion);
    register_state(kStSpecialDeath_Anim, "SpecialDeath_Anim", state_special_death_anim);
    register_state(kStDead, "Dead", state_dead);
    register_state(kStFade, "Fade", state_fade);
    register_state(kStFadeFast, "FadeFast", state_fade_fast);
    register_state(kStTaser, "Taser", state_taser);
    register_state(kStStunned, "Stunned", state_stunned);
    register_state(kStStunned_Recover, "Stunned_Recover", state_stunned_recover);
    register_state(kStStunGrenadeImpact, "StunGrenadeImpact", state_stun_grenade_impact);
    register_state(kStStunGrenadeLoop, "StunGrenadeLoop", state_stun_grenade_loop);
    register_state(kStStunGrenadeRecover, "StunGrenadeRecover", state_stun_grenade_recover);
    register_state(kStStunDartImpact, "StunDartImpact", state_stun_dart_impact);
    register_state(kStStunDartLoop, "StunDartLoop", state_stun_dart_loop);
    register_state(kStStunDartRecover, "StunDartRecover", state_stun_dart_recover);
    register_state(kStPunchImpact, "PunchImpact", state_punch_impact);
    register_state(kStExplosiveImpact, "ExplosiveImpact", state_explosive_impact);
    register_state(kStBulletImpact, "BulletImpact", state_bullet_impact);
    register_state(kStSmokedOut, "SmokedOut", state_smoked_out);
    register_state(kStSmokedOut_Loop, "SmokedOut_Loop", state_smoked_out_loop);
    register_state(kStSmokedOut_Recover, "SmokedOut_Recover", state_smoked_out_recover);
    register_state(kStSeenDeadBody, "SeenDeadBody", state_seen_dead_body);
    register_state(kStSeenSurrenderedDrone, "SeenSurrenderedDrone", state_seen_surrendered);
    register_state(kStHoldItRightThere, "HoldItRightThere", state_hold_it);
    register_state(kStOpenDoor, "OpenDoor", state_open_door);
    register_state(kStKickObject, "KickObject", state_kick_object);
    register_state(kStActionAnim, "ActionAnim", state_action_anim);
    register_state(kStElevatorJumper, "ElevatorJumper", state_elevator_jumper);
    register_state(kStAbseilInit, "AbseilInit", state_abseil_init);
    register_state(kStAbseilSlide, "AbseilSlide", state_abseil_slide);
    register_state(kStAbseilHang, "AbseilHang", state_abseil_hang);
    register_state(kStAbseilStepOff, "AbseilStepOff", state_abseil_step_off);
    register_state(kStAbseilDeath, "AbseilDeath", state_abseil_death);
    register_state(kStNinjaStand, "NinjaStand", state_ninja_stand);
    register_state(kStNinjaAttack, "NinjaAttack", state_ninja_attack);
    register_state(kStNinjaAttackLongRange, "NinjaAttackLongRange", state_ninja_long_range);
    register_state(kStNinjaAttackMidRange, "NinjaAttackMidRange", state_ninja_mid_range);
    register_state(kStNinjaAttackShortRange, "NinjaAttackShortRange", state_ninja_short_range);
    register_state(kStNinjaGetCloseToPlayer, "NinjaGetCloseToPlayer", state_ninja_get_close);
    register_state(kStNinjaSword, "NinjaSword", state_ninja_sword);
    register_state(kStNinjaSomersault, "NinjaSomersault", state_ninja_somersault);
    register_state(kStNinjaBackflip, "NinjaBackflip", state_ninja_backflip);
    register_state(kStNinjaSideflip, "NinjaSideflip", state_ninja_sideflip);
    register_state(kStNinjaSideflipLeft, "NinjaSideflipLeft", state_ninja_sideflip_left);
    register_state(kStNinjaSideflipRight, "NinjaSideflipRight", state_ninja_sideflip_right);
    register_state(kStNinjaStandFire, "NinjaStandFire", state_ninja_stand_fire);
    register_state(kStNinjaNoRoute, "NinjaNoRoute", state_ninja_no_route);
    register_state(kStAstronautLaunch, "AstronautLaunch", state_astronaut_launch);
    register_state(kStAstronautHit, "AstronautHit", state_astronaut_hit);
    register_state(kStAstronautDeath, "AstronautDeath", state_astronaut_death);
    register_state(kStAstronautCombat, "AstronautCombat", state_astronaut_combat);
    register_state(kStAstronautCombatMove, "AstronautCombatMove", state_astronaut_combat_move);
    register_state(kStSpaceDrake, "SpaceDrake", state_space_drake);
    register_state(kStDeleteMe, "DeleteMe", state_delete_me);
    register_state(kStFailMission, "FailMission", state_fail_mission);
    register_state(kStJustStand, "JustStand", state_just_stand);
    register_state(kStTester1, "Tester1", state_tester1);
    register_state(kStTester2, "Tester2", state_tester2);
    register_state(kStTester3, "Tester3", state_tester3);
    register_state(kStTester4, "Tester4", state_tester4);
    register_state(kStHangUp, "HangUp", state_hang_up);
    register_state(kStWaitForever, "WaitForever", state_wait_forever);
}

}  // namespace nf::sp

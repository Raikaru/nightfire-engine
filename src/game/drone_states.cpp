// States that live in the shared drone code (docs/spec-arena-ai.md Part 3 §3.2, §4.3, §9):
//   0 Global, 0x44 Death_Anim, 0x47 Dead, 0x48 Fade, 0x53 PunchImpact, 0x54 ExplosiveImpact, 0x55 BulletImpact.
#include "game/drone_anim.hpp"
#include "game/drone_impact.hpp"
#include "game/drone_system.hpp"
#include "game/drone_vision.hpp"

namespace nf::drone {

namespace {

bool no_script(const Drone& d) { return d.script_id == 0 || d.script_id == 0x6000000; }

// NDrone2_DSTATE_Global 0x15b8c0: the fall-through handler of every drone.
int state_global(Drone& d, const Msg& m) {
    switch (m.id) {
        case kMsgEnter:
            // A fresh drone picks its first state here.
            if (d.dtype == kDtypeBot) { d.set_state(kStateBotInit); return 1; }
            if (d.start_channel != 0 && !d.start_channel_snapshot) { d.set_state(kStateWaitSwitch); return 1; }
            if (d.dtype == 0x17) { d.set_state(0x32); return 1; }   // TruckDriverInit
            if (no_script(d)) { d.set_state(d.initial_state); return 1; }
            d.set_state(kStatePlayScript);
            return 1;
        case kMsgHostageKillerOrder:
            if ((d.flags & flag::kActive) && d.dtype == kDtypeHostageKiller && d.health > 0.0f) {
                d.set_state(9);   // HostageKillerAttack
                return 1;
            }
            return 0;
        case kMsgGotoState:
            if (m.arg != 0) {
                d.set_state(int(m.arg));
                return 1;
            }
            return 0;
        default:
            return 0;
    }
}

void seen_and_attacking(Drone& d) {
    if (d.sys->callbacks().seen_and_attacking) d.sys->callbacks().seen_and_attacking(d);
}

// NDrone2_DSTATE_Death_Anim 0x163?: play the location death clip, then Dead.
int state_death_anim(Drone& d, const Msg& m) {
    switch (m.id) {
        case kMsgNone: case 10:   // 0 / 10: nothing
            return 1;
        case kMsgEnter: {
            d.ghost = true;   // obj+0xf0 |= 0x20: the corpse no longer collides with bullets
            if (d.dtype == 0x1b || d.dtype == 0x1c) { d.set_state(0x92); return 1; }
            if (d.dtype == 0x1d) { d.set_state(0xbf); return 1; }
            alert_status_set(d, AlertStatus::Dead);
            if (d.carried_item != 0) { d.set_state(0x46); return 1; }   // DeathItem (SP layer)
            on_init_death(d);
            d.death_pending_channel = true;
            location_death_anim(d, kStateDead);
            return 1;
        }
        case kMsgLeave:
            if (d.death_pending_channel) {
                d.death_pending_channel = false;
                if (d.sys->callbacks().on_death_channel) d.sys->callbacks().on_death_channel(d);
            }
            return 1;
        case kMsgTick:
            if (d.sys->callbacks().on_drop_weapon && !d.weapon_dropped) {
                d.weapon_dropped = true;   // DroneWeap_DropWeapon
                d.sys->callbacks().on_drop_weapon(d);
            }
            return 1;
        case kMsgTimer1:
            if (d.death_pending_channel) {
                d.death_pending_channel = false;
                if (d.sys->callbacks().on_death_channel) d.sys->callbacks().on_death_channel(d);
            }
            return 1;
        default:
            return 0;
    }
}

// NDrone2_SetAsDead 0x14a2f0
void set_as_dead(Drone& d) {
    if (d.sys->callbacks().on_drop_weapon && !d.weapon_dropped) {
        d.weapon_dropped = true;
        d.sys->callbacks().on_drop_weapon(d);
    }
    d.flags |= flag::kDisabled;
    d.flags &= ~std::uint32_t(0x8000);
    d.flags &= ~std::uint32_t(0x20 | 0x80);
    d.ghost = true;
    if ((d.flags & 0x1000000) == 0) d.health = 0.0f;
    d.fire_requested = false;
}

// NDrone2_DSTATE_Dead 0x172?: stays as a corpse for 10 s, then fades.
int state_dead(Drone& d, const Msg& m) {
    switch (m.id) {
        case kMsgNone: case 10:
            return 1;
        case kMsgEnter:
            set_as_dead(d);
            alert_status_set(d, AlertStatus::Dead);
            d.timer1 = {d.now() + d.seconds(10.0f), 0};   // NPCGlobals+0x148 = 10 * FRAME_RATE
            if (d.has_beh(0x31) && !d.head_shot) {
                d.alert_flags |= 8;
                alert_others(d, kMsgShoutType5, TargetRef::drone(d.id), d.pos);   // "drone died" alert
            }
            return 1;
        case kMsgTick:
            if (d.sys->callbacks().dead_tick) d.sys->callbacks().dead_tick(d);
            return 1;
        case kMsgTimer1:
            d.set_state(kStateFade);
            return 1;
        case kMsgTimer2:
            if (d.has_beh(0x31) && !d.head_shot) {
                d.alert_flags |= 8;
                alert_others(d, kMsgShoutType5, TargetRef::drone(d.id), d.pos);
            }
            d.timer2 = {d.now() + d.seconds(1.0f), 0};
            return 1;
        default:
            return 0;
    }
}

// NDrone2_DSTATE_Fade 0x172d10: the corpse fades out and the drone is removed.
int state_fade(Drone& d, const Msg& m) {
    switch (m.id) {
        case kMsgEnter:
            d.flags &= ~std::uint32_t(0x20 | 0x80);
            d.fade = 1.0f;
            return 1;
        case kMsgNone: case 10:
            return 1;
        case kMsgTick: {
            d.fade -= 1.0f / std::max(1.0f, d.rate() * d.sys->config().fade_seconds);   // NDrone2_FadeOut
            if (d.fade <= 0.0f) {
                d.fade = 0.0f;
                d.pending_delete = true;   // NDrone2_Enable(0) + obj+0xfe |= 1
            }
            return 1;
        }
        default:
            return 0;
    }
}

// NDrone2_DSTATE_PunchImpact 0x173?
int state_punch_impact(Drone& d, const Msg& m) {
    switch (m.id) {
        case kMsgNone: return 1;
        case kMsgEnter:
            d.flags |= flag::kAware;
            punch_impact(d, static_cast<const DroneHit*>(m.ptr), 0, true);
            return 1;
        case kMsgTick: seen_and_attacking(d); return 1;
        case kMsgPunch: case kMsgBullet: return handle_impact(d, m, 0, false);
        case kMsgStunElectric: case kMsgExplosive: case kMsgStunDart: return handle_impact(d, m, 0, true);
        default: return 0;
    }
}

// NDrone2_DSTATE_ExplosiveImpact
int state_explosive_impact(Drone& d, const Msg& m) {
    switch (m.id) {
        case kMsgNone: return 1;
        case kMsgEnter:
            d.flags |= flag::kAware;
            explosive_impact(d, static_cast<const DroneHit*>(m.ptr), 0, true);
            return 1;
        case kMsgTick: seen_and_attacking(d); return 1;
        case kMsgPunch: case kMsgStunElectric: case kMsgBullet: return handle_impact(d, m, 0, true);
        case kMsgExplosive: return handle_impact(d, m, 0, false);
        default: return 0;
    }
}

// NDrone2_DSTATE_BulletImpact
int state_bullet_impact(Drone& d, const Msg& m) {
    switch (m.id) {
        case kMsgEnter:
            d.flags |= flag::kAware;
            d.fire_requested = false;   // Drone+0x3b
            return 1;
        case kMsgTick: seen_and_attacking(d); return 1;
        case kMsgNone: return 1;
        case kMsgPunch: return handle_impact(d, m, 0, false);
        case kMsgStunElectric: return handle_impact(d, m, 0, true);
        case kMsgBullet: bullet_impact(d, static_cast<const DroneHit*>(m.ptr), 0, false); return 1;
        case kMsgExplosive: d.set_state(kStateExplosiveImpact, m.arg); return 1;
        default: return 0;
    }
}

}  // namespace

void register_core_states() {
    static bool done = false;
    if (done) return;
    done = true;
    register_state(kStateGlobal, "Global", state_global);
    register_state(kStateDeathAnim, "Death_Anim", state_death_anim);
    register_state(kStateDead, "Dead", state_dead);
    register_state(kStateFade, "Fade", state_fade);
    register_state(kStatePunchImpact, "PunchImpact", state_punch_impact);
    register_state(kStateExplosiveImpact, "ExplosiveImpact", state_explosive_impact);
    register_state(kStateBulletImpact, "BulletImpact", state_bullet_impact);
}

}  // namespace nf::drone

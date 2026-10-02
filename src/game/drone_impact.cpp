// Damage intake and impact reactions (docs/spec-arena-ai.md Part 3 §4.3, §6.5, §9).
#include "game/drone_impact.hpp"

#include <algorithm>
#include <cmath>

#include "game/drone_anim.hpp"
#include "game/drone_system.hpp"
#include "game/drone_vision.hpp"
#include "game/drone_weap.hpp"

namespace nf::drone {

namespace {

// State ids the impact code treats specially.
constexpr int kStateAttack = 0x56;

bool is_impact_state(int s) { return s >= kStatePunchImpact && s <= kStateBulletImpact; }
// "usable return state": non-zero and not one of the impact states themselves.
bool valid_return(int s) { return s != 0 && !is_impact_state(s); }

const DroneTuning& tuning(const Drone& d) { return d.sys->config().tuning; }

}  // namespace

// ---- NDrone2_HitDamage ----------------------------------------------------------------------------------------------
float hit_damage(Drone& d, float dmg, int region, int weapon, bool force) {
    // Bots have their own pain model (BOT_handlePain) which Drone::hurt routes to before this.
    const DroneTuning& t = tuning(d);
    const std::uint32_t level = d.sys->config().level_id;
    if (region < 0) region = 1;
    bool located = true;
    if (region == 5 && (weapon == 1 || weapon == 0x4a || weapon == 0x4b || weapon == 0x4c || weapon == 0x4d)) {
        region = 1;      // melee-like weapons never score head shots
        located = false;
    }
    if (level == 0x700001b && region != 5) region = 1;
    if (!d.damageable && !force) {
        d.last_damage = dmg;
        return 0.0f;
    }
    const std::uint8_t arm = d.armour;
    switch (region) {
        case 5: {
            float k;
            if (d.dtype == kDtypeNinja || d.skin_hash == 0x50000ba) k = t.damage_torso * 2.0f;
            else if (d.char_class == 0x10 && level == 0x7000014) k = t.damage_torso * 2.0f;
            else {
                d.head_shot = true;   // Drone+0x3a: suppresses DeathTalk
                k = t.damage_head;
            }
            dmg *= k;
            if (arm & 4) dmg *= t.armour_helmet;
            break;
        }
        case 0x14: case 0x15: case 0x17: case 0x20: case 0x23: case 0x27:
            dmg *= t.damage_arms;
            if (arm & 8) dmg *= t.armour_combat;
            if (arm & 2) dmg *= t.armour_jacket;
            break;
        case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37: case 0x38:
            dmg *= t.damage_legs;
            if (arm & 8) dmg *= t.armour_combat;
            break;
        default:
            dmg *= t.damage_torso;
            if (arm & 8) dmg *= t.armour_combat;
            if (arm & 2) dmg *= t.armour_jacket;
            if (arm & 1) dmg *= t.armour_vest;
            break;
    }
    (void)located;
    dmg *= t.damage_diff[std::size_t(weap::difficulty_index(d.sys->config().difficulty))];
    d.health -= dmg;
    d.last_damage = dmg;
    return dmg;
}

// ---- hurt messages ---------------------------------------------------------------------------------------------------
void send_hurt_message(Drone& d) {
    // DroneFunc_SendHurtMessage: behaviour 0x32, once per drone (flag 8), not for DTYPE 2 snipers.
    if (!d.has_beh(beh::kShoutsWhenHurt) || (d.flags & flag::kAlertedShout) || d.dtype == kDtypeSniper) return;
    d.alert_flags |= 2;
    alert_others(d, kMsgShoutHurt, TargetRef::drone(d.id), d.pos);
    d.flags |= flag::kAlertedShout;
}

void on_init_death(Drone& d) {
    alert_status_set(d, AlertStatus::Dead);
    if (!d.death_reported) {
        d.death_reported = true;
        if (d.sys->callbacks().on_death) d.sys->callbacks().on_death(d, d.last_shooter);
    }
}

// ---- hit-location clips -----------------------------------------------------------------------------------------------
void location_death_anim(Drone& d, int end_state) {
    // DroneAnim_LocationDeathAnim 0x13c7d0: head hits play DeathHead (facing decides the variant), others Death.
    if (d.has_last_hit && (d.last_hit.part == 4 || d.last_hit.part == 5)) {
        const float hit_heading = std::atan2(d.last_hit.direction[0], d.last_hit.direction[2]);
        const bool from_front = std::fabs(angle_diff(hit_heading, d.yaw)) <= 1.5707964f;
        anim_call(d, 0, kDeathHead, from_front ? 1 : 0, end_state, 0);
        return;
    }
    d.head_shot = false;
    const int variants = 4;
    anim_call(d, 0, kDeath, int(d.sys->rand_int(variants)), end_state, 0);
}

void location_impact_anim(Drone& d) {
    // DroneAnim_LocationImpactAnim 0x13c8b0: flinch clip by the bone that was hit.
    int variant;
    if (!d.has_last_hit) {
        variant = 5 + int(d.sys->rand_int(8));
    } else {
        switch (d.last_hit.part) {
            case 4: case 5: variant = 5; break;
            case 0x12: case 0x13: case 0x14: case 0x15: variant = 8; break;
            case 0x1e: case 0x1f: case 0x20: case 0x23: variant = 9; break;
            case 0x31: case 0x32: case 0x33: variant = 0xd; break;
            case 0x35: case 0x36: case 0x37: variant = 0xc; break;
            default: variant = 6 + int(d.sys->rand_int(2)); break;
        }
    }
    anim_call(d, 0, kImpact, variant, d.smi.saved, 0);
}

// ---- NDrone2_*Impact ------------------------------------------------------------------------------------------------------
int bullet_impact(Drone& d, const DroneHit* hit, int state, bool non_punch) {
    // NDrone2_BulletImpact 0x146?
    (void)hit;
    int ret_state = d.smi.cur;
    if (state != 0) ret_state = state;
    if (state == d.smi.cur) state = 0;
    d.alertness = 1.0f;
    alert_status_set(d, AlertStatus::Scared);
    send_hurt_message(d);
    if (d.health <= 0.0f) {
        int next = kStateDeathAnim;
        if (d.dtype == kDtypeBot) next = kStateBotDeath;
        else if (d.dtype == 0x1b || d.dtype == 0x1c) next = 0x92;
        else if (d.dtype == 0x1d) next = 0xbf;
        d.set_state(next);
        return 1;
    }
    if (!d.sys->config().multiplayer && non_punch) {
        d.heavy_hit = d.last_damage > d.max_health * 0.25f && d.health > d.max_health * 0.333f;
        DroneSystem& sys = *d.sys;
        const std::uint32_t two_s = d.seconds(2.0f);
        if (d.impact_anim_time + two_s <= d.now() && sys.last_impact_anim_time + two_s <= d.now()) {
            if (valid_return(ret_state)) d.smi.saved = ret_state;
            location_impact_anim(d);
            sys.last_impact_anim_time = d.now();
            d.impact_anim_time = d.now();
            return 0;
        }
    }
    if (d.dtype == 0x10) {   // Ambush drones
        if (d.smi.cur == 0x38) { d.set_state(kStateAttack); return 1; }
        if (d.smi.cur == 0x94) {
            d.behaviour[d.active_behaviour].set(0x10, 0);
            d.set_state(0x9c);
            return 1;
        }
    }
    if (state == 0 || is_impact_state(state)) return 1;
    d.set_state(state);
    return 1;
}

int explosive_impact(Drone& d, const DroneHit* hit, int state, bool in_state) {
    // NDrone2_ExplosiveImpact 0x146480 (damage was applied by Drone::hurt)
    int ret_state = d.smi.cur;
    if (state != 0) ret_state = state;
    if (state == d.smi.cur) state = 0;
    d.alertness = 1.0f;
    alert_status_set(d, AlertStatus::Scared);
    (void)hit;
    if (d.health <= 0.0f) {
        int next = 0x45;   // ExplosionDeath
        if (d.dtype == kDtypeBot) next = 0xf3;
        else if (d.dtype == 0x1b || d.dtype == 0x1c) next = 0x92;
        else if (d.dtype == 0x1d) next = 0xbf;
        else if (d.dtype == kDtypeNinja) next = kStateDeathAnim;
        d.set_state(next);
        return 1;
    }
    send_hurt_message(d);
    if (in_state) {
        if (d.dtype == kDtypeBot) {
            anim_call(d, 0, kExplosive, 1, 0xcf, 0);
            return 1;
        }
        if (valid_return(ret_state)) d.smi.saved = ret_state;
        anim_call(d, 0, kExplosive, 0, d.smi.saved, 0);
        return 1;
    }
    if (state == 0 || is_impact_state(state)) return 0;
    d.set_state(state);
    return 1;
}

int punch_impact(Drone& d, const DroneHit* hit, int state, bool in_state) {
    // NDrone2_PunchImpact 0x1460?
    (void)hit;
    if (d.invulnerable_while_anim) return 1;
    int ret_state = d.smi.cur;
    if (state != 0) ret_state = state;
    if (state == d.smi.cur) state = 0;
    if (valid_return(ret_state)) d.smi.saved = ret_state;
    d.alertness = 1.0f;
    alert_status_set(d, AlertStatus::Scared);
    send_hurt_message(d);
    if (d.health <= 0.0f) {
        d.set_state(d.dtype == kDtypeBot ? kStateBotDeath : 0x42);   // 0x42: punched-to-death state (SP layer)
        return 1;
    }
    if (in_state) {
        anim_call(d, 0, kPunched, 0, d.smi.saved, 0);
        d.impact_anim_time = d.now();
        d.sys->last_impact_anim_time = d.now();
        return 1;
    }
    if (state == 0 || is_impact_state(state)) return 0;
    d.set_state(state);
    return 1;
}

// ---- DroneFunc_HandleImpact -------------------------------------------------------------------------------------------------
int handle_impact(Drone& d, const Msg& m, int default_state, bool non_punch) {
    if (d.invulnerable_while_anim) return 1;
    const DroneHit* hit = static_cast<const DroneHit*>(m.ptr);
    const int cur = d.smi.cur;
    int state = default_state != cur ? default_state : 0;   // "uVar12"
    int ret = default_state != 0 ? default_state : cur;     // "uVar10"
    const std::uint32_t level = d.sys->config().level_id;
    auto save_return = [&] {
        if (valid_return(ret)) d.smi.saved = ret;
    };
    // Tower 1 (levels 9-a): un-alerted civilians switch to a scared civilian type on non-lethal stun effects.
    auto tower_conversion = [&] {
        if (level != 0x7000009 && level != 0x700000a) return;
        if ((d.flags & flag::kAlertedByNoise) == 0) {
            d.smi.saved = 0x14;
        } else {
            d.flags &= ~std::uint32_t(0x2000000);
            d.alt_state = 0x89;
            d.alert_flags = 0;
            d.dtype = 9;
            d.dtype_alt = 7;
            d.flags &= ~std::uint32_t(0x4000000);
            d.initial_state = 0x14;
            d.smi.saved = 0x15;
        }
    };
    switch (m.id) {
        case kMsgPunch:
            if (!non_punch) {
                punch_impact(d, hit, ret, false);
                return 1;
            }
            save_return();
            d.set_state(kStatePunchImpact, m.arg);
            return 1;
        case kMsgStunElectric: {
            d.taser_hits += int(std::lround(60.0f / d.rate()));   // += FRAME_RATE_MUL
            const int limit = int(d.health) * 2;
            if (d.taser_hits <= limit) {
                if (d.taser_hits == 1 && !(d.flags & flag::kAlertedByNoise) && state != 0) d.set_state(state);
                return 1;
            }
            save_return();
            tower_conversion();
            d.set_state(0x4a, m.arg);
            return 1;
        }
        case kMsgBullet: {
            int st = default_state;
            if (d.char_class == 0xc) st = 0x6e;
            if (valid_return(ret)) st = ret;
            if (bullet_impact(d, hit, st, non_punch)) return 1;
            save_return();
            d.set_state(kStateBulletImpact, m.arg);
            return 1;
        }
        case kMsgExplosive:
            if (!non_punch) {
                explosive_impact(d, hit, ret, false);
                return 1;
            }
            save_return();
            d.set_state(kStateExplosiveImpact, m.arg);
            return 1;
        case kMsgGas:
            if (d.has_beh(beh::kIgnoresGas) || d.has_beh(beh::kIgnoresExplosives)) return 1;
            save_return();
            tower_conversion();
            d.set_state(0x7b);
            return 1;
        case kMsgStunGrenade:
            if (d.has_beh(0x1c)) {
                if (d.sub_class != 5) return 1;
                if (anim_can_do(d, 0x57)) anim_call(d, 0, 0x57);
                else if (anim_can_do(d, 0x11)) anim_call(d, 0, 0x11);
                else anim_call(d, 0, 0x19);
                return 1;
            }
            save_return();
            tower_conversion();
            d.set_state(0x4d);
            return 1;
        case kMsgStunDart:
            save_return();
            tower_conversion();
            d.set_state(0x50);
            return 1;
        default:
            return 0;
    }
}

}  // namespace nf::drone

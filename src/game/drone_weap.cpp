// DroneWeap_* (docs/spec-arena-ai.md Part 3 §6.3-6.4).
#include "game/drone_weap.hpp"

#include <algorithm>
#include <cmath>

#include "game/drone_anim.hpp"
#include "game/drone_system.hpp"
#include "game/drone_vision.hpp"
#include "game/weapons.hpp"

namespace nf::drone::weap {

int difficulty_index(int difficulty) {
    switch (difficulty) {
        case 1: return 0;
        case 3: case 4: return 2;
        default: return 1;
    }
}

namespace {

const DroneTuning& tuning(const Drone& d) { return d.sys->config().tuning; }
bool multiplayer(const Drone& d) { return d.sys->config().multiplayer; }

// PS2Sinf range reduction. The EE sub.s/add.s round toward zero (not to nearest), so every loop
// iteration errs downward and the error accumulates linearly (~80 ulp at x=1000); a round-to-nearest
// host loop random-walks instead and diverges past tolerance. Replicated exactly: double subtract,
// float convert, one-ulp pull toward zero when the conversion rounded away from it.
// Used by the aim-wobble path so differential comparisons against the original hold to 1e-5.
float ps2_sinf_arg(float x) {
    constexpr double two_pi = 6.2831854820251465;   // 0x40C90FDB
    if (3.1415927f <= x) {
        do {
            const double d = double(x) - two_pi;
            float r = float(d);
            if (double(r) > d) r = std::nextafterf(r, 0.0f);
            x = r;
        } while (3.1415927f <= x);
    } else {
        while (x < -3.1415927f) {
            const double d = double(x) + two_pi;
            float r = float(d);
            if (double(r) < d) r = std::nextafterf(r, 0.0f);
            x = r;
        }
    }
    if (x <= 1.5707964f) {
        if (-1.5707964f <= x) return x;
        return -3.1415927f - x;
    }
    return 3.1415927f - x;
}

// ps2_sin lives at weap scope below (shared with the bot aim-wobble); ps2_sinf_arg stays file-local.

// 60 Hz "frames" of the original -> ticks of the running rate (n * FRAME_RATE_DIV).
std::uint32_t frames60(const Drone& d, float n) { return std::uint32_t(std::max(n * d.rate() / 60.0f, 0.0f)); }

bool target_moving(const Drone& d) {
    if (d.opponent.kind == TargetRef::Kind::Player) {
        const Player* p = d.sys->world().player(d.opponent.index);
        if (!p) return false;
        return length(p->velocity) != 0.0f;   // |player vel| != 0
    }
    if (d.opponent.kind == TargetRef::Kind::Drone) {
        if (const Drone* o = d.sys->find(d.opponent.index)) return o->mv.speed > 1e-4f;
    }
    return false;
}

const WeaponDef* weapon_def(const Drone& d) {
    if (!d.sys->weapons()) return nullptr;
    const WeaponTable& t = d.sys->weapons()->table();
    if (d.weapon < 0 || d.weapon >= WeaponTable::kWeaponCount) return nullptr;
    return &t.weapon(d.weapon);
}

}  // namespace
// PS2Sinf polynomial: Horner in f32 with the .sdata constants @0x2f3b20, verified bit-near-exact
// (max 2.4e-7 over 2001 samples incl. the ±1.00000012 endpoint overshoot) against the original.
// Shared with the bot aim-wobble (BOT_opponentTargetting).
float ps2_sin(float x) {
    // Table: {-0.00019807414, -0.1666665673, 0.0083330255, 2.601887e-6} (lanes c0..c3).
    constexpr float c0 = -0.00019807414f, c1 = -0.1666665673f, c2 = 0.0083330255f, c3 = 2.601887e-6f;
    const float a = ps2_sinf_arg(x);
    const float a2 = a * a;
    const float inner = a2 * c3 + c0;
    const float m2 = a2 * inner + c2;
    const float m1 = a2 * m2 + c1;
    return a * (1.0f + a2 * m1);
}


float aggression_mul(const Drone& d) {
    // BOT_getAggressionMul 0x12b?: 0.3 / 0.5 / 0.7 / 0.85 / 1.0 by Drone+0xb5, boosted at point-blank in MP.
    static const float kMul[5] = {0.3f, 0.5f, 0.7f, 0.85f, 1.0f};
    float m = kMul[d.aggression < 5 ? d.aggression : 4];
    if (d.sys->config().multiplayer && d.opponent.valid() && d.opp_dist < 2.5f)
        m *= std::min(3.0f, (2.5f - d.opp_dist) + 1.0f);
    return m;
}

bool weapon_raised(const Drone& d) {
    // collbody+0xC8 & 0x1000 (anim-driven): the gun is up in every aiming / shooting / cover pose.
    const int s = d.anim.cur_state;
    return d.anim.cur_type == 4 || (s >= kAimBackoff && s <= kCrouchCover) || s == kCStand || s == kShoot ||
           s == k180Aim || s == k90AimLeft || s == k90AimRight || s == kzNinjaAimStand || s == kCCrouch;
}

// ---- targetting / hit chance ----------------------------------------------------------------------------------------
void opponent_targetting(Drone& d) {
    if (d.hooks.opponent_targetting) {
        d.hooks.opponent_targetting(d);
        return;
    }
    if (!d.opponent.valid()) return;
    const DroneTuning& t = tuning(d);
    d.opp_moving = target_moving(d);
    const std::uint32_t now = d.now();
    if (!d.opp_moving) {
        d.opp_first_moved = false;
        d.opp_moved_started = false;
        if (!d.opp_first_stopped && !d.opp_stopped_started) {
            d.opp_stopped_started = true;
            d.opp_motion_time = now;
            d.opp_first_stopped = true;
        }
        if (d.opp_motion_time + d.seconds(t.first_stopped_time) < now) d.opp_first_stopped = false;
    } else {
        d.opp_first_stopped = false;
        d.opp_stopped_started = false;
        if (!d.opp_first_moved && !d.opp_moved_started) {
            d.opp_moved_started = true;
            d.opp_motion_time = now;
            d.opp_first_moved = true;
        }
        if (d.opp_motion_time + d.seconds(t.first_moved_time) < now) d.opp_first_moved = false;
    }
}

bool do_bullet_accuracy(Drone& d) { return do_bullet_accuracy(d, d.sys->frand(100.0f)); }

bool do_bullet_accuracy(Drone& d, float rand_draw) {
    // DroneWeap_DoBulletAccuracy 0x179740
    if (!d.opponent.valid()) return false;
    const DroneTuning& t = tuning(d);
    const std::uint32_t level = d.sys->config().level_id;
    float p = 100.0f - float(d.accuracy_class) * 5.0f;
    const bool close = d.opp_dist < t.too_close_dist;
    if (close) p *= t.too_close_accuracy;
    if (d.opp_first_moved) p *= t.first_moved_accuracy;
    if (d.opp_first_stopped && level >= 0x700000c && level < 0x700000e && d.sub_class == 0x13)
        p *= t.first_stopped_accuracy;
    if (d.opp_moving) p *= t.moving_accuracy;
    p *= t.accuracy[std::size_t(difficulty_index(d.sys->config().difficulty))];
    if (close) p *= t.too_close_accuracy;   // applied twice (4x at close range)
    bool hit = rand_draw < p;
    if (!close) {
        hit = hit && d.seen_frames >= d.seconds(t.new_sighting_time);
        if (d.lost_since_shot) hit = false;
    }
    if (d.opp_dist < 3.0f) hit = true;
    if (hit) {
        d.aim_offset = {0, 0, 0};
    } else {
        // Correlated Lissajous wobble around the aim point, rotated by RotMatrix(obj+0x1c0 euler angles)
        // and ApplyMatrixLV into Drone+0x200. Replicated op-for-op in f32: the PS2Sinf range reduction is
        // exact, the VU polynomial and matrix FMA chains match to ~1e-7 (differential tolerance 1e-5).
        const float tt = float(d.now()) * d.sys->timing().FRAME_RATE_MUL + float(d.rand_phase);
        const float ph = tt * 0.01f;
        const Vec3 v{0.5f * ps2_sin(ph), 1.5f * ps2_sin(ph + 1.5707964f), 1.5f * ps2_sin(tt * 0.02f)};
        const float rx = d.aim_euler[0], ry = d.aim_euler[1], rz = d.aim_euler[2];
        const float sx = ps2_sin(rx), cx = ps2_sin(rx + 1.5707964f);   // PS2Sinf3 pairs
        const float sy = ps2_sin(ry), cy = ps2_sin(ry + 1.5707964f);
        const float sz = ps2_sin(rz), cz = ps2_sin(rz + 1.5707964f);
        const float m0 = cy * cz;
        const float m1 = sx * sy * cz + cx * sz;
        const float m2 = cx * -sy * cz + sx * sz;
        const float m4 = cy * -sz;
        const float m5 = cx * cz - sx * sy * sz;
        const float m6 = sx * cz - cx * -sy * sz;
        const float m8 = sy;
        const float m9 = -sx * cy;
        const float m10 = cx * cy;
        d.aim_offset = {m0 * v[0] + m4 * v[1] + m8 * v[2], m1 * v[0] + m5 * v[1] + m9 * v[2],
                        m2 * v[0] + m6 * v[1] + m10 * v[2]};
    }
    return hit;
}

// ---- cadence --------------------------------------------------------------------------------------------------------
std::uint32_t burst_delay(const Drone& d) {
    // DroneWeap_BurstDelay 0x179ba8
    const DroneTuning& t = tuning(d);
    float base = t.burst_delay_normal;
    switch (d.aggression) {
        case 0: base *= 1.66f; break;
        case 1: base *= 1.33f; break;
        case 3: base *= 0.66f; break;
        case 4: base *= 0.33f; break;
        default: break;
    }
    const float r = d.sys->frand(0.5f);
    float frames = std::floor(base * (r + 0.5f));
    frames = std::max(frames, t.burst_delay_min);
    if (d.opp_dist >= t.burst_min_dist && d.opp_dist > t.burst_max_dist) frames = t.burst_delay_max;
    else if (d.opp_dist >= t.burst_min_dist) frames = std::min(frames, t.burst_delay_max);
    return frames60(d, frames);
}

void next_bullet_time(Drone& d, bool new_burst, bool bot) {
    // DroneWeap_NextBulletTime 0x179d40
    const std::uint32_t now = d.now();
    const WeaponDef* def = weapon_def(d);
    int gap_frames = def ? int(def->fire_interval) : 0;   // weapon_data +0x40
    int min_gap = 0;
    const int w = d.weapon;
    if (w == 6 || w == 10 || (w >= 0xe && w <= 0x10)) min_gap = 60;
    if (gap_frames < min_gap) gap_frames = min_gap;
    const std::uint32_t gap = std::uint32_t(std::max(0.0f, float(gap_frames) * d.rate() / 60.0f + 0.5f));
    std::uint32_t last = d.last_shot_time;
    std::uint32_t extra = 0;
    if (new_burst || bot) {
        int size = def ? int(def->fire_count[2]) : 3;   // weapon_data +0x2c
        if (!multiplayer(d)) {
            if (w == 6 || w == 10) {
                if (size < 2) size = 2 + int(d.sys->rand_int(3));
            } else if (w >= 0xe && w <= 0x10) {
                size = 1 + int(d.sys->rand_int(2));
            }
        }
        d.burst_left = std::max(size, 1);
        if (!multiplayer(d)) {
            const float mul = aggression_mul(d);
            const std::uint32_t delay = burst_delay(d);
            if (!(w == 10 || w == 6 || (w >= 0xe && w <= 0x10)) && d.burst_left >= 4)
                d.burst_left = std::max(3, int(float(d.burst_left) * mul));
            d.last_shot_time += delay;
            last = d.last_shot_time;
        } else {
            const float mul = aggression_mul(d);
            const std::uint32_t jitter = d.sys->rand_int(15);
            const std::uint32_t pause = std::uint32_t(60.0f / std::max(mul, 0.01f));
            extra = frames60(d, float(jitter + pause));
            d.burst_left = std::max(1, int(float(d.burst_left) * mul));
            last = d.last_shot_time;
        }
    }
    d.next_bullet_time = std::max(last + gap, extra + now);
}

bool ready2fire(Drone& d) {
    // DroneWeap_Ready2Fire 0x178c48
    if (!d.opponent.valid()) return false;
    if (!target_valid(*d.sys, d.opponent)) return false;
    const int type = d.anim.cur_type;
    if (type == 4) return true;   // a firing anim is playing
    if (!multiplayer(d)) {
        if (type != 2) return false;
        if (d.opp_dist < 2.0f) return true;
    }
    if (!weapon_raised(d)) return false;
    const Vec3 origin = muzzle_position(d);
    Vec3 to = d.opp_pos - origin;
    const float len = length(to);
    if (len < 1e-4f) return true;
    to = to * (1.0f / len);
    const float dotp = std::clamp(dot(d.forward(), to), -1.0f, 1.0f);
    if (dotp > 0.9998f) return true;
    const float limit = d.dtype == kDtypeNinja ? 0.5235988f : 0.34906587f;
    if (std::fabs(std::acos(dotp)) < limit) return true;
    return d.health <= 0.0f;
}

// ---- firing ---------------------------------------------------------------------------------------------------------
Vec3 muzzle_position(const Drone& d) {
    // AnimDatumGetWeaponInfo: the held weapon's datum. Skins without datums (MP characters) use the right hand
    // height of the torso.
    if (d.character && d.look.hand_datum >= 0 &&
        std::size_t(d.look.hand_datum) < d.character->skin().datums.size()) {
        const Mat4 m = d.character->datum_world(d.look.hand_datum);
        const float s = std::sin(d.yaw), c = std::cos(d.yaw);
        const Vec3 o{m[12], m[13], m[14]};
        return d.feet() + Vec3{o[0] * c + o[2] * s, o[1], -o[0] * s + o[2] * c};
    }
    const Vec3 torso = drone_bone_pos(d, 2);
    const Vec3 fwd = d.forward();
    const Vec3 right{std::cos(d.yaw), 0.0f, -std::sin(d.yaw)};
    return torso + fwd * 0.45f + right * 0.12f + Vec3{0, 0.05f, 0};
}
Vec3 drop_position(const Drone& d, std::uint8_t bone) {
    const Vec3 origin = d.pos;
    Vec3 pos = origin;
    if (d.character) {
        const Palette& palette = d.character->palette();
        const std::size_t index = bone == 0xFF ? 0 : bone;
        if (index < palette.world.size()) {
            const Mat4 m = d.character->bone_world(index);
            const float s = std::sin(d.yaw), c = std::cos(d.yaw);
            pos = origin + Vec3{m[12] * c + m[14] * s, m[13], -m[12] * s + m[14] * c};
        }
    }
    if (d.sys && d.sys->collision().ray(origin, pos, 0x125)) return origin;
    return pos;
}


float bullet_damage_scale(const Drone& d) {
    // Drone_ModBulletDamage 0x139f50 (SP drones; bots use Drone::hooks.damage_mul)
    if (d.is_bot()) return d.hooks.damage_mul ? d.hooks.damage_mul(d) : 1.0f;
    if (d.char_class == 0x0c) return 0.0f;   // allies (Mayhew) cannot hurt the player
    const DroneTuning& t = tuning(d);
    float k = 1.0f;
    if (d.opp_dist < t.too_close_dist) k *= t.too_close_damage;
    k *= d.bullet_damage_mod;
    const std::uint32_t level = d.sys->config().level_id;
    if (d.sub_class == 0x12 && d.dtype != kDtypeSniper && d.dtype != kDtypeSniperAlert && level != 0x700001b &&
        std::fabs(d.opp_facing_b) < 1.5707964f)
        k *= t.back_shot_damage;   // shooting the player in the back
    return k;
}

void fire_weapon(Drone& d) {
    // DroneWeap_FireWeapon 0x178f68
    if (d.hooks.has_ammo && !d.hooks.has_ammo(d)) return;
    const bool bot = d.dtype == kDtypeBot;
    if (!bot && do_bullet_accuracy(d)) ++d.shots_hit_roll;   // accuracy statistic: rolls that aim true
    d.shots_fired++;
    const Vec3 origin = muzzle_position(d);
    Vec3 dir = d.forward();
    if (d.opponent.valid()) {
        Vec3 to = (d.opp_pos + d.aim_offset) - origin;
        const float len = length(to);
        if (len > 1e-4f) dir = to * (1.0f / len);
    }
    DroneSystem& sys = *d.sys;
    bool fired = false;
    if (sys.callbacks().fire) {
        fired = sys.callbacks().fire(d, origin, dir, d.weapon);
    } else if (WeaponSystem* ws = sys.weapons()) {
        WeaponSystem::Shooter s;
        s.id = d.shooter_id;
        s.origin = origin;
        s.direction = dir;
        s.owner_aiming = false;
        s.damage_scale = bullet_damage_scale(d);
        ws->fire(s, d.weapon);
        fired = true;
    }
    if (!fired) return;
    d.fired_flag = true;                // +0x40
    d.last_shot_time = d.now();         // +0xbc8
    d.lost_since_shot = false;          // +0x41
    if (d.hooks.on_round_fired) d.hooks.on_round_fired(d);
}

bool do_firing(Drone& d) {
    // DroneWeap_DoFiring 0x17a080
    if (multiplayer(d) && d.hooks.opponent_targetting) d.hooks.opponent_targetting(d);   // BOT_opponentTargetting
    if (d.burst_left == 0) next_bullet_time(d, true, false);
    if (d.lost_frames < 0x10 || d.has_beh(beh::kFireAtLastKnown)) {
        if (d.next_bullet_time <= d.now()) {
            if (ready2fire(d)) {
                d.firing_now = true;   // +0x3f
                if (!d.fire_lock) {
                    fire_weapon(d);
                    --d.burst_left;
                    if (d.burst_left < 1) return false;
                    next_bullet_time(d, false, false);
                }
            }
        }
        return true;
    }
    return false;
}

void fire(Drone& d, bool new_burst) {
    // DroneWeap_Fire 0x17a?: (SP) a request that is already active does not re-arm the burst.
    if (!multiplayer(d) && d.fire_requested) new_burst = false;
    if (new_burst) next_bullet_time(d, false, true);
    d.fire_requested = true;
    d.burst_done = false;
    d.one_shot = false;
}

void handle_firing(Drone& d) {
    // DroneWeap_HandleFiring 0x17a4c0: +0x3b (fire_requested) is set by the firing states.
    d.firing_now = false;
    auto refresh_aim = [&] {
        if (d.opponent.valid() && !multiplayer(d)) d.opp_pos = target_bone_pos(*d.sys, d.opponent, 2);
    };
    if (!d.fire_requested) {
        if (d.opponent.valid() && !multiplayer(d)) {
            refresh_aim();
            opponent_targetting(d);
        }
        return;
    }
    if (!d.one_shot) {
        d.burst_done = false;                    // continuous fire: every tick starts a fresh check
    } else if (d.burst_done) {
        d.fire_requested = false;                // one burst per request (states 0x44/0x46/0x47/0x55)
        refresh_aim();
        return;
    }
    if (!multiplayer(d)) opponent_targetting(d);
    if (!do_firing(d)) d.burst_done = true;
}

}  // namespace nf::drone::weap

// Drone_BulletHit / Drone_ExplosiveHit (0x13a9f8 / 0x13aab8): damage entry of every drone.
#include "game/drone_impact.hpp"
#include "game/drone_system.hpp"

namespace nf::drone {

float Drone::hurt(const DroneHit& hit) {
    if ((flags & flag::kDeadMask) != 0 || hit.damage <= 0.0f) return 0.0f;
    if (invulnerable_while_anim && !is_bot()) return 0.0f;   // DroneFunc_HandleImpact ignores hits (+0x1b)
    ++hit_count;                        // +0x1d8
    last_shooter = hit.attacker;        // +0x2b4
    shot_at = true;                     // OpponentIsAimingAtMe
    shot_at_time = now();
    last_hit = hit;
    has_last_hit = true;
    float applied;
    if (is_bot() && hooks.bot_pain) applied = hooks.bot_pain(*this, hit);
    else applied = hit_damage(*this, hit.damage, hit.part, hit.weapon);
    const bool melee = hit.weapon == 1 || (hit.weapon >= 0x4a && hit.weapon <= 0x4d);
    const int msg = hit.blast ? kMsgExplosive : (melee ? kMsgPunch : kMsgBullet);
    send_self(msg, 0, 0, &last_hit);
    if (sys->callbacks().on_hurt) sys->callbacks().on_hurt(*this, hit);
    return applied;
}

float DroneSystem::hurt_drone(int id, const DroneHit& hit) {
    Drone* d = find(id);
    return d ? d->hurt(hit) : 0.0f;
}

}  // namespace nf::drone

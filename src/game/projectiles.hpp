#pragma once

#include <cstdint>
#include <optional>

#include "core/math.hpp"

namespace nf {

// A live bullet / grenade / rocket: the `obj_tag` of type 5 with its BU_tag (docs/spec-weapons.md 7.1). Bullets are
// real objects: they advance `speed * FRAME_RATE_MUL` units per tick and sweep that segment against the level and
// every combatant, they are not instantaneous rays.
struct Projectile {
    enum class State : std::uint8_t { Spawn = 0, Flying = 1, Hit = 2, Stuck = 3, OutOfRange = 4 };
    int weapon = 0;              // BU+68 weapon_data id
    int owner = -1;              // BU+48 shooter id (0..3 players, >= 4 registered targets)
    Vec3 pos{}, dir{0, 0, 1};    // obj+0x30, BU+0
    float speed = 0;             // BU+248, units per 60 Hz frame
    float travelled = 0;         // BU+244
    float timer = 210.0f;        // BU+252, 60 Hz frames (fuse of timed grenades)
    State state = State::Spawn;  // obj+244
    std::uint16_t bounces = 0;   // BU+256
    bool delete_me = false;      // obj+254 bit 0
    bool resting = false;        // BU+260: lying still (bounce ended on a floor), gravity off
    Vec3 stuck_normal{};         // surface normal a sticky projectile hangs on
    float damage_scale = 1.0f;   // Shooter::damage_scale
    float age = 0;               // ticks alive (for renderers)
    bool spawned_this_tick = false; // Bullet_init's new obj_tag remains in state 0 through this frame's sample
    Vec3 probe_start{};         // HITTEST probe start retained until Collide_Update consumes it
    bool probe_pending = false; // hit probe is processed after the projectile update, next logic phase
    Vec3 previous_pos{};         // visual-only pose from the preceding logic tick
    std::uint16_t network_id = 0; // snapshot identity for render interpolation
};

// Ray (from + t * delta, t in [0, 1]) against the capsule axis a..b swept by `radius`.
struct CapsuleHit {
    float t = 0;
    Vec3 normal{};
};
std::optional<CapsuleHit> ray_capsule(const Vec3& from, const Vec3& delta, const Vec3& a, const Vec3& b, float radius);

}  // namespace nf

#pragma once

// DroneWeap_*: burst cadence, aiming and hit chance, firing through WeaponSystem, and NDrone2_HitDamage.
// Spec: docs/spec-arena-ai.md Part 3 §6.3-6.5 and Part 2A §4.

#include "game/drone.hpp"

namespace nf::drone::weap {

// The states call `do_firing` (DroneWeap_DoFiring) while they want to shoot, exactly like the original states do;
// the core calls `handle_firing` (DroneWeap_HandleFiring) once per tick after movement.
// Returns false when the burst is finished (the original's 0 -> "burst done" flag).
bool do_firing(Drone& d);
void handle_firing(Drone& d);
// DroneWeap_Fire(new_burst): request continuous firing (fire_requested = 1, burst_done = 0, one_shot = 0); arms a new burst
// first unless one is already requested (SP).
void fire(Drone& d, bool new_burst = true);

// DroneWeap_NextBulletTime(dcv, new_burst, bot_flag): arms Drone::burst_left / next_bullet_time.
void next_bullet_time(Drone& d, bool new_burst, bool bot = false);
// DroneWeap_BurstDelay: pause between bursts in ticks (tuning DroneFiring_BurstDelay_*).
std::uint32_t burst_delay(const Drone& d);
// DroneWeap_Ready2Fire: target alive, weapon raised, aim within 20 degrees (30 for ninjas).
bool ready2fire(Drone& d);
// DroneWeap_DoOpponentTargetting: moving / stopped tracking of the target for the hit-chance model.
void opponent_targetting(Drone& d);
// DroneWeap_DoBulletAccuracy: rolls hit/miss (fills Drone::aim_offset). Returns true when the shot is a hit.
bool do_bullet_accuracy(Drone& d);
// DroneWeap_FireWeapon: one shot (or one burst pellet set) through the weapons system.
void fire_weapon(Drone& d);
// Where bullets leave the drone (weapon datum, else the right hand height on the torso).
Vec3 muzzle_position(const Drone& d);
// Drone_ModBulletDamage: multiplier on the damage of this drone's bullets.
float bullet_damage_scale(const Drone& d);
// DroneWeap_DropWeapon on death: content-layer hook only (DroneCallbacks::on_drop_weapon).

// BOT_getAggressionMul: firing-cadence factor from Drone::aggression (0.3 .. 1.0; x up to 3 at point-blank in MP).
float aggression_mul(const Drone& d);
// The weapon-raised anim flag (collbody+0xC8 & 0x1000).
bool weapon_raised(const Drone& d);

// Difficulty index into the three-entry tuning arrays: 1 easy, 2 normal, 3/4 hard, else normal.
int difficulty_index(int difficulty);

}  // namespace nf::drone::weap

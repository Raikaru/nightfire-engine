#pragma once

// Damage intake and impact reactions shared by every drone (SP enemies and bots):
//   Drone_BulletHit / Drone_ExplosiveHit  (Drone::hurt)   ->  message 8 / 9 to the current state
//   NDrone2_HitDamage 0x145558 (region/armour/difficulty multipliers)
//   DroneFunc_HandleImpact 0x1469b8 + NDrone2_BulletImpact/ExplosiveImpact/PunchImpact
//   DroneFunc_SendHurtMessage 0x144?, DroneAnim_LocationDeathAnim/LocationImpactAnim
// Spec: docs/spec-arena-ai.md Part 3 §4.3, §6.5, §9.

#include "game/drone.hpp"

namespace nf::drone {

// NDrone2_HitDamage(dmg, dcv, region, weapon, flag): applies the region multiplier (head x10 / arms / legs / torso),
// armour bits, DroneDamage_{Easy,Normal,Hard}, subtracts from health and records Drone::last_damage. Returns the
// damage applied. `flag` forces damage on drones whose `damageable` is false (scripted kills).
float hit_damage(Drone& d, float dmg, int region, int weapon, bool force = false);

// DroneFunc_HandleImpact(dcv, msg, default_state, non_punch): the reaction of a state to messages 6..9 and 0x17..0x19.
// Returns non-zero when the message was consumed. Performs the transition itself (d.set_state) exactly where the original
// calls Drone_SM_SetState; `default_state` is the state the drone returns to afterwards (0 = the current one).
int handle_impact(Drone& d, const Msg& m, int default_state, bool non_punch);

// NDrone2_BulletImpact(dcv, hit, state, non_punch): alertness 1.0, alert status Scared, hurt shout, then death
// (Death_Anim 0x44 / bot 0xf2 / abseil 0x92 / astronaut 0xbf), a flinch anim (returns 0) or nothing (returns 1).
int bullet_impact(Drone& d, const DroneHit* hit, int state, bool non_punch);
// NDrone2_ExplosiveImpact(dcv, impact, state, in_state) / NDrone2_PunchImpact(dcv, hit, state, in_state)
int explosive_impact(Drone& d, const DroneHit* hit, int state, bool in_state);
int punch_impact(Drone& d, const DroneHit* hit, int state, bool in_state);

// DroneFunc_SendHurtMessage: behaviour 0x32, once per drone -> broadcast msg 0x12 (30-tick delay) with an alert record.
void send_hurt_message(Drone& d);
// DroneAnim_LocationDeathAnim(dcv, end_state) / DroneAnim_LocationImpactAnim: hit-location dependent clips.
void location_death_anim(Drone& d, int end_state);
void location_impact_anim(Drone& d);

// DroneFunc_OnInitDeath (player stats / mission-fail hooks stay with the content layer): the core part = alert status
// Dead + DroneCallbacks::on_death.
void on_init_death(Drone& d);

}  // namespace nf::drone

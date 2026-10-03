#pragma once

// DroneVision_* / NDrone2_FindOpponent / alert propagation. Spec: docs/spec-arena-ai.md Part 3 §5.
// The state handlers of the content layers call the DroneVision_* entry points below exactly where the original
// state functions do (e.g. the TICK skeleton: `int next = enemy_look_for_opponent(d); if (next) d.set_state(next);`).

#include <cmath>

#include "core/math.hpp"
#include "game/drone.hpp"

namespace nf::drone {

class DroneSystem;

// ---- small math shared by the drone code --------------------------------------------------------------------
constexpr float kPi = 3.14159265358979f;
inline float wrap_pi(float a) {
    while (a > kPi) a -= 2 * kPi;
    while (a < -kPi) a += 2 * kPi;
    return a;
}
// Vec_AngleDifference(from, to): signed shortest turn from `from` to `to`.
inline float angle_diff(float from, float to) { return wrap_pi(to - from); }
// ATAN2(x, z): heading of a vector with forward = (sin, cos).
inline float heading_of(float x, float z) { return std::atan2(x, z); }
// ATAN2_APPROX 0x11f058: the rational approximation the original uses for headings (GetOpponentInfo
// writes it to Drone+0x1c4). Replicated op-for-op in f32 (error vs atan2 up to ~5e-3 rad); do NOT replace
// heading_of elsewhere: only the bearing lane goes through it.
inline float atan2_approx(float x, float z) {
    float r;
    if (z == x) {
        r = 0.7853982f;
        if (z == 0.0f) r = 0.0f;
    } else {
        const float az = std::fabs(z), ax = std::fabs(x);
        if (ax < az) {
            r = (1.0596788f - (ax / az) * 0.27131295f) * (ax / az);
        } else {
            r = 1.5707964f - (1.0596788f - (az / ax) * 0.27131295f) * (az / ax);
        }
    }
    if (z < 0.0f) r = 3.1415927f - r;
    if (x < 0.0f) r = -r;
    return r;
}

// ---- per-tick world passes (called by DroneSystem::tick) ---------------------------------------------------
void process_opponents(DroneSystem& sys);     // Drone_ProcessOpponents: visibility multipliers + opponent info
void process_drone_sight(DroneSystem& sys);   // DroneVision_ProcessDroneSight: visibility, LOS round robin, sight counters
void find_alerted_drones(DroneSystem& sys);   // DroneVision_FindAlertedDrones (called by process_drone_sight)

// ---- positions ---------------------------------------------------------------------------------------------------
// World position of a drone bone (bot head uses NDrone2_GetHeadPos's body-relative MP path).
Vec3 drone_bone_pos(const Drone& d, int bone);
Vec3 head_pos(const Drone& d);                // NDrone2_GetHeadPos
// Position of a bone of an opponent (players: estimated from stand height / crouch).
Vec3 target_bone_pos(const DroneSystem& sys, const TargetRef& t, int bone);
Vec3 target_pos(const DroneSystem& sys, const TargetRef& t);
float target_yaw(const DroneSystem& sys, const TargetRef& t);
bool target_valid(const DroneSystem& sys, const TargetRef& t);     // alive and targetable
// Bones the LOS ray cycles through (Drone_View_Bones @0x2a36f8): torso x2, head, legs, arms.
extern const int kViewBones[8];

// ---- opponent bookkeeping ------------------------------------------------------------------------------------
void set_opponent(Drone& d, const TargetRef& t);   // NDrone2_SetOpponent 0x1422d0
void find_opponent(Drone& d);                       // NDrone2_FindOpponent 0x1424e0 (non-bot branches)
void get_opponent_info(Drone& d);                   // Drone_GetOpponentInfo 0x138f68

// ---- sight ------------------------------------------------------------------------------------------------------------
float opponent_visibility(Drone& d);                // DroneVision_OpponentVisibility 0x175af0 -> Drone::visibility
bool seek_opponent_los(Drone& d);                   // DroneVision_SeekOpponentLOS 0x175cb8
void have_opponent_sight(Drone& d);                 // DroneVision_HaveOpponentSight 0x175ef0
std::uint32_t reaction_time(const Drone& d);        // DroneFunc_ReactionTime 0x14ac60 (ticks)
bool can_see_object(Drone& d, const TargetRef& t, int bone);   // NDrone2_CanSeeObject / DroneVision_CanSeeObjectFrom
bool can_see_position(Drone& d, const Vec3& pos);   // DroneVision_CanSeePosition
bool drone_can_see_drone(Drone& a, Drone& b);       // DroneVision_DroneCanSeeDrone 0x1760b0
// The state-skeleton entry: returns the state to enter (0x56 Attack) when the opponent has been noticed, else 0.
int enemy_look_for_opponent(Drone& d);              // DroneVision_EnemyLookForOpponent 0x177f58

// ---- alerts --------------------------------------------------------------------------------------------------------------
// Shout / alert message handling of a state (DroneVision_EnemyAlerts 0x177c18): msg ids 0x11..0x16, 0x1e; returns the
// next state or 0.
int enemy_alerts(Drone& d, const Msg& m);
int alert_sound(Drone& d);                          // DroneVision_AlertSound 0x177800 (msg 0x14)
bool in_shouting_range(const Drone& d, const Drone& source);   // NDrone2_InShoutingRange 0x1788a0
// NDrone2_DroneAlertToObject / ToPosition: broadcast `msg_id` (+30 ticks) with an alert record
// (radii 20/20, factor 1.0 unless given). `target` may be invalid for position alerts.
void alert_others(Drone& d, int msg_id, const TargetRef& target, const Vec3& pos, float factor = 1.0f);
// DroneFunc_HandleSoundAlerts 0x148a30 is driven by DroneSystem::emit_noise; call this from a state's tick to poll
// noises already stored on the drone.
void alert_status_set(Drone& d, AlertStatus s);     // Drone_AlertStatusSet 0x13a8d8

}  // namespace nf::drone

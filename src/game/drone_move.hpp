#pragma once

// NDrone2 locomotion: route following, steering, combat-move tests, body collision.
// Spec: docs/spec-arena-ai.md Part 2B §6.5 and Part 3 §10; original functions named in the comments.
//
// Content-layer usage (per tick, from a state's TICK handler, exactly like the original states do):
//     move_to_object(d, d.opponent, 6.0f);          // NDrone2_MoveToObject: route + waypoint
//     set_angle_to_dest(d);                         // face the waypoint
//     anim_for_dist(d, d.mv.route_distance);        // pick run / walk / aim anim for the remaining distance
// The core applies the anim's root motion along Drone::yaw after the state ran (NDrone2_Move) and resolves the body
// collision (NDrone2_Collision).

#include "game/drone.hpp"

namespace nf::drone {

// ---- route movement ---------------------------------------------------------------------------------------------------
// NDrone2_MoveToGoalPosition(dcv, CelPos*, radius, mask): (re)build the route to `goal` when invalid, then follow it
// with link creep when `follow`; updates Drone::mv (waypoint, distances, dest_angle). Returns RouteStatus as int
// (0 following, 1 approximate, 2 straight, 3 arrived, 4.. failures; spec 6.6).
int move_to_goal_position(Drone& d, const Vec3& goal, float radius = 0, bool follow = true);
// NDrone2_MoveToObject(dcv, obj, radius, follow): same for a moving target.
int move_to_object(Drone& d, const TargetRef& target, float radius = 0, bool follow = true);
// Patrol / mission route (NDrone2_FollowRoute on the mission route, NDrone2_AssignAIPath).
bool assign_ai_path(Drone& d, std::uint32_t path_mask);
int follow_ai_path(Drone& d);
// NDrone2_InvalidateAttackRoute / AINetwork_InvalidateRoute.
void invalidate_attack_route(Drone& d);
// Stop steering to a destination (no waypoint).
void stop(Drone& d);
inline int route_status(const Drone& d) { return int(d.mv.route_status); }

// Convenience names used across the layers.
inline int move_to_pos(Drone& d, const Vec3& goal, float stop_dist) { return move_to_goal_position(d, goal, stop_dist); }
inline int move_to_target(Drone& d, const TargetRef& t, float dist) { return move_to_object(d, t, dist); }

// ---- facing -----------------------------------------------------------------------------------------------------------------
// NDrone2_SetAngleToDest(dcv, offset): heading = direction to the move target (skipped when the drone has seen the
// player unless a state sets the angle itself with set_angle_to_obj).
void set_angle_to_dest(Drone& d, float offset = 0.0f);
// NDrone2_SetAngleToObj(dcv, obj, offset, force): face the target.
void set_angle_to_obj(Drone& d, const TargetRef& t, float offset = 0.0f, bool force = false);
void face_yaw(Drone& d, float yaw);
inline void face_target(Drone& d, const TargetRef& t) { set_angle_to_obj(d, t, 0.0f, true); }
// DroneAnim_SnapRotate: yaw snaps to the heading immediately.
void snap_yaw(Drone& d, float yaw);

// NDrone2_CanMoveToRelativePos: MoveTest from the feet to `local` in the object frame (+x left, +z forward).
bool can_move_to_relative(Drone& d, const Vec3& local);

// ---- move tests (ray / MoveTest clearance for the combat moves; original distances) --------------------------------
bool can_strafe_left(Drone& d);         // NDrone2_CanStrafeLeft: DASC 0xc, behaviour 0x41, 4.0 m (1.8 m sub-class 6/9)
bool can_strafe_right(Drone& d);
bool can_strafe_dodge_left(Drone& d);   // DASC 0x29, behaviour 0x13, 3.6 m
bool can_strafe_dodge_right(Drone& d);
bool can_step_left(Drone& d);           // DASC 0x5a/0x5b, behaviour 0x42, 1.8 m
bool can_step_right(Drone& d);
bool can_roll_left(Drone& d);           // DASC 0x53, behaviour 0x2e, 5.2 m
bool can_roll_right(Drone& d);
bool can_backoff(Drone& d);             // DASC 4, behaviour 6, 1.8 m backwards
bool can_aim_crouch(Drone& d);          // NDrone2_CanAimCrouch: behaviour 0x11, DASC 5, opponent >= 6.0 m
// NDrone2_EvasiveMove: when the opponent is aiming at the drone picks strafe (0x75/0x76), dodge (0x77/0x78), roll
// (0x79/0x7a) or step (0x73/0x74). Returns the chosen state (0 = none); non-bot drones enter it themselves.
int evasive_move(Drone& d);
// NDrone2_AnimForDist(dcv, dist): run / walk / aim anim for the remaining distance; sets Drone::fire_requested
// while aiming.
void anim_for_dist(Drone& d, float dist);
// DroneAnim_SetCombatMoveAnim(dcv, dist): the run / aim-run / aim-walk / strafe / back-off / step clip that matches the angle
// between the route waypoint and the target (DroneAnim_GetMoveQuad), sets Drone::fire_requested while aiming. `dist` is the
// remaining route distance. Returns true when a request was made.
bool set_combat_move_anim(Drone& d, float dist);
// DroneWeap_OpponentIsAimingAtMe / NDrone2_BondIsFacingMe(degrees)
bool opponent_is_aiming_at_me(const Drone& d, float degrees = 7.5f);
bool opponent_facing_me(const Drone& d, float degrees);

// ---- per-tick (called by the core, ControlSTANDARD) ----------------------------------------------------------------
// NDrone2_Move + DroneMove_NoBunching: steer toward Drone::mv.dest_angle, then apply the anim root motion.
void move_step(Drone& d);
// NDrone2_Collision + Collide_Update: gravity, capsule push-out, ground snap, AI bounds push (r = 0.4).
void collision_step(Drone& d);
// Places the drone on the floor below its feet position (used at spawn / teleport).
void place_on_floor(Drone& d);

}  // namespace nf::drone

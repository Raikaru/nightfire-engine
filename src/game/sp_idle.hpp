#pragma once

// Helpers of the idle / patrol family (sp_states_idle.cpp, sp_states_search.cpp).

#include "game/sp_common.hpp"

namespace nf::sp {

// Per-drone scratch of the idle family.
struct IdleSlot : SlotBase {
    int saved_state = 0;                 // Drone+0xcd8: impact fall-back state (PlayScript) / resume state (Disabled)
    bool script_looks = true;            // Drone+0xcdc: PlayScript polls the opponent
    bool blind_flag = false;             // Drone+0x38 (StandFiddle)
    std::uint32_t next_idle_anim = 0;    // Drone+0x59c: next idle animation change time
};

// DroneAnim_SetStandIdleAnim / SetWalkIdleAnim (0x13cad0 / 0x13cbe0): idle clip variety with the +0x59c timer.
// `timed` = the original's flag argument (respect the timer). Return true when a clip was requested.
bool stand_idle_anim(Drone& d, bool timed);
bool walk_idle_anim(Drone& d, bool timed);

// NDrone2_ObjectIsArmed(player): the opponent (the player) holds a weapon - always true for the SP player.
inline bool opponent_armed(const Drone& d) { return d.opponent.valid(); }

// NDrone2_UpdatePatrolRoute (follow the patrol route + walk / idle animation by route status).
int update_patrol_route(Drone& d);
// NDrone2_PatrolTalk / AttackTalk.
void patrol_talk(Drone& d);
inline void attack_talk(Drone& d) { talk(d, Speech::Attack); }

// Fixed position goal (AIPoint_tag, Drone+0x6f0) shared by the investigate / search / mission states.
// NDrone2_SetSoundAlertRoute 0x1541c0 writes it; NDrone2_MoveToGoalPosition follows it.
void set_ai_goal(Drone& d, const Vec3& pos, float radius);
const SpExt::AiGoal& ai_goal(const Drone& d);
// NDrone2_MoveToGoalPosition on the AI goal: route + link creep, returns RouteStatus as int
// (0 following, 1 approximate, 2 straight, 3 arrived, ...). Updates Drone::mv like move_to_goal_position.
int move_to_ai_goal(Drone& d);
// NDrone2_DistanceToAIPoint: 2-D distance from the feet to the AI goal (1e9 when none).
float distance_to_ai_point(const Drone& d);
// NDrone2_ReFindMissionPath 0x1542c0-ish: re-anchor the mission route at the nearest node to the drone
// and point the AI goal at it, so GoToGoalPosition walks back onto the patrol path.
void refind_mission_path(Drone& d);

// DroneAnim_SetScript: plays character script `script` as the drone's animation; `end_msg` is sent to the drone
// when a non-looping script finishes (msg 5).
void play_script(Drone& d, std::uint32_t script, int dasc, bool loop, int end_msg);

}  // namespace nf::sp

#pragma once

#include "game/drone_move.hpp"
// Registration of the single-player state handlers (NDrone2_StateFuncs @0x29b300, ids 0..194).
// Each content area lives in its own translation unit and exposes one `register_*_states()`; `register_sp_states()`
// (sp_states.cpp) calls them all once (idempotent). Areas / owners:
//   idle       sp_states_idle.cpp     Idle Alert InitPatrol Patrol ReturnToPatrolPath StandBlind SearchArea GoToGoalPosition
//                                     Investigate StandFiddle AlertToPosition Obstructed DroneStuck HeardNoise* Seen*
//                                     WaitSwitch Disabled PlayScript (framework) ...          (SpDrones)
//   combat     sp_states_combat.cpp   Attack, Combat family, Aim*/Strafe*/Step*/Roll*, GrenadeThrow, DrawWeapon
//   cover      sp_states_cover.cpp    RunForCover, UnderCover*, Sniper*, Ambush*
//   civilian   sp_states_civilian.cpp Hostage*, Civilian*, party/guard/truck/castle/interogate, RunToPoint/alarm family
//   ally       sp_states_ally.cpp     AllyLead*/AllyFollow*, Kiko*, surrender/knocked-out family
//   special    sp_states_special.cpp  Ninja, Astronaut, Abseil, stun/taser/smoked, Tester/DeleteMe/FailMission..., doors

namespace nf::sp {

// DroneFunc_CombatState 0x148338 (sp_states_combat.cpp): shared per-tick selector; `st` scripts the
// decision points for the diff-combat checker (empty = real implementations).
int combat_tick(nf::drone::Drone& d, const nf::drone::CombatStubs& st = {});

void register_sp_states();

void register_idle_states();      // also registers the search family (sp_states_search.cpp)
void register_search_states();
void register_combat_states();
void register_cover_states();
void register_civilian_states();
void register_ally_states();
void register_special_states();

}  // namespace nf::sp

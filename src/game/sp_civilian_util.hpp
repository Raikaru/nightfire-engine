#pragma once

// Shared civilian / hostage / alarm / mission layer (docs/spec-arena-ai.md Part 3 §8).
// Every helper names the original function it mirrors. The state handlers live in
// sp_states_civilian.cpp (hostages, civilians, guards, truck, castle, interrogation, alarms,
// scary-object flight) and sp_states_ally.cpp (Mayhew/Zoe/Kiko lead-follow, surrender).

#include "game/sp_common.hpp"
#include "game/sp_idle.hpp"

namespace nf::sp {

// NDrone2_CheckSurrender 0x14a060: behaviour bit 0x43, an opponent, alertness < 0.66,
// within 2.0 m and the target roughly facing the drone (|Drone+0x1d4| small).
bool check_surrender(const Drone& d);

// DroneFunc_SetMissionFailReason 0x13e560: reports (reason 1..0x10, fail label) through
// SpSystem::on_mission_fail; silent when no hook is installed.
void mission_fail(Drone& d, int reason, std::uint32_t label = 0);

// NDrone2_FindAlarmPoint 0x153008: nearest alarm AI point becomes the AI goal (RunToAlarm).
// Returns false when the level has no AI points.
bool find_alarm_point(Drone& d);

// DroneFunc_CheckAlarmRaised: the level alarm (NPCGlobals+0x1c76, SpSystem::alarm_raised)
// widens sight; drones heading for it stop running once it is raised.
inline bool alarm_raised(const Drone& d) { return sp_of(const_cast<Drone&>(d)).alarm_raised; }

// NDrone2_NearArmedDrone: nearest live armed enemy drone within 30 m; -1 when none.
// Stored on SpExt::near_drone like Drone+0x2b8.
int near_armed_drone(Drone& d);

// Paired-drone lookup by id among the SP drones (hostage killer <-> hostage, door guards).
Drone* sp_drone_by_id(Drone& d, int id);

// Civilian fear reaction shared by Civilian / PartyGirl / guards: face the threat, play the
// scared clip and escalate to CivilianScared (msg flow of NDrone2_DSTATE_Civilian 0x15d118).
void civilian_scare(Drone& d);

// Hostage-killer pairing: the killer drone paired with this hostage (Drone+0x2b0), if live.
Drone* hostage_killer_of(Drone& d);
// Test hook mirroring what the level scripts do when they fire a drone's start channel (and the
// Drone_EnableAll 0x1383e8 effect): a drone sitting in WaitSwitch is enabled and resumes with its
// initial state (TruckDriverInit for DTYPE 0x17, PlayScript when it has a script). Headless verification
// only; the game itself releases drones through switch channels. No-op unless the drone waits.
void release_waiting(Drone& d);

// Cover-node queries shared by the cover states and the combat selector (docs/spec-arena-ai.md Part 3 §7).
// Drone_IsCoverNodeUsable: switch gates pass and no live SP drone has claimed the node.
bool cover_usable(Drone& d, std::size_t index);
// NDrone2_CoverAvailable: a usable node exists (no claim made; RunForCover claims with find_cover).
bool cover_available(Drone& d);
// NDrone2_FindCover 0x152548: nearest usable node in 2-D, claimed on success (Drone+0x4f8 & 0x1000).
bool find_cover(Drone& d);
// Release a claimed node (Drone_Delete / UnderCoverLeave path).
void release_cover(Drone& d);
// DroneFunc_SetDeathChannel: set the drone's death switch channel (Drone+0x13c, DIVars key12). SP drones
// only; silent otherwise. Called on entering every death state.
void notify_death(Drone& d);
}  // namespace nf::sp

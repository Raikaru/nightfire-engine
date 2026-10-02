#pragma once

// Reference content layer that proves the drone core end to end (not a game layer): three states registered only when
// no real layer claimed the ids — 4 Idle (watch for the player), 0x56 Attack (approach along the nav route, aim, fire),
// 0x63 GoToGoalPosition (follow a route to DemoExt::goal, then Idle). They use the core exactly like the original
// states do (DroneVision_EnemyLookForOpponent / HandleImpact skeleton, MoveToObject, AnimForDist, DroneWeap_Fire).
// Used by `nfgame --demo-drone`.

#include "game/drone_system.hpp"

namespace nf::drone {

struct DemoExt : DroneExt {
    Vec3 goal{};
    bool has_goal = false;
    bool arrived_logged = false;
};

void register_demo_states();

// Spawns a drone at `feet` with sight profile 0, weapon `weapon`, health `health` and the demo behaviour bits.
// `goal` (optional) starts it in GoToGoalPosition.
Drone& spawn_demo_drone(DroneSystem& sys, const Vec3& feet, float yaw, std::uint32_t skin_hash, int sub_class,
                        int weapon, float health, const Vec3* goal = nullptr);

}  // namespace nf::drone

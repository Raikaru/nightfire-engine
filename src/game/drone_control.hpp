#pragma once

// Per-drone tick: NDrone2_PreDroneControl (timers -> messages) and NDrone2_ControlSTANDARD (alertness decay, TICK
// message, opponent search, movement, collision, firing). Called by DroneSystem::tick.

#include "game/drone.hpp"

namespace nf::drone {

void pre_drone_control(Drone& d);   // NDrone2_PreDroneControl 0x148f?: idle timeout -> msg 4, timers -> msgs 0xc / 0xd
void control_standard(Drone& d);    // NDrone2_ControlSTANDARD 0x1490f0

}  // namespace nf::drone

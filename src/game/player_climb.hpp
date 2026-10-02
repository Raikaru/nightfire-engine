#pragma once

#include "core/math.hpp"
#include "game/ladder.hpp"

namespace nf {

// Player_CollLadder: the player mounts a ladder 0.5 in front of the ladder's bounding-sphere centre (x, z), on
// the side its yaw faces (the climber then looks along yaw + pi).
Vec3 ladder_stand_point(const LadderObject& ladder, float y);

// Player_CollCreepWall / Player_Creep: the segment the creeper is locked to, the wall's bounding-sphere centre
// +- Vec_Spherical_2_Cartesian(radius, yaw + pi/2, 0) (the model's local X axis).
struct CreepLine {
    Vec3 a, b;
};
CreepLine creep_line(const CreepWallObject& wall);

}  // namespace nf

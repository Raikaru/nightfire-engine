#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "assets/level.hpp"
#include "core/math.hpp"

namespace nf {

// Ladder_Create: a control object of class 4 made from a map_data_static whose entity type is 0x22. The model
// is the ladder's visible mesh and its collision volume (thin rails, or one vertical plane for the taller
// ones); the player mounts when the capsule touches that volume (Player_DealWithObjHit -> Player_CollLadder).
// The climbing range is derived from the model's bounding sphere, `Control_BuildWorldSph`: centre = model
// sphere centre through the instance transform (obj+0x80), radius = model radius (obj+0x8C, the object scale
// obj+0xE8 is 1.0).
struct LadderObject {
    std::size_t placement = 0;   // Level::placements() index (collision volume)
    std::size_t instance = 0;    // index into the map chunk's statics
    Vec3 position{};             // obj+0x30
    float yaw = 0;               // obj+0x54: euler y; the ladder's front faces (sin yaw, 0, cos yaw)
    Vec3 center{};               // obj+0x80
    float radius = 0;            // obj+0x8C: half the ladder's height

    // BLData+0x8A0: above this height the climber steps off the top (Player_CollLadder / Player_Climb).
    float top() const { return center[1] + radius + 0.85f; }
    // BLData+0x8A4: below this height the climber is put back on the ground.
    float bottom() const { return (center[1] - radius) + 1.0f; }
};

// CreepWall_Create: a control object of class 8 ('+') made from an entity type 0x3F static ("Wall Hug ...",
// "StealthWall"). The model is a flat quad along its local X axis; the player hugs the wall and shuffles along
// the segment through the bounding sphere (Player_CollCreepWall, Player_Creep).
struct CreepWallObject {
    std::size_t placement = 0;
    std::size_t instance = 0;
    // obj+0xE0 data block: static params 0..3 as u16 (parsemap_block_map_data_dynamic stores param k at
    // level_tag+0x2C+4k). Only data[3] is read by the player code.
    std::array<std::uint16_t, 4> data{};
    Vec3 position{};             // obj+0x30
    float yaw = 0;               // obj+0x54
    Vec3 center{};               // obj+0x80
    float radius = 0;            // obj+0x8C: half the wall's length

    // Switch channel raised while a player creeps here (0 = none).
    unsigned channel() const { return data[3]; }
};

// Every ladder / creep wall of the level in static order (statics flagged 0xA000 are skipped by the original,
// instances whose model did not resolve are dropped).
std::vector<LadderObject> find_ladders(const Level& level);
std::vector<CreepWallObject> find_creep_walls(const Level& level);

}  // namespace nf

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "assets/level.hpp"
#include "core/math.hpp"

namespace nf {

// Grapple_Create: a control object of type 'Q' (0x51) made from a map_data_static of entity type 0x3A: a fixed
// hook point (model `grapple`, `GRAPPLEPOINT_*`, `graple_pulley`) the grapple hook (weapons 0x50 / 0x51) can land
// on. The object never moves and has no behaviour of its own; only its collision volume matters.
struct GrappleTarget {
    std::size_t placement = 0;   // Level::placements() index (the model's collision volume)
    std::size_t instance = 0;    // index into the map chunk's statics
    Vec3 position{};             // obj+0x30
};

std::vector<GrappleTarget> find_grapple_targets(const Level& level);

// The grapple state the player carries in BLData+0x140..0x160 (Player_SetGrapplePoint, Player_SSGrapple).
struct GrappleState {
    // BLData+0x15C. The weapon code reads it: weapons 0x50 / 0x51 cannot fire in substate 2 while it is 1.
    enum Phase : std::uint16_t { Idle = 0, Launch = 1, Hang = 2, Release = 4 };
    Vec3 point{};                 // BL+0x140: where the hook caught
    float speed = 0;              // BL+0x150: current pull, units per frame
    std::uint16_t phase = Idle;   // BL+0x15C
    std::int16_t timer = 0;       // BL+0x15E: frames before landing may end the pull
};

// Constants of Player_SetGrapplePoint / Player_SSGrapple.
constexpr std::int16_t kGrappleGroundGrace = 15;      // BL+0x15E at Player_SetGrapplePoint
constexpr float kGrappleStopDistance = 2.0f;          // pulled until this close, then the player hangs
constexpr float kGrappleAcceleration = 0.05f;         // speed gained per frame
constexpr float kGrappleTopSpeedSingle = 1.5f;        // single player
constexpr float kGrappleTopSpeedMulti = 0.5f;         // MPSettings+0x180 set (multiplayer)

// Player_SetGrapplePoint: the state right after the hook caught at `point`.
GrappleState grapple_begin(const Vec3& point);

// What Player_SSGrapple reads besides the state.
struct GrappleInputs {
    bool fire_held = false;       // Input_Action(pad, 9, 1): the fire button
    bool on_ground = false;       // collbody+0x60 & 8 from the previous collision pass
    bool multiplayer = false;     // MPSettings+0x180
    bool sight_clear = true;      // Collide_LineOfSight(player, point, pick 8); only consulted in multiplayer
};

struct GrappleStep {
    Vec3 position;                // obj+0x30 after this frame's pull
    // Phase 4 was reached on an earlier frame: Player_ChangeSubState(0) and return. Otherwise the caller runs the
    // tail (BL+0x20 = BL+0x10, BL+0x10 = 0, so the walk step of this frame is discarded).
    bool leave = false;
};

// Player_SSGrapple after Player_Move: the countdown, the phase machine and the pull towards the hook point.
GrappleStep grapple_step(GrappleState& g, const Vec3& position, const GrappleInputs& in);

// Player_GrappleSetRope: the rope object is drawn while the grapple weapon is out (weapon 0x50 / 0x51) and either
// the pull is starting (phase 1) or the weapon is still locked by a hook in flight (BL+0x160).
bool grapple_rope_visible(const GrappleState& g, bool weapon_locked, int weapon_id);

}  // namespace nf

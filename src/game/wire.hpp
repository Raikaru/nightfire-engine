#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "assets/level.hpp"
#include "core/math.hpp"

namespace nf {

// Wire_Create: a control object of class ',' (0x2C) made from a map_data_static whose entity type is 0x3E. The
// model is a thin box along its local X axis; the wire runs through the model's bounding sphere.
enum class WireType : std::uint16_t {
    Wire = 0,       // hang and shimmy sideways (Player_Wire, substate 6)
    Reserved = 1,   // Player_CollWire returns at once, the tracking camera treats it like a creep wall
    Zipline = 2,    // slide to the lower end (Player_Zipline, substate 15)
};

// One wire object. The `data` block at obj+0xE0 is {u16 type, u16 camera, u16 unused, u16 channel} = static
// params 1, 0, 2, 3 (parsemap_block_map_data_dynamic stores param k at level_tag+0x2C+4k, Wire_Create copies them).
struct WireObject {
    std::size_t placement = 0;        // Level::placements() index (its collision volume)
    std::size_t instance = 0;         // index into the map chunk's statics
    std::uint16_t type = 0;           // data+0, a WireType
    std::uint16_t camera = 0;         // data+2: ThirdPersonCameras slot (0 = the default tracking camera)
    std::uint16_t channel = 0;        // data+6: switch channel raised while a player uses the wire
    Vec3 position{};                  // obj+0x30
    float yaw = 0;                    // obj+0x54: euler y
    float pitch = 0;                  // obj+0x58: -euler z (Wire_Create negates it before the object is built)
    Vec3 center{};                    // obj+0x80: bounding sphere centre (Control_BuildWorldSph)
    float radius = 0;                 // obj+0x8C: bounding sphere radius, the wire's half length
};

// The static instances of entity type `entity` (StaticInstance::flags, the parsemap_create_dynamic_objects switch
// value) that the original turns into objects: instances flagged 0xA000 are skipped. Instances whose model did not
// resolve have no placement and are dropped.
struct ObjectStatic {
    std::size_t instance;    // index into the map chunk's statics
    std::size_t placement;   // Level::placements() index
};
std::vector<ObjectStatic> find_object_statics(const Level& level, std::uint32_t entity);

// Every wire of the level, in static order (parsemap_create_dynamic_objects case 0x3E; statics flagged 0xA000
// are skipped by the original).
std::vector<WireObject> find_wires(const Level& level);

// Vec_Spherical_2_Cartesian(rho, theta, phi): theta counts from +Z towards +X, phi is the elevation.
Vec3 spherical_to_cartesian(float rho, float theta, float phi);

struct WireEnds {
    Vec3 a, b;
};
// The wire's end points: centre +- Vec_Spherical_2_Cartesian(radius - trim, yaw + pi/2, phi). The callers use
// different trims (Player_CollWire 2.0, Player_Wire 0.5, Player_Zipline and the camera none) and Player_Wire
// flattens the line with phi = 0.
WireEnds wire_ends(const WireObject& wire, float trim, float phi);

// vecutil_calculate_closest_point_on_line: nearest point of the segment a-b to p (a for a degenerate segment).
Vec3 closest_point_on_segment(const Vec3& a, const Vec3& b, const Vec3& p);
// Vec_AngleDifference(a, b): b - a with both angles taken modulo 2 pi, wrapped into (-pi, pi).
float angle_difference(float a, float b);
float dist_2d_sq(const Vec3& a, const Vec3& b);

// ThirdIcon_Create (entity type 0xFA): an invisible trigger volume that tells the player which prompt applies
// while it stands inside (Player_HandleJump reads icon 3 = "jump onto a wire").
struct IconZone {
    std::size_t placement = 0;
    std::uint32_t icon = 0;           // data+0 (param 0)
    std::uint16_t on_channel = 0;     // data+4 (param 1): the zone is inactive until this switch channel is set (0 = always)
    std::uint16_t off_channel = 0;    // data+6 (param 2): setting it deletes the zone
};
std::vector<IconZone> find_icon_zones(const Level& level);

}  // namespace nf

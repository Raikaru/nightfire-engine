#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "assets/level.hpp"
#include "core/math.hpp"
#include "game/collision_world.hpp"

namespace nf {

// cel+0x90 of a cel that holds no water (build_alloc_cel writes 0xC3FA0000).
inline constexpr float kNoWater = -500.0f;

// Low 16 bits of StaticInstance::flags of the cels that matter for water (parseentity_fixup_entity rewrites them
// to 0x804C047 / 0x804C023 / 0x8017: the 0x40000 bit marks a "room" cel, the unit build_FindCel searches).
namespace celflag {
constexpr std::uint32_t kRoom = 0xC047;         // a room: cel+0x90 starts at kNoWater
constexpr std::uint32_t kWaterRoom = 0xC023;    // an underwater room: cel+0x90 starts at the top of its box
constexpr std::uint32_t kWaterSurface = 0x8017; // a water plane: sets cel+0x90 of the room that holds it
}  // namespace celflag

// The level's rooms, the "cel" the original links every object to (obj+0x20) and whose `cel+0x90` is the water
// level Player_InWater / Player_MonitorAir / Player_Collision read. Rooms are the world statics flagged 0xC047 or
// 0xC023; their box is the model's box (their placement is zeroed, so it is already in world space). Rooms touch
// through portals (block 0x21): crossing one moves the object to the room behind it (Cel_ObjectLeftCel), and an
// object outside its room's box is re-found by box test plus vertical ray tie-break (build_FindCel).
class RoomMap {
public:
    static constexpr int kNone = -1;

    // Builds the rooms, their portals and the water levels: a 0xC023 room starts at the top of its box (cel+0x54),
    // then every 0x8017 water plane sets cel+0x90 of the room it lies in to its own lowest y (build_link_objects_to_rooms).
    RoomMap(const Level& level, const CollisionWorld& world);

    std::size_t size() const { return rooms_.size(); }

    // build_FindCel: the room holding `p`, kNone outside all of them.
    int find(const Vec3& p, const CollisionWorld& world) const;
    // control_handle_cel_change for an object that moved `from` -> `to` while in `room` (kNone = not linked):
    // through a portal of the room, else (still inside the room's box) unchanged, else find(to).
    int track(int room, const Vec3& from, const Vec3& to, const CollisionWorld& world) const;
    // View_AddCels candidates: seed from the viewer's linked cel and follow only portals visible through
    // the progressively clipped frustum, matching Vision_Portal_Recurse's cell list.
    void mark_visible_cells(int viewer, const Vec3& eye, const Vec3& right, const Vec3& up,
                            const Vec3& forward, float tan_half_x, float tan_half_y,
                            std::vector<std::uint8_t>& visible) const;
    bool cell_in_view(int target, const std::vector<std::uint8_t>& visible, const Vec3& eye, const Vec3& right,
                      const Vec3& up, const Vec3& forward, float tan_half_x, float tan_half_y) const;
    // cel+0x90 (kNoWater for kNone).
    float water_level(int room) const;

    // Tools / tests.
    const std::string& name(int room) const { return rooms_.at(std::size_t(room)).name; }
    std::uint32_t flags(int room) const { return rooms_.at(std::size_t(room)).flags; }
    const Vec3& box_min(int room) const { return rooms_.at(std::size_t(room)).min; }
    const Vec3& box_max(int room) const { return rooms_.at(std::size_t(room)).max; }
    bool contains(int room, const Vec3& p) const;

private:
    struct Portal {
        std::array<Vec3, 4> quad;
        Vec3 lo, hi;   // quad bounds padded by 0.1 (build_alloc_portal)
        int dest;
    };
    struct Room {
        std::string name;
        std::uint32_t flags;
        std::size_t placement;   // index into Level::placements(): where the collision of this room lives
        Vec3 min, max;
        float water;
        Vec3 sphere_center;
        float sphere_radius;
        std::vector<Portal> portals;   // in the order Cel_ObjectLeftCel visits them (last built first)
    };
    bool crosses_portal(const Portal& portal, const Vec3& from, const Vec3& to) const;

    std::vector<Room> rooms_;   // file order; the original's cel list is walked from the back
};

// The player's air / cel state (BLData +0x8C0..+0x8C8, obj+0x20, the `surfaced` global).
struct WaterState {
    int room = RoomMap::kNone;     // obj+0x20, updated by the collision pass (control_handle_cel_change)
    Vec3 room_from{};              // obj+0x40: where the last portal test started
    float air = 100.0f;            // BL+0x8C0: 100 = full, -5 per second under water, +20 per second above; 0 drowns
    bool surfaced = true;          // `surfaced`: head out of the water; also the camera's eye-height target
    float meter_alpha = 0.0f;      // BL+0x8C4: HUD air meter fade (1 under water, eases out at half speed)
    std::uint32_t meter_flags = 0; // BL+0x8C8: 0x10 = under water (or on a wire), 1 = the meter was just surfaced from
    bool meter_enabled = false;    // HUD pane 5 is up in substates 3 and 6
    std::uint64_t frame = 0;       // GameState+0x34: the logic frame counter Player_MonitorAir paces the drown damage with
};

// Player_MonitorAir's sounds (Sound_Play ids): the gasp on surfacing, the cough with each drowning hit, the
// bubble loop started when the head goes under.
namespace watersound {
constexpr int kGasp = 0x60C, kDrownHit = 0x60B, kBubbles = 0x27;
}

}  // namespace nf

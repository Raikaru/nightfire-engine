#pragma once

// Single-player enemy placement from map statics (docs/spec-arena-ai.md Part 3 §1): the entity types that
// parsemap_create_dynamic_objects (0x1d04f0) turns into Drone_Create (0x0f), Drone_CoverCornerNode (0xe5),
// Drone_CoverLowNode (0xe6), Drone_AIPoint (0xe9), DroneSpawner_Create (0xf1) and Drone_AIVolume_Create (0xf5/0xf8).
// Params live at level_tag+0x2c+4*key, i.e. StaticInstance::param(key).

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "assets/level.hpp"
#include "game/sp_tables.hpp"

namespace nf::sp {

// Map object ids.
constexpr std::uint32_t kClassNpc = 0x0f, kClassCoverCorner = 0xe5, kClassCoverLow = 0xe6, kClassAiPoint = 0xe9,
                        kClassSpawner = 0xf1, kClassAiVolumeA = 0xf5, kClassAiVolumeB = 0xf8;

// CoverNode_tag (0xb0 B, CoverNodes @0x283e10): the placement part (spec §1.3).
struct CoverNodeDef {
    enum class Type : std::uint8_t { Corner = 0, Low = 1 } type = Type::Corner;
    std::uint8_t require_on = 0;     // +1  switch channel that must be ON (0 = none)
    std::uint8_t require_off = 0;    // +2  switch channel that must be OFF
    std::uint8_t anim_a = 0, anim_b = 0;   // +3/+4 allowed anim ids (low nodes) ...
    std::uint8_t range_a = 8, range_b = 8; // +5/+6 corner ranges
    float max_angle = 0.7853982f;    // +8  max approach angle (45 deg default)
    std::uint32_t flags = 0;         // +0xc  (bits: 4/8 usable from right/left, 0x20/0x40 & 0x80/0x100 lean/step-out, 1/2 low-cover)
    Vec3 pos{};                      // +0x20
    float yaw = 0;                   // +0x34 (rot.y of the placement)
    std::uint32_t static_index = 0;  // index into the map statics
};

// AIPoint (0xa0 B, AIPoints @0x28ee10).
struct AiPointDef {
    std::uint16_t id = 0;            // +6
    float radius = 0;                // +0x14 (< 0: world cel lookup)
    std::uint32_t param34 = 0;       // +0xc
    Vec3 pos{};                      // +0x20
    Vec3 rot{};                      // +0x40
};

// AI volume (obj type 0x3e, data at obj+0xe0): an oriented box on the placement transform. The original also
// negates the placement's z angle on level 0x7000005 / kind 4; we use the placed quaternion as is.
struct AiVolumeDef {
    Vec3 center{};
    std::array<float, 9> axes{1, 0, 0, 0, 1, 0, 0, 0, 1};   // box axes (unit columns) from the placement quaternion
    Vec3 half{};                     // +0x10.. half extents = |bbox size| * 0.5 * scale
    float visibility = 1.0f;         // +0x20 = param0 * 0.01 (multiplier for the player standing inside)
    std::uint16_t kind = 0;          // +0x24: param1 0 -> 0, 2 -> 1, 1 -> 2, 4 -> 3 (only kind 0 scales visibility)
    std::uint32_t p28 = 0;           // +0x28 = param2
    std::uint8_t p2c = 0;            // +0x2c = param3
    std::uint16_t p26 = 0;           // +0x26 = param4
    bool contains(const Vec3& p) const;   // Intersect_PointOOBox
};

// DroneSpawner data (0x3c B at obj+0xe0), placement part.
struct SpawnerDef {
    std::uint32_t mode = 0;          // +0: 2 trickle, 3 wave (must be >= 2)
    std::uint32_t group = 0;         // +4: 1..30
    std::uint16_t max_alive = 0;     // +0xa
    std::uint16_t budget = 32000;    // +0xc (0 -> 32000)
    // Channels (0 = none): activate (key 4) waits for ON; stop (key 5) ON -> no more spawns, drones stay; complete
    // (key 6) is set ON when the budget is spent and nothing is alive; kill (key 7) ON -> remaining drones deleted.
    std::uint16_t activate_channel = 0, stop_channel = 0, complete_channel = 0, kill_channel = 0;
    float min_distance = 10.0f;      // +0x18 (0 -> 10.0)
    Vec3 pos{};
    float yaw = 0;
};

// A placed NPC plus where it came from.
struct PlacedNpc {
    SpNpcSpec spec;
    std::uint32_t static_index = 0;
    std::uint32_t group() const { return spec.key4; }   // DIVars+0x44 -> Drone+0x144 (NDrone2_AddToGroup)
};

struct SpLevel {
    std::uint32_t level_id = 0;
    std::vector<PlacedNpc> npcs;              // all class-0x0f statics, map order, before the difficulty gate
    std::vector<CoverNodeDef> cover_nodes;
    std::vector<AiPointDef> ai_points;
    std::vector<AiVolumeDef> volumes;
    std::vector<SpawnerDef> spawners;         // only mode >= 2 (DroneSpawner_Create)

    // Drone_Create's gate: `level_tag[+0x34] <= difficulty`.
    bool placed_at(const PlacedNpc& n, int difficulty) const { return n.spec.min_difficulty <= std::uint32_t(difficulty); }
    // NPCs that exist on `difficulty` (Drone_Create), map order.
    std::vector<const PlacedNpc*> npcs_for(int difficulty) const;
    // Templates of spawner group `g` among those (a group only spawns if a spawner for it exists).
    std::vector<const PlacedNpc*> group_members(std::uint32_t g, int difficulty) const;
    // Drone_VisibilityForPosition: product of the multipliers of the kind-0 volumes containing `p`.
    float visibility_at(const Vec3& p) const;
    // Drone_PointInAnyAIBox(p, kind).
    bool in_volume(const Vec3& p, std::uint16_t kind) const;
};

// Parses the map statics of `level` (level id from the bin name, 0 if unknown).
SpLevel parse_sp_level(const nf::Level& level, std::uint32_t level_id);

// Chunk-file model name of a static (empty if unresolved) - used by tools.
std::string static_model_name(const nf::Level& level, const nf::StaticInstance& s);

}  // namespace nf::sp

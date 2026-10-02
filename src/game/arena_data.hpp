#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "assets/elf.hpp"
#include "assets/level.hpp"
#include "core/math.hpp"

namespace nf {

// Level data the multiplayer rules read (Part 1 of docs/spec-arena-ai.md): every map_data_static record whose
// placement type (StaticInstance::flags, the switch value of parsemap_create_dynamic_objects) is one of
//   0x25 (37)  MP_RegisterSpawnPoint(pos, euler, param0 = team)
//   0x26 (38)  MP_RegisterMPObject(pos, euler, level_tag, celglist)      objective objects (Part 1B)
//   0xF0 (240) Pickup_Create(pos, quat, 0, celglist, param0..param6, grounded = 1)
constexpr std::uint32_t kPlacementMpSpawn = 0x25, kPlacementMpObject = 0x26, kPlacementPickup = 0xF0;

// A registered MP spawn point (MP_RegisterSpawnPoint): the marker position and heading. `team` 0/1 restricts
// the point to that team's range in team games; 2 is ignored by the original and dropped here.
struct MpSpawnMarker {
    Vec3 pos;        // marker position as authored (the runtime point is `floor + 1.6`, see ArenaSystem)
    float yaw;       // euler.y
    int team;        // param0
    std::size_t instance;
};

// Pickup_Create arguments from the placement's params (level_tag+0x2c + 4*key).
struct PickupPlacement {
    std::size_t instance = 0;   // index into the Map chunk's statics
    Vec3 pos{};
    Vec3 euler{};
    int category = 0;           // param0 (PICKUPINFO+0x22): 0 weapon, 1 ammo, 2 key, 3 armour, 4 message, 5 clear flag, 6 bonus, 7..11 weapon-set slot
    int item = 0;               // param1 (+0x24): weapon id / ammo weapon id / key id
    int amount = 0;             // param2 (+0x26)
    int channel = 0;            // param3 (+0x28): switch channel set on pickup
    int sound = 0;              // param4 (+0x2a): 0 = default
    int respawn_units = 0;      // param5 (+0x2c): 10 s units, 0 one-shot, 0xFFFF never removed
    std::uint32_t message = 0;  // param6 (+0x34): override message label
};

// MP_RegisterMPObject input: param0 = kind, param1 = team/side, param2 = label / link id, param3 = flags
// (8 or 1), param5 = second link id. The remaining params carry the effect ids the visuals use.
struct MpObjectPlacement {
    std::size_t instance = 0;
    Vec3 pos{};
    Vec3 euler{};
    int kind = 0;
    int team = 0;
    int label = 0;
    int flags = 0;
    int link = 0;
};

struct ArenaLevelData {
    std::vector<MpSpawnMarker> spawns;       // team 0/1 markers, in map order (team 2 dropped)
    std::vector<PickupPlacement> pickups;
    std::vector<MpObjectPlacement> objects;
};

// Reads the three placement kinds from the Map chunk (skipping records with the 0xA000 flag bits,
// parsemap_block_map_data_dynamic).
ArenaLevelData read_arena_level(const Level& level);

// PickupMatrix @0x2B9100 (11 rows x 5 x i16): the weapon set each pickup slot 7..11 resolves to.
// Row 10 (Random) is rebuilt per match by Pickup_MakeRandomWeaponSet.
struct WeaponSets {
    static constexpr int kRows = 11, kSlots = 5, kRandomRow = 10, kUseableGuns = 27;
    std::array<std::array<std::int16_t, kSlots>, kRows> matrix{};
    std::array<std::int16_t, kUseableGuns> useable{};   // UseableGuns @0x2B9170

    // Both tables straight out of ACTION.ELF's initialised data.
    static WeaponSets from_elf(const Elf32& action);
    // The tables as the shipped binary holds them (used when no ELF is at hand, and cross-checked by nfdump validate).
    static WeaponSets builtin();
};

}  // namespace nf

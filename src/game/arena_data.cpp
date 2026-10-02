#include "game/arena_data.hpp"

#include <cstring>
#include <stdexcept>

namespace nf {

namespace {

constexpr std::uint32_t kHiddenFlags = 0xA000;  // parsemap_block_map_data_dynamic skips these records

// Embedded copies of PickupMatrix / UseableGuns (values read from ACTION.ELF, USA SLUS-20579).
constexpr std::array<std::array<std::int16_t, 5>, 11> kPickupMatrix = {{
    {6, 28, 30, 24, 44},    // 0 Normal
    {2, 6, 10, 16, 66},     // 1 Pistols
    {18, 21, 24, 28, 82},   // 2 Automatic
    {14, 30, 17, 36, 50},   // 3 Sniping
    {42, 44, 58, 46, 26},   // 4 Explosives 1
    {53, 52, 58, 55, 59},   // 5 Explosives 2
    {2, 17, 55, 36, 66},    // 6 MI6 Operative
    {10, 30, 82, 26, 50},   // 7 Phoenix Weapons
    {26, 59, 50, 58, 44},   // 8 State of the Art
    {21, 22, 55, 17, 36},   // 9 Cloak and Dagger
    {0, 0, 0, 0, 0},        // 10 Random (rebuilt per match)
}};
constexpr std::array<std::int16_t, 27> kUseableGunsTable = {2,  6,  10, 12, 14, 16, 18, 20, 22, 25, 26, 28, 30, 36,
                                                       42, 44, 46, 51, 55, 53, 54, 52, 58, 59, 66, 17, 82};

Vec3 vec3(const std::array<float, 3>& a) { return {a[0], a[1], a[2]}; }

}  // namespace

ArenaLevelData read_arena_level(const Level& level) {
    ArenaLevelData out;
    const ChunkFile* map = level.map();
    if (!map) return out;
    const auto& statics = map->chunk.statics;
    for (std::size_t i = 0; i < statics.size(); ++i) {
        const StaticInstance& s = statics[i];
        if (s.flags & kHiddenFlags) continue;
        switch (s.flags) {
            case kPlacementMpSpawn: {
                const int team = int(std::uint16_t(s.param(0)));   // (u16)param0
                if (team == 2) break;   // MP_RegisterSpawnPoint: team 2 => ignored
                out.spawns.push_back({vec3(s.position), s.euler[1], team, i});
                break;
            }
            case kPlacementMpObject: {
                MpObjectPlacement o;
                o.instance = i;
                o.pos = vec3(s.position);
                o.euler = vec3(s.euler);
                o.kind = int(s.param(0));
                o.team = int(s.param(1));
                o.label = int(s.param(2));
                o.flags = int(s.param(3));
                o.link = int(s.param(5));
                out.objects.push_back(o);
                break;
            }
            case kPlacementPickup: {
                PickupPlacement p;
                p.instance = i;
                p.pos = vec3(s.position);
                p.euler = vec3(s.euler);
                p.category = int(std::uint16_t(s.param(0)));
                p.item = int(std::uint16_t(s.param(1)));
                p.amount = int(std::int16_t(s.param(2)));
                p.channel = int(std::int16_t(s.param(3)));
                p.sound = int(s.param(4));
                p.respawn_units = int(std::uint16_t(s.param(5)));
                p.message = s.param(6);
                out.pickups.push_back(p);
                break;
            }
            default:
                break;
        }
    }
    return out;
}

WeaponSets WeaponSets::builtin() {
    WeaponSets w;
    w.matrix = kPickupMatrix;
    w.useable = kUseableGunsTable;
    return w;
}

WeaponSets WeaponSets::from_elf(const Elf32& action) {
    WeaponSets w;
    const auto matrix = action.image_range(0x2B9100, kRows * kSlots * 2);
    const auto useable = action.image_range(0x2B9170, kUseableGuns * 2);
    if (matrix.size() != std::size_t(kRows * kSlots * 2) || useable.size() != std::size_t(kUseableGuns * 2))
        throw std::runtime_error("ACTION.ELF: PickupMatrix/UseableGuns out of range");
    for (int r = 0; r < kRows; ++r)
        for (int s = 0; s < kSlots; ++s) {
            std::int16_t v;
            std::memcpy(&v, &matrix[std::size_t(r * kSlots + s) * 2], 2);
            w.matrix[std::size_t(r)][std::size_t(s)] = v;
        }
    for (int i = 0; i < kUseableGuns; ++i) std::memcpy(&w.useable[std::size_t(i)], &useable[std::size_t(i) * 2], 2);
    return w;
}

}  // namespace nf

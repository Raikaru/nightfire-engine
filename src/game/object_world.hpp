#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "game/collision_world.hpp"
#include "game/ladder.hpp"
#include "game/wire.hpp"

namespace nf {

// What the capsule's collision pass learned from the trigger volumes besides solid hits.
struct ObjectContacts {
    // ThirdIcon_Update: `icon` of the active ThirdIcon zone the capsule overlaps (the highest-numbered when
    // several do). The player latches it as BLData+0x95F for the next frame.
    std::optional<std::uint32_t> icon;
};

// The object-class part of the level that the player's collision pass and player-only interactions see.
// `CollisionWorld` holds the world cels; the objects created by parsemap_create_dynamic_objects that have
// collision models live here:
//   * solids: ladders (0x22), grapple points (0x3A), wires (0x3E). Collide_Pick treats them like cels, so their
//     triangles push the capsule and appear in the hit list (CollisionHit::placement identifies the object).
//   * trigger zones: ThirdIcon (0xFA, ghost volumes, model flag 0x40): overlaps are recorded, never pushed.
// It also owns the object registries and the game's `switch_channels` array.
class ObjectWorld {
public:
    explicit ObjectWorld(Level& level);

    // Continues a capsule query whose world-cel part is in `merged` (the result of CollisionWorld::cylinder(q)):
    // the objects are tested from the capsule as the cels left it, their push is added to `merged`, their hits
    // are merged in ascending distance (stable) and the trigger zones fill `contacts`.
    void collide(const CylinderQuery& q, CylinderResult& merged, ObjectContacts& contacts) const;

    // `switch_channels`: 1 byte per channel, ids beyond the array read as clear.
    static constexpr std::size_t kChannels = 256;
    bool channel(unsigned id) const { return id < kChannels && switch_channels_[id] != 0; }
    void set_channel(unsigned id, bool on) {
        if (id < kChannels) switch_channels_[id] = on ? 1 : 0;
    }

    const std::vector<LadderObject>& ladders() const { return ladders_; }
    const std::vector<CreepWallObject>& creep_walls() const { return creep_walls_; }
    const LadderObject* ladder_at(std::size_t placement) const;

private:
    CollisionWorld solids_;
    CollisionWorld zones_;
    std::vector<LadderObject> ladders_;
    std::vector<CreepWallObject> creep_walls_;
    std::vector<IconZone> icons_;
    std::array<std::uint8_t, kChannels> switch_channels_{};
};

}  // namespace nf

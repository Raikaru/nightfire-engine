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

// A solid volume a script drives around (lifts, doors, sliding platforms, the vehicles the player can
// stand on). The original moves such objects through their own update and the player's capsule pass
// pushes out of them like any other solid; this port additionally carries a standing player by the
// frame's displacement (ride_displacement), which the original gets via the object link. [INFERENCE:
// the carry path is a reimplementation: no ACTION.ELF lift trace pins down its exact frame order.]
struct Mover {
    Vec3 min{}, max{};          // world-space bounds this frame
    Vec3 displacement{};        // max minus last frame's max: how far the volume moved this frame
    std::uint32_t id = 0;       // script handle (the Driving/Scripting slice's object id)
};

class ObjectWorld {
public:
    explicit ObjectWorld(Level& level);

    // Continues a capsule query whose world-cel part is in `merged` (the result of CollisionWorld::cylinder(q)):
    // the objects are tested from the capsule as the cels left it, their push is added to `merged`, their hits
    // are merged in ascending distance (stable) and the trigger zones fill `contacts`.
    void collide(const CylinderQuery& q, CylinderResult& merged, ObjectContacts& contacts) const;
    // Script-driven solids for this frame (lifts, doors, platforms). The Driving/Scripting slice calls
    // this once per tick before the players resolve; collide() pushes the capsule out of them and a
    // standing player is carried by ride_displacement(). Empty by default (no movers, no cost).
    void set_movers(std::vector<Mover> movers) { movers_ = std::move(movers); }
    const std::vector<Mover>& movers() const { return movers_; }
    // Displacement of the mover whose top face is under `feet` (within the capsule radius sideways and
    // 0.3 below), or zero: standing on it carries the player. `feet` is the ground probe point.
    Vec3 ride_displacement(const Vec3& feet, float radius) const;

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
    std::vector<Mover> movers_;
};

}  // namespace nf

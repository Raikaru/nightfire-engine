#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "assets/level.hpp"
#include "core/math.hpp"
#include "game/collision_world.hpp"
#include "game/grapple.hpp"
#include "game/wire.hpp"

namespace nf {

class CharacterBank;
struct SkinDef;

// The level's rope-like objects: the wires / zip lines (Wire_Create), the grapple hook points (Grapple_Create)
// and the ThirdIcon prompt volumes, built once from the map's statics. Collision against them goes through the
// shared ObjectWorld; this class is the object data the player code needs.
class RopeWorld {
public:
    explicit RopeWorld(Level& level);

    const std::vector<WireObject>& wires() const { return wires_; }
    const std::vector<GrappleTarget>& grapples() const { return grapples_; }
    const std::vector<IconZone>& icon_zones() const { return zones_; }

    // The wire whose collision volume is Level::placements()[placement], if it is one.
    const WireObject* wire_at(std::size_t placement) const;
    // Player_HandleJump's scan over the control list: the ',' object closest to `pos` (obj+0x30 distance, any type)
    // within 1000 units.
    const WireObject* nearest_wire(const Vec3& pos) const;

    // A hook (weapons 0x50 / 0x51) that ends on a 'Q' object calls Player_SetGrapplePoint at the bullet's position.
    // This is the object test for it: the first grapple point a segment from -> to hits, and where.
    struct GrappleHit {
        std::size_t target;   // index into grapples()
        Vec3 point;
    };
    std::optional<GrappleHit> grapple_ray(const Vec3& from, const Vec3& to) const;

    // MPSettings+0x180: multiplayer rules (slower pull, line of sight required).
    bool multiplayer = false;

private:
    std::vector<WireObject> wires_;
    std::vector<GrappleTarget> grapples_;
    std::vector<IconZone> zones_;
    CollisionWorld grapple_collision_;
};

// The animation calls Player_Wire / Player_Zipline / Player_CollWire make on the player's AnimObject. Their
// movement is animation-driven: a shimmy step moves the player by the root bone of its script, and the next step
// starts when `AnimScriptIsStopped`. A frontend that has the level's CharacterBank supplies one per player
// (make_rope_animator); without one the scripts finish instantly and move nothing.
class RopeAnimator {
public:
    virtual ~RopeAnimator() = default;
    // AnimScriptAdd / AnimScriptAddSpeed: replace the running scripts, `speed` = frames per logic frame.
    virtual void play(std::uint32_t script, float speed) = 0;
    // AnimScriptIsStopped: a script is loaded and has reached its end.
    virtual bool stopped() const = 0;
    // AnimObjectUpdate: one logic frame; returns the root bone's translation change in the object's frame.
    virtual Vec3 tick() = 0;
    // AnimGetFrameTransRaw: root bone translation of `sequence` at `frame` (1-based, clamped).
    virtual std::optional<Vec3> frame_translation(std::uint32_t sequence, int frame) const = 0;
};

std::shared_ptr<RopeAnimator> make_rope_animator(const CharacterBank& bank, const SkinDef& skin);

// Player_PositionCamera mode 0xB while on a wire or zip line: eye, heading (forward = (sin, cos)) and pitch
// (positive looks up) of the third-person tracking camera.
struct RopeCamera {
    Vec3 eye;
    float yaw;
    float pitch;
};

// The wire / zip line / grapple half of the player's BLData (offsets in the original in comments).
struct RopeState {
    GrappleState grapple;                   // BL+0x140..0x15E
    const WireObject* wire = nullptr;       // BL+0x884: the wire object hung on
    std::uint8_t wire_state = 0;            // BL+0x94D: 6 idle, 7 / 8 shimmying, 9 leaving a zip line, 10 turned, 0xC mounting
    float wire_timer = 0;                   // BL+0x8CC: 100 / (30 * FRAME_RATE) per frame; the grip gives out at 100
    std::uint32_t last_script = 0;          // `oldhash`: the animation script Player_Wire started last
    std::uint8_t attach_lockout = 0;        // BL+0x951: frames during which no wire / ladder can be grabbed
    bool weapon_stowed = false;             // Player_WeaponNone on attach, collbody+0x63 = +0x64 on release
};

}  // namespace nf

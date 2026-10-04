#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "assets/collision.hpp"
#include "assets/level.hpp"
#include "core/math.hpp"

namespace nf {

// Static-level collision, a port of ACTION.ELF's Collide_* / Intersect_* family.
//
// The original runs every query as a HITTEST record (see docs/gameplay.md "Collision"): a ray,
// point or capsule plus two flag words. Collide_Pick gathers the candidate static instances,
// Collide_Intersect runs Intersect_RayGeom / Intersect_CylGeom on each in its model space and
// yields at most ONE HITDATA per instance (its nearest triangle), and the resulting list is
// sorted by HITDATA+0xC. The wrappers below expose exactly those records.

// HITTEST+0x98 (`pick`): Collide_Filter / Collide_Pick mask. Only the bits that act on static
// collision are listed; others are accepted and ignored.
namespace pick {
constexpr unsigned kIgnoreMaterials0D0E = 0x01;  // materials 0x0D, 0x0E
constexpr unsigned kIgnoreMaterial10 = 0x02;     // material 0x10
constexpr unsigned kIgnoreGhost = 0x08;          // hit ghost (flags & 2) or material bit 0x40/0x80
constexpr unsigned kSkipObjects = 0x10;          // no effect: the world holds no objects
constexpr unsigned kSkipStatics = 0x80;          // skip every static instance
constexpr unsigned kIgnoreMaterialBit40 = 0x800; // material bit 0x40 rejected
}  // namespace pick

// HITTEST+0x9A (`hit`): Collide_Intersect / Intersect_*Geom flags. The low byte is OR-ed into
// CollisionHit::flags before intersecting.
namespace hitflag {
constexpr unsigned kNoPushOut = 0x06;    // (flags & 6): capsule hits are recorded but never push
constexpr unsigned kFirstHit = 0x10;     // stop at the first accepted hit (Collide_LineOfSight)
}  // namespace hitflag

// HITDATA (0x60 bytes), the parts static collision fills in.
struct CollisionHit {
    float dist = 0;              // +0x0C. Ray: |from - point|. Capsule: axis-to-surface distance (< radius).
    Vec3 normal{};               // +0x10 triangle normal, world space (unit).
    Vec3 point{};                // +0x40 ray: hit point. Capsule: closest point on the triangle.
    std::uint8_t material = 0;   // +0x50 material byte of the triangle
    std::uint8_t flags = 0;      // +0x51: low byte of the `hit` flags passed in (bits 1/2: no push-out)
    std::size_t placement = 0;   // index into Level::placements()
    std::size_t triangle = 0;    // index into that placement's Collision::tris
};

// Player_Collision's capsule: BLData+0x760 (a) and +0x770 (b) = HITTEST+0x20/+0x30, radius +0x7CC.
struct CylinderQuery {
    Vec3 a{}, b{};
    float radius = 0;
    unsigned pick = 0;   // HITTEST+0x98
    unsigned hit = 0;    // HITTEST+0x9A
    // Placement (Level::placements() index) whose collision is skipped: HITTEST+0x78, the object the
    // capsule must ignore (Player_CollWire stores the wire it hangs on there).
    std::size_t ignore = SIZE_MAX;
};

struct CylinderResult {
    // HITTEST+0x10 (BLData+0x750), accumulated push-out; Player_CollisionHandler adds it to obj+0x30.
    Vec3 push_out{};
    // HITTEST+0x20/+0x30 after every push (capsule end points the original leaves in BLData).
    Vec3 a{}, b{};
    // HITTEST+0x9C; bit 0 = the capsule rests on a surface facing its axis (floor contact).
    unsigned contact = 0;
    // obj+0xD0: one hit per touched placement, ascending `dist` (ties ordered like QuickSort).
    std::vector<CollisionHit> hits;
};

struct FeetResult {
    // collbody+0x60 bit 8 (on ground) = `on_ground`.
    bool on_ground = false;
    // BLData+0x110 (written only when a ground ray hit was in range; otherwise 1.0).
    float ground_normal_y = 1.0f;
    // First accepted ray hit, regardless of feet range or material filter.
    std::optional<CollisionHit> nearest;
    // The in-range ground ray hit (iVar10 in the original), if any.
    std::optional<CollisionHit> ground;
    // The nearest ground ray hit whose material is non-zero (iVar9), used for footsteps / collbody.
    std::optional<CollisionHit> surface;
};

// Orientation rows of the original's MATRIX (+0x00 Mat_GetNorm, +0x10 Mat_GetUp, +0x20 Mat_GetDir).
struct PlaceFrame {
    Vec3 right{1, 0, 0}, up{0, 1, 0}, dir{0, 0, 1};
};

// Collide_PlaceObject: `pos`/`frame` are the in-out values of the original's VECTOR*/MATRIX*.
struct PlaceResult {
    bool grounded = false;  // the probe ray hit a surface (otherwise only pos.y += 0.05 happened)
    bool fits = false;      // the function's return value
    Vec3 pos{};
    PlaceFrame frame{};     // re-aligned to the surface only when the flatness scan completed
};

class CollisionWorld {
public:
    // Parses the coll_data_new block of every world placement (map_data_static flag 0x8000: the
    // original's cels) and builds their world-space bounds. Placements without collision, and the
    // object-class statics (spawn markers, props, pickups: parsemap_create_dynamic_objects), are
    // kept out of every query.
    explicit CollisionWorld(Level& level);
    // The same queries over the *object* statics of the given classes (StaticInstance::flags & 0xFFFF,
    // the parsemap_create_dynamic_objects switch value; instances flagged 0xA000 are skipped like the
    // original does) instead of the world cels: their model collision at the static's transform. Used for
    // the object-class colliders the player code reacts to (wires, grapple points, trigger volumes).
    static CollisionWorld of_objects(Level& level, std::span<const std::uint32_t> classes);
    ~CollisionWorld();
    CollisionWorld(CollisionWorld&&) noexcept;
    CollisionWorld& operator=(CollisionWorld&&) noexcept;

    // Collide_RayIntersect: all hits of the segment from->to, ascending by distance (empty = none).
    std::vector<CollisionHit> ray_hits(const Vec3& from, const Vec3& to, unsigned pick, unsigned hit = 0) const;
    std::optional<CollisionHit> ray(const Vec3& from, const Vec3& to, unsigned pick, unsigned hit = 0) const;

    // Collide_LineOfSight: true when nothing blocks the segment (pick | 4, hit = kFirstHit).
    bool line_of_sight(const Vec3& from, const Vec3& to, unsigned pick = 0) const;

    // Collide_CylinderIntersect / Collide_Update for a HITTEST of type 0x800.
    CylinderResult cylinder(const CylinderQuery& q) const;

    // Player_FeetOnPoint. `pos` = obj+0x30, `top` = HITTEST+0x30 after the cylinder pass,
    // `up` = Mat_GetUp(obj+0x90), `stand_height` = collbody+0xCC, `contact` = CylinderResult::contact.
    FeetResult feet_on_point(const Vec3& pos, const Vec3& top, const Vec3& up, float stand_height,
                             unsigned contact) const;

    // build_PointOnFloor: casts `pos` along `dir` (default straight down) for `range` and returns the hit point.
    std::optional<Vec3> point_on_floor(const Vec3& pos, float range, const Vec3& dir = {0, -1, 0}) const;

    // Collide_PlaceObject (GT_DeployMiniGun): drops `pos` onto the surface under `frame.up`, offsets
    // it by `height` along the surface normal and checks the surrounding triangles are flat enough
    // for an object of half-size `extent`.
    PlaceResult place_object(const Vec3& pos, const PlaceFrame& frame, float height, float extent) const;

    // Placements that actually carry collision triangles.
    std::size_t solid_count() const;

    // The world-geometry placements that carry collision (index i < solid_count()), for tools and self-checks.
    struct Solid {
        std::size_t placement;       // index into Level::placements()
        const Collision& collision;  // model space
        const Mat4& transform;       // model -> world
    };
    Solid solid(std::size_t i) const;

    // Instrumentation for tools.
    struct Stats {
        mutable std::size_t rays = 0, cylinders = 0, triangles_tested = 0;
    };
    const Stats& stats() const { return stats_; }

private:
    CollisionWorld(Level& level, std::optional<std::span<const std::uint32_t>> object_classes);

    struct Item;
    std::vector<Item> items_;
    Stats stats_;
};

}  // namespace nf

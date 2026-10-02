#pragma once

#include <cstdint>

#include "core/math.hpp"

namespace nf::driving {

// Surface material of a track triangle. Values are the original WSurface_* enum used to index the
// per-surface friction / lateral-loss tables (DRIVING.ELF tables at 0x3319D8 and 0x331A18, and the
// per-surface height offsets of sub_213618). They are the byte stored in the collision triangle's
// surface field (WHEEL_INFO1 + 0x2C).
enum class Surface : std::uint8_t {
    NoDrive = 0,
    Paved = 1,
    Gravel = 2,
    Grass = 3,
    Cobble = 4,
    Dirt = 5,
    Water = 6,
    Wood = 7,
    Ice = 8,
    Snow = 9,
    PavedRough = 10,
    PavedGrate = 11,
    Railroad = 12,
    Metal = 13,
    NoCollide = 14,  // body probes ignore hits on this material (CollideWithWorld, sub_1FC2F0)
    Terrain = 15,    // canyon/cliff terrain of the jungle and snow tracks; table slots are 1.0
};
constexpr int kSurfaceCount = 16;

// Result of the wheel "ground under this point" query (original sub_1F6AE0 -> sub_2322C8 ->
// sub_2324E8 -> sub_214338): the nearest track triangle at or below the query point, found with a
// vertical drop test. Y is up.
struct GroundHit {
    Vec3 point{};   // point on the triangle straight below the query point
    Vec3 normal{0, 1, 0};  // unit triangle normal, Y >= 0 (flipped to face up by the caller's rule)
    Surface surface = Surface::Paved;
};

// Result of a body probe segment against the track (original sub_2187F0, called per box corner from
// RigidBody::CollideWithWorld = sub_1FC2F0).
struct SegmentHit {
    float t = 1;          // fraction along from -> to in [0,1]
    Vec3 point{};         // world hit position
    Vec3 normal{0, 1, 0}; // unit surface normal, facing the segment start
    Surface surface = Surface::Paved;
};

// The queries the vehicle physics needs from the track collision data. All in Y-up world space
// (the same frame as the track data). Implementations must be const and side-effect free: the
// physics may call them several times per tick.
class CollisionWorld {
public:
    virtual ~CollisionWorld() = default;

    // Nearest solid triangle at or below `p` (highest triangle whose XZ footprint contains p and whose
    // surface height is <= p.y). Only surfaces facing up are of interest. False when there is none.
    virtual bool ground_below(const Vec3& p, GroundHit& out) const = 0;

    // First solid triangle crossed by the segment from -> to. Used for body/wall collision and
    // the camera. False when the segment is clear.
    virtual bool segment_hit(const Vec3& from, const Vec3& to, SegmentHit& out) const = 0;
};

}  // namespace nf::driving

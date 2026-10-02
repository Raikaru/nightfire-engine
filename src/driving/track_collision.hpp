#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "assets/carp_file.hpp"
#include "core/math.hpp"
#include "driving/world_query.hpp"

namespace nf::driving {

// One world-space collision triangle of a track. Built from the per-article `ca` strips of the
// track's `ci` collision instances (see docs/driving-collision.md).
//
// Frame: Y up, the frame of the track's instance matrices. `normal` is the unit vector
// (v[1]-v[0]) x (v[2]-v[0]) (right-handed rule, front face = counter-clockwise seen from the front);
// every one-sided triangle of the data faces up on floors, so a road triangle has normal.y > 0. A renderer
// that mirrors the frame (e.g. negates Z) has to flip the winding.
struct Tri {
    std::array<Vec3, 3> v;
    Vec3 normal;                    // unit; arbitrary side when `two_sided`
    Surface surface = Surface::Paved;
    std::uint8_t flags = 0;         // original per-triangle flag byte (bit 0x02 / 0x10 / 0x20 ... see doc)
    std::uint16_t instance = 0;     // index of the `ci` collision instance it came from
    bool two_sided = false;         // strip group is flagged orientation independent

    // Triangles with any of the top nibble flag bits set are skipped by the original's default queries
    // (WCollisionMgr mask 0xF0, sub_213110).
    bool solid() const { return (flags & 0xF0) == 0; }
};

// A vertical cylinder of the `co` table (poles, trees): the original's body-probe obstacle.
struct Cylinder {
    Vec3 base;       // bottom centre
    float radius = 0;
    float height = 0;
};

struct CollisionStats {
    std::size_t boxes = 0;          // `ci` collision instances (OBBs)
    std::size_t articles = 0;       // distinct collision articles (`ca`) instantiated
    std::size_t strip_triangles = 0;// non-join strip triangles before removing duplicates / degenerates
    std::size_t triangles = 0;      // triangles kept
    std::size_t cylinders = 0;      // `co` records
    std::size_t cells = 0;          // populated `cn` grid cells of the original grid
    std::array<std::size_t, kSurfaceCount> materials{};  // kept triangles per Surface
    Vec3 min{}, max{};              // world bounds of the triangles
};

// The track's solid geometry with a uniform-grid acceleration structure. Thread-safe for const use.
class TrackCollision final : public CollisionWorld {
public:
    // Parses a track .crp (paris_mis01.crp, ...). Throws nf::FormatError on malformed data.
    static TrackCollision load(const CarpFile& carp);

    // Highest floor triangle whose XZ footprint contains p with plane height <= p.y (skipping
    // down-facing one-sided triangles, near-vertical walls and non-solid flags). Normal is oriented up.
    bool ground_below(const Vec3& p, GroundHit& out) const override;
    // First solid triangle (either side) or cylinder crossed by the segment. Normal faces `from`.
    bool segment_hit(const Vec3& from, const Vec3& to, SegmentHit& out) const override;

    const std::vector<Tri>& triangles() const { return tris_; }
    const std::vector<Cylinder>& cylinders() const { return cylinders_; }
    const CollisionStats& stats() const { return stats_; }

private:
    TrackCollision() = default;
    void build_grid();

    std::vector<Tri> tris_;
    std::vector<Cylinder> cylinders_;
    CollisionStats stats_;

    // Uniform grid over XZ aligned with the original WGrid (`CGrd`); CSR layout, one triangle list per cell.
    float grid_x0_ = 0, grid_z0_ = 0, cell_ = 1, inv_cell_ = 1;
    int cells_x_ = 0, cells_z_ = 0;
    std::vector<std::uint32_t> cell_start_;   // cells_x_ * cells_z_ + 1
    std::vector<std::uint32_t> cell_tris_;
};

// Parses and sanity-checks the collision data of a track (all `ci`/`ca`/`cn`/`co`/`CGrd` records), throwing
// nf::FormatError on bad data. For `nfdump validate`.
CollisionStats validate_collision(const CarpFile& carp);

}  // namespace nf::driving

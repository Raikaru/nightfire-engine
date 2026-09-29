#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "assets/reader.hpp"
#include "core/math.hpp"

namespace nf {

// coll_data_new block (0x2E), parsed by parsemap_block_Coll_Data_New and walked by
// Intersect_RayGeom / Intersect_CylGeom. Precedes the PS2_GFX + entity_params pair of its model;
// coordinates are model-local.
//
// Header: u32 id|size, u32 version (4 or 5; v5 adds 0x10 bytes before the data),
//         u16 box_count, u16 tri_count, u16 unit_count. Data, in order:
//   unit_count x 0x40  quantised vertex pool (i16 triples, addressed in halfwords per box)
//   box_count  x 0x30  BVH boxes (root first)
//   tri_count  x 8     u16 v0, v1, v2, normal  (halfword offsets into the box's pool)
//   tri_count  x 1     material byte (Collide_Filter; bits 0xC0 = pass-through flags)
struct CollisionBox {
    Vec3 min, max;
    bool leaf;
    std::int16_t child_a, child_b;   // interior nodes
    std::uint16_t first_tri, end_tri; // leaves: [first, end)
};

struct CollisionTri {
    std::array<Vec3, 3> v;
    Vec3 normal;           // stored, unit length (i16 * 2^-14)
    std::uint8_t material;
};

struct Collision {
    std::uint32_t version = 0;
    std::vector<CollisionBox> boxes;
    std::vector<CollisionTri> tris;  // dequantised, in file order
};

Collision parse_collision(Bytes block);

}  // namespace nf

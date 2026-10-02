#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "assets/carp_file.hpp"
#include "core/math.hpp"

namespace nf::driving {

struct MeshVertex {
    float pos[3];
    float uv[2];
    std::uint32_t rgba;  // vertex colour, 0xFF = 1.0 per channel
};

// One draw batch: triangles sharing a texture shape and render state.
struct MeshBatch {
    std::string shape;       // 4-character `.ssh` shape name of the bound TAR texture; empty = untextured
    bool alpha_test = false; // GeoPrimState "alphatest=on"
    bool translucent = false;// GeoPrimState texalpha != noA
    std::vector<MeshVertex> vertices;
    std::vector<std::uint32_t> indices;  // triangle list
};

// A decoded world (or vehicle) mesh set.
struct SceneMesh {
    std::vector<MeshBatch> batches;
    Vec3 min{0, 0, 0}, max{0, 0, 0};
    std::size_t instances = 0;      // placed instances
    std::size_t unresolved = 0;     // instances whose geometry could not be found
};

// Bakes every instance of a track .crp (CARP file + its embedded ELF object) into world space.
//
// Layout (DRIVING.ELF sub_22FAF8 world setup, sub_1C7658/sub_1C7D78 scene objects; docs/driving.md):
//   * the world group's `in` #0 record array holds one 0x40-byte row-major matrix per instance
//     (rows = X/Y/Z axes, row 3 = translation); word +0x1C of each is a `{u16 idx, "sr"}` reference to a
//     `CARP::<article>::{as  NNNN}` string of the same group, naming article + part;
//   * article groups ("Arti" heads) carry Name / Base (bounding box) / as / sr members; an `as` member
//     refers to the `EAGL::<article>::{as  0000}::Model[0].NN` symbol of its part, whose model header
//     points at a list of DMA-chain segments; the part draws segment `idx` (header +0x0C);
//   * a chain is VIF data: header quad (vertex count), three quads of cumulative strip ends (byte / 3),
//     V3-16 positions quantised over the article's bounding box, V2-16 texture coordinates, optional
//     V3-8 vertex colours.
SceneMesh build_track_scene(const CarpFile& carp, const ElfImage& elf);

// One `{as  NNNN}` part of a vehicle model: geometry in car space (metres, +Z rear, -Z front).
struct VehiclePart {
    int id = 0;
    SceneMesh mesh;
};

// Vehicle .crp: chains carry float V3-32 positions and are grouped by the asset id in their GeoPrim name.
std::vector<VehiclePart> build_vehicle_parts(const CarpFile& carp, const ElfImage& elf);

}  // namespace nf::driving

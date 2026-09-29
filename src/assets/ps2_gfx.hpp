#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "assets/reader.hpp"

namespace nf {

struct GfxVertex {
    std::array<float, 3> pos;
    std::array<float, 2> uv;
    std::uint32_t rgba;              // PS2 colour, 0x80 = 1.0 per channel
    std::array<std::int8_t, 3> normal;
};

// One VU1 kick (MSCNT): a triangle strip set sharing the currently bound texture.
struct GfxBatch {
    std::int32_t texture;            // index into the owning chunk's texture table, -1 if none bound
    std::vector<GfxVertex> vertices;
    std::vector<std::uint32_t> indices;  // triangle list expanded from the strip (ADC flags honoured)
};

struct GfxMesh {
    std::array<float, 3> bbox_min{}, bbox_max{};
    std::vector<GfxBatch> batches;
};

// PS2_GFX block layout (psiDrawObjectMatrix / DrawThisBox):
//   +0x00 block header, +0x04 u32 info_offset (from block start)
//   info: u32 box_count, u32 box_offset
//   box (0x38): f32 min[3], max[3]; i32 child0 (-1 = leaf), i32 child1; u32 chain_offset;
//               u32 texref_list_offset; u32 chain_length; u32 ?; u32 vertex_count; u32 flags
//   leaf chain: DMA tags (CNT / REF / RET) carrying VIF: STCYCL, UNPACK V4-32 GIFtag, V2-32 ST,
//               V3-32 XYZ, V4-8 normal (w bit7 = ADC/no-kick), V4-8u colour, MSCNT.
//   REF tag address = texture index (texref list holds the offsets of those tags for relocation).
GfxMesh decode_ps2_gfx(Bytes block);

}  // namespace nf

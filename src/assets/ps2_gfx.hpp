#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "assets/reader.hpp"

namespace nf {

// GS registers a batch is drawn with (the A+D writes of the DIRECT blocks in the leaf chain).
// Register values are the raw 64-bit GS words; `prim` is the PRIM field of the batch's GIFtag.
struct GsRegs {
    std::uint64_t alpha_1 = 0;       // ALPHA_1 (0x42): blend equation ((A - B) * C >> 7) + D
    std::uint64_t test_1 = 0;        // TEST_1 (0x47): alpha test + depth test
    std::uint64_t zbuf_1 = 0;        // ZBUF_1 (0x4E): ZMSK = depth write mask
    std::uint64_t prim = 0;          // PRIM: bit 4 TME (textured), bit 6 ABE (alpha blend)
};

enum class BlendOp : std::uint8_t { Add, Subtract, ReverseSubtract };  // Subtract: src - dst
enum class BlendFactor : std::uint8_t { Zero, One, SrcAlpha, OneMinusSrcAlpha, ConstAlpha };

// The GS blend equation reduced to fixed-function terms: out = src * s (op) dst * d.
// Ad (destination alpha) is taken as 1.0; `constant` is the ConstAlpha value (ALPHA.FIX / 0x80).
struct BlendState {
    bool enabled = false;            // false: the fragment replaces the framebuffer colour
    BlendOp op = BlendOp::Add;
    BlendFactor src = BlendFactor::One, dst = BlendFactor::Zero;
    float constant = 0;
    bool operator==(const BlendState&) const = default;
};

enum class TestMethod : std::uint8_t { Never, Always, Less, LessEqual, Equal, GreaterEqual, Greater, NotEqual };
// GS depth compare on the (inverted) PS2 Z: GEQUAL is the usual "nearer or equal passes".
enum class DepthMethod : std::uint8_t { Never, Always, GreaterEqual, Greater };

struct Material {
    BlendState blend;
    bool alpha_test = false;         // TEST.ATE; failing fragments are discarded (AFAIL = KEEP)
    TestMethod alpha_method = TestMethod::Always;
    std::uint8_t alpha_ref = 0;      // compared with the 8-bit GS alpha (0x80 = 1.0)
    DepthMethod depth_test = DepthMethod::GreaterEqual;
    bool depth_write = true;
    bool textured = true;            // PRIM.TME
    bool operator==(const Material&) const = default;
};

// Resolves raw registers into a Material. Throws FormatError for combinations the fixed-function
// pipeline cannot express or that never occur on the disc (AFAIL != KEEP, reserved ALPHA selectors).
Material decode_material(const GsRegs& regs);

struct GfxVertex {
    std::array<float, 3> pos;
    std::array<float, 2> uv;
    std::uint32_t rgba;              // PS2 colour, 0x80 = 1.0 per channel
    std::array<std::int8_t, 3> normal;
};

// One VU1 kick (MSCNT): a triangle strip set sharing the currently bound texture.
struct GfxBatch {
    std::int32_t texture;            // index into the owning chunk's texture table, -1 if none bound
    GsRegs gs;                       // registers in effect when the batch was kicked
    Material material;               // decode_material(gs)
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
//   A REF's DIRECT block (TEX0/CLAMP, filled from the texture at load time) carries nothing in the
//   file; the GS state of a batch comes from inline DIRECT blocks of A+D writes to ALPHA_1, ZBUF_1 and
//   TEST_1 that precede its data. Every leaf chain starts with a full set; later blocks change part of
//   it and the state is sticky within the leaf. UNPACK V4-32 x1 is the batch's GIFtag (PRIM with PRE).
GfxMesh decode_ps2_gfx(Bytes block);

}  // namespace nf

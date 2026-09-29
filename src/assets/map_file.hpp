#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "assets/reader.hpp"

namespace nf {

// Block ids from parsemap_handle_block_id. Block header: u32 (id << 24 | size_including_header).
enum class BlockId : std::uint8_t {
    DictXyz = 0x00,
    DictUv = 0x01,
    DictRgba = 0x02,
    DictComlist = 0x03,
    EntityParams = 0x04,
    AiPath = 0x05,
    MapHeader = 0x0E,
    PaletteHeader = 0x0F,
    TextureHeader = 0x10,
    PaletteDataPc = 0x11,
    PaletteDataPsx = 0x12,
    TextureDataPc = 0x16,   // 0x15 also routes to texture_data_pc
    TextureDataGameCube = 0x17,
    TextureDataXbox = 0x18,
    PathData = 0x19,
    MapDataStatic = 0x1A,
    MemoryDiscard = 0x1C,
    End = 0x1D,
    DictNorm = 0x20,
    PortalData = 0x21,
    CollData = 0x22,
    Unknown24 = 0x24,
    Datums = 0x25,
    AmbientRadiators = 0x26,
    Lod = 0x27,
    MapSounds = 0x28,
    TextureDataPcDx = 0x29,
    MorphData = 0x2B,
    HashList = 0x2C,
    Ps2Gfx = 0x2D,
    CollDataNew = 0x2E,
    Particles = 0x30,
};

struct Block {
    std::uint8_t id;
    std::size_t offset;  // of the block header within the chunk file
    Bytes data;          // whole block including its 4-byte header
};

// Walk a parsemap chunk file. Version 1 files carry an offset table (u32 per block, relative to
// file+4) right after the version word; version 0 files are a plain block stream. Stops at End.
std::vector<Block> walk_blocks(Bytes file);

// Texture: texture_header entry (12 bytes: u16 flags, u16 w-1, u16 h-1, u16 ?, u32 -1)
// + palette_data_psx (16 or 256 RGBA entries, alpha 0x80 = opaque, 256-entry CLUTs in PS2 CSM1 order)
// + texture_data_pc (4bpp low-nibble-first or 8bpp indices, linear; animated textures append
//   further frames, only frame 0 is decoded).
struct Texture {
    std::uint16_t flags;
    std::uint32_t width, height;
    std::vector<std::uint32_t> rgba;  // decoded, R in low byte
};

// Model = entity_params block paired with the PS2_GFX block that precedes it (celglist_tag), and the
// optional coll_data_new block that precedes the PS2_GFX (hung off celglist+8).
struct Model {
    std::string name;
    std::int32_t hash;               // -1 = not registered in the global hashtable
    std::array<float, 9> params;     // bounding sphere (x,y,z,r) + bbox min/max (partial); see docs
    Bytes gfx;                       // PS2_GFX block (may be a 0x20-byte stub with no geometry)
    Bytes collision;                 // coll_data_new block, empty if the model has none
};

// map_data_static record (0x4C bytes + n * 8 params).
struct StaticInstance {
    std::uint16_t model_index;       // into this file's models when hash == -1
    std::int32_t hash;               // otherwise: model registered by another chunk file
    std::uint32_t flags;
    std::array<float, 3> position;
    std::array<float, 3> euler;      // radians
    std::array<float, 4> quat;       // x, y, z, w
    std::array<float, 3> scale;
    std::vector<std::pair<std::int32_t, std::uint32_t>> params;
};

struct MapChunk {
    std::uint32_t version = 0;
    std::vector<Block> blocks;
    std::vector<Texture> textures;
    std::vector<Model> models;
    std::vector<StaticInstance> statics;
};

MapChunk parse_map_chunk(Bytes file);

}  // namespace nf

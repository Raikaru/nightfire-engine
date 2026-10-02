#include "assets/map_file.hpp"

namespace nf {

namespace {

Block block_at(Bytes file, std::size_t offset) {
    auto header = load<std::uint32_t>(file, offset);
    std::uint8_t id = header >> 24;
    std::size_t size = header & 0xFFFFFF;
    // End blocks are bare headers; everything else must fit.
    Bytes data = id == std::uint8_t(BlockId::End) ? slice(file, offset, 4) : slice(file, offset, size);
    if (id != std::uint8_t(BlockId::End) && size < 4) throw FormatError("block smaller than its header");
    return {id, offset, data};
}

std::uint32_t unswizzle_clut_index(std::uint32_t i) {
    // CSM1: within every 32 entries, 8..15 and 16..23 are swapped.
    switch (i & 0x18) {
        case 0x08: return i + 8;
        case 0x10: return i - 8;
        default: return i;
    }
}

std::vector<Texture> decode_textures(const std::vector<Block>& blocks) {
    const Block* header = nullptr;
    std::vector<const Block*> palettes, pixels;
    for (const auto& b : blocks) {
        if (b.id == std::uint8_t(BlockId::TextureHeader)) header = &b;
        else if (b.id == std::uint8_t(BlockId::PaletteDataPsx)) palettes.push_back(&b);
        else if (b.id == std::uint8_t(BlockId::TextureDataPc)) pixels.push_back(&b);
    }
    std::vector<Texture> out;
    if (!header) return out;
    std::size_t count = (header->data.size() - 4) / 12;
    if (palettes.size() != count || pixels.size() != count)
        throw FormatError("texture header/palette/pixel block counts disagree");

    for (std::size_t i = 0; i < count; ++i) {
        Texture t;
        std::size_t e = 4 + i * 12;
        t.flags = load<std::uint16_t>(header->data, e);
        t.width = load<std::uint16_t>(header->data, e + 2) + 1u;
        t.height = load<std::uint16_t>(header->data, e + 4) + 1u;
        t.frames = load<std::uint8_t>(header->data, e + 6);
        if (t.frames == 0) throw FormatError("texture with zero frames");
        // psiCreateMapTextures: fps in 1..59 -> 60 / fps ticks per frame, anything else every tick.
        auto fps = load<std::uint8_t>(header->data, e + 7);
        t.frame_ticks = fps >= 1 && fps <= 59 ? 60u / fps : 1u;

        Bytes pal = palettes[i]->data.subspan(4);
        std::size_t colors = pal.size() / 4;
        if (colors != 16 && colors != 256) throw FormatError("palette is neither 16 nor 256 entries");
        std::vector<std::uint32_t> clut(colors);
        for (std::uint32_t c = 0; c < colors; ++c) {
            auto src = colors == 256 ? unswizzle_clut_index(c) : c;
            auto rgba = load<std::uint32_t>(pal, src * 4);
            std::uint32_t a = std::min<std::uint32_t>(255, (rgba >> 24) * 2);  // PS2 alpha: 0x80 = 1.0
            clut[c] = (rgba & 0x00FFFFFF) | (a << 24);
        }

        Bytes idx = pixels[i]->data.subspan(4);
        std::size_t bits = colors == 16 ? 4 : 8;
        std::size_t stored = idx.size() * 8 / bits;  // texels present
        // One texture on the USA disc stores fewer texels than its header claims; it is the header
        // size halved.
        while (stored < std::size_t(t.width) * t.height * t.frames) {
            if (t.width == 1 || t.height == 1) throw FormatError("texture pixel data too short");
            t.width /= 2;
            t.height /= 2;
        }
        std::size_t n = std::size_t(t.width) * t.height * t.frames;
        t.rgba.resize(n);
        for (std::size_t p = 0; p < n; ++p) {
            std::uint8_t v = colors == 16 ? ((idx[p / 2] >> ((p & 1) * 4)) & 0xF) : idx[p];
            t.rgba[p] = clut[v];
        }
        out.push_back(std::move(t));
    }
    return out;
}

std::vector<Model> parse_models(const std::vector<Block>& blocks) {
    std::vector<Model> models;
    Bytes pending_gfx, pending_coll;
    for (const auto& b : blocks) {
        if (b.id == std::uint8_t(BlockId::CollDataNew)) {
            pending_coll = b.data;
        } else if (b.id == std::uint8_t(BlockId::Ps2Gfx)) {
            pending_gfx = b.data;
        } else if (b.id == std::uint8_t(BlockId::EntityParams)) {
            Model m;
            m.hash = load<std::int32_t>(b.data, 4);
            m.flags = load<std::uint32_t>(b.data, 8);
            for (std::size_t k = 0; k < 9; ++k) m.params[k] = load<float>(b.data, 0x0C + k * 4);
            m.name = std::string(load_cstr(b.data, 0x34));
            m.gfx = pending_gfx;
            m.collision = pending_coll;
            pending_gfx = {};
            pending_coll = {};
            models.push_back(std::move(m));
        }
    }
    return models;
}

std::vector<StaticInstance> parse_statics(const std::vector<Block>& blocks) {
    std::vector<StaticInstance> out;
    for (const auto& b : blocks) {
        if (b.id != std::uint8_t(BlockId::MapDataStatic)) continue;
        std::size_t p = 4;
        while (p < b.data.size()) {
            StaticInstance s;
            s.model_index = load<std::uint16_t>(b.data, p);
            s.hash = load<std::int32_t>(b.data, p + 4);
            s.flags = load<std::uint32_t>(b.data, p + 8);
            for (int k = 0; k < 3; ++k) s.position[k] = load<float>(b.data, p + 0x0C + k * 4);
            for (int k = 0; k < 3; ++k) s.euler[k] = load<float>(b.data, p + 0x18 + k * 4);
            for (int k = 0; k < 4; ++k) s.quat[k] = load<float>(b.data, p + 0x24 + k * 4);
            for (int k = 0; k < 3; ++k) s.scale[k] = load<float>(b.data, p + 0x34 + k * 4);
            auto nparams = load<std::uint32_t>(b.data, p + 0x48);
            for (std::uint32_t k = 0; k < nparams; ++k)
                s.params.emplace_back(load<std::int32_t>(b.data, p + 0x4C + k * 8),
                                      load<std::uint32_t>(b.data, p + 0x50 + k * 8));
            p += 0x4C + std::size_t(nparams) * 8;
            out.push_back(std::move(s));
        }
        if (p != b.data.size()) throw FormatError("map_data_static records overrun block");
    }
    return out;
}

}  // namespace

std::vector<Block> walk_blocks(Bytes file) {
    std::vector<Block> blocks;
    auto version = load<std::uint32_t>(file, 0);
    constexpr std::size_t kMaxBlocks = 1 << 20;
    if (version == 1) {
        for (std::size_t table = 4; blocks.size() < kMaxBlocks; table += 4) {
            auto b = block_at(file, std::size_t(load<std::uint32_t>(file, table)) + 4);
            blocks.push_back(b);
            if (b.id == std::uint8_t(BlockId::End)) return blocks;
        }
    } else {
        for (std::size_t p = 4; blocks.size() < kMaxBlocks;) {
            auto b = block_at(file, p);
            blocks.push_back(b);
            if (b.id == std::uint8_t(BlockId::End)) return blocks;
            p += b.data.size();
        }
    }
    throw FormatError("block stream has no End block");
}

MapChunk parse_map_chunk(Bytes file) {
    MapChunk chunk;
    chunk.version = load<std::uint32_t>(file, 0);
    chunk.blocks = walk_blocks(file);
    chunk.textures = decode_textures(chunk.blocks);
    chunk.models = parse_models(chunk.blocks);
    chunk.statics = parse_statics(chunk.blocks);
    return chunk;
}

}  // namespace nf

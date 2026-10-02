#include "assets/nav_data.hpp"

#include <string>

namespace nf {

namespace {

constexpr std::size_t kNodeStride = 0x40, kBnodeStride = 0x20, kLinkStride = 0x10, kBlinkStride = 0x14;
constexpr std::size_t kRecordHeader = 0xac;  // name[0x80] flags reserved[9] nnodes extra_size
constexpr float kNodeRaise = 0.4f;           // AIPath_Parse adds this to every node y
constexpr std::uint32_t kEditorFiller = 0x2468abce;

Vec3 load_vec3(Bytes b, std::size_t o) {
    return {load<float>(b, o), load<float>(b, o + 4) , load<float>(b, o + 8)};
}

[[noreturn]] void fail(const std::string& what, std::size_t rec) {
    throw FormatError("ai network record " + std::to_string(rec) + ": " + what);
}

}  // namespace

std::size_t AiNetworkData::path_count() const {
    std::size_t n = 0;
    for (const auto& r : records) n += !r.is_bounds();
    return n;
}

std::size_t AiNetworkData::bounds_count() const {
    std::size_t n = 0;
    for (const auto& r : records) n += r.is_bounds();
    return n;
}

std::optional<AiNetworkData> parse_ai_network(Bytes block) {
    if ((load<std::uint32_t>(block, 0) >> 24) != std::uint32_t(BlockId::AiPath))
        throw FormatError("not an AI network block");
    AiNetworkData out;
    out.version = load<std::uint32_t>(block, 4);
    if (out.version != kAiNetworkVersion) return std::nullopt;
    auto nrec = load<std::uint32_t>(block, 8);
    std::size_t p = 0x0c;
    for (std::uint32_t r = 0; r < nrec; ++r) {
        AiRecord rec;
        rec.name = std::string(load_cstr(slice(block, p, 0x80), 0));
        rec.flags = load<std::uint32_t>(block, p + 0x80);
        auto nnodes = load<std::uint32_t>(block, p + 0xa4);
        rec.extra_size = load<std::uint32_t>(block, p + 0xa8);
        if (nnodes > 0xffff) fail("node count exceeds u16 index range", r);
        p += kRecordHeader + rec.extra_size;
        const bool bounds = rec.is_bounds();
        if (bounds) {
            rec.bnodes.reserve(nnodes);
            for (std::uint32_t i = 0; i < nnodes; ++i, p += kBnodeStride) {
                AiBoundsNode n;
                n.index = load<std::uint16_t>(block, p);
                n.ordinal = load<std::uint16_t>(block, p + 2);
                n.flags = load<std::uint32_t>(block, p + 4);
                n.pos = load_vec3(block, p + 0x10);
                n.pos[1] += kNodeRaise;
                if (n.index != i) fail("bounds node index not sequential", r);
                rec.bnodes.push_back(n);
            }
            auto nlinks = load<std::uint32_t>(block, p);
            p += 4;
            rec.blinks.reserve(nlinks);
            for (std::uint32_t i = 0; i < nlinks; ++i, p += kBlinkStride) {
                AiBoundsLink l;
                l.index = load<std::uint16_t>(block, p);
                l.ordinal = load<std::uint16_t>(block, p + 2);
                l.flags = load<std::uint32_t>(block, p + 4);
                l.a = load<std::uint16_t>(block, p + 8);
                l.b = load<std::uint16_t>(block, p + 0xa);
                if (l.index != i) fail("bounds link index not sequential", r);
                if (l.a >= nnodes || l.b >= nnodes) fail("bounds link node out of range", r);
                rec.blinks.push_back(l);
            }
        } else {
            rec.nodes.reserve(nnodes);
            for (std::uint32_t i = 0; i < nnodes; ++i, p += kNodeStride) {
                AiPathNode n;
                n.index = load<std::uint16_t>(block, p);
                n.ordinal = load<std::uint16_t>(block, p + 2);
                n.flags = load<std::uint32_t>(block, p + 4);
                if (n.flags == kEditorFiller) n.flags = 0;
                n.pos = load_vec3(block, p + 0x10);
                n.pos[1] += kNodeRaise;
                if (n.index != i) fail("node index not sequential", r);
                rec.nodes.push_back(n);
            }
            auto nlinks = load<std::uint32_t>(block, p);
            p += 4;
            rec.links.reserve(nlinks);
            for (std::uint32_t i = 0; i < nlinks; ++i, p += kLinkStride) {
                AiPathLink l;
                l.index = load<std::uint16_t>(block, p);
                l.ordinal = load<std::uint16_t>(block, p + 2);
                l.a = load<std::uint16_t>(block, p + 8);
                l.b = load<std::uint16_t>(block, p + 0xa);
                if (l.index != i) fail("link index not sequential", r);
                if (l.a >= nnodes || l.b >= nnodes) fail("link node out of range", r);
                rec.links.push_back(l);
            }
        }
        out.records.push_back(std::move(rec));
    }
    if (p != block.size())
        throw FormatError("ai network records end at " + std::to_string(p) + ", block size " +
                          std::to_string(block.size()));
    if ((load<std::uint32_t>(block, 0) & 0xffffff) != block.size()) throw FormatError("ai network block size mismatch");
    return out;
}

std::optional<AiNetworkData> parse_ai_network(const MapChunk& chunk) {
    for (const auto& b : chunk.blocks)
        if (b.id == std::uint8_t(BlockId::AiPath)) return parse_ai_network(b.data);
    return std::nullopt;
}

std::vector<PathTrack> parse_path_data(const MapChunk& chunk) {
    std::vector<PathTrack> out;
    for (const auto& b : chunk.blocks) {
        if (b.id != std::uint8_t(BlockId::PathData)) continue;
        if (b.data.size() < 4 || (b.data.size() - 4) % 28 != 0)
            throw FormatError("path_data payload is not a whole number of 28-byte records");
        PathTrack t;
        for (std::size_t p = 4; p < b.data.size(); p += 28) {
            PathKey k;
            k.pos = load_vec3(b.data, p);
            for (int i = 0; i < 4; ++i) k.quat[std::size_t(i)] = load<float>(b.data, p + 12 + std::size_t(i) * 4);
            t.keys.push_back(k);
        }
        out.push_back(std::move(t));
    }
    return out;
}

std::vector<StaticPathRef> static_path_refs(const MapChunk& chunk) {
    std::vector<StaticPathRef> out;
    std::size_t index = 0;
    for (const auto& b : chunk.blocks) {
        if (b.id != std::uint8_t(BlockId::MapDataStatic)) continue;
        std::size_t p = 4;
        while (p < b.data.size()) {
            StaticPathRef r;
            r.static_index = index++;
            r.count = load<std::uint16_t>(b.data, p + 0x40);
            r.aux = load<std::uint16_t>(b.data, p + 0x42);
            r.path_index = load<std::uint16_t>(b.data, p + 0x44);
            if (r.count != 0) out.push_back(r);
            p += 0x4C + std::size_t(load<std::uint32_t>(b.data, p + 0x48)) * 8;
        }
    }
    return out;
}

std::vector<PortalRecord> parse_portals(const MapChunk& chunk) {
    std::vector<PortalRecord> out;
    for (const auto& b : chunk.blocks) {
        if (b.id != std::uint8_t(BlockId::PortalData)) continue;
        auto count = load<std::uint32_t>(b.data, 4);
        if (b.data.size() != 8 + std::size_t(count) * 56)
            throw FormatError("portal_data size does not match its record count");
        for (std::uint32_t i = 0; i < count; ++i) {
            std::size_t p = 8 + std::size_t(i) * 56;
            PortalRecord r;
            for (std::size_t v = 0; v < 4; ++v) r.quad[v] = load_vec3(b.data, p + v * 12);
            r.model_a = load<std::uint16_t>(b.data, p + 48);
            r.model_b = load<std::uint16_t>(b.data, p + 50);
            r.flags = load<std::uint16_t>(b.data, p + 52);
            out.push_back(r);
        }
    }
    return out;
}

ModelBox model_box(const MapChunk& chunk, std::size_t model_index) {
    std::size_t n = 0;
    for (const auto& b : chunk.blocks) {
        if (b.id != std::uint8_t(BlockId::EntityParams)) continue;
        if (n++ != model_index) continue;
        return {load_vec3(b.data, 0x1c), load_vec3(b.data, 0x28)};
    }
    throw FormatError("model index " + std::to_string(model_index) + " has no entity_params block");
}

}  // namespace nf

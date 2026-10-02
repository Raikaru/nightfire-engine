#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "assets/map_file.hpp"
#include "assets/reader.hpp"
#include "core/math.hpp"

namespace nf {

// Map block 0x05 (AI network, parsed by AIPath_Parse @0x1d2598) and block 0x19 (path_data,
// parsemap_block_path_data @0x1d24d0). Layouts: docs/spec-arena-ai.md Part 2B section 1 and
// docs/ai-nav.md. Everything here is the *file* content after the parser's fix-ups (node y += 0.4);
// runtime scratch (cel binding, link lengths, adjacency) lives in game/nav.hpp.

constexpr std::uint32_t kAiNetworkVersion = 8;

// R+0x80 record flags. Bit 0 selects AIBounds; bits the code tests: 0x4 patrol, 0x8 mission,
// 0x10 (-> route flag 0x20); (flags & 0xd) != 0 excludes a path from goal marking / NavPathForPosition.
namespace pathflag {
constexpr std::uint32_t kBounds = 0x1;
constexpr std::uint32_t kNav = 0x2;
constexpr std::uint32_t kPatrol = 0x4;
constexpr std::uint32_t kMission = 0x8;
constexpr std::uint32_t kRouteFlag20 = 0x10;
constexpr std::uint32_t kNotPlain = 0xd;
}  // namespace pathflag

// MAPNODE+4 file flag bits (spec 2.5).
namespace nodeflag {
constexpr std::uint32_t kWait = 0x1;       // patrol/mission wait-look node (NDrone2_ReachedDestNode)
constexpr std::uint32_t kDoor = 0x2;       // bound to the nearest door object by NDrone_InitDoorNodes
constexpr std::uint32_t kKick = 0x4;       // bound to the nearest kick object by NDrone_InitKickNodes
constexpr std::uint32_t kSpecialMask = 0xf007f;  // LinkCreep_ForNodes "special" nodes
constexpr std::uint32_t kOpenClosed = 0x40000000;  // runtime scratch (A*/emit)
constexpr std::uint32_t kScratchMask = 0xC0000000;
}  // namespace nodeflag

// One MAPNODE (0x40 bytes on disk).
struct AiPathNode {
    std::uint16_t index = 0;
    std::uint16_t ordinal = 0;   // record ordinal in the file
    std::uint32_t flags = 0;     // 0x2468abce (editor filler) already mapped to 0
    Vec3 pos{};                  // file position with y += 0.4 (AIPath_Parse)
};

// One path link (0x10 bytes on disk). Undirected.
struct AiPathLink {
    std::uint16_t index = 0;
    std::uint16_t ordinal = 0;
    std::uint16_t a = 0, b = 0;
};

// One BNODE (0x20 bytes on disk).
struct AiBoundsNode {
    std::uint16_t index = 0;
    std::uint16_t ordinal = 0;
    std::uint32_t flags = 0;
    Vec3 pos{};                  // y += 0.4
};

// One BLINK (0x14 bytes on disk).
struct AiBoundsLink {
    std::uint16_t index = 0;
    std::uint16_t ordinal = 0;
    std::uint32_t flags = 0;
    std::uint16_t a = 0, b = 0;
};

// One record of block 0x05: an AIPath (flags & 1 == 0) or an AIBounds (flags & 1).
struct AiRecord {
    std::string name;            // editor name ("BotPath", "Boundary", "Patrol Path 3"...)
    std::uint32_t flags = 0;     // R+0x80
    std::uint32_t extra_size = 0;
    bool is_bounds() const { return flags & pathflag::kBounds; }
    // AIPath payload
    std::vector<AiPathNode> nodes;
    std::vector<AiPathLink> links;
    // AIBounds payload
    std::vector<AiBoundsNode> bnodes;
    std::vector<AiBoundsLink> blinks;
};

struct AiNetworkData {
    std::uint32_t version = 0;
    std::vector<AiRecord> records;   // file order; AIPath / AIBounds indices count each kind separately
    std::size_t path_count() const;
    std::size_t bounds_count() const;
};

// AIPath_Parse. Throws FormatError when the records do not end exactly at the block end, indices are
// not sequential or a link references a node out of range. Returns nullopt for version != 8 (the
// original then leaves the network empty).
std::optional<AiNetworkData> parse_ai_network(Bytes block);

// The level's block 0x05 (at most one per map chunk), nullopt when the chunk has none.
std::optional<AiNetworkData> parse_ai_network(const MapChunk& chunk);

// Block 0x19: (size-4)/28 records of `f32 pos[3]; f32 quat[4]` (component order of the quaternion is
// not fixed by the original loader; it is exposed as stored).
struct PathKey {
    Vec3 pos{};
    std::array<float, 4> quat{};
};
struct PathTrack {
    std::vector<PathKey> keys;
};

// All block 0x19 payloads of a map chunk in block order (= the original's m_paths[] array).
std::vector<PathTrack> parse_path_data(const MapChunk& chunk);

// Static instance -> path reference (parsemap_block_map_data_dynamic reads the raw record fields
// +0x40 u16 (count/flag), +0x42 u16 (aux), +0x44 u16 (index into m_paths)). `count != 0` marks an
// instance that carries a path.
struct StaticPathRef {
    std::size_t static_index = 0;     // index into MapChunk::statics
    std::uint16_t count = 0;          // +0x40
    std::uint16_t aux = 0;            // +0x42
    std::uint16_t path_index = 0;     // +0x44
};
std::vector<StaticPathRef> static_path_refs(const MapChunk& chunk);

// Block 0x21 portal_data (parsemap_block_portal_data): `u32 header, u32 count`, then `count`
// records of 56 bytes: 4 quad vertices (3 x f32 each), u16 model A, u16 model B (indices into the map
// chunk's models: the cels built from those models are connected), u16 flags, u16 pad.
struct PortalRecord {
    std::array<Vec3, 4> quad{};
    std::uint16_t model_a = 0, model_b = 0;
    std::uint16_t flags = 0;
};
std::vector<PortalRecord> parse_portals(const MapChunk& chunk);

// Model-space axis aligned box of entity_params model `model_index` as the original copies it into
// cel+0x40..0x58 (min at file +0x1c..0x24, max at +0x28..0x30); this is the box build_FindCel tests.
struct ModelBox {
    Vec3 min{}, max{};
};
ModelBox model_box(const MapChunk& chunk, std::size_t model_index);

}  // namespace nf

#include "game/sp_placement.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <unordered_map>

#include "assets/reader.hpp"

namespace nf::sp {

namespace {

// `{0x11, 5, 0xa, 7, 0x1d}` indexed by param (Drone_CoverLowNode's stack table, byte stride 2).
constexpr std::uint8_t kLowAnimTable[5] = {0x11, 5, 0xa, 7, 0x1d};

std::uint8_t low_anim(std::uint32_t param) { return param < 5 ? kLowAnimTable[param] : 0; }

CoverNodeDef make_corner(const nf::StaticInstance& s, std::uint32_t index) {
    CoverNodeDef n;
    n.type = CoverNodeDef::Type::Corner;
    n.pos = {s.position[0], s.position[1], s.position[2]};
    n.yaw = s.euler[1];
    n.static_index = index;
    const std::uint32_t p0 = s.param(0), p1 = s.param(1), p2 = s.param(2), angle = s.param(3);
    n.flags |= p0 == 1 ? 8u : p0 == 2 ? 4u : 0xcu;
    n.flags |= p1 == 1 ? 0x20u : p1 == 2 ? 0x40u : 0x60u;
    n.flags |= p2 == 1 ? 0x80u : p2 == 2 ? 0x100u : 0x180u;
    n.max_angle = angle == 0 ? 0.7853982f : float(angle) * 0.017453294f;
    n.require_on = std::uint8_t(s.param(4));
    n.require_off = std::uint8_t(s.param(5));
    n.range_a = s.param(6) != 0 ? std::uint8_t(s.param(6)) : std::uint8_t(8);
    n.range_b = s.param(7) != 0 ? std::uint8_t(s.param(7)) : std::uint8_t(8);
    return n;
}

CoverNodeDef make_low(const nf::StaticInstance& s, std::uint32_t index) {
    CoverNodeDef n;
    n.type = CoverNodeDef::Type::Low;
    n.pos = {s.position[0], s.position[1], s.position[2]};
    n.yaw = s.euler[1];
    n.static_index = index;
    const std::uint32_t p0 = s.param(0);
    if (p0 == 2) {
        n.flags = 1;
        n.anim_a = low_anim(s.param(1));
        n.anim_b = low_anim(s.param(2));
        if (s.param(3) == 1) n.flags |= 2;
    } else {
        n.anim_a = 0x11;
        n.anim_b = p0 == 1 ? 5 : 10;
    }
    n.require_on = std::uint8_t(s.param(4));
    n.require_off = std::uint8_t(s.param(5));
    return n;
}

struct ModelSlot {
    std::size_t chunk, model;
};

}  // namespace

bool AiVolumeDef::contains(const Vec3& p) const {
    // Intersect_PointOOBox: the point in the box's frame (rotation = the placement's, scale folded into `half`).
    const Vec3 d = p - center;
    for (int c = 0; c < 3; ++c) {
        const float local = axes[std::size_t(c) * 3] * d[0] + axes[std::size_t(c) * 3 + 1] * d[1] +
                            axes[std::size_t(c) * 3 + 2] * d[2];
        if (std::fabs(local) > half[std::size_t(c)]) return false;
    }
    return true;
}

std::vector<const PlacedNpc*> SpLevel::npcs_for(int difficulty) const {
    std::vector<const PlacedNpc*> out;
    for (const PlacedNpc& n : npcs)
        if (placed_at(n, difficulty)) out.push_back(&n);
    return out;
}

std::vector<const PlacedNpc*> SpLevel::group_members(std::uint32_t g, int difficulty) const {
    std::vector<const PlacedNpc*> out;
    for (const PlacedNpc& n : npcs)
        if (placed_at(n, difficulty) && n.group() == g) out.push_back(&n);
    return out;
}

float SpLevel::visibility_at(const Vec3& p) const {
    float v = 1.0f;
    for (const AiVolumeDef& b : volumes)
        if (b.kind == 0 && b.contains(p)) v *= b.visibility;
    return v;
}

bool SpLevel::in_volume(const Vec3& p, std::uint16_t kind) const {
    for (const AiVolumeDef& b : volumes)
        if (b.kind == kind && b.contains(p)) return true;
    return false;
}

std::string static_model_name(const nf::Level& level, const nf::StaticInstance& s) {
    const nf::ChunkFile* map = level.map();
    if (!map) return {};
    if (s.hash == -1) {
        if (s.model_index < map->chunk.models.size()) return map->chunk.models[s.model_index].name;
        return {};
    }
    for (const auto& cf : level.chunks())
        for (const auto& m : cf.chunk.models)
            if (m.hash == s.hash) return m.name;
    return {};
}

SpLevel parse_sp_level(const nf::Level& level, std::uint32_t level_id) {
    SpLevel out;
    out.level_id = level_id;
    const nf::ChunkFile* map = level.map();
    if (!map) return out;

    // instance -> model (for the AI volumes' bounding boxes)
    std::unordered_map<std::size_t, ModelSlot> model_of;
    for (const nf::Placement& p : level.placements()) model_of[p.instance] = {p.chunk, p.model};

    const auto& statics = map->chunk.statics;
    for (std::size_t i = 0; i < statics.size(); ++i) {
        const nf::StaticInstance& s = statics[i];
        const std::uint32_t cls = s.object_class();
        const std::uint32_t index = std::uint32_t(i);
        switch (cls) {
        case kClassNpc:
            out.npcs.push_back({npc_spec_from_static(s), index});
            break;
        case kClassCoverCorner:
            out.cover_nodes.push_back(make_corner(s, index));
            break;
        case kClassCoverLow:
            out.cover_nodes.push_back(make_low(s, index));
            break;
        case kClassAiPoint: {
            AiPointDef p;
            p.id = std::uint16_t(s.param(0));
            p.radius = float(std::int32_t(s.param(1)));
            p.param34 = s.param(2);
            p.pos = {s.position[0], s.position[1], s.position[2]};
            p.rot = {s.euler[0], s.euler[1], s.euler[2]};
            out.ai_points.push_back(p);
            break;
        }
        case kClassSpawner: {
            SpawnerDef d;
            d.mode = s.param(0);
            if (d.mode < 2) break;   // DroneSpawner_Create: `1 < mode`
            d.group = s.param(1);
            d.max_alive = std::uint16_t(s.param(2));
            d.budget = std::uint16_t(s.param(3));
            if (d.budget == 0) d.budget = 32000;
            d.activate_channel = std::uint16_t(s.param(4));
            d.stop_channel = std::uint16_t(s.param(5));
            d.complete_channel = std::uint16_t(s.param(6));
            d.kill_channel = std::uint16_t(s.param(7));
            d.min_distance = float(s.param(8));
            if (d.min_distance == 0.0f) d.min_distance = 10.0f;
            d.pos = {s.position[0], s.position[1], s.position[2]};
            d.yaw = s.euler[1];
            out.spawners.push_back(d);
            break;
        }
        case kClassAiVolumeA:
        case kClassAiVolumeB: {
            // bounding box of the volume's model: entity_params floats at +0x1c..+0x30 = min.xyz, max.xyz
            std::optional<std::array<float, 6>> box;
            if (auto it = model_of.find(i); it != model_of.end()) {
                const nf::ChunkFile& cf = level.chunks()[it->second.chunk];
                const nf::Model& m = cf.chunk.models[it->second.model];
                float max_z = m.params[7];   // symmetric fallback when the block is not found
                for (const nf::Block& b : cf.chunk.blocks) {
                    if (b.id != std::uint8_t(nf::BlockId::EntityParams) || b.data.size() < 0x34) continue;
                    if (nf::load_cstr(b.data, 0x34) != m.name) continue;
                    max_z = nf::load<float>(b.data, 0x30);
                    break;
                }
                box = std::array<float, 6>{m.params[4], m.params[5], m.params[6], m.params[7], m.params[8], max_z};
            }
            AiVolumeDef v;
            v.center = {s.position[0], s.position[1], s.position[2]};
            {
                const auto [x, y, z, w] = s.quat;   // the placement's orientation (same convention as instance_transform)
                v.axes = {1 - 2 * (y * y + z * z), 2 * (x * y + z * w),     2 * (x * z - y * w),
                          2 * (x * y - z * w),     1 - 2 * (x * x + z * z), 2 * (y * z + x * w),
                          2 * (x * z + y * w),     2 * (y * z - x * w),     1 - 2 * (x * x + y * y)};
            }
            const std::uint32_t kind_param = s.param(1);
            if (level_id == 0x7000005 || kind_param == 4) v.center[1] += s.scale[1] * 1.5f;
            if (box) {
                v.half = {std::fabs((*box)[3] - (*box)[0]) * 0.5f * s.scale[0],
                          std::fabs((*box)[4] - (*box)[1]) * 0.5f * s.scale[1],
                          std::fabs((*box)[5] - (*box)[2]) * 0.5f * s.scale[2]};
            }
            v.visibility = float(s.param(0)) * 0.01f;
            v.p28 = s.param(2);
            v.p2c = std::uint8_t(s.param(3));
            v.p26 = std::uint16_t(s.param(4));
            v.kind = kind_param == 2 ? 1 : kind_param == 1 ? 2 : kind_param == 4 ? 3 : 0;   // other values keep the zeroed +0x24
            out.volumes.push_back(v);
            break;
        }
        default:
            break;
        }
    }
    return out;
}

}  // namespace nf::sp

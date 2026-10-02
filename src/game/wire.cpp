#include "game/wire.hpp"

#include <cmath>
#include <unordered_map>

namespace nf {

namespace {

constexpr std::uint32_t kEntityWire = 0x3E, kEntityThirdIcon = 0xFA;
constexpr std::uint32_t kHiddenFlags = 0xA000;   // parsemap_block_map_data_dynamic skips these
constexpr float kPi = 3.1415927f, kHalfPi = 1.5707964f, kTwoPi = 6.2831855f;

}  // namespace

std::vector<ObjectStatic> find_object_statics(const Level& level, std::uint32_t entity) {
    std::vector<ObjectStatic> found;
    if (!level.map()) return found;
    const auto& statics = level.map()->chunk.statics;
    std::unordered_map<std::size_t, std::size_t> placement_of;
    for (std::size_t p = 0; p < level.placements().size(); ++p) placement_of.emplace(level.placements()[p].instance, p);
    for (std::size_t i = 0; i < statics.size(); ++i) {
        if ((statics[i].flags & kHiddenFlags) != 0 || statics[i].flags != entity) continue;
        const auto it = placement_of.find(i);
        if (it != placement_of.end()) found.push_back({i, it->second});
    }
    return found;
}

std::vector<WireObject> find_wires(const Level& level) {
    std::vector<WireObject> wires;
    for (const auto [instance, placement] : find_object_statics(level, kEntityWire)) {
        const StaticInstance& s = level.map()->chunk.statics[instance];
        const Placement& p = level.placements()[placement];
        const Model& model = level.chunks().at(p.chunk).chunk.models.at(p.model);
        WireObject w;
        w.placement = placement;
        w.instance = instance;
        w.type = std::uint16_t(s.param(1));
        w.camera = std::uint16_t(s.param(0));
        w.channel = std::uint16_t(s.param(3));
        w.position = {s.position[0], s.position[1], s.position[2]};
        w.yaw = s.euler[1];
        w.pitch = -s.euler[2];
        w.center = transform_point(p.transform, {model.params[0], model.params[1], model.params[2]});
        w.radius = model.params[3];   // the control object's scale (obj+0xE8) is 1.0
        wires.push_back(w);
    }
    return wires;
}

std::vector<IconZone> find_icon_zones(const Level& level) {
    std::vector<IconZone> zones;
    for (const auto [instance, placement] : find_object_statics(level, kEntityThirdIcon)) {
        const StaticInstance& s = level.map()->chunk.statics[instance];
        zones.push_back({placement, s.param(0), std::uint16_t(s.param(1)), std::uint16_t(s.param(2))});
    }
    return zones;
}

Vec3 spherical_to_cartesian(float rho, float theta, float phi) {
    const float cos_phi = std::cos(phi);
    return {rho * std::sin(theta) * cos_phi, rho * std::sin(phi), rho * std::cos(theta) * cos_phi};
}

WireEnds wire_ends(const WireObject& wire, float trim, float phi) {
    const Vec3 v = spherical_to_cartesian(wire.radius - trim, wire.yaw + kHalfPi, phi);
    return {wire.center + v, wire.center - v};
}

Vec3 closest_point_on_segment(const Vec3& a, const Vec3& b, const Vec3& p) {
    const Vec3 d = b - a;
    const float len_sq = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
    if (!(1e-20f < std::fabs(len_sq))) return a;
    const Vec3 ap = p - a;
    const float t = (d[0] * ap[0] + d[1] * ap[1] + d[2] * ap[2]) / len_sq;
    if (t > 1.0f) return b;
    if (t < 0.0f) return a;
    return a + d * t;
}

float angle_difference(float a, float b) {
    auto wrap = [](float v) {
        if (v < 0.0f || kTwoPi <= v) {
            v = std::fmod(v, kTwoPi);
            if (v < 0.0f) v += kTwoPi;
        }
        return v;
    };
    float d = wrap(b) - wrap(a);
    if (kPi <= std::fabs(d)) d += d >= 0.0f ? -kTwoPi : kTwoPi;
    return d;
}

float dist_2d_sq(const Vec3& a, const Vec3& b) {
    const float dx = a[0] - b[0], dz = a[2] - b[2];
    return dx * dx + dz * dz;
}

}  // namespace nf

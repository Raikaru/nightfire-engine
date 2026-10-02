#include "game/grapple.hpp"

#include <cmath>

#include "game/wire.hpp"

namespace nf {

namespace {
constexpr std::uint32_t kEntityGrapple = 0x3A;
}

std::vector<GrappleTarget> find_grapple_targets(const Level& level) {
    std::vector<GrappleTarget> targets;
    for (const auto [instance, placement] : find_object_statics(level, kEntityGrapple)) {
        const StaticInstance& s = level.map()->chunk.statics[instance];
        targets.push_back({placement, instance, {s.position[0], s.position[1], s.position[2]}});
    }
    return targets;
}

GrappleState grapple_begin(const Vec3& point) {
    GrappleState g;
    g.point = point;
    g.phase = GrappleState::Launch;
    g.timer = kGrappleGroundGrace;
    g.speed = 0.0f;
    return g;
}

GrappleStep grapple_step(GrappleState& g, const Vec3& position, const GrappleInputs& in) {
    GrappleStep step{position, false};
    if (g.timer != 0) --g.timer;

    if (g.phase == GrappleState::Hang) {
        if (!in.fire_held) g.phase = GrappleState::Release;
    } else if (g.phase == GrappleState::Release) {
        step.leave = true;
        g.phase = GrappleState::Idle;
    } else if (g.phase == GrappleState::Launch) {
        if ((in.multiplayer && !in.sight_clear) || (in.on_ground && g.timer == 0)) {
            g.phase = GrappleState::Release;
        } else {
            const Vec3 to_hook = g.point - position;
            const float distance = length(to_hook);
            if (distance < kGrappleStopDistance) {
                g.phase = GrappleState::Hang;
            } else {
                const float cap = in.multiplayer ? kGrappleTopSpeedMulti : kGrappleTopSpeedSingle;
                const float gained = g.speed + kGrappleAcceleration;
                g.speed = gained < cap ? gained : cap;
                step.position = position + to_hook * (g.speed / distance);
            }
        }
    }
    return step;
}

bool grapple_rope_visible(const GrappleState& g, bool weapon_locked, int weapon_id) {
    if (weapon_id != 0x50 && weapon_id != 0x51) return false;
    return g.phase == GrappleState::Launch || weapon_locked;
}

}  // namespace nf

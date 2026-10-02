#pragma once

#include "driving/world_query.hpp"

namespace nf::driving {

// An infinite horizontal plane at y = height: the simplest CollisionWorld, for physics verification and
// early nfdrive tests.
class FlatWorld final : public CollisionWorld {
public:
    explicit FlatWorld(float height = 0, Surface surface = Surface::Paved) : height_(height), surface_(surface) {}

    bool ground_below(const Vec3& p, GroundHit& out) const override {
        if (p[1] < height_) return false;  // only surfaces at or below the point count
        out.point = {p[0], height_, p[2]};
        out.normal = {0, 1, 0};
        out.surface = surface_;
        return true;
    }

    bool segment_hit(const Vec3& from, const Vec3& to, SegmentHit& out) const override {
        if (from[1] < height_ || to[1] >= height_) return false;  // must cross the plane downwards
        const float t = (from[1] - height_) / (from[1] - to[1]);
        out.t = t;
        out.point = {from[0] + (to[0] - from[0]) * t, height_, from[2] + (to[2] - from[2]) * t};
        out.normal = {0, 1, 0};
        out.surface = surface_;
        return true;
    }

private:
    float height_;
    Surface surface_;
};

}  // namespace nf::driving

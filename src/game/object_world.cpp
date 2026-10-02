#include "game/object_world.hpp"

#include <cmath>

#include <algorithm>
#include <array>

namespace nf {

namespace {

constexpr std::array<std::uint32_t, 3> kSolidClasses = {0x22, 0x3A, 0x3E};   // Ladder_Create, Grapple_Create, Wire_Create
constexpr std::array<std::uint32_t, 1> kZoneClasses = {0xFA};                // ThirdIcon_Create



// Pushes the sphere (`center`, `radius`) out of the AABB; zero when clear. The capsule tests its two
// end spheres plus its midpoint, which covers movers thinner than the capsule is long.
Vec3 push_sphere_box(const Vec3& center, float radius, const Vec3& mn, const Vec3& mx) {
    const Vec3 clamped{std::clamp(center[0], mn[0], mx[0]), std::clamp(center[1], mn[1], mx[1]),
                       std::clamp(center[2], mn[2], mx[2])};
    const Vec3 d = center - clamped;
    const float dist2 = dot(d, d);
    if (dist2 > radius * radius) return {};
    if (dist2 > 1e-12f) {
        const float dist = std::sqrt(dist2);
        return d * ((radius - dist) / dist);
    }
    // The centre is inside: leave through the nearest face.
    float best = radius + (mx[0] - mn[0]);
    Vec3 push{};
    for (int i = 0; i < 3; ++i) {
        const float to_min = center[i] - mn[i] + radius;
        if (to_min < best) {
            best = to_min;
            push = {};
            push[i] = -to_min;
        }
        const float to_max = mx[i] - center[i] + radius;
        if (to_max < best) {
            best = to_max;
            push = {};
            push[i] = to_max;
        }
    }
    return push;
}

}  // namespace

ObjectWorld::ObjectWorld(Level& level)
    : solids_(CollisionWorld::of_objects(level, kSolidClasses)),
      zones_(CollisionWorld::of_objects(level, kZoneClasses)),
      ladders_(find_ladders(level)),
      creep_walls_(find_creep_walls(level)),
      icons_(find_icon_zones(level)) {}

const LadderObject* ObjectWorld::ladder_at(std::size_t placement) const {
    for (const LadderObject& l : ladders_)
        if (l.placement == placement) return &l;
    return nullptr;
}

void ObjectWorld::collide(const CylinderQuery& q, CylinderResult& merged, ObjectContacts& contacts) const {
    // Solid objects continue from the capsule as the cels left it, so their push adds to the cels'.
    CylinderQuery solid = q;
    solid.a = merged.a;
    solid.b = merged.b;
    const CylinderResult found = solids_.cylinder(solid);
    merged.push_out += found.push_out;
    merged.a = found.a;
    merged.b = found.b;
    merged.contact |= found.contact;
    if (!found.hits.empty()) {
        merged.hits.insert(merged.hits.end(), found.hits.begin(), found.hits.end());
        std::stable_sort(merged.hits.begin(), merged.hits.end(),
                         [](const CollisionHit& x, const CollisionHit& y) { return x.dist < y.dist; });
    }

    // ThirdIcon_Update: every active zone the capsule overlaps offers its icon; the last one updated wins.
    CylinderQuery zone = solid;
    zone.a = merged.a;
    zone.b = merged.b;
    zone.hit = hitflag::kNoPushOut;
    zone.ignore = SIZE_MAX;
    contacts.icon.reset();
    std::size_t winner = 0;
    for (const CollisionHit& hit : zones_.cylinder(zone).hits) {
        for (const IconZone& icon : icons_) {
            if (icon.placement != hit.placement) continue;
            if (icon.on_channel != 0 && !channel(icon.on_channel)) break;   // still waiting for its switch
            if (icon.off_channel != 0 && channel(icon.off_channel)) break;  // deleted by its off switch
            if (!contacts.icon || icon.placement >= winner) {
                contacts.icon = icon.icon;
                winner = icon.placement;
            }
            break;
        }
    }
    // Script-driven solids: the capsule's end spheres and midpoint are pushed out of every mover, from
    // the capsule as the statics left it. placement SIZE_MAX marks a dynamic hit (no static instance).
    for (const Mover& mover : movers_) {
        const Vec3 mid = {(merged.a[0] + merged.b[0]) * 0.5f, (merged.a[1] + merged.b[1]) * 0.5f,
                          (merged.a[2] + merged.b[2]) * 0.5f};
        const Vec3 push = push_sphere_box(merged.a, q.radius, mover.min, mover.max) +
                          push_sphere_box(mid, q.radius, mover.min, mover.max) +
                          push_sphere_box(merged.b, q.radius, mover.min, mover.max);
        if (push[0] == 0.0f && push[1] == 0.0f && push[2] == 0.0f) continue;
        merged.push_out += push;
        merged.a += push;
        merged.b += push;
        CollisionHit hit;
        hit.dist = length(push);
        hit.point = mid;
        hit.normal = hit.dist > 1e-6f ? push * (1.0f / hit.dist) : Vec3{0, 1, 0};
        hit.placement = SIZE_MAX;
        merged.hits.push_back(hit);
    }
    if (!movers_.empty())
        std::stable_sort(merged.hits.begin(), merged.hits.end(),
                         [](const CollisionHit& x, const CollisionHit& y) { return x.dist < y.dist; });
}

Vec3 ObjectWorld::ride_displacement(const Vec3& feet, float radius) const {
    // The mover whose top face carries `feet`: horizontally within the capsule radius (feet dangle
    // over the edge) and vertically within 0.3 below the face. The deepest such mover wins.
    Vec3 ride{};
    float best = -1.0f;
    for (const Mover& mover : movers_) {
        if (feet[0] < mover.min[0] - radius || feet[0] > mover.max[0] + radius) continue;
        if (feet[2] < mover.min[2] - radius || feet[2] > mover.max[2] + radius) continue;
        const float drop = mover.max[1] - feet[1];
        if (drop < -0.3f || drop > 0.3f) continue;
        if (best < 0.0f || drop < best) {
            best = drop;
            ride = mover.displacement;
        }
    }
    return ride;
}

bool ObjectWorld::standing_on_mover(const Vec3& sole, float radius) const {
    for (const Mover& mover : movers_) {
        if (sole[0] < mover.min[0] - radius || sole[0] > mover.max[0] + radius) continue;
        if (sole[2] < mover.min[2] - radius || sole[2] > mover.max[2] + radius) continue;
        const float drop = mover.max[1] - sole[1];
        if (drop < -0.3f || drop > 0.3f) continue;
        return true;
    }
    return false;
}
}  // namespace nf

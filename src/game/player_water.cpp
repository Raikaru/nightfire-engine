#include "game/player_water.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <unordered_map>

#include "assets/nav_data.hpp"
#include "game/player.hpp"

namespace nf {

namespace {

constexpr std::size_t kMaxCandidates = 64;        // build_FindCel's CelList_153
constexpr float kFindRayLength = 256.0f;          // build_FindCel: vertical rays up and down
constexpr float kPortalPad = 0.1f;                // build_alloc_portal
constexpr float kSurfaceOffset = 0.3f;            // Player_InWater / Player_Update: the swimmer sits 0.3 below the surface
constexpr float kHeadHeight = 0.7f;               // the head bone (0x80000005) above obj+0x30 [INFERENCE: the eye height]

// Closest point of triangle abc to p (Ericson, Real-Time Collision Detection 5.1.5).
Vec3 closest_on_triangle(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c) {
    const Vec3 ab = b - a, ac = c - a, ap = p - a;
    const float d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) return a;
    const Vec3 bp = p - b;
    const float d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) return b;
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) return a + ab * (d1 / (d1 - d3));
    const Vec3 cp = p - c;
    const float d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) return c;
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) return a + ac * (d2 / (d2 - d6));
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    const float denom = 1.0f / (va + vb + vc);
    return a + ab * (vb * denom) + ac * (vc * denom);
}

// Collide_RayTriangle: plane intersection of origin + dir * t with the triangle (either face), t = distance.
bool ray_triangle(const Vec3& origin, const Vec3& dir, const Vec3& a, const Vec3& b, const Vec3& c, float& t) {
    const Vec3 n = cross(b - a, c - a);
    const float denom = dot(n, dir);
    if (denom == 0.0f) return false;
    t = dot(n, a - origin) / denom;
    const Vec3 p = origin + dir * t;
    return dot(cross(b - a, p - a), n) >= 0.0f && dot(cross(c - b, p - b), n) >= 0.0f &&
           dot(cross(a - c, p - c), n) >= 0.0f;
}

bool boxes_overlap(const Vec3& amin, const Vec3& amax, const Vec3& bmin, const Vec3& bmax) {
    for (int i = 0; i < 3; ++i)
        if (amax[std::size_t(i)] < bmin[std::size_t(i)] || bmax[std::size_t(i)] < amin[std::size_t(i)]) return false;
    return true;
}

}  // namespace

bool RoomMap::contains(int room, const Vec3& p) const {
    const Room& r = rooms_.at(std::size_t(room));
    return r.min[0] <= p[0] && p[0] <= r.max[0] && r.min[1] <= p[1] && p[1] <= r.max[1] && r.min[2] <= p[2] &&
           p[2] <= r.max[2];
}

RoomMap::RoomMap(const Level& level, const CollisionWorld& world) {
    const ChunkFile* map = level.map();
    if (!map) return;
    const std::size_t map_chunk = std::size_t(map - level.chunks().data());
    std::unordered_map<std::size_t, std::size_t> solid_of;   // placement -> CollisionWorld::solid index
    for (std::size_t i = 0; i < world.solid_count(); ++i) solid_of[world.solid(i).placement] = i;

    // The cels this cares about, in file order. A water plane keeps the sphere its model carries (cel+0x80/+0x8C).
    struct Surface {
        Vec3 centre;
        float radius, level;
    };
    std::vector<Surface> surfaces;
    std::unordered_map<std::size_t, std::size_t> room_of_model;   // map-chunk model -> room (first in file order)
    const auto& placements = level.placements();
    for (std::size_t pi = 0; pi < placements.size(); ++pi) {
        const Placement& p = placements[pi];
        const StaticInstance& s = level.chunks()[map_chunk].chunk.statics[p.instance];
        const std::uint32_t cls = s.flags & 0xFFFF;
        if (cls != celflag::kRoom && cls != celflag::kWaterRoom && cls != celflag::kWaterSurface) continue;
        const MapChunk& chunk = level.chunks()[p.chunk].chunk;
        const Model& model = chunk.models.at(p.model);
        const ModelBox box = model_box(chunk, p.model);
        if (cls == celflag::kWaterSurface) {
            const Vec3 centre = transform_point(p.transform, {model.params[0], model.params[1], model.params[2]});
            surfaces.push_back({centre, model.params[3], box.min[1]});   // cel+0x44: the lowest y of the box
            continue;
        }
        Room r;
        r.name = model.name;
        r.flags = cls;
        r.placement = pi;
        r.min = box.min;
        r.max = box.max;
        r.water = cls == celflag::kWaterRoom ? box.max[1] : kNoWater;   // parseentity_fixup_entity: cel+0x90
        if (p.chunk == map_chunk) room_of_model.emplace(p.model, rooms_.size());
        r.sphere_center = transform_point(p.transform, {model.params[0], model.params[1], model.params[2]});
        r.sphere_radius = model.params[3];   // parseentity_transform_bounding_box copies model radius unchanged to cel+0x8C
        rooms_.push_back(std::move(r));
    }

    // build_link_objects_to_rooms: the water plane belongs to the room that contains its centre, is at least as big
    // as the plane and has geometry closest to it (Collide_SphereIntersect, radius 4001, against the room's mesh;
    // the nearest triangle distance stands in for the hit distance) - a later plane overrides an earlier one.
    // The original walks the cel list back to front.
    for (auto it = surfaces.rbegin(); it != surfaces.rend(); ++it) {
        int best = kNone;
        float best_dist = 4000.0f;
        for (std::size_t i = rooms_.size(); i-- > 0;) {
            const Room& r = rooms_[i];
            if (it->radius > r.sphere_radius || !contains(int(i), it->centre)) continue;
            const auto solid = solid_of.find(r.placement);
            if (solid == solid_of.end()) continue;
            const CollisionWorld::Solid geometry = world.solid(solid->second);
            const Mat4& m = geometry.transform;
            float nearest = std::numeric_limits<float>::max();
            for (const CollisionTri& t : geometry.collision.tris) {
                const Vec3 c = closest_on_triangle(it->centre, transform_point(m, t.v[0]), transform_point(m, t.v[1]),
                                                   transform_point(m, t.v[2]));
                nearest = std::min(nearest, length(c - it->centre));
            }
            if (nearest < best_dist) {
                best = int(i);
                best_dist = nearest;
            }
        }
        if (best != kNone) rooms_[std::size_t(best)].water = it->level;
    }

    // parsemap_block_portal_data: a portal joins the rooms built from two models; each side gets one that leads to
    // the other. Lists grow at the head, so the last portal of a room is tested first.
    for (const PortalRecord& rec : parse_portals(level.chunks()[map_chunk].chunk)) {
        const auto a = room_of_model.find(rec.model_a), b = room_of_model.find(rec.model_b);
        if (a == room_of_model.end() || b == room_of_model.end()) continue;
        Portal portal;
        portal.quad = rec.quad;
        portal.lo = portal.hi = rec.quad[0];
        for (const Vec3& v : rec.quad)
            for (std::size_t k = 0; k < 3; ++k) {
                portal.lo[k] = std::min(portal.lo[k], v[k]);
                portal.hi[k] = std::max(portal.hi[k], v[k]);
            }
        for (std::size_t k = 0; k < 3; ++k) {
            portal.lo[k] -= kPortalPad;
            portal.hi[k] += kPortalPad;
        }
        portal.dest = int(b->second);
        rooms_[a->second].portals.push_back(portal);
        portal.dest = int(a->second);
        rooms_[b->second].portals.push_back(portal);
    }
    for (Room& r : rooms_) std::reverse(r.portals.begin(), r.portals.end());
}

float RoomMap::water_level(int room) const { return room == kNone ? kNoWater : rooms_.at(std::size_t(room)).water; }

int RoomMap::find(const Vec3& p, const CollisionWorld& world) const {
    std::vector<int> candidates;
    for (std::size_t i = rooms_.size(); i-- > 0 && candidates.size() < kMaxCandidates;)
        if (contains(int(i), p)) candidates.push_back(int(i));
    if (candidates.size() <= 1) return candidates.empty() ? kNone : candidates.front();

    // Several rooms overlap the point: the one whose geometry is nearest straight above or below wins. Both rays
    // only ever get shorter, so a later room must beat every hit so far.
    const auto nearest_hit = [&](const Vec3& to, std::size_t placement) -> std::optional<float> {
        for (const CollisionHit& h : world.ray_hits(p, to, 0))
            if (h.placement == placement) return h.dist;
        return std::nullopt;
    };
    float ray = kFindRayLength, best_dist = kFindRayLength;
    int best = kNone;
    for (int room : candidates) {
        const std::size_t placement = rooms_[std::size_t(room)].placement;
        std::optional<float> up = nearest_hit({p[0], p[1] + ray, p[2]}, placement);
        if (up) ray = std::min(*up, ray);
        std::optional<float> down = nearest_hit({p[0], p[1] - ray, p[2]}, placement);
        if (down) ray = std::min(*down, ray);
        std::optional<float> dist = up;
        if (down && (!up || *down <= *up)) dist = down;
        if (dist && *dist < best_dist) {
            best = room;
            best_dist = *dist;
        }
    }
    return best != kNone ? best : candidates.front();
}

bool RoomMap::crosses_portal(const Portal& portal, const Vec3& from, const Vec3& to) const {
    Vec3 lo = from, hi = from;
    for (std::size_t k = 0; k < 3; ++k) {
        lo[k] = std::min(from[k], to[k]);
        hi[k] = std::max(from[k], to[k]);
    }
    if (!boxes_overlap(lo, hi, portal.lo, portal.hi)) return false;
    const float len = length(to - from);
    const Vec3 dir = normalised(to - from);
    float t;
    const auto& q = portal.quad;
    if (ray_triangle(from, dir, q[0], q[1], q[2], t) && 0.0f <= t && t <= len) return true;
    return ray_triangle(from, dir, q[2], q[3], q[0], t) && 0.0f <= t && t <= len;
}

int RoomMap::track(int room, const Vec3& from, const Vec3& to, const CollisionWorld& world) const {
    if (room == kNone) return find(to, world);
    for (const Portal& portal : rooms_[std::size_t(room)].portals)
        if (crosses_portal(portal, from, to)) return portal.dest;
    return contains(room, to) ? room : find(to, world);
}

void RoomMap::mark_visible_cells(int viewer, const Vec3& eye, const Vec3& right, const Vec3& up,
                                const Vec3& forward, float tan_half_x, float tan_half_y,
                                std::vector<std::uint8_t>& visible) const {
    visible.assign(rooms_.size(), 0);
    if (viewer < 0 || std::size_t(viewer) >= rooms_.size()) return;

    struct Plane {
        Vec3 normal;
    };
    constexpr std::size_t kPlaneCapacity = 64;
    constexpr std::size_t kPolygonCapacity = 64;
    constexpr int kMaxVisibleCels = 64;
    constexpr int kMaxRecursionDepth = 46;
    constexpr int kMaxRecursions = 101;

    std::array<Plane, kPlaneCapacity> initial{};
    initial[0].normal = forward * tan_half_x + right;
    initial[1].normal = forward * tan_half_x - right;
    initial[2].normal = forward * tan_half_y + up;
    initial[3].normal = forward * tan_half_y - up;

    visible[std::size_t(viewer)] = 1;
    int visible_count = 1;
    int recursion_count = 0;
    const auto recurse = [&](auto&& self, int room, const std::array<Plane, kPlaneCapacity>& planes,
                             std::size_t plane_count, int depth) -> void {
        if (depth >= kMaxRecursionDepth || recursion_count >= kMaxRecursions) return;
        for (const Portal& portal : rooms_[std::size_t(room)].portals) {
            if (++recursion_count >= kMaxRecursions) return;

            std::array<Vec3, kPolygonCapacity> polygon{};
            std::array<Vec3, kPolygonCapacity> clipped{};
            std::size_t polygon_size = portal.quad.size();
            for (std::size_t i = 0; i < polygon_size; ++i) polygon[i] = portal.quad[i];
            for (std::size_t plane = 0; plane < plane_count && polygon_size != 0; ++plane) {
                std::size_t clipped_size = 0;
                Vec3 previous = polygon[polygon_size - 1];
                float previous_distance = dot(planes[plane].normal, previous - eye);
                bool previous_inside = previous_distance >= 0.0f;
                for (std::size_t i = 0; i < polygon_size; ++i) {
                    const Vec3 current = polygon[i];
                    const float current_distance = dot(planes[plane].normal, current - eye);
                    const bool current_inside = current_distance >= 0.0f;
                    if (current_inside != previous_inside && clipped_size < kPolygonCapacity) {
                        const float t = previous_distance / (previous_distance - current_distance);
                        clipped[clipped_size++] = previous + (current - previous) * t;
                    }
                    if (current_inside && clipped_size < kPolygonCapacity) clipped[clipped_size++] = current;
                    previous = current;
                    previous_distance = current_distance;
                    previous_inside = current_inside;
                }
                polygon.swap(clipped);
                polygon_size = clipped_size;
            }
            if (polygon_size < 3 || portal.dest < 0 || std::size_t(portal.dest) >= rooms_.size()) continue;
            if (!visible[std::size_t(portal.dest)]) {
                if (visible_count >= kMaxVisibleCels) continue;
                visible[std::size_t(portal.dest)] = 1;
                ++visible_count;
            } else {
                continue;
            }

            std::array<Plane, kPlaneCapacity> child_planes = planes;
            std::size_t child_plane_count = plane_count;
            Vec3 centre{};
            for (std::size_t i = 0; i < polygon_size; ++i) centre += polygon[i];
            centre = centre * (1.0f / float(polygon_size));
            for (std::size_t i = 0; i < polygon_size && child_plane_count < kPlaneCapacity; ++i) {
                const Vec3 from_eye = polygon[i] - eye;
                const Vec3 to_eye = polygon[(i + 1) % polygon_size] - eye;
                Vec3 normal = cross(from_eye, to_eye);
                const float magnitude = length(normal);
                if (magnitude == 0.0f) continue;
                normal = normal * (1.0f / magnitude);
                if (dot(normal, centre - eye) < 0.0f) normal = normal * -1.0f;
                child_planes[child_plane_count++].normal = normal;
            }
            self(self, portal.dest, child_planes, child_plane_count, depth + 1);
        }
    };
    recurse(recurse, viewer, initial, 4, 0);
}

bool RoomMap::cell_in_view(int target, const std::vector<std::uint8_t>& visible, const Vec3& eye, const Vec3& right,
                           const Vec3& up, const Vec3& forward, float tan_half_x, float tan_half_y) const {
    if (target < 0 || std::size_t(target) >= rooms_.size() || std::size_t(target) >= visible.size() ||
        !visible[std::size_t(target)])
        return false;

    // Vision_InView tests cel+0x80/+0x8C against the four viewer side planes.
    const Room& cel = rooms_[std::size_t(target)];
    const Vec3 delta = cel.sphere_center - eye;
    const float depth = dot(delta, forward);
    const float horizontal_margin = cel.sphere_radius * std::sqrt(1.0f + tan_half_x * tan_half_x);
    const float vertical_margin = cel.sphere_radius * std::sqrt(1.0f + tan_half_y * tan_half_y);
    return depth * tan_half_x + horizontal_margin >= std::abs(dot(delta, right)) &&
           depth * tan_half_y + vertical_margin >= std::abs(dot(delta, up));
}

// --- Player: water -------------------------------------------------------------------------------------------

float Player::water_level() const { return rooms ? rooms->water_level(water.room) : kNoWater; }

Vec3 Player::head_position() const { return pos + orientation()[1] * (kHeadHeight - crouch_dip); }

bool Player::in_water() {
    if (!rooms || water.room == RoomMap::kNone) return false;   // obj+0x20 == 0
    if (pos[1] - kSurfaceOffset < water_level()) {
        body_flags |= body::kInWater;
        // MPSettings+384: nobody swims in multiplayer, the flag is all that is kept.
        if (params_.health.damage.mode == GameMode::Multiplayer) return substate == SubState::Swim;
        if (life != LifeState::Dead) {
            set_substate(SubState::Swim, timing_);
            jump_state = 0;
            look_state_ = 2;
            rope_.attach_lockout = std::uint8_t(int(timing_.FRAME_RATE * (1.0f / 60.0f) * 5.0f));   // BL+0x951 = FRAME_RATE_DIV * 5
            velocity = prev_velocity_ = {};   // Vec_Zero of BL+0x10/+0x20/+0x30/+0x40, BL+0x14
            yaw_step_ = 0.0f;
        }
    } else {
        body_flags &= std::uint16_t(~body::kInWater);
    }
    return substate == SubState::Swim;
}

void Player::swim_direction() {
    // Player_Move, substate 3: the forward step follows the view pitch: Mat_GetDir(RotMatrixX(-pitch * pi/2)).
    const Vec3 dir = rot_x(-pitch * 1.5707964f)[2];
    const float forward = velocity[2];
    velocity[1] = dir[1] * forward;
    velocity[2] = dir[2] * forward;
}

void Player::update_swim(const ActionInput& input, FrameTiming timing) {
    // Player_Update, case 3.
    move(input, timing, 1.0f);
    if (!rooms || water.room == RoomMap::kNone) return;
    const float level = water_level();
    if ((body_flags & body::kOnGround) != 0 && level <= pos[1] - kSurfaceOffset) {
        look_state_ = 2;   // stepped out of the pool onto the floor
        set_substate(SubState::Walk, timing);
    }
    // The swimmer may not rise above the surface: clamp the world-space step, then bring it back to body space.
    const Basis axes = orientation();
    Vec3 world = axes[0] * velocity[0] + axes[1] * velocity[1] + axes[2] * velocity[2];
    const float surface = level - kSurfaceOffset;
    if (pos[1] < surface) {
        if (surface < pos[1] + world[1]) world[1] = surface - pos[1];
    } else if (world[1] > 0.0f) {
        world[1] = 0.0f;
    }
    velocity = {dot(world, axes[0]), dot(world, axes[1]), dot(world, axes[2])};
}

void Player::monitor_air(FrameTiming timing) {
    // Player_MonitorAir.
    ++water.frame;
    water.meter_enabled = substate == SubState::Swim || substate == SubState::Wire;
    if (!rooms || water.room == RoomMap::kNone) return;
    const float level = water_level();
    const bool zero_g = substate == SubState::ZeroG || substate == SubState::ZeroGWalk;
    const bool head_under = head_position()[1] < level;
    const float rec = timing.rec();

    if (head_under) {
        const float gap = 1.0f - water.meter_alpha;
        water.meter_alpha = std::min(water.meter_alpha + gap * rec, 1.0f);
        water.meter_flags |= 0x10;
    } else {
        const float gap = -water.meter_alpha;
        water.meter_alpha = std::max(water.meter_alpha + gap * rec * 0.5f, 0.0f);
        if ((water.meter_flags & 0x10) != 0) water.meter_flags |= 1;
        water.meter_flags &= ~0x10u;
    }
    if (substate == SubState::Wire) water.meter_flags |= 0x10;

    if (!head_under || zero_g) {
        water.air += rec * 20.0f;
        if (!water.surfaced) {
            events_.sounds.push_back({watersound::kGasp, pos});
            water.surfaced = true;
        }
    } else {
        water.air -= rec * 5.0f;
        if (water.surfaced) {
            events_.sounds.push_back({watersound::kBubbles, pos});
            water.surfaced = false;
        }
    }
    water.air = water.air >= 0.0f ? std::min(water.air, 100.0f) : 0.0f;

    if (water.air < 1.0f) {
        const std::uint64_t period = std::uint64_t(timing.FRAME_RATE * 0.5f);
        if (period != 0 && water.frame % period == 0) {
            events_.sounds.push_back({watersound::kDrownHit, pos});
            hurt(2.0f, pos, {}, DamageType::Drown);   // Player_HandlePain(obj, bl, 2.0, 7, -1)
        }
    }
}

void Player::track_room(const CollisionWorld& world) {
    // control_handle_cel_change, run once the collision pass has settled the position.
    if (!rooms) return;
    water.room = rooms->track(water.room, water.room_from, pos, world);
    water.room_from = pos;
}

}  // namespace nf

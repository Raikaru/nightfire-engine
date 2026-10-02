#include "game/nav.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <list>

namespace nf {

namespace {

constexpr float kFeetSkin = 0.4f;
constexpr float kNodeYTol = 2.0f;          // NodeSearchCel box half height / boundary min height / clip trigger
constexpr float kDefaultBox = 15.0f;       // NodeSearchCel default half extent
constexpr float kSeedRadiusSq = 625.0f;    // seeds: radius^2 = 25^2 (0x441c4000)
constexpr std::uint32_t kRoomFlag = 0x40000;

float sqdist2d(const Vec3& a, const Vec3& b) {
    float dx = a[0] - b[0], dz = a[2] - b[2];
    return dx * dx + dz * dz;
}
float dist2d(const Vec3& a, const Vec3& b) { return std::sqrt(sqdist2d(a, b)); }

// vecutil_calculate_closest_point_on_line: closest point of the segment a->b to p (3-D projection, clamped).
Vec3 closest_on_segment(const Vec3& a, const Vec3& b, const Vec3& p, float* t_out = nullptr) {
    Vec3 d = b - a;
    float len2 = dot(d, d);
    float t = 0;
    if (std::fabs(len2) > 1e-20f) {
        t = dot(d, p - a) / len2;
        t = t > 1.0f ? 1.0f : (t < 0.0f ? 0.0f : t);
    }
    if (t_out) *t_out = t;
    return a + d * t;
}

// vecutil_intersect_line_line_2d @ 0x1e0a.. (xz plane). 0 = none, 1 = (near) parallel overlap (no point
// written by the original), 2 = crossing with `out` = point.
int intersect_2d(const Vec3& p1, const Vec3& p2, const Vec3& p3, const Vec3& p4, float& ox, float& oz) {
    float dx1 = p2[0] - p1[0];
    float dx34 = p3[0] - p4[0];
    float max1 = dx1 >= 0 ? p2[0] : p1[0], min1 = dx1 >= 0 ? p1[0] : p2[0];
    float max2 = p3[0] > p4[0] ? p3[0] : p4[0], min2 = p3[0] > p4[0] ? p4[0] : p3[0];
    if (max1 < min2 || min1 > max2) return 0;
    float dz1 = p2[2] - p1[2];
    float dz34 = p3[2] - p4[2];
    float zmax1 = dz1 >= 0 ? p2[2] : p1[2], zmin1 = dz1 >= 0 ? p1[2] : p2[2];
    float zmax2 = p3[2] > p4[2] ? p3[2] : p4[2], zmin2 = p3[2] > p4[2] ? p4[2] : p3[2];
    if (zmax1 < zmin2 || zmin1 > zmax2) return 0;
    float det = dz1 * dx34 - dx1 * dz34;
    float na = dz34 * (p1[0] - p3[0]) - dx34 * (p1[2] - p3[2]);
    float nb = dx1 * (p1[2] - p3[2]) - dz1 * (p1[0] - p3[0]);
    if (det > 0) {
        if (na < 0 || det < na) return 0;
        if (nb < 0 || nb > det) return 0;
    } else {
        if (na > 0 || na < det) return 0;
        if (nb > 0 || nb < det) return 0;
    }
    if (det <= 0.0002f && det >= -0.0002f) return 1;
    ox = p1[0] + na * dx1 / det;
    oz = p1[2] + na * dz1 / det;
    return 2;
}

// vecutil_point_on_poly for the portal triangles: the point lies inside triangle (a, b, c) of its plane.
bool point_in_triangle(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& n) {
    auto side = [&](const Vec3& u, const Vec3& v) { return dot(cross(v - u, p - u), n); };
    constexpr float eps = -1e-4f;
    return side(a, b) >= eps && side(b, c) >= eps && side(c, a) >= eps;
}

// Collide_RayTriangle as used by Intersect_Portal: the *line* through from->to meets the triangle
// (the original never range-checks the ray parameter).
bool line_hits_triangle(const Vec3& from, const Vec3& to, const Vec3& a, const Vec3& b, const Vec3& c) {
    Vec3 n = cross(b - a, c - a);
    float nl = length(n);
    if (nl < 1e-12f) return false;
    n = n * (1.0f / nl);
    Vec3 d = to - from;
    float den = dot(n, d);
    if (den == 0.0f) return false;
    float t = dot(n, a - from) / den;
    return point_in_triangle(from + d * t, a, b, c, n);
}

}  // namespace

// ---- small public helpers -----------------------------------------------------------------------

NavLimits NavLimits::for_level(std::uint32_t level_id) {
    NavLimits l;
    if (level_id == 0x07000008) {   // Drone_PostLoad_Init @0x136938
        l.max_slope = 0.7853982f;
        l.slope_dy = 1.5f;
    }
    return l;
}

std::uint32_t level_id_from_name(const std::string& bin_name) {
    std::string stem = bin_name.substr(0, bin_name.find('.'));
    char* end = nullptr;
    unsigned long v = std::strtoul(stem.c_str(), &end, 16);
    return end && *end == 0 && !stem.empty() ? std::uint32_t(v) : 0;
}

const char* route_status_name(RouteStatus s) {
    switch (s) {
    case RouteStatus::Following: return "following";
    case RouteStatus::Approximate: return "approximate";
    case RouteStatus::Straight: return "straight";
    case RouteStatus::Arrived: return "arrived";
    case RouteStatus::NoTargetNodes: return "no-target-nodes";
    case RouteStatus::NoStartNode: return "no-start-node";
    case RouteStatus::Exhausted: return "exhausted";
    case RouteStatus::NoPath: return "no-path";
    case RouteStatus::CannotCalc: return "cannot-calc";
    case RouteStatus::CreepFailed9: return "creep-failed-9";
    case RouteStatus::Reset: return "reset";
    case RouteStatus::CreepFailed: return "creep-failed";
    }
    return "?";
}

int NavPath::link_between(std::uint16_t a, std::uint16_t b) const {
    if (a >= nodes.size()) return -1;
    const auto& n = nodes[a];
    for (unsigned k = 0; k < n.adj_count; ++k) {
        std::uint16_t l = adjacency[n.adj_first + k];
        if ((links[l].a == a && links[l].b == b) || (links[l].a == b && links[l].b == a)) return l;
    }
    return -1;
}

// ---- construction -------------------------------------------------------------------------------

NavNetwork::NavNetwork(Level& level, const CollisionWorld& world, NavLimits limits)
    : world_(world), level_(level), limits_(limits) {
    const ChunkFile* map = level.map();
    if (!map) return;
    const MapChunk& chunk = map->chunk;
    const std::size_t map_index = std::size_t(map - level.chunks().data());
    tracks_ = parse_path_data(chunk);
    static_tracks_ = static_path_refs(chunk);

    // Rooms: build_FindCel walks glb_world (newest cel first) for cels with flag 0x40000, which
    // parseentity_fixup_entity gives to statics of class 0xc023 / 0xc047 (0x804c023 / 0x804c047).
    std::vector<std::size_t> placement_of(chunk.statics.size(), SIZE_MAX);
    for (std::size_t i = 0; i < level.placements().size(); ++i) {
        const Placement& p = level.placements()[i];
        if (p.chunk == map_index && p.instance < placement_of.size()) placement_of[p.instance] = i;
    }
    std::vector<std::size_t> room_model;   // local model index per cel (portal_data addresses models)
    for (std::size_t si = chunk.statics.size(); si-- > 0;) {
        std::uint32_t cls = chunk.statics[si].flags & 0xFFFF;
        if ((cls != 0xc023 && cls != 0xc047) || placement_of[si] == SIZE_MAX) continue;
        const Placement& p = level.placements()[placement_of[si]];
        NavCel c;
        c.placement = placement_of[si];
        ModelBox box = model_box(level.chunks()[p.chunk].chunk, p.model);
        c.min = box.min;
        c.max = box.max;
        cels_.push_back(std::move(c));
        room_model.push_back(p.model);
    }
    (void)kRoomFlag;

    // Portals link the cels built from their two models (build_alloc_portal, both directions).
    cel_portals_.resize(cels_.size());
    for (const PortalRecord& pr : parse_portals(chunk)) {
        int ca = -1, cb = -1;
        for (std::size_t c = 0; c < cels_.size(); ++c) {   // list order; the last match wins like the original
            if (room_model[c] == pr.model_a) ca = int(c);
            if (room_model[c] == pr.model_b) cb = int(c);
        }
        if (ca < 0 || cb < 0) continue;
        portals_.push_back({pr.quad, ca, cb});
        cel_portals_[std::size_t(ca)].push_back(int(portals_.size() - 1));
        cel_portals_[std::size_t(cb)].push_back(int(portals_.size() - 1));
    }

    auto data = parse_ai_network(chunk);
    if (!data) return;

    // AIPath_Parse: records -> paths / bounds.
    for (const AiRecord& r : data->records) {
        if (r.is_bounds()) {
            Bounds b;
            b.index = int(bounds_.size());
            b.name = r.name;
            for (const auto& n : r.bnodes) b.nodes.push_back({n.pos, 0.0f, kNoCel});
            for (const auto& l : r.blinks) b.links.push_back({l.a, l.b, 0});
            bounds_.push_back(std::move(b));
        } else {
            NavPath p;
            p.index = int(paths_.size());
            p.name = r.name;
            p.flags = r.flags;
            for (const auto& n : r.nodes) {
                NavNode nn;
                nn.pos = n.pos;
                nn.flags = n.flags;
                p.nodes.push_back(nn);
            }
            for (const auto& l : r.links) p.links.push_back({l.a, l.b, 0, 0, 0.0f});
            max_nodes_ = std::max(max_nodes_, p.nodes.size());
            max_links_ = std::max(max_links_, p.links.size());
            paths_.push_back(std::move(p));
        }
    }

    // AIPath_BindNodes: AIPath_Link2Cel (head insertion into the cel's node list) then AIPath_Prepare.
    for (NavPath& p : paths_) {
        for (std::size_t i = 0; i < p.nodes.size(); ++i) {
            int cel = find_cel(p.nodes[i].pos);
            p.nodes[i].cel = cel;
            if (cel != kNoCel) cels_[std::size_t(cel)].nodes.push_back({std::uint16_t(p.index), std::uint16_t(i)});
        }
        // AIPath_Prepare: link lengths and per-node adjacency (links in index order).
        for (NavLink& l : p.links) l.length = length(p.nodes[l.a].pos - p.nodes[l.b].pos);
        for (std::size_t i = 0; i < p.nodes.size(); ++i) {
            p.nodes[i].adj_first = std::uint16_t(p.adjacency.size());
            for (std::size_t j = 0; j < p.links.size(); ++j)
                if (p.links[j].a == i || p.links[j].b == i) p.adjacency.push_back(std::uint16_t(j));
            p.nodes[i].adj_count = std::uint8_t(p.adjacency.size() - p.nodes[i].adj_first);
        }
    }
    // AIBounds_Link2Cel (cel + ceiling height) then AIBounds_Prepare (BLINK lists of both end cels).
    for (Bounds& b : bounds_) {
        for (std::size_t i = 0; i < b.nodes.size(); ++i) {
            BoundsNode& n = b.nodes[i];
            n.cel = find_cel(n.pos);
            if (n.cel == kNoCel) continue;
            float height = 256.0f;
            if (auto d = cel_ray_distance(n.cel, n.pos, n.pos + Vec3{0, 256.0f, 0})) height = *d;
            n.height = std::max(height, 2.0f);
        }
        for (std::size_t j = 0; j < b.links.size(); ++j) {
            int ca = b.nodes[b.links[j].a].cel, cb = b.nodes[b.links[j].b].cel;
            BlinkRef ref{std::uint16_t(b.index), std::uint16_t(j)};
            if (ca != kNoCel) cels_[std::size_t(ca)].blinks.push_back(ref);
            if (cb != kNoCel && cb != ca) cels_[std::size_t(cb)].blinks.push_back(ref);
        }
    }
    // Head insertion: the most recently linked entry is walked first.
    for (NavCel& c : cels_) {
        std::reverse(c.nodes.begin(), c.nodes.end());
        std::reverse(c.blinks.begin(), c.blinks.end());
    }
    init_passable_boundaries();
}

NavNetwork::~NavNetwork() = default;

void NavNetwork::begin_frame(std::uint32_t frame) {
    frame_ = frame;
    clear_link_flags(0x310);
}

void NavNetwork::clear_link_flags(std::uint16_t mask) {
    // AINetwork_ClearLinkFlags: clear the flag bits and zero every used-count.
    for (NavPath& p : paths_)
        for (NavLink& l : p.links) {
            l.flags &= std::uint16_t(~mask);
            l.used = 0;
        }
}

// ---- cels ---------------------------------------------------------------------------------------

std::optional<float> NavNetwork::cel_ray_distance(int cel, const Vec3& from, const Vec3& to) const {
    // Collide_RayIntersect(from, to, cel): the nearest hit inside that room's own collision.
    std::size_t placement = cels_[std::size_t(cel)].placement;
    for (const CollisionHit& h : world_.ray_hits(from, to, 0))
        if (h.placement == placement) return h.dist;
    return std::nullopt;
}

int NavNetwork::find_cel(const Vec3& pos) const {
    // build_FindCel(pos, glb_world) @0x1c8ff0: rooms whose box contains the point.
    std::vector<int> cand;
    for (std::size_t c = 0; c < cels_.size() && cand.size() < 0x40; ++c) {
        const NavCel& cel = cels_[c];
        if (pos[0] >= cel.min[0] && pos[0] <= cel.max[0] && pos[2] >= cel.min[2] && pos[2] <= cel.max[2] &&
            pos[1] >= cel.min[1] && pos[1] <= cel.max[1])
            cand.push_back(int(c));
    }
    if (cand.size() <= 1) return cand.empty() ? kNoCel : cand[0];
    // Several overlapping rooms: vertical rays +-256 (range shrinks with every hit); the room with the
    // nearest surface wins, the first candidate when none is hit.
    float range = 256.0f, best_d = 256.0f;
    int best = kNoCel;
    for (int c : cand) {
        auto up = cel_ray_distance(c, pos, pos + Vec3{0, range, 0});
        if (up) range = std::min(range, *up);
        auto down = cel_ray_distance(c, pos, pos - Vec3{0, range, 0});
        if (down) range = std::min(range, *down);
        std::optional<float> d = up;
        if (down && (!d || *down <= *d)) d = down;
        if (d && *d < best_d) {
            best = c;
            best_d = *d;
        }
    }
    return best != kNoCel ? best : cand[0];
}

// Collide_StraddleCels for a ray HITTEST (type 0x201): flood from the start cel through the portals
// whose quad the (infinite) line meets, each portal pair tested once.
void NavNetwork::straddle(int start, const Vec3& from, const Vec3& to, std::vector<int>& out) const {
    out.clear();
    out.push_back(start);
    std::vector<char> seen(portals_.size(), 0);
    for (std::size_t i = 0; i < out.size() && out.size() < 0x7e; ++i) {
        for (int pi : cel_portals_[std::size_t(out[i])]) {
            if (seen[std::size_t(pi)]) continue;
            seen[std::size_t(pi)] = 1;
            const Portal& p = portals_[std::size_t(pi)];
            if (!line_hits_triangle(from, to, p.quad[0], p.quad[1], p.quad[2]) &&
                !line_hits_triangle(from, to, p.quad[2], p.quad[3], p.quad[0]))
                continue;
            int other = p.cel_a == out[i] ? p.cel_b : p.cel_a;
            if (std::find(out.begin(), out.end(), other) == out.end()) out.push_back(other);
        }
    }
}

// ---- boundaries ---------------------------------------------------------------------------------

NavNetwork::LineCross NavNetwork::cross_boundary(BlinkRef ref, const Vec3& from, const Vec3& to) const {
    // AINetwork_LineIntersectsBoundry @0x157990 (without the clip side effect).
    LineCross r;
    const Bounds& b = bounds_[ref.bounds];
    const BoundsLink& l = b.links[ref.link];
    const BoundsNode& a = b.nodes[l.a];
    const BoundsNode& c = b.nodes[l.b];
    float x = 0, z = 0;
    int k = intersect_2d(from, to, a.pos, c.pos, x, z);
    if (k == 0) return r;
    Vec3 ix{x, from[1], z};
    if (k == 1) ix = from;   // collinear: the original reads an unwritten buffer; treat as a touch at the start
    float span = dist2d(from, to);
    float line_y = from[1] + (to[1] - from[1]) * (span > 0 ? dist2d(from, ix) / span : 0.0f);
    float bl = dist2d(a.pos, c.pos);
    float f = bl > 0 ? dist2d(a.pos, ix) / bl : 0.0f;
    float floor_y = a.pos[1] + (c.pos[1] - a.pos[1]) * f;
    float top = floor_y + (a.height + (c.height - a.height) * f);
    if (line_y > top || floor_y - 2.4f > line_y) return r;
    r.hit = true;
    ix[1] = line_y;
    r.point = ix;
    r.clip = line_y - floor_y > 2.0f;
    r.clip_height = line_y - 0.5f;
    return r;
}

bool NavNetwork::line_intersects_boundary(BlinkRef link, const Vec3& from, const Vec3& to, Vec3* hit, float* nearest,
                                          bool clip_height) {
    LineCross c = cross_boundary(link, from, to);
    if (!c.hit) return false;
    if (clip_height && c.clip) {
        Bounds& b = bounds_[link.bounds];
        b.nodes[b.links[link.link].a].height = c.clip_height;
        b.nodes[b.links[link.link].b].height = c.clip_height;
    }
    if (nearest) {
        Vec3 d = c.point - from;
        d[1] = 0;   // the original measures from `from` with y equalised
        float dist = length(d);
        if (*nearest <= dist) return true;
        *nearest = dist;
    }
    if (hit) *hit = c.point;
    return true;
}

int NavNetwork::bounds_test_i(const CelPos& from, const CelPos& to, Vec3* hit, bool corner) const {
    // AINetwork_BoundsTest @0x157d80.
    if (from.cel == kNoCel) return 0;
    Vec3 f = from.pos + Vec3{0, 1, 0}, t = to.pos + Vec3{0, 1, 0};
    std::vector<int> cels;
    straddle(from.cel, f, t, cels);
    if (std::find(cels.begin(), cels.end(), to.cel) == cels.end()) return 0;
    int result = 1;
    float nearest = 100000.0f;
    for (int c : cels) {
        for (BlinkRef ref : cels_[std::size_t(c)].blinks) {
            LineCross x = cross_boundary(ref, from.pos, to.pos);
            if (x.hit) {
                if (!hit) return 0;
                result = -1;
                float dist = length(Vec3{x.point[0] - from.pos[0], 0, x.point[2] - from.pos[2]});
                if (dist < nearest) {
                    nearest = dist;
                    *hit = x.point;
                }
            }
            if (!corner || hit) continue;
            // Corner-post test: skipped when either end lies within 0.4 of a post.
            const Bounds& b = bounds_[ref.bounds];
            const Vec3& pa = b.nodes[b.links[ref.link].a].pos;
            const Vec3& pb = b.nodes[b.links[ref.link].b].pos;
            constexpr float r2 = 0.16000001f;
            if (sqdist2d(pa, to.pos) <= r2 || sqdist2d(pb, to.pos) <= r2) continue;
            if (sqdist2d(pa, from.pos) <= r2 || sqdist2d(pb, from.pos) <= r2) continue;
            Vec3 cp = closest_on_segment(from.pos, to.pos, pa);
            if (sqdist2d(cp, pa) < r2 && std::fabs(cp[1] - pa[1]) < kNodeYTol) return 0;
            cp = closest_on_segment(from.pos, to.pos, pb);
            // second post: the original compares the (unsquared) 2-D distance with 0.16 and node A's y
            if (dist2d(cp, pb) < r2 && std::fabs(cp[1] - pa[1]) < kNodeYTol) return 0;
        }
    }
    return result;
}

bool NavNetwork::bounds_test(const CelPos& from, const CelPos& to, Vec3* hit, bool corner_test) const {
    return bounds_test_i(from, to, hit, corner_test) == 1;
}

bool NavNetwork::test_ray_cels(const CelPos& from, const CelPos& to) const {
    if (from.cel == kNoCel) return false;
    std::vector<int> cels;
    straddle(from.cel, from.pos + Vec3{0, 1, 0}, to.pos + Vec3{0, 1, 0}, cels);
    return std::find(cels.begin(), cels.end(), to.cel) != cels.end();
}

bool NavNetwork::bounds_node_test(const CelPos& from, const CelPos& to, float radius) const {
    // AINetwork_BoundsNodeTest @0x158140.
    if (from.cel == kNoCel) return false;
    std::vector<int> cels;
    straddle(from.cel, from.pos, to.pos, cels);
    if (to.cel != kNoCel && std::find(cels.begin(), cels.end(), to.cel) == cels.end()) return false;
    const float r2 = radius * radius, near_sq = r2 * 1.3f;
    for (int c : cels) {
        for (BlinkRef ref : cels_[std::size_t(c)].blinks) {
            const Bounds& b = bounds_[ref.bounds];
            const Vec3& pa = b.nodes[b.links[ref.link].a].pos;
            const Vec3& pb = b.nodes[b.links[ref.link].b].pos;
            for (const Vec3* post : {&pa, &pb}) {
                float t = r2 * 0.5f;
                if (sqdist2d(*post, to.pos) >= near_sq && sqdist2d(*post, from.pos) >= near_sq) t = r2;
                Vec3 cp = closest_on_segment(from.pos, to.pos, *post);
                // both posts are compared against node A's y in the original
                if (sqdist2d(cp, *post) < t && std::fabs(cp[1] - pa[1]) < kNodeYTol) return false;
            }
        }
    }
    return true;
}

bool NavNetwork::furthest_position(const Vec3& from, const Vec3& to, Vec3& out) const {
    // AINetwork_FurthestPosition @0x15a8d8: nearest crossing over all boundaries.
    out = to;
    float nearest = std::numeric_limits<float>::max();
    bool any = false;
    for (std::size_t bi = 0; bi < bounds_.size(); ++bi)
        for (std::size_t li = 0; li < bounds_[bi].links.size(); ++li) {
            LineCross x = cross_boundary({std::uint16_t(bi), std::uint16_t(li)}, from, to);
            if (!x.hit) continue;
            float d = length(x.point - from);
            if (d < nearest) {
                nearest = d;
                out = x.point;
                any = true;
            }
        }
    return any;
}

bool NavNetwork::bounds_push_vector(float radius, const CelPos& pos, Vec3& push, std::uint32_t mask) const {
    // AINetwork_GetBoundsPushVectorForSphere @0x157bd0 with AINetwork_HitBoundry @0x157780.
    push = {0, 0, 0};
    if (pos.cel == kNoCel) return false;
    Vec3 p = pos.pos;
    bool any = false;
    for (BlinkRef ref : cels_[std::size_t(pos.cel)].blinks) {
        const Bounds& b = bounds_[ref.bounds];
        const BoundsLink& l = b.links[ref.link];
        float r = radius;
        if (!(mask & kBlinkDoor) && (l.flags & kBlinkDoor)) r = radius + radius;
        if (l.flags & mask) continue;
        const BoundsNode& na = b.nodes[l.a];
        const BoundsNode& nb = b.nodes[l.b];
        float t = 0;
        Vec3 cp = closest_on_segment(na.pos, nb.pos, p, &t);
        float d2 = sqdist2d(p, cp);
        if (d2 > r * r) continue;
        float top = cp[1] + (na.height + (nb.height - na.height) * t);
        if (p[1] > top || cp[1] - 0.4f > p[1]) continue;
        Vec3 dir = p - cp;
        dir[1] = 0;
        float len = length(dir);
        if (len > 0) dir = dir * (1.0f / len);
        p = p + dir * (r - len);
        any = true;
    }
    push = p - pos.pos;
    return any;
}

int NavNetwork::mod_intersected_boundaries(const Vec3& a, const Vec3& b, std::uint32_t set, std::uint32_t skip) {
    // AINetwork_ModIntersectedLinkFlags_Bounds @0x15b6f0.
    int n = 0;
    for (std::size_t bi = 0; bi < bounds_.size(); ++bi)
        for (std::size_t li = 0; li < bounds_[bi].links.size(); ++li) {
            BoundsLink& l = bounds_[bi].links[li];
            if (l.flags & skip) continue;
            if (cross_boundary({std::uint16_t(bi), std::uint16_t(li)}, a, b).hit) {
                l.flags |= set;
                ++n;
            }
        }
    return n;
}

void NavNetwork::init_passable_boundaries() {
    // AINetwork_InitPassableBoundries @0x15a5c8: every nav link (of every AIPath) marks the boundary
    // segments it crosses passable; crossing high above the boundary floor clips its height.
    for (const NavPath& p : paths_)
        for (const NavLink& l : p.links)
            for (std::size_t bi = 0; bi < bounds_.size(); ++bi)
                for (std::size_t li = 0; li < bounds_[bi].links.size(); ++li) {
                    BoundsLink& bl = bounds_[bi].links[li];
                    if ((bl.flags & 0x1800) != 0 || (bl.flags & kBlinkPassable) == kBlinkPassable) continue;
                    if (line_intersects_boundary({std::uint16_t(bi), std::uint16_t(li)}, p.nodes[l.a].pos,
                                                 p.nodes[l.b].pos, nullptr, nullptr, true))
                        bl.flags |= kBlinkPassable;
                }
}

// ---- MoveTest family ----------------------------------------------------------------------------

int NavNetwork::move_test(const CelPos& from, const CelPos& to, Vec3* hit, bool corner_test) const {
    // NDrone2_MoveTest @0x155330 (and the raw/node overloads: identical logic).
    Vec3 d = to.pos - from.pos;
    if (dot(d, d) > limits_.max_dist_sq) return 0;
    float ady = std::fabs(from.pos[1] - to.pos[1]);
    if (ady > limits_.max_dy) return 0;
    if (ady > limits_.slope_dy) {
        float angle = std::fabs(std::atan2(d[1], std::sqrt(d[0] * d[0] + d[2] * d[2])));
        if (angle > limits_.max_slope) return 0;
    }
    if (bounds_.empty()) return 1;
    return bounds_test_i(from, to, hit, corner_test);
}

bool NavNetwork::move_ok(const Vec3& from, const Vec3& to) const { return move_test(locate(from), locate(to)) == 1; }

int NavNetwork::move_test_from_node(NodeRef node, const CelPos& to, Vec3* hit, bool corner_test) const {
    const NavNode& n = paths_[node.path].nodes[node.node];
    return move_test({n.pos, n.cel}, to, hit, corner_test);
}

int NavNetwork::move_test_to_node(const CelPos& from, NodeRef node, Vec3* hit, bool corner_test) const {
    const NavNode& n = paths_[node.path].nodes[node.node];
    return move_test(from, {n.pos, n.cel}, hit, corner_test);
}

// ---- node search / goal marking -----------------------------------------------------------------

namespace {
// Sorted singly linked open list semantics: insert after every entry with f <= new f.
template <class Nodes>
void open_insert(std::list<std::uint16_t>& open, Nodes& nodes, std::uint16_t id) {
    auto it = open.begin();
    while (it != open.end() && nodes[*it].f <= nodes[id].f) ++it;
    open.insert(it, id);
}
}  // namespace

bool NavNetwork::node_search(NodeSearch& s) {
    // AINetwork_NodeSearchCel @0x156e18, driven by AINetwork_NodeSearch: the query's own cel first, then the
    // neighbour cels (portals) whose vertical span overlaps the query (y +/- 2), until something is found.
    if (s.query.cel == kNoCel || s.path < 0) return false;
    NavPath& path = paths_[std::size_t(s.path)];
    const float half = s.radius_sq != 0.0f ? std::sqrt(s.radius_sq) : kDefaultBox;
    const Vec3& q = s.query.pos;
    auto search_cel = [&](int cel) {
        std::vector<std::uint16_t> cand;
        for (NodeRef ref : cels_[std::size_t(cel)].nodes) {
            if (ref.path != path.index) continue;
            NavNode& n = path.nodes[ref.node];
            if (n.pos[0] < q[0] - half || n.pos[0] > q[0] + half || n.pos[2] < q[2] - half || n.pos[2] > q[2] + half ||
                n.pos[1] < q[1] - kNodeYTol || n.pos[1] > q[1] + kNodeYTol)
                continue;
            n.dist2d = dist2d(n.pos, q);
            if (s.radius_sq > 0 && n.dist2d > s.radius_sq) continue;   // compares distance with radius^2 (original)
            auto it = cand.begin();
            while (it != cand.end() && path.nodes[*it].dist2d <= n.dist2d) ++it;
            cand.insert(it, ref.node);
        }
        for (std::uint16_t id : cand) {
            NavNode& n = path.nodes[id];
            NodeRef ref{std::uint16_t(path.index), id};
            if (!s.seed_mode) {
                if (move_test_from_node(ref, s.query) == 1) {
                    n.flags |= s.mask;
                    ++s.found;
                    return;
                }
                if (s.fallback == 0) {
                    n.flags |= s.mask << 1;
                    s.fallback = 1;
                }
            } else {
                if (move_test_to_node(s.query, ref) == 1) {
                    n.g = n.dist2d;
                    n.h = s.goal ? dist2d(s.goal->pos, n.pos) : 0.0f;
                    n.f = n.g + n.h;
                    n.parent = kNoNode;
                    s.result = id;
                    if (s.add_open) n.flags |= nodeflag::kOpenClosed;
                    ++s.found;
                    return;
                }
            }
        }
    };
    search_cel(s.query.cel);
    if (s.found == 0) {
        for (int pi : cel_portals_[std::size_t(s.query.cel)]) {
            const Portal& p = portals_[std::size_t(pi)];
            const int neighbour = p.cel_a == s.query.cel ? p.cel_b : p.cel_a;
            float lo = p.quad[0][1], hi = p.quad[0][1];
            for (const Vec3& v : p.quad) {
                lo = std::min(lo, v[1]);
                hi = std::max(hi, v[1]);
            }
            if (q[1] < lo - kNodeYTol || q[1] > hi + kNodeYTol) continue;
            search_cel(neighbour);
            if (s.found != 0) break;
        }
    }
    return true;
}

std::uint16_t NavNetwork::nearest_node(const CelPos& pos, int path) {
    // NDrone2_NearestNode @0x1558d8.
    NodeSearch s;
    s.query = pos;
    s.path = path;
    s.seed_mode = true;
    s.radius_sq = kSeedRadiusSq;
    node_search(s);
    return s.found < 1 ? kNoNode : s.result;
}

int NavNetwork::nav_path_for_position(const CelPos& pos, bool move_test_flag) const {
    // AINetwork_NavPathForPosition @0x158488.
    if (pos.cel == kNoCel) return -1;
    int best_path = -1;
    float best = std::numeric_limits<float>::max();
    for (NodeRef ref : cels_[std::size_t(pos.cel)].nodes) {
        const NavPath& p = paths_[ref.path];
        if (!p.plain()) continue;
        const NavNode& n = p.nodes[ref.node];
        float d = dist2d(pos.pos, n.pos);
        if (d < best && (!move_test_flag || move_test(pos, {n.pos, n.cel}) == 1)) {
            best = d;
            best_path = p.index;
        }
    }
    if (best_path >= 0) return best_path;
    for (const NavPath& p : paths_) {
        if (!p.plain()) continue;
        for (const NavNode& n : p.nodes) {
            float d = dist2d(pos.pos, n.pos);
            if (d < best) {
                best = d;
                best_path = p.index;
            }
        }
    }
    return best_path;
}

void NavNetwork::nodes_for_position(float radius, NavTarget& t) {
    // AINetwork_NodesForPosition @0x1586a8.
    t.prev = t.goal;   // amemcpy(+0x30, +0x10, 0x20) happens before the goal is overwritten by the callers
    t.direct = t.fallback = 0;
    const float r2 = radius * radius;
    for (NavPath& p : paths_) {
        if (!p.plain()) continue;
        for (NavNode& n : p.nodes) n.flags &= ~(t.mask | (t.mask << 1));
        NodeSearch s;
        s.query = t.goal;
        s.path = p.index;
        s.mask = t.mask;
        s.radius_sq = r2;
        node_search(s);
        t.direct = std::int16_t(t.direct + s.found);
        t.fallback = std::int16_t(t.fallback + s.fallback);
        if (s.found) p.target_marks |= t.mask; else p.target_marks &= ~t.mask;
        if (s.fallback) p.target_marks |= t.mask << 1; else p.target_marks &= ~(t.mask << 1);
    }
    t.stamp = frame_ != 0 ? frame_ : 1;   // 0 means "never built"
}

void NavNetwork::build_target(NavTarget& t) {
    // AINetwork_BuildAITarget @0x15ac78.
    if (t.stamp == 0 || length(t.prev.pos - t.goal.pos) >= 2.0f) nodes_for_position(0, t);
}

void NavNetwork::update_player_target(const Vec3& pos) {
    // NDrone2_NavNodeCache @0x155cd8: mask 0x1000000, goal = player feet, BuildAITarget.
    player_target_.mask = NavTarget::kPlayerMask;
    player_target_.object = true;
    player_target_.goal = locate(pos);
    build_target(player_target_);
}

// ---- A* / emitters ------------------------------------------------------------------------------

bool NavNetwork::edge_blocked(const NavPath& p, std::uint16_t from, std::uint16_t to) const {
    // Both ends door nodes and the door bound to the node being expanded is locked.
    const NavNode& a = p.nodes[from];
    const NavNode& b = p.nodes[to];
    if (a.object < 0 || !(a.flags & nodeflag::kDoor) || !(b.flags & nodeflag::kDoor)) return false;
    return door_locked_ && door_locked_(a.object);
}

RouteStatus NavNetwork::do_astar(NavRoute& route, const NavTarget& target) {
    // AINetwork_DoAStarPath @0x1571b0.
    NavPath& path = paths_[std::size_t(route.path)];
    for (NavNode& n : path.nodes) {
        n.flags &= ~nodeflag::kScratchMask;
        n.g = 0;
    }
    if (target.stamp == 0) return RouteStatus::NoTargetNodes;
    std::uint32_t mark;
    RouteStatus cls;
    if (target.direct == 0) {
        if (target.fallback == 0) return RouteStatus::NoTargetNodes;
        mark = target.mask << 1;
        cls = RouteStatus::Approximate;
    } else if (path.target_marks & target.mask) {
        mark = target.mask;
        cls = RouteStatus::Following;
    } else {
        if (target.fallback == 0) return RouteStatus::NoTargetNodes;
        mark = target.mask << 1;
        cls = RouteStatus::Approximate;
    }
    NodeSearch seed;
    seed.query = route.start;
    seed.goal = &route.goal;
    seed.path = path.index;
    seed.seed_mode = true;
    seed.add_open = true;
    seed.radius_sq = kSeedRadiusSq;
    node_search(seed);
    if (seed.found < 1) return RouteStatus::NoStartNode;

    std::list<std::uint16_t> open{seed.result};
    while (!open.empty()) {
        std::uint16_t cur = open.front();
        open.pop_front();
        NavNode& cn = path.nodes[cur];
        cn.flags &= ~nodeflag::kOpenClosed;
        if (cn.flags & mark) {
            route.dest_node = cur;
            route.distance = cn.g + dist2d(cn.pos, route.goal.pos);
            std::vector<std::uint16_t> chain;
            for (std::uint16_t n = cur; n != kNoNode; n = path.nodes[n].parent) chain.push_back(n);
            route.prev_first = route.first_node;
            route.first_node = chain.back();
            route.nodes.assign(chain.rbegin(), chain.rend());
            route.index = 0;
            return cls;
        }
        for (unsigned k = 0; k < cn.adj_count; ++k) {
            std::uint16_t li = path.adjacency[cn.adj_first + k];
            const NavLink& link = path.links[li];
            std::uint16_t nid = path.other_end(li, cur);
            if (edge_blocked(path, cur, nid)) continue;
            NavNode& nb = path.nodes[nid];
            float new_g = cn.g + link.length * float(link.used + 1);
            if ((nb.flags & nodeflag::kScratchMask) == 0 || new_g < nb.g) {
                nb.parent = cur;
                nb.g = new_g;
                nb.h = dist2d(nb.pos, route.goal.pos);
                nb.f = nb.g + nb.h;
                nb.arrival = std::uint8_t(k);
                if (!(nb.flags & nodeflag::kOpenClosed)) {
                    open_insert(open, path.nodes, nid);
                    nb.flags |= nodeflag::kOpenClosed;
                }
            }
        }
        cn.flags |= nodeflag::kOpenClosed;   // pushed onto the closed chain
    }
    return RouteStatus::Exhausted;
}

// ---- emitters -----------------------------------------------------------------------------------

bool NavNetwork::init_emitter(NavEmitter& e, const Vec3& pos, int cel, int path, bool reuse) {
    // AINetwork_InitEmitter / InitEmitter2 @0x15b1e8 / 0x15b2b8.
    if (cel == kNoCel) cel = find_cel(pos);
    if (cel == kNoCel) return false;
    e.pos = pos;
    e.cel = cel;
    e.path = path >= 0 ? path : nav_path_for_position({pos, cel}, false);
    if (e.path < 0 || paths_[std::size_t(e.path)].nodes.empty()) return false;
    if (!reuse || !e.allocated) {
        e.table.assign(paths_[std::size_t(e.path)].nodes.size(), 0xff);
        e.allocated = true;
    }
    return true;
}

bool NavNetwork::emit_path(NavEmitter& e, float range) {
    // AINetwork_EmitPath @0x158cf8: Dijkstra flood with the A* open-list quirks, h = 0.
    if (e.path < 0 || !e.allocated) return false;
    NavPath& path = paths_[std::size_t(e.path)];
    if (range == 0.0f) range = 255.0f;
    for (NavNode& n : path.nodes) {
        n.flags &= ~nodeflag::kScratchMask;
        n.g = 255.0f;
    }
    std::fill(e.table.begin(), e.table.end(), std::uint8_t(0xff));
    NodeSearch seed;
    seed.query = {e.pos, e.cel};
    seed.path = path.index;
    seed.seed_mode = true;
    seed.add_open = true;
    seed.radius_sq = kSeedRadiusSq;
    node_search(seed);
    if (seed.found < 1) return false;
    e.seed = seed.result;
    std::list<std::uint16_t> open{seed.result};
    while (!open.empty()) {
        std::uint16_t cur = open.front();
        open.pop_front();
        NavNode& cn = path.nodes[cur];
        cn.flags &= ~nodeflag::kOpenClosed;
        for (unsigned k = 0; k < cn.adj_count; ++k) {
            std::uint16_t li = path.adjacency[cn.adj_first + k];
            std::uint16_t nid = path.other_end(li, cur);
            if (edge_blocked(path, cur, nid)) continue;
            NavNode& nb = path.nodes[nid];
            float new_g = cn.g + path.links[li].length * float(path.links[li].used + 1);
            if (new_g > range) continue;
            if ((nb.flags & nodeflag::kScratchMask) != 0 && nb.g <= new_g) continue;
            nb.f = new_g;
            nb.parent = cur;
            nb.g = new_g;
            nb.h = 0;
            nb.arrival = std::uint8_t(k);
            if (!(nb.flags & nodeflag::kOpenClosed)) {
                open_insert(open, path.nodes, nid);
                nb.flags |= nodeflag::kOpenClosed;
            }
        }
        cn.flags |= nodeflag::kOpenClosed;
    }
    for (std::size_t i = 0; i < path.nodes.size(); ++i) {
        float g = path.nodes[i].g;
        if (g == 255.0f) continue;
        auto v = std::uint32_t(std::fabs(g * 0.00390625f) * 255.0f);
        e.table[i] = std::uint8_t(std::min<std::uint32_t>(v, 0xfe));
    }
    return true;
}

bool NavNetwork::distance_to_emitter(const CelPos& pos, int path, std::uint16_t& cached_node, const NavEmitter& e,
                                     float* out) {
    // NDrone2_DistanceToEmitter @0x155798.
    if (out) *out = 255.0f;
    if (e.path != path || e.table.empty() || path < 0) return false;
    if (cached_node == kNoNode) cached_node = nearest_node(pos, path);
    std::uint8_t d = e.at(cached_node);
    if (out) *out = float(d);
    return d != 0xff;
}

std::optional<NodeRef> NavNetwork::emitter_node_at_distance(std::uint8_t min_dist, int start_node, const NavEmitter& e) {
    // AINetwork_Emitter_GetNodeAtDistance @0x159268 ("flee" search). Breadth search from the start
    // node (argument, else the emitter's seed node, else its nearest node) over an open list sorted by
    // the nodes' stale A*/emit `f` values (the original never sets f here); it returns the first
    // neighbour, in expansion order, whose table byte is >= min_dist (0xff counts).
    if (e.path < 0 || e.table.empty()) return std::nullopt;
    NavPath& path = paths_[std::size_t(e.path)];
    int start = start_node;
    if (start < 0) start = e.seed == kNoNode ? -1 : int(e.seed);
    if (start < 0) {
        std::uint16_t n = nearest_node({e.pos, e.cel}, e.path);
        if (n == kNoNode) return std::nullopt;
        start = n;
    }
    if (start >= int(path.nodes.size())) return std::nullopt;
    for (NavNode& n : path.nodes) n.flags &= ~nodeflag::kScratchMask;
    std::list<std::uint16_t> open{std::uint16_t(start)};
    path.nodes[std::size_t(start)].flags |= nodeflag::kOpenClosed;
    while (!open.empty()) {
        std::uint16_t cur = open.front();
        open.pop_front();
        NavNode& cn = path.nodes[cur];
        cn.flags &= ~nodeflag::kOpenClosed;
        for (unsigned k = 0; k < cn.adj_count; ++k) {
            std::uint16_t nid = path.other_end(path.adjacency[cn.adj_first + k], cur);
            if (edge_blocked(path, cur, nid)) continue;
            NavNode& nb = path.nodes[nid];
            if (e.at(nid) >= min_dist) return NodeRef{std::uint16_t(path.index), nid};
            if (!(nb.flags & nodeflag::kOpenClosed)) {
                open_insert(open, path.nodes, nid);
                nb.flags |= nodeflag::kOpenClosed;
            }
        }
        cn.flags |= nodeflag::kOpenClosed;
    }
    return std::nullopt;
}

int NavNetwork::set_link_flags_in_circle(float radius, const CelPos& pos, std::uint16_t set, std::uint16_t clear) {
    // AINetwork_SetLinksFlagInCircle @0x15b548: over every path's links (no path filter): clear `clear`,
    // then set `set` when the link's height range is within 2.0 of pos.y and its closest point to pos
    // is within `radius` (2-D).
    int n = 0;
    for (NavPath& p : paths_)
        for (NavLink& l : p.links) {
            l.flags &= std::uint16_t(~clear);
            const Vec3& a = p.nodes[l.a].pos;
            const Vec3& b = p.nodes[l.b].pos;
            if (std::min(a[1], b[1]) > pos.pos[1] + 2.0f || pos.pos[1] - 2.0f > std::max(a[1], b[1])) continue;
            if (sqdist2d(closest_on_segment(a, b, pos.pos), pos.pos) <= radius * radius) {
                l.flags |= set;
                ++n;
            }
        }
    return n;
}

void NavNetwork::bind_hook_nodes(const std::vector<HookObject>& objs, std::uint32_t node_flag, std::uint16_t link_flag,
                                 std::uint32_t boundary_flag, bool clear_missing) {
    for (NavPath& p : paths_) {
        for (std::size_t i = 0; i < p.nodes.size(); ++i) {
            NavNode& n = p.nodes[i];
            if (clear_missing) n.object = -1;
            if (!(n.flags & node_flag)) continue;
            const HookObject* best = nullptr;
            float best_d = 0;
            for (const HookObject& o : objs) {
                float d = dot(o.pos - n.pos, o.pos - n.pos);
                if (!best || d < best_d) best = &o, best_d = d;
            }
            if (best) n.object = best->id;
            else if (clear_missing) n.flags &= ~node_flag;
            for (unsigned k = 0; k < n.adj_count; ++k) {
                NavLink& l = p.links[p.adjacency[n.adj_first + k]];
                if (l.a != i) continue;   // the original walks links whose A end is this node
                const NavNode& other = p.nodes[l.b];
                if (!(other.flags & node_flag)) continue;
                if (mod_intersected_boundaries(n.pos, other.pos, boundary_flag, 0) != 0) l.flags |= link_flag;
            }
        }
    }
}

void NavNetwork::bind_door_nodes(const std::vector<HookObject>& doors) {
    bind_hook_nodes(doors, nodeflag::kDoor, 0x20, kBlinkDoor, true);
}

void NavNetwork::bind_kick_nodes(const std::vector<HookObject>& kicks) {
    bind_hook_nodes(kicks, nodeflag::kKick, 0x40, kBlinkKick, false);
}

}  // namespace nf

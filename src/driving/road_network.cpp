#include "driving/road_network.hpp"

#include <cmath>
#include <queue>
namespace nf::driving {
namespace {
// Lane segments usable for bridging/routing (real street spans). The `rs` tag also holds
// parameter blocks of the form a=(k,k,z) (small matching ints): lane-count headers, not
// geometry. True segment endpoints are float world coords.
struct Lane {
    Vec3 a, b;
};
bool lane_ok(const Vec3& a, const Vec3& b) {
    if (a[0] == a[1] && a[0] >= 0 && a[0] <= 8 && a[0] == std::floor(a[0])) return false;
    // Short pieces (intersections split lanes into 1-5 m fragments) still chain; only
    // padding (zero length) is cut. Long straights are single records (race lines).
    const float L = length(b - a);
    return L > 0.5f && L < 300.0f;
}
float flat_dist(const Vec3& p, const Vec3& q) {
    const Vec3 d = p - q;
    return std::sqrt(d[0] * d[0] + d[2] * d[2]);
}
}  // namespace


RoadNetwork RoadNetwork::build_route(const std::vector<Vec3>& points) {
    RoadNetwork net;
    for (std::size_t i = 0; i < points.size(); ++i) {
        Node n;
        n.pos = points[i];
        if (i + 1 < points.size()) {
            const Vec3 d = points[i + 1] - points[i];
            n.length = length(d);
            n.dir = n.length > 1e-6f ? d * (1.0f / n.length) : Vec3{0, 0, 1};
            n.next = static_cast<int>(i + 1);
        } else {
            n.dir = net.nodes_.empty() ? Vec3{0, 0, 1} : net.nodes_.back().dir;
        }
        n.width = 8.0f;
        net.nodes_.push_back(n);
    }
    return net;
}

RoadNetwork RoadNetwork::build(const std::vector<RoadSeg>& segs) {
    RoadNetwork net;
    for (const RoadSeg& s : segs) {
        Node n;
        n.pos = s.a;
        const Vec3 d = s.b - s.a;
        n.length = length(d);
        n.dir = n.length > 1e-6f ? d * (1.0f / n.length) : Vec3{0, 0, 1};
        n.width = s.width > 1.0f && s.width < 60.0f ? s.width : 8.0f;
        net.nodes_.push_back(n);
    }
    // Chain end-to-start: successor = segment whose start is closest to this end, within a gap
    // tolerance scaled by segment length (junctions share endpoints exactly in the data).
    for (std::size_t i = 0; i < net.nodes_.size(); ++i) {
        const Vec3 end = segs[i].b;
        float best = 25.0f;
        int pick = -1;
        for (std::size_t j = 0; j < net.nodes_.size(); ++j) {
            if (i == j) continue;
            const float d = length(net.nodes_[j].pos - end);
            if (d >= best) continue;
            // Prefer continuations (don't U-turn): the angle test happens in successor(); the
            // static chain keeps the best positional match so dead ends stay detectable.
            best = d;
            pick = static_cast<int>(j);
        }
        net.nodes_[i].next = pick;
    }
    return net;
}

int RoadNetwork::nearest(const Vec3& p) const {
    float best = 1e30f;
    int pick = -1;
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        const Vec3 d = nodes_[i].pos - p;
        const float q = dot(d, d);
        if (q < best) best = q, pick = static_cast<int>(i);
    }
    return pick;
}
int RoadNetwork::successor(int i, const Vec3& dir) const {
    if (i < 0 || std::size_t(i) >= nodes_.size()) return -1;
    const int chained = nodes_[std::size_t(i)].next;
    // The precomputed link wins when it is near and continues the heading (route-built
    // networks chain i -> i+1; the distance cap keeps index-order gaps from teleporting).
    if (chained >= 0 && nodes_[std::size_t(i)].length < 120.0f &&
        dot(nodes_[std::size_t(chained)].dir, dir) > 0.5f)
        return chained;
    float best = -2.0f;
    // Near-chained fallback keeps continuity through sharp turns; across index-order gaps
    // there is no local continuation, so report none (-1) instead of teleporting.
    int pick = (chained >= 0 && nodes_[std::size_t(i)].length < 120.0f) ? chained : -1;
    // Capped projection: on index-order gaps the raw segment end can be kilometres away;
    const Vec3 end = nodes_[std::size_t(i)].pos +
                     nodes_[std::size_t(i)].dir * std::min(nodes_[std::size_t(i)].length, 40.0f);
    for (std::size_t j = 0; j < nodes_.size(); ++j) {
        if (int(j) == i) continue;  // never succeed yourself
        if (length(nodes_[j].pos - end) > 25.0f) continue;
        const float q = dot(nodes_[j].dir, dir);
        if (q > best) best = q, pick = static_cast<int>(j);
    }
    return pick;
}
float RoadNetwork::progress(int i, const Vec3& p) const {
    if (i < 0 || std::size_t(i) >= nodes_.size() || nodes_[std::size_t(i)].length <= 0) return 0;
    const Node& n = nodes_[std::size_t(i)];
    return dot(p - n.pos, n.dir) / n.length;
}

std::vector<int> RoadNetwork::chain_from(int start, std::size_t max_len) const {
    std::vector<int> chain;
    if (start < 0 || std::size_t(start) >= nodes_.size()) return chain;
    std::vector<char> seen(nodes_.size(), 0);
    int n = start;
    while (n >= 0 && chain.size() < max_len) {
        chain.push_back(n);
        seen[std::size_t(n)] = 1;
        int nx = successor(n, nodes_[std::size_t(n)].dir);
        if (nx < 0 || seen[std::size_t(nx)]) {
            // Dead end or loop: jump to the nearest unvisited node roughly ahead.
            const Vec3 end = nodes_[std::size_t(n)].pos + nodes_[std::size_t(n)].dir * nodes_[std::size_t(n)].length;
            const Vec3 dir = nodes_[std::size_t(n)].dir;
            float best = 150.0f;
            nx = -1;
            for (std::size_t j = 0; j < nodes_.size(); ++j) {
                if (seen[j]) continue;
                const Vec3 d = nodes_[j].pos - end;
                const float dist = length(d);
                if (dist >= best || dot(d, dir) < -20.0f) continue;
                if (dot(nodes_[j].dir, dir) < 0.3f) continue;
                best = dist;
                nx = static_cast<int>(j);
            }
        }
        n = nx;
    }
    return chain;
}


std::vector<int> RoadNetwork::walk_from(int start, const std::vector<RoadSeg>& lanes_in) const {
    // Mission-route walk: follow the road with heading coherence (prefer the candidate most
    // ahead), bridging gaps up to 400 m (sparse submarine-level waypoints need it; in dense
    // grids nearer aligned candidates always win the score below). Always terminates (unvisited only).
    // Dead-end spur refusal (needs `lanes`; skipped when empty): a candidate with no graded
    // road beyond it in the travel direction ends the mission right there (cul-de-sac) or is
    // a wrong turn off it. Take it only when nothing live remains (two passes).
    std::vector<Lane> lanes;
    for (const RoadSeg& s : lanes_in)
        if (lane_ok(s.a, s.b)) lanes.push_back({s.a, s.b});
    auto terminus = [&](const Vec3& here, const Vec3& cand) {
        // Needs a real lane graph to mean anything (a handful of fragments cannot judge
        // continuity; treating everything as terminus just adds noise).
        if (lanes.size() < 20) return false;
        Vec3 d = cand - here;
        const float L = length(d);
        if (L < 1.0f) return false;
        const Vec3 beyond = cand + d * (30.0f / L);
        for (const Lane& l : lanes) {
            if (length(l.a - beyond) < 25.0f || length(l.b - beyond) < 25.0f) return false;
        }
        return true;
    };
    std::vector<int> walk;
    if (start < 0 || std::size_t(start) >= nodes_.size()) return walk;
    std::vector<char> seen(nodes_.size(), 0);
    int n = start;
    Vec3 heading = nodes_[std::size_t(start)].dir;
    while (n >= 0 && walk.size() < nodes_.size()) {
        walk.push_back(n);
        seen[std::size_t(n)] = 1;
        const Vec3 here = nodes_[std::size_t(n)].pos;
        // Chained successor wins unless it terminates the mission here (no graded road
        // beyond it in this travel direction): a spur off the live road, not the route.
        int nx = successor(n, heading);
        if (nx >= 0 && (seen[std::size_t(nx)] || terminus(here, nodes_[std::size_t(nx)].pos))) nx = -1;
        if (nx < 0) {
            // Two passes: live candidates first (heading-coherent nearest), terminus ones
            // only when nothing live remains (the line truly ends here).
            for (int pass = 0; pass < 2 && nx < 0; ++pass) {
                float best_score = 1e30f;
                for (std::size_t j = 0; j < nodes_.size(); ++j) {
                    if (seen[j]) continue;
                    const Vec3 d = nodes_[j].pos - here;
                    const float dist = length(d);
                    if (dist > 400.0f) continue;
                    if (pass == 0 && terminus(here, nodes_[j].pos)) continue;
                    const float align = dist > 1.0f ? dot(d * (1.0f / dist), heading) : 1.0f;
                    const float score = dist + (1.0f - align) * 100.0f;
                    if (score < best_score) best_score = score, nx = static_cast<int>(j);
                }
            }
        }
        if (nx >= 0) {
            const Vec3 d = nodes_[std::size_t(nx)].pos - here;
            if (length(d) > 1.0f) heading = d * (1.0f / length(d));
        }
        n = nx;
    }
    // Strip out-and-back spikes (parallel-lane ping-pong where index order interleaves
    // carriageways): a middle node whose neighbours nearly meet behind it is a detour,
    // not a corner. Offset returns (hairpins, switchbacks) survive the ratio test.
    for (bool cut = true; cut;) {
        cut = false;
        for (std::size_t k = 1; k + 1 < walk.size(); ++k) {
            const Vec3 a = nodes_[std::size_t(walk[k - 1])].pos;
            const Vec3 b = nodes_[std::size_t(walk[k])].pos;
            const Vec3 c = nodes_[std::size_t(walk[k + 1])].pos;
            const float lab = length(b - a), lbc = length(c - b), lac = length(c - a);
            if (lab < 1.0f || lbc < 1.0f) {
                walk.erase(walk.begin() + long(k));
                cut = true;
                break;
            }
            const float backtrack = dot((b - a) * (1.0f / lab), (c - b) * (1.0f / lbc));
            if (backtrack < -0.5f && lac < 0.55f * std::min(lab, lbc)) {
                walk.erase(walk.begin() + long(k));
                cut = true;
                break;
            }
        }
    }
    return walk;
}
namespace {
// BFS lane path from `from` to near `to` (end-to-start chaining, 25 m tolerance, bidirectional
// travel). Returns the far endpoints in order, excluding `from`. Empty when unroutable or
// when the detour exceeds 3x the direct distance (wrong side of the map, not a corner).
std::vector<Vec3> lane_bridge(const std::vector<Lane>& lanes, const Vec3& from, const Vec3& to) {
    const float direct = flat_dist(from, to);
    if (direct <= 0 || lanes.empty()) return {};
    struct Visit {
        int prev = -2;
        bool enter_a = true;  // entered this lane at its `a` end (exit at `b`)
        float cost = 1e30f;
    };
    std::vector<Visit> vis(lanes.size());
    // Multi-seed starts: the single nearest lane may sit on an isolated fragment (short
    // intersection pieces), while a slightly farther lane connects through. Seed the three
    // nearest by endpoint distance.
    struct Seed { float d; int i; };
    Seed seeds[3] = {{1e30f, -1}, {1e30f, -1}, {1e30f, -1}};
    for (std::size_t i = 0; i < lanes.size(); ++i) {
        const float d =
            std::min(flat_dist(lanes[i].a, from), flat_dist(lanes[i].b, from));
        for (Seed& s : seeds) {
            if (d < s.d) {
                s.d = d;
                s.i = int(i);
                break;
            }
        }
    }
    if (seeds[0].i < 0 || seeds[0].d > 40.0f) return {};
    auto exit_of = [&](int i) {
        return vis[std::size_t(i)].enter_a ? lanes[std::size_t(i)].b : lanes[std::size_t(i)].a;
    };
    // A* over lanes (goal-directed: plain BFS wanders down parallel streets instead of
    // taking the corner). Each lane is settled once, so no stale queue entries.
    using PQ = std::priority_queue<std::pair<float, int>>;
    PQ q;
    for (const Seed& s : seeds) {
        if (s.i < 0 || s.d > 40.0f) continue;
        vis[std::size_t(s.i)] = {-1, flat_dist(lanes[std::size_t(s.i)].a, from) <
                                         flat_dist(lanes[std::size_t(s.i)].b, from),
                                 0};
        q.emplace(-flat_dist(exit_of(s.i), to), s.i);
    }
    // Tight budget: a bridge much longer than the direct jump is a wrong-way detour that
    // rediscovers the walk's own future (it passes the progress filter by metres while
    // wasting minutes and poisoning the corner speed with its kinks). Real corners are
    // barely longer than direct.
    const float budget = 1.5f * direct + 60.0f;
    int found = -1;
    std::size_t expansions = 0;
    while (!q.empty() && expansions < 20000) {
        ++expansions;
        const int i = q.top().second;
        q.pop();
        const Vec3 exit = exit_of(i);
        if (flat_dist(exit, to) < 30.0f) {
            found = i;
            break;
        }
        if (vis[std::size_t(i)].cost > budget) continue;
        for (std::size_t j = 0; j < lanes.size(); ++j) {
            if (vis[j].prev != -2) continue;
            const float la = flat_dist(lanes[j].a, exit);
            const float lb = flat_dist(lanes[j].b, exit);
            const bool use_a = la < lb;
            if ((use_a ? la : lb) > 25.0f) continue;
            const float nc = vis[std::size_t(i)].cost + (use_a ? la : lb) + length(lanes[j].b - lanes[j].a);
            if (nc > budget) continue;
            vis[j] = {i, use_a, nc};
            const Vec3 j_exit = use_a ? lanes[j].b : lanes[j].a;
            q.emplace(-(nc + flat_dist(j_exit, to)), int(j));
        }
    }
    if (found < 0) return {};
    std::vector<int> rev;
    for (int i = found; i >= 0; i = vis[std::size_t(i)].prev) rev.push_back(i);
    std::vector<Vec3> out;
    for (std::size_t k = rev.size(); k-- > 0;) out.push_back(exit_of(rev[k]));
    return out;
}
}  // namespace


RoadNetwork RoadNetwork::splice_jumps(const RoadNetwork& base, const std::vector<int>& walk,
                                      const std::vector<RoadSeg>& lanes_in) {
    std::vector<Lane> lanes;
    for (const RoadSeg& s : lanes_in)
        if (lane_ok(s.a, s.b)) lanes.push_back({s.a, s.b});
    std::vector<Vec3> pts;
    for (const int i : walk) pts.push_back(base.node(std::size_t(i)).pos);
    std::vector<Vec3> out;
    for (std::size_t k = 0; k < pts.size(); ++k) {
        if (k > 0 && length(pts[k] - pts[k - 1]) > 60.0f && !lanes.empty()) {
            // Only keep bridge points that make progress: a lane that leaves `from` the
            // wrong way starts the bridge with a hook back over ground already covered
            // (an out-and-back spike the strip pass never sees, post-splice).
            const float from_to = length(pts[k] - pts[k - 1]);
            for (const Vec3& p : lane_bridge(lanes, pts[k - 1], pts[k])) {
                if (length(p - pts[k]) > from_to + 10.0f) continue;
                // Exact-duplicate guard only: bridge lanes run dense (1-5 m pieces) and the
                // density is the point (smooth corner following).
                if (out.empty() || length(p - out.back()) > 0.5f) out.push_back(p);
            }
        }
        if (out.empty() || length(pts[k] - out.back()) > 0.5f) out.push_back(pts[k]);
    }
    if (out.size() < 2) return base;
    return build_route(out);
}

Vec3 RoadNetwork::snap_to_lanes(const std::vector<RoadSeg>& lanes_in, const Vec3& p, float max_dist) {
    Vec3 best = p;
    float bd = max_dist;
    for (const RoadSeg& s : lanes_in) {
        if (!lane_ok(s.a, s.b)) continue;
        const Vec3 ab = s.b - s.a;
        const float denom = dot(ab, ab);
        if (denom <= 0) continue;
        float t = dot(p - s.a, ab) / denom;
        t = t < 0 ? 0 : (t > 1 ? 1 : t);
        const Vec3 q = s.a + ab * t;
        const float d = length(q - p);
        if (d < bd) bd = d, best = q;
    }
    return best;
}

}  // namespace nf::driving

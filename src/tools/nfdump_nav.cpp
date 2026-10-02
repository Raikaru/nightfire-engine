// nfdump nav / validate: map blocks 0x05 (AI network) and 0x19 (path_data), and the runtime NavNetwork.
#include "tools/nfdump_nav.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <exception>
#include <fstream>
#include <map>
#include <queue>
#include <random>
#include <set>

#include "assets/level.hpp"
#include "assets/nav_data.hpp"
#include "game/collision_world.hpp"
#include "game/nav.hpp"

namespace nf {

namespace {

// ---- helpers ------------------------------------------------------------------------------------

std::size_t components(const NavPath& p, std::vector<int>* comp_out = nullptr) {
    std::vector<int> comp(p.nodes.size(), -1);
    int n = 0;
    for (std::size_t s = 0; s < p.nodes.size(); ++s) {
        if (comp[s] >= 0) continue;
        std::queue<std::uint16_t> q;
        q.push(std::uint16_t(s));
        comp[s] = n;
        while (!q.empty()) {
            auto c = q.front();
            q.pop();
            for (unsigned k = 0; k < p.nodes[c].adj_count; ++k) {
                auto o = p.other_end(p.adjacency[p.nodes[c].adj_first + k], c);
                if (comp[o] < 0) comp[o] = n, q.push(o);
            }
        }
        ++n;
    }
    if (comp_out) *comp_out = std::move(comp);
    return std::size_t(n);
}

const NavPath* main_nav_path(const NavNetwork& nav) {
    const NavPath* best = nullptr;
    for (const auto& p : nav.paths())
        if (p.plain() && (!best || p.nodes.size() > best->nodes.size())) best = &p;
    return best;
}

// Headless proof: an agent walks `from` -> `to` following only the waypoints link creep hands out.
struct WalkResult {
    RouteStatus status = RouteStatus::Reset;
    int ticks = 0;
    float walked = 0;
    std::size_t route_nodes = 0;
    float first_distance = 0;
};

WalkResult walk(NavNetwork& nav, const Vec3& from, const Vec3& to, float speed, int max_ticks) {
    WalkResult r;
    NavAgent agent(nav);
    agent.set_path_for(from);
    agent.set_goal_position(to, 0);
    Vec3 feet = from;
    for (int t = 0; t < max_ticks; ++t) {
        nav.begin_frame(std::uint32_t(t + 1));
        NavMove m = agent.move_to_goal(feet);
        if (t == 0) {
            r.first_distance = m.distance;
            r.route_nodes = agent.route().nodes.size();
        }
        r.status = m.status;
        r.ticks = t;
        if (m.status != RouteStatus::Following) break;
        Vec3 d = m.waypoint - feet;
        float dh = std::sqrt(d[0] * d[0] + d[2] * d[2]);
        float step = std::min(speed, dh);
        if (dh > 1e-6f) {
            feet[0] += d[0] / dh * step;
            feet[2] += d[2] / dh * step;
        }
        feet[1] = m.waypoint[1];
        r.walked += step;
    }
    return r;
}

// Patrol / mission routes (NDrone2_AssignAIPath + NDrone2_InitAIPath): stand on `start`, assign, follow.
struct PatrolResult {
    bool assigned = false;
    RouteStatus status = RouteStatus::Reset;
    RouteMode mode = RouteMode::Goal;
    std::size_t nodes = 0;
    std::size_t visited = 0;
    int ticks = 0;
    int wait_events = 0;
};

PatrolResult walk_patrol(NavNetwork& nav, const NavPath& path, std::uint16_t start, std::uint32_t mask, int max_ticks) {
    PatrolResult r;
    NavAgent agent(nav);
    Vec3 feet = path.nodes[start].pos;
    nav.begin_frame(1);
    r.assigned = agent.assign_ai_path(feet, path.nodes[start].cel, mask);
    if (!r.assigned) return r;
    r.mode = agent.mission_route().mode();
    r.nodes = agent.mission_route().nodes.size();
    std::set<int> seen;
    for (int t = 0; t < max_ticks; ++t) {
        nav.begin_frame(std::uint32_t(t + 2));
        NavMove m = agent.follow_ai_path(feet);
        r.status = m.status;
        r.ticks = t;
        r.wait_events += m.reached_special;
        seen.insert(agent.mission_route().index);
        if (m.status == RouteStatus::Arrived || m.failed()) break;
        Vec3 d = m.waypoint - feet;
        float dh = std::sqrt(d[0] * d[0] + d[2] * d[2]);
        float step = std::min(0.25f, dh);
        if (dh > 1e-6f) feet[0] += d[0] / dh * step, feet[2] += d[2] / dh * step;
        feet[1] = m.waypoint[1];
    }
    r.visited = seen.size();
    return r;
}

// A floor point near a node that the node can reach (so both are valid route ends), or the node itself.
Vec3 near_node_point(const NavNetwork& nav, const NavPath& p, std::size_t node, std::mt19937& rng) {
    for (int attempt = 0; attempt < 20; ++attempt) {
        float ang = float(rng() % 628) / 100.0f, rad = 1.0f + float(rng() % 300) / 100.0f;
        Vec3 q = p.nodes[node].pos + Vec3{std::cos(ang) * rad, 0, std::sin(ang) * rad};
        if (nav.move_ok(p.nodes[node].pos, q) && nav.move_ok(q, p.nodes[node].pos)) return q;
    }
    return p.nodes[node].pos;
}

void write_bmp(const std::string& path, int w, int h, const std::vector<std::uint8_t>& rgb) {
    std::ofstream f(path, std::ios::binary);
    std::uint32_t size = 54 + std::uint32_t(rgb.size());
    std::uint8_t hdr[54] = {'B', 'M'};
    auto put32 = [&](int o, std::uint32_t v) { for (int i = 0; i < 4; ++i) hdr[o + i] = std::uint8_t(v >> (8 * i)); };
    put32(2, size);
    put32(10, 54);
    put32(14, 40);
    put32(18, std::uint32_t(w));
    put32(22, std::uint32_t(h));
    hdr[26] = 1;
    hdr[28] = 24;
    put32(34, std::uint32_t(rgb.size()));
    f.write(reinterpret_cast<const char*>(hdr), 54);
    f.write(reinterpret_cast<const char*>(rgb.data()), std::streamsize(rgb.size()));
}

struct Canvas {
    int w, h;
    float minx, minz, scale;
    std::vector<std::uint8_t> px;   // bottom-up BGR
    Canvas(int w_, int h_, float mnx, float mnz, float sc) : w(w_), h(h_), minx(mnx), minz(mnz), scale(sc), px(std::size_t(w_ * h_ * 3), 20) {}
    void set(int x, int y, std::array<std::uint8_t, 3> c) {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        std::size_t o = (std::size_t(h - 1 - y) * std::size_t(w) + std::size_t(x)) * 3;
        px[o] = c[2], px[o + 1] = c[1], px[o + 2] = c[0];
    }
    void line(const Vec3& a, const Vec3& b, std::array<std::uint8_t, 3> c) {
        float ax = (a[0] - minx) * scale, az = (a[2] - minz) * scale, bx = (b[0] - minx) * scale, bz = (b[2] - minz) * scale;
        int n = int(std::max(std::fabs(bx - ax), std::fabs(bz - az))) + 1;
        for (int i = 0; i <= n; ++i) set(int(ax + (bx - ax) * float(i) / float(n)), int(az + (bz - az) * float(i) / float(n)), c);
    }
    void dot(const Vec3& a, std::array<std::uint8_t, 3> c) {
        int x = int((a[0] - minx) * scale), y = int((a[2] - minz) * scale);
        for (int dx = -1; dx <= 1; ++dx) for (int dy = -1; dy <= 1; ++dy) set(x + dx, y + dy, c);
    }
};

}  // namespace

// ---- validate -----------------------------------------------------------------------------------

int validate_nav(GameFiles& files, const std::string&) {
    int failures = 0;
    auto fail = [&](const std::string& name, const std::string& what) {
        std::printf("nav FAIL %s: %s\n", name.c_str(), what.c_str());
        ++failures;
    };
    std::size_t with_ai = 0, blocks19 = 0, records19 = 0, portals = 0, refs19 = 0;
    std::size_t mp_levels = 0, mp_links = 0, mp_link_movetest_fail = 0, mp_link_limited = 0, mp_nodes_unbound = 0;
    std::size_t walked_pairs = 0, walked_fail = 0, emit_levels = 0, sp_paths = 0, sp_assigned = 0, sp_unbound = 0;
    float max_qerr = 0, max_link = 0;
    std::vector<std::string> ai_levels;
    for (const auto& f : files.files()) {
        if (!f.name.ends_with(".bin") || f.name.size() <= 4) continue;
        try {
            Level level(files.read(f));
            const ChunkFile* map = level.map();
            if (!map) continue;
            // Block 0x05: exactly one per map chunk that has one, none in other chunk files.
            std::size_t n05 = 0;
            for (const auto& c : level.chunks())
                for (const auto& b : c.chunk.blocks) n05 += b.id == std::uint8_t(BlockId::AiPath);
            if (n05 > 1) fail(f.name, "more than one block 0x05");
            auto net = parse_ai_network(map->chunk);
            if (net) {
                ++with_ai;
                ai_levels.push_back(f.name);
                if (net->version != kAiNetworkVersion) fail(f.name, "version != 8");
                for (const auto& r : net->records) {
                    if (r.extra_size > 12 || r.extra_size % 4) fail(f.name, "extra_size out of 0/4/8/12");
                    std::size_t total = 0;
                    for (const auto& l : r.links) {
                        if (l.a == l.b) fail(f.name, "self link");
                    }
                    (void)total;
                    if (!r.is_bounds() && r.nodes.empty()) fail(f.name, "empty AIPath record");
                }
            }
            for (const auto& c : level.chunks()) {
                if (&c != map) continue;
                for (const auto& t : parse_path_data(c.chunk)) {
                    ++blocks19;
                    records19 += t.keys.size();
                    for (const auto& k : t.keys) {
                        float q = std::sqrt(k.quat[0] * k.quat[0] + k.quat[1] * k.quat[1] + k.quat[2] * k.quat[2] + k.quat[3] * k.quat[3]);
                        max_qerr = std::max(max_qerr, std::fabs(q - 1.0f));
                    }
                }
                auto tracks = parse_path_data(c.chunk);
                for (const auto& r : static_path_refs(c.chunk)) {
                    ++refs19;
                    if (r.path_index >= tracks.size() && &c == map) fail(f.name, "static path index beyond path_data blocks");
                }
                portals += parse_portals(c.chunk).size();
            }
        } catch (const std::exception& e) {
            fail(f.name, e.what());
        }
    }
    if (with_ai != 28) fail("all", "expected 28 levels with block 0x05, found " + std::to_string(with_ai));
    if (blocks19 != 612 || records19 != 13328)
        fail("all", "expected 612 path_data blocks / 13328 records, found " + std::to_string(blocks19) + " / " +
                        std::to_string(records19));
    if (max_qerr > 5e-3f) fail("all", "path_data quaternion not unit length (max err " + std::to_string(max_qerr) + ")");

    // Runtime network on every level with block 0x05.
    for (const std::string& name : ai_levels) {
        try {
            std::string n = name;
            Level level(files.read(*files.find(n)));
            CollisionWorld world(level);
            NavNetwork nav(level, world, NavLimits::for_level(level_id_from_name(name)));
            const bool mp = level_id_from_name(name) >= 0x07000023 && level_id_from_name(name) <= 0x07000029;
            for (const auto& p : nav.paths()) {
                for (const auto& l : p.links) max_link = std::max(max_link, l.length);
                std::size_t adj = 0;
                for (const auto& nd : p.nodes) adj += nd.adj_count;
                if (adj != p.links.size() * 2) fail(name, "adjacency does not cover every link end twice");
            }
            // Patrol (flag 4) / mission (flag 8) paths: assign at a node, then follow the route.
            for (const auto& p : nav.paths()) {
                if (!(p.flags & (pathflag::kPatrol | pathflag::kMission)) || p.nodes.empty()) continue;
                ++sp_paths;
                std::uint32_t mask = (p.flags & pathflag::kMission) ? pathflag::kMission : pathflag::kPatrol;
                std::size_t unbound = 0;
                for (const auto& nd : p.nodes) unbound += nd.cel == kNoCel;
                sp_unbound += unbound;
                if (unbound == p.nodes.size()) { fail(name, "patrol/mission path \"" + p.name + "\" lies outside every room"); continue; }
                std::uint16_t start = 0;
                while (start < p.nodes.size() && p.nodes[start].cel == kNoCel) ++start;
                PatrolResult pr = walk_patrol(nav, p, start, mask, 30000);
                if (!pr.assigned) { fail(name, "assign_ai_path failed on path \"" + p.name + "\" node " + std::to_string(start)); continue; }
                ++sp_assigned;
                bool loop = pr.mode == RouteMode::Loop;
                if (pr.status == RouteStatus::CreepFailed || pr.status == RouteStatus::CreepFailed9 || pr.status == RouteStatus::NoPath)
                    fail(name, "patrol path \"" + p.name + "\" creep failed: " + route_status_name(pr.status));
                else if (!loop && pr.status != RouteStatus::Arrived && pr.nodes > 1)
                    fail(name, "once-route \"" + p.name + "\" never arrived (" + route_status_name(pr.status) + ")");
                else if (loop && pr.nodes > 2 && pr.visited < 2)
                    fail(name, "loop route \"" + p.name + "\" never advanced");
            }
            const NavPath* main = main_nav_path(nav);
            if (!mp) continue;
            if (!main) {
                fail(name, "no plain nav path");
                continue;
            }
            ++mp_levels;
            if (components(*main) != 1) fail(name, "MP nav graph is not a single component");
            for (const auto& nd : main->nodes)
                if (nd.cel == kNoCel) ++mp_nodes_unbound;
            for (const auto& l : main->links) {
                ++mp_links;
                if (nav.move_test_from_node({std::uint16_t(main->index), l.a}, {main->nodes[l.b].pos, main->nodes[l.b].cel}) == 1)
                    continue;
                // Blocked links must be explained by one of NDrone2_MoveTest's limits: |dy| > 4.0, 3-D length > 30,
                // slope, or the link crossing a boundary segment (those segments are exactly the ones flagged passable).
                const NavNode& na = main->nodes[l.a];
                const NavNode& nb = main->nodes[l.b];
                Vec3 d = nb.pos - na.pos, cross_out;
                float hd = std::sqrt(d[0] * d[0] + d[2] * d[2]);
                bool explained = std::fabs(d[1]) > nav.limits().max_dy || dot(d, d) > nav.limits().max_dist_sq ||
                                 (std::fabs(d[1]) > nav.limits().slope_dy && std::fabs(std::atan2(d[1], hd)) > nav.limits().max_slope) ||
                                 nav.furthest_position(na.pos, nb.pos, cross_out);
                if (explained) ++mp_link_limited; else ++mp_link_movetest_fail;
            }
            // EmitPath range is 255 units: some node's emitter must reach every node, all emitters must seed.
            {
                std::size_t full = 0, seeded = 0;
                for (std::size_t i = 0; i < main->nodes.size(); ++i) {
                    NavEmitter em;
                    if (!nav.init_emitter(em, main->nodes[i].pos, main->nodes[i].cel, main->index) || !nav.emit_path(em, 0)) continue;
                    ++seeded;
                    if (std::count(em.table.begin(), em.table.end(), std::uint8_t(0xff)) == 0) ++full;
                }
                if (seeded != main->nodes.size()) fail(name, "emitter could not seed at every node");
                if (full == 0) fail(name, "no emitter reaches every node");
                else ++emit_levels;
            }
            // Route + link-creep walk between deterministic node pairs.
            std::mt19937 rng(1234);
            for (int i = 0; i < 12; ++i) {
                auto a = rng() % main->nodes.size(), b = rng() % main->nodes.size();
                if (a == b) continue;
                WalkResult w = walk(nav, main->nodes[a].pos, main->nodes[b].pos, 0.25f, 6000);
                ++walked_pairs;
                Vec3 qa = near_node_point(nav, *main, a, rng), qb = near_node_point(nav, *main, b, rng);
                WalkResult w2 = walk(nav, qa, qb, 0.25f, 6000);
                ++walked_pairs;
                if (w2.status != RouteStatus::Arrived) {
                    ++walked_fail;
                    fail(name, "off-node walk " + std::to_string(a) + "->" + std::to_string(b) + " ended " +
                                   route_status_name(w2.status) + " after " + std::to_string(w2.ticks) + " ticks");
                }
                if (w.status != RouteStatus::Arrived) {
                    ++walked_fail;
                    fail(name, "walk " + std::to_string(a) + "->" + std::to_string(b) + " ended " +
                                   route_status_name(w.status) + " after " + std::to_string(w.ticks) + " ticks");
                }
            }
        } catch (const std::exception& e) {
            fail(name, std::string("runtime: ") + e.what());
        }
    }
    if (mp_link_movetest_fail) fail("mp", std::to_string(mp_link_movetest_fail) + " MP nav links fail MoveTest without hitting a limit or boundary");
    if (mp_nodes_unbound) fail("mp", std::to_string(mp_nodes_unbound) + " MP nav nodes outside every room");
    std::printf("nav: %zu levels with block 0x05, %zu path_data blocks (%zu records, %zu static refs, max |q|-1 %.2g), "
                "%zu portals\n",
                with_ai, blocks19, records19, refs19, double(max_qerr), portals);
    std::printf("nav runtime: %zu MP levels: %zu links (%zu blocked by a MoveTest limit or boundary crossing, %zu unexplained failures, %zu nodes outside rooms), emitters ok on %zu, "
                "%zu/%zu routes walked to the goal, longest link %.2f, %d failures\n",
                mp_levels, mp_links, mp_link_limited, mp_link_movetest_fail, mp_nodes_unbound, emit_levels, walked_pairs - walked_fail,
                walked_pairs, double(max_link), failures);
    std::printf("nav patrol/mission paths: %zu, %zu assigned and followed (%zu nodes outside rooms)\n", sp_paths, sp_assigned,
                sp_unbound);
    return failures;
}

// ---- nfdump nav <level> -------------------------------------------------------------------------

int cmd_nav(GameFiles& files, const std::string& level_arg, const std::string& bmp_path) {
    std::string name = level_arg;
    auto bin = read_level_bin(files, name);
    if (bin.empty()) {
        std::fprintf(stderr, "no level %s\n", level_arg.c_str());
        return 1;
    }
    Level level(std::move(bin));
    CollisionWorld world(level);
    NavNetwork nav(level, world, NavLimits::for_level(level_id_from_name(name)));
    std::printf("%s: %zu rooms (cels), %zu nav paths, %zu boundary sets\n", name.c_str(), nav.cels().size(),
                nav.paths().size(), nav.bounds().size());
    if (nav.empty()) {
        std::printf("no block 0x05 in this level\n");
        return 0;
    }
    for (const auto& b : nav.bounds()) {
        std::size_t passable = 0;
        for (const auto& l : b.links) passable += (l.flags & kBlinkPassable) != 0;
        std::size_t nocel = 0;
        for (const auto& n : b.nodes) nocel += n.cel == kNoCel;
        std::printf("  bounds %d \"%s\": %zu nodes / %zu links, %zu passable links, %zu nodes outside rooms\n", b.index,
                    b.name.c_str(), b.nodes.size(), b.links.size(), passable, nocel);
    }
    for (const auto& p : nav.paths()) {
        float sum = 0, mx = 0, mn = 1e9f;
        for (const auto& l : p.links) sum += l.length, mx = std::max(mx, l.length), mn = std::min(mn, l.length);
        std::size_t nocel = 0, maxdeg = 0;
        for (const auto& n : p.nodes) nocel += n.cel == kNoCel, maxdeg = std::max<std::size_t>(maxdeg, n.adj_count);
        std::vector<int> comp;
        std::size_t nc = components(p, &comp);
        std::size_t mt_ok = 0, mt_fail = 0;
        for (const auto& l : p.links) {
            const NavNode& b = p.nodes[l.b];
            (nav.move_test_from_node({std::uint16_t(p.index), l.a}, {b.pos, b.cel}) == 1 ? mt_ok : mt_fail)++;
        }
        std::printf("  path %d \"%s\" flags 0x%x: %zu nodes / %zu links, %zu components, link length %.2f..%.2f avg %.2f, "
                    "max degree %zu, %zu nodes outside rooms, MoveTest along links %zu ok / %zu blocked\n",
                    p.index, p.name.c_str(), p.flags, p.nodes.size(), p.links.size(), nc, double(mn), double(mx),
                    p.links.empty() ? 0.0 : double(sum / float(p.links.size())), maxdeg, nocel, mt_ok, mt_fail);
    }
    const NavPath* main = main_nav_path(nav);
    if (!main || main->nodes.size() < 2) return 0;

    // Farthest node pair (euclidean) -> A* + link-creep walk.
    std::size_t fa = 0, fb = 0;
    float best = -1;
    for (std::size_t a = 0; a < main->nodes.size(); ++a)
        for (std::size_t b = a + 1; b < main->nodes.size(); ++b) {
            float d = length(main->nodes[a].pos - main->nodes[b].pos);
            if (d > best) best = d, fa = a, fb = b;
        }
    std::printf("far pair: node %zu (%.1f %.1f %.1f) -> node %zu (%.1f %.1f %.1f), straight %.1f\n", fa, double(main->nodes[fa].pos[0]),
                double(main->nodes[fa].pos[1]), double(main->nodes[fa].pos[2]), fb, double(main->nodes[fb].pos[0]),
                double(main->nodes[fb].pos[1]), double(main->nodes[fb].pos[2]), double(best));
    NavAgent agent(nav);
    agent.set_path_for(main->nodes[fa].pos);
    agent.set_goal_position(main->nodes[fb].pos, 0);
    nav.begin_frame(1);
    NavMove m = agent.move_to_goal(main->nodes[fa].pos);
    const NavRoute& route = agent.route();
    std::printf("A*: status %s, %zu route nodes, route length %.1f (%.2fx straight), target marks direct %d fallback %d\n",
                route_status_name(route.status), route.nodes.size(), double(route.distance), double(route.distance / best),
                agent.target().direct, agent.target().fallback);
    (void)m;
    std::vector<Vec3> trail;
    for (auto id : route.nodes) trail.push_back(main->nodes[id].pos);
    WalkResult w = walk(nav, main->nodes[fa].pos, main->nodes[fb].pos, 0.25f, 20000);
    std::printf("link-creep walk at 0.25 u/tick: %s after %d ticks, %.1f units walked, %zu route nodes\n",
                route_status_name(w.status), w.ticks, double(w.walked), w.route_nodes);

    // Random pairs: routes and walks.
    std::mt19937 rng(7);
    int ok = 0, tot = 0, off_ok = 0, off_tot = 0;
    std::map<std::string, int> statuses, off_statuses;
    for (int i = 0; i < 100; ++i) {
        auto a = rng() % main->nodes.size(), b = rng() % main->nodes.size();
        if (a == b) continue;
        WalkResult r = walk(nav, main->nodes[a].pos, main->nodes[b].pos, 0.25f, 20000);
        ++tot;
        ok += r.status == RouteStatus::Arrived;
        ++statuses[route_status_name(r.status)];
    }
    for (int i = 0; i < 100; ++i) {   // off-node ends: exercises the straight-line route and OptimiseRoute trimming
        auto a = rng() % main->nodes.size(), b = rng() % main->nodes.size();
        if (a == b) continue;
        WalkResult r = walk(nav, near_node_point(nav, *main, a, rng), near_node_point(nav, *main, b, rng), 0.25f, 20000);
        ++off_tot;
        off_ok += r.status == RouteStatus::Arrived;
        ++off_statuses[route_status_name(r.status)];
    }
    std::printf("100 random off-node point pairs: %d/%d arrived;", off_ok, off_tot);
    for (auto& [k, v] : off_statuses) std::printf(" %s=%d", k.c_str(), v);
    std::printf("\n");
    std::printf("100 random node pairs: %d/%d arrived;", ok, tot);
    for (auto& [k, v] : statuses) std::printf(" %s=%d", k.c_str(), v);
    std::printf("\n");

    // Emitters.
    NavEmitter em;
    if (nav.init_emitter(em, main->nodes[0].pos, main->nodes[0].cel, main->index) && nav.emit_path(em, 0)) {
        std::size_t reached = 0;
        int mx = 0;
        double sum = 0;
        for (auto v : em.table)
            if (v != 0xff) ++reached, mx = std::max<int>(mx, v), sum += v;
        std::printf("emitter at node 0: seed %u, reaches %zu/%zu nodes, table max %d avg %.1f\n", em.seed, reached,
                    em.table.size(), mx, reached ? sum / double(reached) : 0.0);
        auto fl = nav.emitter_node_at_distance(std::uint8_t(mx / 2), -1, em);
        if (fl) std::printf("  flee search (min dist %d): node %u table %u\n", mx / 2, fl->node, em.at(fl->node));
    }

    if (!bmp_path.empty()) {
        Vec3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
        for (const auto& b : nav.bounds())
            for (const auto& n : b.nodes)
                for (int k = 0; k < 3; ++k) lo[std::size_t(k)] = std::min(lo[std::size_t(k)], n.pos[std::size_t(k)]), hi[std::size_t(k)] = std::max(hi[std::size_t(k)], n.pos[std::size_t(k)]);
        for (const auto& p : nav.paths())
            for (const auto& n : p.nodes)
                for (int k = 0; k < 3; ++k) lo[std::size_t(k)] = std::min(lo[std::size_t(k)], n.pos[std::size_t(k)]), hi[std::size_t(k)] = std::max(hi[std::size_t(k)], n.pos[std::size_t(k)]);
        float scale = 8.0f;
        int w2 = int((hi[0] - lo[0]) * scale) + 20, h2 = int((hi[2] - lo[2]) * scale) + 20;
        Canvas c(w2, h2, lo[0] - 10 / scale, lo[2] - 10 / scale, scale);
        for (const auto& b : nav.bounds())
            for (const auto& l : b.links) c.line(b.nodes[l.a].pos, b.nodes[l.b].pos, (l.flags & kBlinkPassable) ? std::array<std::uint8_t, 3>{90, 90, 40} : std::array<std::uint8_t, 3>{200, 60, 60});
        for (const auto& p : nav.paths())
            for (const auto& l : p.links) c.line(p.nodes[l.a].pos, p.nodes[l.b].pos, p.plain() ? std::array<std::uint8_t, 3>{110, 110, 110} : std::array<std::uint8_t, 3>{60, 160, 220});
        for (std::size_t i = 1; i < trail.size(); ++i) c.line(trail[i - 1], trail[i], {255, 255, 0});
        for (const auto& p : nav.paths())
            for (const auto& n : p.nodes) c.dot(n.pos, n.cel == kNoCel ? std::array<std::uint8_t, 3>{255, 0, 255} : std::array<std::uint8_t, 3>{240, 240, 240});
        write_bmp(bmp_path, w2, h2, c.px);
        std::printf("wrote %s (%dx%d): boundaries red (passable olive), nav links grey, A* route yellow\n", bmp_path.c_str(), w2, h2);
    }
    return 0;
}

}  // namespace nf

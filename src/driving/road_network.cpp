#include "driving/road_network.hpp"

#include <cmath>

namespace nf::driving {

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
    if (chained < 0) return -1;
    float best = dot(nodes_[std::size_t(chained)].dir, dir);
    int pick = chained;
    const Vec3 end = nodes_[std::size_t(i)].pos + nodes_[std::size_t(i)].dir * nodes_[std::size_t(i)].length;
    for (std::size_t j = 0; j < nodes_.size(); ++j) {
        if (int(j) == chained || int(j) == i) continue;  // never succeed yourself
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

std::vector<int> RoadNetwork::walk_from(int start) const {
    std::vector<int> walk;
    if (start < 0 || std::size_t(start) >= nodes_.size()) return walk;
    std::vector<char> seen(nodes_.size(), 0);
    int n = start;
    while (n >= 0 && walk.size() < nodes_.size()) {
        walk.push_back(n);
        seen[std::size_t(n)] = 1;
        // Exact successor first (chained street grids); else nearest unvisited node.
        int nx = successor(n, nodes_[std::size_t(n)].dir);
        if (nx < 0 || seen[std::size_t(nx)]) {
            float best = 1e30f;
            nx = -1;
            for (std::size_t j = 0; j < nodes_.size(); ++j) {
                if (seen[j]) continue;
                const Vec3 d = nodes_[j].pos - nodes_[std::size_t(n)].pos;
                const float q = dot(d, d);
                if (q < best) best = q, nx = static_cast<int>(j);
            }
        }
        n = nx;
    }
    return walk;
}

}  // namespace nf::driving

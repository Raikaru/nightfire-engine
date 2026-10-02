#pragma once

#include <cstddef>
#include <vector>

#include "core/math.hpp"
#include "driving/mission_data.hpp"

namespace nf::driving {

// The driveable lane graph of a track, built from the `rs` segments (RNgp group, sub_220C28).
// Segments chain end-to-start (rs#0 ends where rs#1 starts in Paris); junctions are handled by
// continuing on the segment whose direction best matches the current heading — the functional
// equivalent of AIGroundVehicle_GetBestWeightedLane/DriveToPointTraffic. Used by AI drivers,
// mission checkpoint progress and pickup placement.
class RoadNetwork {
public:
    struct Node {
        Vec3 pos{};          // segment start (lane centre)
        Vec3 dir{};          // unit heading start -> end
        float length = 0;    // segment length, metres
        int next = -1;       // chained successor segment, -1 at dead ends
        float width = 0;
    };

    static RoadNetwork build(const std::vector<RoadSeg>& segs);
    bool empty() const { return nodes_.empty(); }
    std::size_t size() const { return nodes_.size(); }
    const Node& node(std::size_t i) const { return nodes_[i]; }
    // Route-first construction from ordered lane points (MissionData::route): node i looks
    // down the route, linked i -> i+1. Used on every track; build() stays for rs chains.
    static RoadNetwork build_route(const std::vector<Vec3>& points);
    // Spliced route: `walk` (indices into `base`) with `rs`-lane bridges spliced across
    // index-order jumps. The `rn` route skips corner nodes the `rs` lanes keep (a 110 m
    // jump where the street turns), which strands lookahead steering; the bridge follows
    // real lane segments instead. Unbridged jumps stay jumps (existing guards handle them).
    static RoadNetwork splice_jumps(const RoadNetwork& base, const std::vector<int>& walk,
                                    const std::vector<RoadSeg>& lanes);
    // Snap a steering target onto the nearest graded lane (same lane filter as the
    // bridges). `rn` waypoints wander off the graded surface (rocks, berms); the lanes
    // are what wheels belong on. Returns `p` unchanged when no lane is near.
    static Vec3 snap_to_lanes(const std::vector<RoadSeg>& lanes_in, const Vec3& p, float max_dist);

    // Nearest node to p (full scan; networks are small: 811 segments in Paris).
    int nearest(const Vec3& p) const;
    // Successor to drive from node i given heading dir (straightest continuation).
    int successor(int i, const Vec3& dir) const;
    // Progress parameter of p along node i (0 = at node, 1 = at its end).
    float progress(int i, const Vec3& p) const;
    // Long route from `start`: greedy successors, bridging dead ends by jumping to the
    // nearest unvisited node within 150 m roughly ahead (intersection gaps, split levels).
    // Stops at `max_len` nodes or when no continuation exists.
    std::vector<int> chain_from(int start, std::size_t max_len = 4000) const;
    // Route walk from `start` over the lane points: repeatedly goes to the nearest unvisited
    // node (exact successors first). The `rs` records are not stored in route order on every
    // track, so this nearest-neighbour walk is what spreads traffic/pickups/checkpoints and
    // routes AI lanes. Returns rs indices; always terminates.
    std::vector<int> walk_from(int start) const;

private:
    std::vector<Node> nodes_;
};

}  // namespace nf::driving

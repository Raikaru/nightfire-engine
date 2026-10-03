#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "assets/level.hpp"
#include "assets/nav_data.hpp"
#include "core/math.hpp"
#include "game/collision_world.hpp"

namespace nf {

// Runtime navigation of the original's drones and bots: the AINetwork of ACTION.ELF (@0x2c7940).
// Design, function mapping and verification: docs/ai-nav.md. Spec: docs/spec-arena-ai.md Part 2B.
//
// Usage from drone code
//   NavNetwork nav(level, world.collision(), NavLimits::for_level(level_id));   // once per level
//   nav.begin_frame(tick);                                                      // once per tick (Drone_InitComms)
//   NavAgent agent(nav);  agent.set_path_for(feet, cel);                        // NDrone2_PostLoad_Init
//   agent.set_goal_position(goal, radius);                                      // AINetwork_SetupGoalPosition
//   NavMove m = agent.move_to_goal(feet);                                       // NDrone2_MoveToGoalPosition
//   if (m.status == RouteStatus::Following) steer_towards(m.waypoint);
// Feet positions are the original's NDrone2_FeetPos (obj.pos - (stand_height - 0.4)), y up.

constexpr std::uint16_t kNoNode = 0xffff;
constexpr int kNoCel = -1;

// Node/cel reference used across the API. `path` indexes NavNetwork::paths().
struct NodeRef {
    std::uint16_t path = kNoNode;
    std::uint16_t node = kNoNode;
    bool valid() const { return path != kNoNode && node != kNoNode; }
    friend bool operator==(const NodeRef&, const NodeRef&) = default;
};

// AINetwork+0x04..0x14 (set by Drone_PostLoad_Init @0x136938).
struct NavLimits {
    float max_slope = 0.8726647f;   // +0x04 (50 deg); 0.7853982 (45 deg) on level 0x07000008
    float slope_dy = 2.0f;          // +0x08 dy above which the slope test applies; 1.5 on 0x07000008
    float max_dy = 4.0f;            // +0x0c
    float max_dist_sq = 900.0f;     // +0x14
    static NavLimits for_level(std::uint32_t level_id);   // level_id: 0x07000024 for "07000024.bin"
};
// "07000024.bin" -> 0x07000024 (0 if the name is not hexadecimal).
std::uint32_t level_id_from_name(const std::string& bin_name);

// CelPos_tag: position + the cel (room) it lies in. cel = index into NavNetwork::cels(), kNoCel if outside all rooms.
struct CelPos {
    Vec3 pos{};
    int cel = kNoCel;
};

// AIRoute_tag+4 status byte (spec 6.6).
enum class RouteStatus : std::uint8_t {
    Following = 0,      // route valid, waypoint returned
    Approximate = 1,    // route found but goal only reachable approximately (fallback marks)
    Straight = 2,       // straight-line route (MoveTest start->goal ok)
    Arrived = 3,
    NoTargetNodes = 4,  // goal has no marked nodes / target never built
    NoStartNode = 5,    // no node reachable from the start (also: start cel holds no nodes)
    Exhausted = 6,      // A* open list exhausted
    NoPath = 7,         // route has no AIPath
    CannotCalc = 8,     // no target / no path data
    CreepFailed9 = 9,   // LinkCreep_Handler failed while route.+2 == 0
    Reset = 0xb,        // freshly reset
    CreepFailed = 0xc,  // LinkCreep_Handler failed
};
const char* route_status_name(RouteStatus s);

enum class RouteMode : std::uint8_t { Goal = 0, Once = 1, Loop = 2, PingPong = 3 };  // AIRoute flags & 7

// AIRoute_tag (spec 2.10). Owned by an agent; public so drone code can inspect it.
struct NavRoute {
    static constexpr std::uint16_t kReverse = 0x8, kValid = 0x10, kFlag20 = 0x20, kDestAltered = 0x40, kSkipStraight = 0x80;
    std::uint16_t flags = 0;                  // +0x00 mode | reverse | valid | ...
    RouteStatus status = RouteStatus::Reset;  // +0x04
    std::uint16_t last_node = kNoNode;        // +0x06
    std::uint32_t target_stamp = 0;           // +0x08
    std::uint16_t prev_first = kNoNode;       // +0x0c
    std::uint16_t first_node = kNoNode;       // +0x0e
    std::uint16_t dest_node = kNoNode;        // +0x10
    CelPos start, goal, waypoint;             // +0x20 / +0x40 / +0x60
    int index = -1;                           // +0x80 current index into `nodes`
    float distance = 0;                       // +0x88 remaining route distance
    float radius = 0;                         // +0x8c arrival radius
    int path = -1;                            // +0xf4 nav path index (NavNetwork::paths())
    std::vector<std::uint16_t> nodes;         // +0xf0 node id array (+0x82 count = size())
    // link creep (+0xa0..+0xe0)
    bool creep_active = false;
    bool special_pending = false;
    int creep_link = -1;                      // +0xa4 link carrying flag 0x10
    std::int16_t creep_a = -1, creep_b = -1;  // +0xac/+0xae
    std::int16_t creep_step = 0;              // +0xb0
    std::uint16_t creep_steps = 0;            // +0xb2
    Vec3 step_vec{}, seg_start{}, seg_end{};  // +0xc0 / +0xd0 / +0xe0

    float lookahead = 2.0f;                   // +0x90 creep look-ahead: waypoint advances while closer than this
                                              //      (NDrone2_DefaultInit: 2.0, 4.0 when drone+0x23, 8.0 for type 0xd)
    std::uint8_t flag2 = 0;                   // +0x02 (selects status 9 vs 0xc when link creep fails)
    bool ignore_bounds = false;               // behaviour property 0x36: LinkCreep_Handler skips the pass test

    RouteMode mode() const { return RouteMode(flags & 7); }
    bool valid() const { return flags & kValid; }
};

// AITarget_tag (spec 2.8): goal descriptor whose marks are recomputed by NodesForPosition.
struct NavTarget {
    static constexpr std::uint32_t kDroneMask = 0x10000000, kPlayerMask = 0x1000000;
    std::uint32_t mask = kDroneMask;
    bool object = false;                      // +0x04 != 0 (moving object goal)
    CelPos goal, prev;                        // +0x10 / +0x30
    std::int16_t direct = 0, fallback = 0;    // +0x50 / +0x52
    std::uint32_t stamp = 0;                  // +0x54 (0 = never built)
};

// What one movement step yields to the drone: status + the waypoint to steer to (link creep).
struct NavMove {
    RouteStatus status = RouteStatus::Reset;
    Vec3 waypoint{};          // route.+0x60, valid when status is Following/Approximate/Straight
    float distance = 0;       // remaining route distance (GetRouteDistance)
    // NDrone2_ReachedDestNode: the owner reached a special (flag & 0xf007f) route node this step.
    bool reached_special = false;
    NodeRef reached_node;
    bool arrived() const { return status == RouteStatus::Arrived; }
    bool failed() const { return status != RouteStatus::Following && status != RouteStatus::Approximate &&
                                 status != RouteStatus::Straight && status != RouteStatus::Arrived; }
};

// ---- runtime graph ----------------------------------------------------------------------------

struct NavNode {                    // MAPNODE + NODE_tag
    Vec3 pos{};                     // y already +0.4
    std::uint32_t flags = 0;        // file flags | runtime scratch (bit30 open/closed, target marks)
    int cel = kNoCel;
    std::uint16_t adj_first = 0;    // NODE+0x12 into NavPath::adjacency
    std::uint8_t adj_count = 0;     // NODE+0x11
    std::int32_t object = -1;       // NODE+0x28 door/kick object handle (see NavNetwork::bind_node_object)
    // search scratch (NODE+0x14.. )
    float g = 0, h = 0, f = 0;
    std::uint16_t parent = kNoNode;
    std::uint8_t arrival = 0;
    float dist2d = 0;               // NODE+0x0c
};

struct NavLink {
    std::uint16_t a = 0, b = 0;
    std::uint16_t flags = 0;        // 0x10 traversed, 0x20 door, 0x40 kick, 0x100 danger area
    std::uint16_t used = 0;         // crowd used-count
    float length = 0;               // 3-D |A-B|
};

struct NavPath {                    // AIPath_tag
    int index = 0;
    std::string name;
    std::uint32_t flags = 0;
    std::vector<NavNode> nodes;
    std::vector<NavLink> links;
    std::vector<std::uint16_t> adjacency;   // link ids, node i owns [adj_first, adj_first + adj_count)
    std::uint32_t target_marks = 0;         // AIPath+0x44
    bool plain() const { return (flags & pathflag::kNotPlain) == 0; }
    std::uint16_t other_end(std::uint16_t link, std::uint16_t node) const {
        return links[link].a == node ? links[link].b : links[link].a;
    }
    // Link between two nodes, -1 if none.
    int link_between(std::uint16_t a, std::uint16_t b) const;
};

struct BoundsNode {                 // BNODE
    Vec3 pos{};
    float height = 0;               // AIBounds_Link2Cel (>= 2.0)
    int cel = kNoCel;
};
struct BoundsLink {                 // BLINK
    std::uint16_t a = 0, b = 0;
    std::uint32_t flags = 0;        // 0x400 passable, 0x800 door, 0x1000 kick
};
constexpr std::uint32_t kBlinkPassable = 0x400, kBlinkDoor = 0x800, kBlinkKick = 0x1000;
struct Bounds {                     // AIBounds_tag
    int index = 0;
    std::string name;
    std::vector<BoundsNode> nodes;
    std::vector<BoundsLink> links;
};
struct BlinkRef {
    std::uint16_t bounds = 0, link = 0;
};

// A room (cel): a world-cel static of class 0xc023 / 0xc047 (cel flag 0x40000 in build_FindCel).
struct NavCel {
    std::size_t placement = 0;      // Level::placements() index (its collision answers the disambiguation rays)
    Vec3 min{}, max{};              // model box, used as-is in world space (room cels sit at the origin)
    std::vector<NodeRef> nodes;     // cel+0x24 list, head first
    std::vector<BlinkRef> blinks;   // cel+0x34 list
};

// Per-emitter distance field (AIEmitter_tag, spec 2.11): one byte per node of `path`, 0xff = unreachable.
struct NavEmitter {
    Vec3 pos{};
    int cel = kNoCel;
    int path = -1;
    std::vector<std::uint8_t> table;
    std::uint16_t seed = kNoNode;   // +0x3c
    bool allocated = false;         // +0x40
    std::uint8_t at(std::uint16_t node) const { return node < table.size() ? table[node] : 0xff; }
};

// AINodeSearch (spec 2.7). Exposed for tools/tests; drone code normally uses NavAgent.
struct NodeSearch {
    CelPos query;
    const CelPos* goal = nullptr;
    int path = -1;
    bool seed_mode = false;         // false: mark goal nodes (mode 0), true: seed A*/nearest (mode 1)
    bool add_open = false;          // seed_mode: put the seed on the open list
    std::uint32_t mask = 0;
    float radius_sq = 0;            // 0 -> box half extent 15.0
    // results
    std::uint16_t result = kNoNode;
    int found = 0, fallback = 0;
};

class NavAgent;

class NavNetwork {
public:
    // Parses block 0x05 of the level's map chunk, binds nodes/boundaries to rooms (AIPath_BindNodes),
    // computes link lengths and adjacency (AIPath_Prepare) and the passable-boundary flags
    // (AINetwork_InitPassableBoundries). An absent block yields an empty network (empty() == true).
    NavNetwork(Level& level, const CollisionWorld& world, NavLimits limits = {});
    ~NavNetwork();
    NavNetwork(const NavNetwork&) = delete;
    NavNetwork& operator=(const NavNetwork&) = delete;

    bool empty() const { return paths_.empty(); }
    const NavLimits& limits() const { return limits_; }
    const std::vector<NavPath>& paths() const { return paths_; }
    std::vector<NavPath>& paths() { return paths_; }
    const std::vector<Bounds>& bounds() const { return bounds_; }
    std::vector<Bounds>& bounds() { return bounds_; }
    const std::vector<NavCel>& cels() const { return cels_; }
    std::size_t max_nodes() const { return max_nodes_; }   // AINetwork+0x18
    std::size_t max_links() const { return max_links_; }   // AINetwork+0x1c
    const CollisionWorld& collision() const { return world_; }

    // Drone_InitComms: call once per world tick before any agent runs. Sets the AI clock used for
    // target stamps and ClearLinkFlags(0x310) (link flags 0x10|0x100|0x200 and every used-count).
    void begin_frame(std::uint32_t frame);
    std::uint32_t frame() const { return frame_; }

    // ---- cels ----
    // build_FindCel(pos, glb_world): room containing `pos`, kNoCel if none. Several overlapping room
    // boxes are disambiguated like the original (vertical +-256 rays into each candidate's collision,
    // nearest hit wins, first candidate when none is hit).
    int find_cel(const Vec3& pos) const;
    CelPos locate(const Vec3& pos) const { return {pos, find_cel(pos)}; }

    // ---- MoveTest family (spec 4.1) ----
    // NDrone2_MoveTest (all overloads): 1 = unobstructed, 0 = blocked, -1 = a boundary was hit and
    // `hit` was supplied (then *hit = nearest crossing).
    int move_test(const CelPos& from, const CelPos& to, Vec3* hit = nullptr, bool corner_test = false) const;
    bool move_ok(const Vec3& from, const Vec3& to) const;   // locate both ends, move_test == 1
    int move_test_from_node(NodeRef node, const CelPos& to, Vec3* hit = nullptr, bool corner_test = false) const;
    int move_test_to_node(const CelPos& from, NodeRef node, Vec3* hit = nullptr, bool corner_test = false) const;
    // AINetwork_BoundsTest: no boundary crossed between the points (corner-post test optional).
    bool bounds_test(const CelPos& from, const CelPos& to, Vec3* hit = nullptr, bool corner_test = false) const;
    // AINetwork_BoundsNodeTest (r = 0.2 from LinkCreep_Handler).
    bool bounds_node_test(const CelPos& from, const CelPos& to, float radius) const;
    // AINetwork_TestRayCels: same straddle + destination-cel test as BoundsTest, without boundary checks.
    bool test_ray_cels(const CelPos& from, const CelPos& to) const;
    // AINetwork_LineIntersectsBoundry; clip_height applies the permanent boundary height clip.
    bool line_intersects_boundary(BlinkRef link, const Vec3& from, const Vec3& to, Vec3* hit, float* nearest,
                                  bool clip_height);
    // AINetwork_FurthestPosition: out = `to`, or the nearest boundary crossing; true if crossed.
    bool furthest_position(const Vec3& from, const Vec3& to, Vec3& out) const;
    // AINetwork_HitBoundry / GetBoundsPushVectorForSphere (NDrone2_Collision uses r = 0.4): push-out
    // vector keeping a sphere of radius r inside the boundaries around `pos` (mask: link flags to skip).
    bool bounds_push_vector(float radius, const CelPos& pos, Vec3& push, std::uint32_t mask = 0) const;

    // ---- node search / goal marking (spec 5) ----
    // AINetwork_NodeSearchCel: only nodes of the query's own cel (the original's neighbour-cel loop is a no-op).
    bool node_search(NodeSearch& s);
    // NDrone2_NearestNode(pos, path): nearest visible node id of `path` (or kNoNode).
    std::uint16_t nearest_node(const CelPos& pos, int path);
    // AINetwork_NavPathForPosition: path whose node is nearest to `pos`, -1 if none.
    int nav_path_for_position(const CelPos& pos, bool move_test) const;
    // AINetwork_NodesForPosition: recompute the goal marks of `target` around target.goal.
    void nodes_for_position(float radius, NavTarget& target);
    // AINetwork_BuildAITarget: recompute only when never built or the goal moved >= 2.0.
    void build_target(NavTarget& target);

    // ---- routes (spec 6) ----
    // AINetwork_CalcRoute / DoAStarPath / OptimiseRoute / LinkCreep_Calc for `route` (route.goal, route.radius,
    // route.path and route.flags are the caller's; route.start is set from `feet`, the NDrone2_FeetPos of the owner).
    // `target` = the AITarget whose marks the search follows (null -> CannotCalc). Returns the status (also stored).
    RouteStatus calc_route(NavRoute& route, const NavTarget* target, const CelPos& feet);
    // AINetwork_AlterDestFor_DROUTE_Nearest: pull an approximate route's goal back from the boundary hit.
    bool alter_dest_for_nearest(NavRoute& route);
    // AINetwork_FollowRoute: link creep along the route; returns the new status (0 / 3 / 7 / 9 / 0xc).
    // `feet` = owner feet + cel. Events (special node reached) are reported through `events` (may be null).
    RouteStatus follow_route(NavRoute& route, const CelPos& feet, NavMove* events = nullptr);
    // AINetwork_GetRouteDistance (2-D) / GetRouteDistance3D: owner feet through the remaining nodes to the goal.
    float route_distance(const NavRoute& route, const Vec3& feet, bool three_d = false) const;
    // AINetwork_GetRouteDistanceBetweenNodes: signed link-length sum between two route indices.
    float route_distance_between(const NavRoute& route, int a, int b) const;
    // AINetwork_ResetRoute.
    void reset_route(NavRoute& route) const;
    // AINetwork_OptimiseRoute (end trimming only).
    void optimise_route(NavRoute& route, bool trim_start, bool trim_end);
    // AINetwork_SetupNextNode / RouteNodeOffset / LinkCreep_* (waypoint sliding along the current link).
    void setup_next_node(int step, NavRoute& route);
    int route_node_offset(int index, int delta, const NavRoute& route) const;
    bool link_creep_calc(NavRoute& route, bool from_end);
    bool link_creep_calc_to_route_end(NavRoute& route);
    // AINetwork_UpdateLinksUsedCount / ClearLinkFlags.
    void update_links_used_count(const NavRoute& route);
    void clear_link_flags(std::uint16_t mask);
    // DroneMove_NextRouteAngle @0x154568: angle between (owner - previous move target) and the current creep
    // segment direction (0 when nearly parallel, dot > 0.9998).
    static float next_route_angle(const NavRoute& route, const Vec3& owner_pos, const Vec3& prev_move_target);
    // DroneMove_SetBoundryFlags @0x14f140: probes 0.5 * scale to +z / -z / +x / -x of `feet` (local axes given in
    // world space) with NDrone2_MoveTest; returns bits 2 / 4 / 8 / 0x10 for every blocked probe.
    unsigned probe_blocked_sides(const CelPos& feet, const Vec3& right, const Vec3& forward, float scale = 1.0f) const;
    // NDrone2_DroneNearPos: is some other actor standing at the position (used to release wait nodes).
    void set_near_pos_callback(std::function<bool(const Vec3&)> fn) { near_pos_ = std::move(fn); }

    // ---- door / kick hooks (NDrone_InitDoorNodes / InitKickNodes; doors are SP-only) ----
    // Binds node flag 0x2 / 0x4 nodes to game objects. `is_locked(object)` answers Door_IsLocked for
    // the object bound to the node being expanded (A*, EmitPath and GetNodeAtDistance skip a link whose
    // both ends are door nodes when locked).
    void bind_node_object(NodeRef node, std::int32_t object) { paths_[node.path].nodes[node.node].object = object; }
    void set_door_locked_callback(std::function<bool(std::int32_t object)> fn) { door_locked_ = std::move(fn); }
    // NDrone_InitDoorNodes @0x139a70: every node with flag 0x2 binds to the nearest object of `doors` (type 0x1c
    // objects) - the flag is cleared when there is none - then every link between two door nodes that crosses a
    // boundary gets link flag 0x20 and the crossed boundaries flag 0x800. NDrone_InitKickNodes @0x139ce0 does the
    // same for flag 0x4 / `kicks` (type 0x1b objects with script id 0x60005bb) / link 0x40 / boundary 0x1000 and
    // never clears the node flag.
    struct HookObject {
        std::int32_t id;
        Vec3 pos;
    };
    void bind_door_nodes(const std::vector<HookObject>& doors);
    void bind_kick_nodes(const std::vector<HookObject>& kicks);
    // Marks the boundaries crossed by a door/kick link (ModIntersectedLinkFlags_Bounds): sets `set` on
    // every BLINK crossed by the segment whose flags & skip == 0. Returns how many were changed.
    int mod_intersected_boundaries(const Vec3& a, const Vec3& b, std::uint32_t set, std::uint32_t skip);
    // AINetwork_SetLinksFlagInCircle(r, pos, cel, set, clear): path links with an end inside the circle.
    int set_link_flags_in_circle(float radius, const CelPos& pos, std::uint16_t set, std::uint16_t clear);

    // ---- emitters (spec 7) ----
    bool init_emitter(NavEmitter& e, const Vec3& pos, int cel = kNoCel, int path = -1, bool reuse = false);
    // AINetwork_EmitPath: Dijkstra flood; range 0 -> 255. Returns false when no seed node exists.
    bool emit_path(NavEmitter& e, float range = 0);
    // NDrone2_DistanceToEmitter: false when `e` is not on `path` or `pos` reaches no node/emitter cell.
    // `cached_node` (0xffff = unset) caches NDrone2_NearestNode(pos).
    bool distance_to_emitter(const CelPos& pos, int path, std::uint16_t& cached_node, const NavEmitter& e,
                             float* out = nullptr);
    // AINetwork_Emitter_GetNodeAtDistance ("flee" search): first neighbour whose table value >= min_dist.
    std::optional<NodeRef> emitter_node_at_distance(std::uint8_t min_dist, int start_node, const NavEmitter& e);

    // ---- passable boundaries ----
    // AINetwork_InitPassableBoundries (called by the constructor; safe to call again after binding doors).
    void init_passable_boundaries();

    // ---- shared player target (NPCGlobals+0x1c0, mask 0x1000000; NDrone2_NavNodeCache) ----
    NavTarget& player_target() { return player_target_; }
    void update_player_target(const Vec3& pos);   // NDrone2_NavNodeCache

    // ---- block 0x19 ----
    const std::vector<PathTrack>& path_tracks() const { return tracks_; }
    const std::vector<StaticPathRef>& static_tracks() const { return static_tracks_; }

private:
    friend class NavAgent;
    struct Portal {
        std::array<Vec3, 4> quad;
        int cel_a, cel_b;
    };
    struct LineCross {
        bool hit = false;
        Vec3 point{};
        bool clip = false;
        float clip_height = 0;
    };
    std::optional<float> cel_ray_distance(int cel, const Vec3& from, const Vec3& to) const;
    void straddle(int start, const Vec3& from, const Vec3& to, std::vector<int>& out) const;
    LineCross cross_boundary(BlinkRef link, const Vec3& from, const Vec3& to) const;
    int bounds_test_i(const CelPos& from, const CelPos& to, Vec3* hit, bool corner) const;
    bool edge_blocked(const NavPath& p, std::uint16_t from, std::uint16_t to) const;
    RouteStatus do_astar(NavRoute& route, const NavTarget& target);
    bool creep_handler(NavRoute& route, const CelPos& feet, NavMove* events);
    bool creep_increment(NavRoute& route, const CelPos& feet);
    bool creep_decrement(NavRoute& route, const CelPos& feet);
    bool at_special_node(const NavRoute& route, const CelPos& feet);
    void link_creep_for_nodes(NavRoute& route, int a, int b, bool from_end);
    Vec3 link_creep_dest(const NavRoute& route, int offset) const;
    std::function<bool(const Vec3&)> near_pos_;
    void bind_hook_nodes(const std::vector<HookObject>& objs, std::uint32_t node_flag, std::uint16_t link_flag,
                         std::uint32_t boundary_flag, bool clear_missing);
    std::vector<Portal> portals_;
    std::vector<std::vector<int>> cel_portals_;
    const CollisionWorld& world_;
    Level& level_;
    NavLimits limits_;
    std::vector<NavPath> paths_;
    std::vector<Bounds> bounds_;
    std::vector<NavCel> cels_;
    std::vector<PathTrack> tracks_;
    std::vector<StaticPathRef> static_tracks_;
    std::size_t max_nodes_ = 0, max_links_ = 0;
    std::uint32_t frame_ = 0;
    NavTarget player_target_;
    std::function<bool(std::int32_t)> door_locked_;
};

// Per-drone navigation state: movement route (drone+0x860), mission/patrol route (drone+0x970),
// AITarget (drone+0xa80), AIPoint (drone+0x6f0), default nav path (drone+0x954), nearest-node cache
// (botvars+0x760).
class NavAgent {
public:
    explicit NavAgent(NavNetwork& net) : net_(&net) {}
    NavNetwork& network() { return *net_; }

    // NDrone2_PostLoad_Init: default nav path = NavPathForPosition(pos, cel, false). Returns false if none.
    bool set_path_for(const Vec3& pos, int cel = kNoCel);
    void set_path(int path) { path_ = path; }
    int path() const { return path_; }

    // ---- goals ----
    // AINetwork_SetupGoalPosition (radius 0 -> 2.0): fixed position goal; marks nodes now.
    void set_goal_position(const Vec3& pos, float radius = 0);
    // AINetwork_SetupGoalPositionToObj: moving object goal (BOTSTATE_gotoGoal status 9). Call
    // update_goal_object each tick with the object's position (BuildAITarget rebuilds when it moved >= 2.0).
    void set_goal_object(const Vec3& pos, float radius = 0);
    void update_goal_object(const Vec3& pos);
    // Goal = the player: uses the network's shared player target (SetupRouteToObject with NDrone2_Player()).
    void set_goal_player(const Vec3& pos, float radius = 0);
    void clear_goal();
    bool has_goal() const { return goal_set_; }
    const CelPos& goal() const { return point_goal_; }

    // ---- movement ----
    // NDrone2_MoveToGoalPosition: (re)build the route when invalid/stale, arrived-test, then FollowRoute.
    NavMove move_to_goal(const Vec3& feet, int cel = kNoCel);
    // AINetwork_CalcRouteToPosition / ToObject (result == Approximate additionally runs AlterDestFor_DROUTE_Nearest).
    RouteStatus calc_route_to_goal(const Vec3& feet, int cel = kNoCel);
    // Restores the captured AIPoint and AITarget state used by the next movement tick.
    bool restore_movement_goal(std::span<const std::byte> point_raw,
                               std::span<const std::byte> target_raw);
    // Restores the captured AIRoute and its per-drone node list without recalculating the route.
    bool restore_movement_route(std::span<const std::byte> route_raw,
                                std::span<const std::byte> route_nodes_raw, int path,
                                float arrive_radius, bool ignore_bounds);
    // NDrone2_InvalidateAttackRoute / AINetwork_InvalidateRoute.
    void invalidate_route() { move_route_.flags &= std::uint16_t(~NavRoute::kValid); }
    bool route_valid() const { return move_route_.valid(); }
    const NavRoute& route() const { return move_route_; }
    NavRoute& route() { return move_route_; }
    const NavTarget& target() const { return target_; }
    float route_distance(const Vec3& feet) const { return net_->route_distance(move_route_, feet); }
    // NDrone2_DefaultInit route parameters: creep look-ahead (2.0 / 4.0 / 8.0), skip-straight-test (flag 0x80),
    // arrival threshold of NDrone2_MoveToGoalPosition (drone+0x8ec, 2.0), behaviour 0x36 (ignore bounds).
    void set_creep_lookahead(float d) { move_route_.lookahead = mission_route_.lookahead = d; }
    void set_skip_straight_test(bool on) {
        for (NavRoute* r : {&move_route_, &mission_route_}) r->flags = on ? (r->flags | NavRoute::kSkipStraight) : (r->flags & ~NavRoute::kSkipStraight);
    }
    void set_arrive_radius(float r) { arrive_radius_ = r; }
    void set_ignore_bounds(bool on) { move_route_.ignore_bounds = mission_route_.ignore_bounds = on; }

    // ---- patrol / mission routes (NDrone2_AssignAIPath + NDrone2_InitAIPath, mode once/loop/pingpong) ----
    // `path_mask`: pathflag::kPatrol or kMission. The agent must stand within 0.3 of one of the path's
    // nodes (same cel). Builds the ordered node list and the first waypoint. False if no path is there.
    bool assign_ai_path(const Vec3& feet, int cel, std::uint32_t path_mask);
    bool has_ai_path() const { return mission_route_.valid(); }
    // Follows the patrol/mission route with link creep; Arrived at the end of a once-route.
    NavMove follow_ai_path(const Vec3& feet, int cel = kNoCel);
    const NavRoute& mission_route() const { return mission_route_; }
    NavRoute& mission_route() { return mission_route_; }
    // DroneMove_ObjectMissionPathDistance @0x153f50: signed route distance along the mission route from its
    // current index to the node nearest (2-D, within `radius` unless it is the first candidate) to `feet` that
    // `feet` can reach (MoveTest); 0 when there is none.
    float mission_path_distance(const CelPos& feet, float radius) const;
    // Node the mission route stands on (flags for wait nodes etc.), invalid when none.
    NodeRef mission_node() const;

    // ---- queries ----
    // NDrone2_NearestNode on this agent's path, cached until invalidate_nearest_node().
    std::uint16_t nearest_node(const Vec3& feet, int cel = kNoCel);
    void invalidate_nearest_node() { nearest_cache_ = kNoNode; }
    std::uint16_t& nearest_node_cache() { return nearest_cache_; }
    // NDrone2_DistanceToEmitter with this agent's path and nearest-node cache.
    bool distance_to_emitter(const Vec3& feet, const NavEmitter& e, float* out = nullptr);

private:
    void build_target(const CelPos& goal_pos, bool object);
    NavNetwork* net_;
    int path_ = -1;
    NavRoute move_route_, mission_route_;
    NavTarget target_;
    CelPos point_goal_;                     // AIPoint+0x20
    float point_radius_ = 2.0f;             // AIPoint+0x14 (0 -> 2.0)
    float arrive_radius_ = 2.0f;            // drone+0x8ec
    bool goal_set_ = false;
    bool goal_is_object_ = false;
    bool goal_is_player_ = false;
    std::uint16_t nearest_cache_ = kNoNode;
};

}  // namespace nf

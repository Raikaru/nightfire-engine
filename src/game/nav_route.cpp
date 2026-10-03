// Routes, link creep and per-drone navigation state of the original's AINetwork / LinkCreep / NDrone2 nav
// helpers (docs/spec-arena-ai.md Part 2B sections 6 and 9, docs/ai-nav.md).
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <utility>

#include "game/nav.hpp"

namespace nf {

namespace {

constexpr float kCreepStep = 0.4f;   // LinkCreep step length; also the node y skin
constexpr std::uint32_t kSpecial = nodeflag::kSpecialMask;

float sqdist2d(const Vec3& a, const Vec3& b) {
    float dx = a[0] - b[0], dz = a[2] - b[2];
    return dx * dx + dz * dz;
}
float dist2d(const Vec3& a, const Vec3& b) { return std::sqrt(sqdist2d(a, b)); }

}  // namespace
namespace {

std::uint16_t route_u16(std::span<const std::byte> raw, std::size_t offset) {
    return std::uint16_t(std::to_integer<std::uint8_t>(raw[offset])) |
           (std::uint16_t(std::to_integer<std::uint8_t>(raw[offset + 1])) << 8);
}

std::uint32_t route_u32(std::span<const std::byte> raw, std::size_t offset) {
    return std::uint32_t(route_u16(raw, offset)) |
           (std::uint32_t(route_u16(raw, offset + 2)) << 16);
}

float route_f32(std::span<const std::byte> raw, std::size_t offset) {
    return std::bit_cast<float>(route_u32(raw, offset));
}

CelPos route_cel_pos(std::span<const std::byte> raw, std::size_t offset) {
    return {{route_f32(raw, offset), route_f32(raw, offset + 4), route_f32(raw, offset + 8)},
            std::bit_cast<std::int32_t>(route_u32(raw, offset + 12))};
}

bool route_status(std::uint8_t raw, RouteStatus& status) {
    switch (raw) {
        case 0: status = RouteStatus::Following; return true;
        case 1: status = RouteStatus::Approximate; return true;
        case 2: status = RouteStatus::Straight; return true;
        case 3: status = RouteStatus::Arrived; return true;
        case 4: status = RouteStatus::NoTargetNodes; return true;
        case 5: status = RouteStatus::NoStartNode; return true;
        case 6: status = RouteStatus::Exhausted; return true;
        case 7: status = RouteStatus::NoPath; return true;
        case 8: status = RouteStatus::CannotCalc; return true;
        case 9: status = RouteStatus::CreepFailed9; return true;
        case 0xb: status = RouteStatus::Reset; return true;
        case 0xc: status = RouteStatus::CreepFailed; return true;
        default: return false;
    }
}

}  // namespace


bool NavAgent::restore_movement_route(std::span<const std::byte> route_raw,
                                      std::span<const std::byte> route_nodes_raw, int path,
                                      float arrive_radius, bool ignore_bounds) {
    if (route_raw.size() != 0x100 || (route_nodes_raw.size() & 1) != 0)
        return false;

    const std::size_t node_count = route_u16(route_raw, 0x82);
    if (route_nodes_raw.size() != node_count * sizeof(std::uint16_t))
        return false;

    NavRoute route;
    route.flags = route_u16(route_raw, 0x00);
    route.flag2 = std::to_integer<std::uint8_t>(route_raw[0x02]);
    if (!route_status(std::to_integer<std::uint8_t>(route_raw[0x04]), route.status))
        return false;
    route.last_node = route_u16(route_raw, 0x06);
    route.target_stamp = route_u32(route_raw, 0x08);
    route.prev_first = route_u16(route_raw, 0x0c);
    route.first_node = route_u16(route_raw, 0x0e);
    route.dest_node = route_u16(route_raw, 0x10);
    route.start = route_cel_pos(route_raw, 0x20);
    route.goal = route_cel_pos(route_raw, 0x40);
    route.waypoint = route_cel_pos(route_raw, 0x60);
    route.index = std::bit_cast<std::int16_t>(route_u16(route_raw, 0x80));
    const bool goal_or_once = route.mode() == RouteMode::Goal || route.mode() == RouteMode::Once;
    if (route.index < -1 || route.index > int(node_count) || (route.index == int(node_count) && !goal_or_once))
        return false;
    route.distance = route_f32(route_raw, 0x88);
    route.radius = route_f32(route_raw, 0x8c);
    route.lookahead = route_f32(route_raw, 0x90);
    route.path = path;
    route.creep_active = std::to_integer<std::uint8_t>(route_raw[0xa0]) != 0;
    route.special_pending = std::to_integer<std::uint8_t>(route_raw[0xa1]) != 0;
    route.creep_link = std::bit_cast<std::int16_t>(route_u16(route_raw, 0xa4));
    route.creep_a = std::bit_cast<std::int16_t>(route_u16(route_raw, 0xac));
    route.creep_b = std::bit_cast<std::int16_t>(route_u16(route_raw, 0xae));
    route.creep_step = std::bit_cast<std::int16_t>(route_u16(route_raw, 0xb0));
    route.creep_steps = route_u16(route_raw, 0xb2);
    route.step_vec = {route_f32(route_raw, 0xc0), route_f32(route_raw, 0xc4), route_f32(route_raw, 0xc8)};
    route.seg_start = {route_f32(route_raw, 0xd0), route_f32(route_raw, 0xd4), route_f32(route_raw, 0xd8)};
    route.seg_end = {route_f32(route_raw, 0xe0), route_f32(route_raw, 0xe4), route_f32(route_raw, 0xe8)};
    route.ignore_bounds = ignore_bounds;
    route.nodes.resize(node_count);
    for (std::size_t i = 0; i < node_count; ++i)
        route.nodes[i] = route_u16(route_nodes_raw, i * sizeof(std::uint16_t));
    if (route.valid() && path < 0)
        return false;

    path_ = path;
    arrive_radius_ = arrive_radius;
    move_route_ = std::move(route);
    return true;
}



// ---- route bookkeeping --------------------------------------------------------------------------

void NavNetwork::reset_route(NavRoute& r) const {
    // AINetwork_ResetRoute @0x15aa00.
    r.flags &= std::uint16_t(~NavRoute::kDestAltered);
    r.status = RouteStatus::Reset;
    r.dest_node = kNoNode;
    r.first_node = kNoNode;
    r.last_node = kNoNode;
    r.index = -1;
    r.creep_active = false;
    r.special_pending = false;
    r.creep_link = -1;
    r.creep_a = r.creep_b = -1;
    r.creep_step = 0;
    r.creep_steps = 0;
    r.target_stamp = 0;
    r.nodes.clear();
    r.distance = 0;
}

int NavNetwork::route_node_offset(int index, int delta, const NavRoute& r) const {
    // AINetwork_RouteNodeOffset @0x15a820: index + delta wrapped per mode.
    int i = index + delta;
    int count = int(r.nodes.size());
    if (i >= 0 && i < count) return i;
    switch (r.mode()) {
    case RouteMode::Loop: return i < 0 ? i + count : i - count;
    case RouteMode::PingPong: return i < 0 ? -i : count * 2 - i;
    default: return i < 0 ? -1 : count - 1;
    }
}

float NavNetwork::route_distance(const NavRoute& r, const Vec3& feet, bool three_d) const {
    // AINetwork_GetRouteDistance @0x156320 / GetRouteDistance3D @0x156540.
    auto d = [&](const Vec3& a, const Vec3& b) { return three_d ? length(a - b) : dist2d(a, b); };
    int count = int(r.nodes.size());
    if (count != 0 && r.index < count && r.path >= 0) {
        int i = std::max(r.index + 1, 0);
        if (i <= count - 1) {
            const NavPath& p = paths_[std::size_t(r.path)];
            float total = d(feet, p.nodes[r.nodes[std::size_t(i)]].pos);
            for (int j = i + 1; j < count; ++j) {
                int l = p.link_between(r.nodes[std::size_t(j - 1)], r.nodes[std::size_t(j)]);
                if (l >= 0) total += p.links[std::size_t(l)].length;
            }
            return total + d(r.goal.pos, p.nodes[r.nodes.back()].pos);
        }
    }
    return d(feet, r.goal.pos);
}

float NavNetwork::route_distance_between(const NavRoute& r, int a, int b) const {
    // AINetwork_GetRouteDistanceBetweenNodes @0x156160.
    int count = int(r.nodes.size());
    if (count == 0 || r.path < 0 || a > count - 1 || b > count - 1 || a == b) return 0;
    a = std::max(a, 0);
    b = std::max(b, 0);
    int lo = std::min(a, b), hi = std::max(a, b);
    const NavPath& p = paths_[std::size_t(r.path)];
    float total = 0;
    for (int i = lo + 1; i <= hi; ++i) {
        int l = p.link_between(r.nodes[std::size_t(i - 1)], r.nodes[std::size_t(i)]);
        if (l >= 0) total += p.links[std::size_t(l)].length;
    }
    return b > a ? total : -total;
}

void NavNetwork::update_links_used_count(const NavRoute& r) {
    // AINetwork_UpdateLinksUsedCount @0x15a490: plain paths only; crowd cost for the remaining route.
    if (r.path < 0) return;
    NavPath& p = paths_[std::size_t(r.path)];
    if (p.flags & 0xc) return;
    int count = int(r.nodes.size());
    int start = std::max(r.index - 1, 0);
    if (start >= count) return;
    std::uint16_t prev = r.nodes[std::size_t(start)];
    for (int i = start + 1; i < count; ++i) {
        std::uint16_t cur = r.nodes[std::size_t(i)];
        int l = p.link_between(cur, prev);
        if (l >= 0) ++p.links[std::size_t(l)].used;
        prev = cur;
    }
}

float NavNetwork::next_route_angle(const NavRoute& r, const Vec3& owner_pos, const Vec3& prev_move_target) {
    auto norm = [](Vec3 v) {
        float l = length(v);
        return l > 0 ? v * (1.0f / l) : v;
    };
    float d = dot(norm(owner_pos - prev_move_target), norm(r.seg_end - r.seg_start));
    return d <= 0.9998f ? std::acos(std::max(-1.0f, d)) : 0.0f;
}

unsigned NavNetwork::probe_blocked_sides(const CelPos& feet, const Vec3& right, const Vec3& forward, float scale) const {
    struct Probe {
        float x, z;
        unsigned bit;
    };
    unsigned bits = 0;
    for (Probe p : {Probe{0, 0.5f, 2}, Probe{0, -0.5f, 4}, Probe{0.5f, 0, 8}, Probe{-0.5f, 0, 0x10}}) {
        Vec3 to = feet.pos + right * (p.x * scale) + forward * (p.z * scale);
        if (move_test(feet, {to, find_cel(to)}) != 1) bits |= p.bit;
    }
    return bits;
}

// ---- link creep ---------------------------------------------------------------------------------

Vec3 NavNetwork::link_creep_dest(const NavRoute& r, int offset) const {
    // LinkCreep_Dest @0x15b420: waypoint = start + step * (creep index + offset).
    return r.seg_start + r.step_vec * float(r.creep_step + offset);
}

void NavNetwork::link_creep_for_nodes(NavRoute& r, int a, int b, bool from_end) {
    // LinkCreep_ForNodes @0x159810.
    r.special_pending = false;
    Vec3 start{}, end{};
    r.creep_link = -1;
    const NavPath* p = r.path >= 0 ? &paths_[std::size_t(r.path)] : nullptr;
    auto node_pos = [&](int i) { return p->nodes[r.nodes[std::size_t(i)]].pos; };
    int count = int(r.nodes.size());
    if (a < 0) {
        start = r.start.pos;
        end = (b >= 0 && b < count) ? node_pos(b) : r.goal.pos;
    } else {
        const NavNode& na = p->nodes[r.nodes[std::size_t(a)]];
        if (na.flags & kSpecial) r.special_pending = true;
        if (a == b && r.mode() != RouteMode::Loop) {
            if (r.mode() == RouteMode::Once) r.special_pending = true;
            start = na.pos;
            end = r.goal.pos;
        } else {
            start = na.pos;
            end = (b >= 0 && b < count) ? node_pos(b) : na.pos;
            if (b >= 0 && b < count) {
                int l = p->link_between(r.nodes[std::size_t(a)], r.nodes[std::size_t(b)]);
                if (l >= 0) {
                    r.creep_link = l;
                    paths_[std::size_t(r.path)].links[std::size_t(l)].flags |= 0x10;
                }
            }
        }
    }
    r.creep_a = std::int16_t(a);
    r.creep_b = std::int16_t(b);
    r.seg_start = start;
    r.seg_end = end;
    r.step_vec = end - start;
    float mag = length(r.step_vec);
    if (mag > kCreepStep) {
        r.creep_steps = std::uint16_t(int(float(int(mag) & 0xffff) * 2.5f));
        r.step_vec = r.step_vec * (kCreepStep / mag);
    } else {
        r.creep_steps = 1;
    }
    r.creep_active = true;
    r.creep_step = 0;
    if (from_end) r.creep_step = std::int16_t(r.creep_steps - 1);
    r.waypoint.pos = link_creep_dest(r, 0);
    r.waypoint.cel = find_cel(r.waypoint.pos);
}

bool NavNetwork::link_creep_calc(NavRoute& r, bool from_end) {
    // LinkCreep_Calc @0x159b10.
    int count = int(r.nodes.size());
    if (count < r.index) {
        r.creep_active = false;
        r.special_pending = false;
        r.creep_link = -1;
        r.creep_step = 0;
        r.creep_steps = 0;
        return false;
    }
    if (r.creep_link > 0 && r.path >= 0) paths_[std::size_t(r.path)].links[std::size_t(r.creep_link)].flags &= std::uint16_t(~0x10);
    int a = route_node_offset(r.index, 0, r);
    int b = route_node_offset(r.index, 1, r);
    link_creep_for_nodes(r, a, b, from_end);
    return r.creep_active;
}

bool NavNetwork::link_creep_calc_to_route_end(NavRoute& r) {
    // LinkCreep_CalcToRouteEnd: a single segment start -> goal (straight-line route).
    r.seg_start = r.start.pos;
    r.seg_end = r.goal.pos;
    r.step_vec = r.goal.pos - r.start.pos;
    float mag = length(r.step_vec);
    if (mag > kCreepStep) {
        r.creep_steps = std::uint16_t(int(float(int(mag) & 0xffff) * 2.5f));
        r.step_vec = r.step_vec * (kCreepStep / mag);
    } else {
        r.creep_steps = 1;
    }
    r.creep_step = 0;
    r.creep_active = true;
    r.waypoint.pos = link_creep_dest(r, 0);
    r.waypoint.cel = find_cel(r.waypoint.pos);
    r.special_pending = false;
    r.creep_a = r.creep_b = -1;
    return true;
}

void NavNetwork::setup_next_node(int step, NavRoute& r) {
    // AINetwork_SetupNextNode @0x159668.
    if (!(step < 0 && r.index < -1)) r.index += (r.flags & NavRoute::kReverse) ? -step : step;
    int count = int(r.nodes.size());
    bool calc = r.index < count;
    if (!calc) {
        switch (r.mode()) {
        case RouteMode::Loop:
            r.index = 0;
            calc = true;
            break;
        case RouteMode::PingPong:
            r.index = r.index + ((r.flags & NavRoute::kReverse) ? 2 : -2);
            r.flags ^= NavRoute::kReverse;
            calc = true;
            break;
        default:   // goal / once: park on the goal
            if (r.index != count) {
                r.index = count;
                r.last_node = std::uint16_t(r.index);
            }
            r.waypoint.pos = r.goal.pos;
            r.waypoint.cel = r.goal.cel != kNoCel ? r.goal.cel : find_cel(r.goal.pos);
            return;
        }
    }
    if (!link_creep_calc(r, step < 1)) return;
    r.waypoint.pos = link_creep_dest(r, 0);
    r.waypoint.cel = find_cel(r.waypoint.pos);
}

bool NavNetwork::at_special_node(const NavRoute& r, const CelPos& feet) {
    // LinkCreep_AtSpecialNode @0x159dd0.
    int idx = route_node_offset(r.index, 0, r);
    if (idx < 0 || idx >= int(r.nodes.size()) || r.path < 0) return true;
    const Vec3& np = paths_[std::size_t(r.path)].nodes[r.nodes[std::size_t(idx)]].pos;
    float d = dist2d(np, feet.pos);
    if (d < 0.4f) return true;
    if (d >= 1.0f) return false;
    return near_pos_ ? near_pos_(np) : false;   // NDrone2_DroneNearPos
}

bool NavNetwork::creep_increment(NavRoute& r, const CelPos& feet) {
    // LinkCreep_Increment @0x159f...: advance the waypoint one 0.4 step while it is closer than r.lookahead.
    if (!r.creep_active) return true;
    if (r.creep_steps <= r.creep_step) {
        setup_next_node(1, r);
        return true;
    }
    Vec3 next = link_creep_dest(r, 1);
    int ncel = find_cel(next);
    float d = dist2d(feet.pos, next);
    bool ok = bounds_.empty() || sqdist2d(feet.pos, next) < 1.0f || bounds_node_test(feet, {next, ncel}, 0.4f);
    if (r.lookahead <= d) return true;
    if (d < 0.4f || ok) {
        r.waypoint.pos = next;
        r.waypoint.cel = ncel;
        ++r.creep_step;
    } else if (d < 0.3f) {
        return false;
    }
    return true;
}

bool NavNetwork::creep_decrement(NavRoute& r, const CelPos& feet) {
    // LinkCreep_Decrement @0x15a1...: step the waypoint back (0.1 pass radius).
    if (!r.creep_active || r.creep_step < 1) {
        if (r.index < 0) return false;
        setup_next_node(-1, r);
        return true;
    }
    Vec3 prev = link_creep_dest(r, -1);
    int pcel = find_cel(prev);
    bool ok = bounds_.empty() || sqdist2d(feet.pos, prev) < 1.0f || bounds_node_test(feet, {prev, pcel}, 0.1f);
    if (!ok) return true;
    r.waypoint.pos = prev;
    r.waypoint.cel = pcel;
    --r.creep_step;
    return true;
}

bool NavNetwork::creep_handler(NavRoute& r, const CelPos& feet, NavMove* events) {
    // LinkCreep_Handler @0x15a2c8.
    if (!r.creep_active) return true;
    Vec3 wp = link_creep_dest(r, 0);
    if (r.special_pending) {
        if (!at_special_node(r, feet)) return true;
        if (events) {   // NDrone2_ReachedDestNode
            int idx = route_node_offset(r.index, 0, r);
            events->reached_special = true;
            if (idx >= 0 && idx < int(r.nodes.size()) && r.path >= 0)
                events->reached_node = {std::uint16_t(r.path), r.nodes[std::size_t(idx)]};
        }
        r.special_pending = false;
    }
    if (!r.ignore_bounds) {
        bool pass = true;
        if (!bounds_.empty()) {
            pass = bounds_node_test(feet, {wp, r.waypoint.cel}, 0.2f);
            if (sqdist2d(feet.pos, wp) < 1.0f) pass = true;
        }
        if (!pass) return creep_decrement(r, feet);
    }
    return creep_increment(r, feet);
}

RouteStatus NavNetwork::follow_route(NavRoute& r, const CelPos& feet, NavMove* events) {
    // AINetwork_FollowRoute @0x15b110.
    if (r.path < 0) return RouteStatus::NoPath;
    float dist = route_distance(r, feet.pos);
    r.distance = dist;
    update_links_used_count(r);
    if (r.index < int(r.nodes.size()) || r.mode() == RouteMode::Loop || r.radius <= dist) {
        bool ok = creep_handler(r, feet, events);
        r.status = ok ? RouteStatus::Following : (r.flag2 == 0 ? RouteStatus::CreepFailed9 : RouteStatus::CreepFailed);
    } else {
        r.status = RouteStatus::Arrived;
    }
    return r.status;
}

// ---- route calculation --------------------------------------------------------------------------

void NavNetwork::optimise_route(NavRoute& r, bool trim_start, bool trim_end) {
    // AINetwork_OptimiseRoute @0x156760: drop the first / last node when the start / goal can reach the
    // second / second-to-last node directly (never across two door or two kick nodes).
    if (r.path < 0) return;
    NavPath& p = paths_[std::size_t(r.path)];
    auto allowed = [&](std::uint16_t x, std::uint16_t y) {
        std::uint32_t fx = p.nodes[x].flags, fy = p.nodes[y].flags;
        if ((fx & nodeflag::kDoor) && (fy & nodeflag::kDoor)) return false;
        return !((fx & nodeflag::kKick) && (fy & nodeflag::kKick));
    };
    std::size_t count = r.nodes.size();
    int dropped = 0;
    if (trim_start && count > 1 && r.nodes[0] < p.nodes.size() && r.nodes[1] < p.nodes.size() &&
        allowed(r.nodes[0], r.nodes[1]) &&
        move_test_to_node(r.start, {std::uint16_t(p.index), r.nodes[1]}, nullptr, true) == 1) {
        dropped = 1;
        r.first_node = r.nodes[1];
    }
    if (trim_end && count > std::size_t(dropped) + 1 && allowed(r.nodes[count - 1], r.nodes[count - 2]) &&
        move_test_to_node(r.goal, {std::uint16_t(p.index), r.nodes[count - 2]}, nullptr, true) == 1) {
        --count;
        r.dest_node = r.nodes[count - 1];
    }
    r.nodes.resize(count);
    r.nodes.erase(r.nodes.begin(), r.nodes.begin() + dropped);
    // Exact remaining length (the original's link lookup here is approximate; FollowRoute recomputes it).
    Vec3 first = r.nodes.empty() ? r.goal.pos : p.nodes[r.nodes.front()].pos;
    float total = dist2d(r.start.pos, first);
    for (std::size_t i = 1; i < r.nodes.size(); ++i) {
        int l = p.link_between(r.nodes[i - 1], r.nodes[i]);
        if (l >= 0) total += p.links[std::size_t(l)].length;
    }
    if (!r.nodes.empty()) total += dist2d(r.goal.pos, p.nodes[r.nodes.back()].pos);
    r.distance = total;
}

RouteStatus NavNetwork::calc_route(NavRoute& r, const NavTarget* target, const CelPos& feet) {
    // AINetwork_CalcRoute @0x156ba8.
    reset_route(r);
    if (!target) return r.status = RouteStatus::CannotCalc;
    r.start = feet;
    if (r.start.cel == kNoCel) r.start.cel = find_cel(feet.pos);
    r.target_stamp = target->stamp;
    if (!(r.flags & NavRoute::kSkipStraight) && move_test(r.start, r.goal, nullptr, true) == 1) {
        reset_route(r);
        r.status = RouteStatus::Straight;
        r.target_stamp = target->stamp;
        link_creep_calc_to_route_end(r);
        r.flags |= NavRoute::kValid;
        return r.status;
    }
    if (r.path < 0 || paths_[std::size_t(r.path)].nodes.empty()) return r.status = RouteStatus::CannotCalc;
    RouteStatus st = do_astar(r, *target);
    r.status = st;
    if (st == RouteStatus::Following || st == RouteStatus::Approximate) {
        optimise_route(r, true, true);
        r.flags |= NavRoute::kValid;
        link_creep_calc(r, false);
    }
    return st;
}

bool NavNetwork::alter_dest_for_nearest(NavRoute& r) {
    // AINetwork_AlterDestFor_DROUTE_Nearest @0x156088.
    if (r.nodes.empty() || r.path < 0) return false;
    NodeRef last{std::uint16_t(r.path), r.nodes.back()};
    Vec3 hit{};
    if (move_test_from_node(last, r.goal, &hit) != -1) return false;
    Vec3 d = hit - paths_[std::size_t(r.path)].nodes[last.node].pos;
    float len = length(d);
    if (len > 0) d = d * (1.0f / len);
    r.goal.pos = hit - d;
    r.goal.cel = find_cel(r.goal.pos);
    r.flags |= NavRoute::kDestAltered;
    return true;
}

// ---- NavAgent -----------------------------------------------------------------------------------

bool NavAgent::set_path_for(const Vec3& pos, int cel) {
    if (cel == kNoCel) cel = net_->find_cel(pos);
    path_ = net_->nav_path_for_position({pos, cel}, false);
    move_route_.path = path_;
    return path_ >= 0;
}

void NavAgent::build_target(const CelPos& goal_pos, bool object) {
    target_.mask = NavTarget::kDroneMask;
    target_.object = object;
    target_.goal = goal_pos;
    target_.stamp = 0;
    net_->nodes_for_position(0, target_);
}

void NavAgent::set_goal_position(const Vec3& pos, float radius) {
    point_radius_ = radius == 0 ? 2.0f : radius;
    point_goal_ = net_->locate(pos);
    goal_set_ = true;
    goal_is_object_ = goal_is_player_ = false;
    build_target(point_goal_, false);
}

void NavAgent::set_goal_object(const Vec3& pos, float radius) {
    point_radius_ = radius == 0 ? 2.0f : radius;
    point_goal_ = net_->locate(pos);
    goal_set_ = true;
    goal_is_object_ = true;
    goal_is_player_ = false;
    build_target(point_goal_, true);
}

void NavAgent::update_goal_object(const Vec3& pos) {
    point_goal_ = net_->locate(pos);
    if (goal_is_player_) {
        net_->update_player_target(pos);
    } else {
        target_.goal = point_goal_;
        net_->build_target(target_);   // AINetwork_BuildAITarget: only when never built or moved >= 2.0
    }
}

void NavAgent::set_goal_player(const Vec3& pos, float radius) {
    point_radius_ = radius == 0 ? 2.0f : radius;
    goal_set_ = true;
    goal_is_object_ = goal_is_player_ = true;
    point_goal_ = net_->locate(pos);
    net_->update_player_target(pos);
}

void NavAgent::clear_goal() {
    goal_set_ = goal_is_object_ = goal_is_player_ = false;
    invalidate_route();
}

RouteStatus NavAgent::calc_route_to_goal(const Vec3& feet, int cel) {
    // AINetwork_CalcRouteToPosition @0x15afe0 / CalcRouteToObject @0x15aee8.
    NavRoute& r = move_route_;
    if (!goal_set_) return r.status = RouteStatus::CannotCalc;
    r.path = path_;
    NavTarget* tgt = goal_is_player_ ? &net_->player_target() : &target_;
    CelPos f{feet, cel == kNoCel ? net_->find_cel(feet) : cel};
    bool keep;
    if (goal_is_object_) {
        keep = r.valid() && tgt->stamp != 0 && tgt->stamp <= r.target_stamp;
    } else {
        keep = r.valid() && !target_.object && sqdist2d(point_goal_.pos, target_.goal.pos) <= 1.0f;
    }
    if (!keep) {
        // SetupRouteToPosition / SetupRouteToObject @0x158b10 / 0x158948.
        net_->reset_route(r);
        r.flags &= std::uint16_t(~(NavRoute::kValid | NavRoute::kDestAltered));
        if (point_goal_.cel == kNoCel) point_goal_.cel = net_->find_cel(point_goal_.pos);
        if (point_goal_.cel == kNoCel) return r.status = RouteStatus::CannotCalc;
        if (goal_is_player_) {
            r.goal = tgt->goal;
        } else {
            if (goal_is_object_) target_.goal = point_goal_; else build_target(point_goal_, false);
            if (goal_is_object_ && target_.stamp == 0) net_->nodes_for_position(0, target_);
            r.goal = point_goal_;
        }
        r.radius = point_radius_;
    }
    RouteStatus st = net_->calc_route(r, tgt, f);
    if (st == RouteStatus::Approximate) net_->alter_dest_for_nearest(r);
    return st;
}

NavMove NavAgent::move_to_goal(const Vec3& feet, int cel) {
    // NDrone2_MoveToGoalPosition @0x1517d8 (+ NDrone2_FollowRoute @0x155be0).
    NavMove out;
    NavRoute& r = move_route_;
    CelPos f{feet, cel == kNoCel ? net_->find_cel(feet) : cel};
    if (goal_is_object_ && r.valid()) {   // AINetwork_VerifyRouteTarget: a rebuilt target makes the route stale
        const NavTarget& t = goal_is_player_ ? net_->player_target() : target_;
        if (t.stamp > r.target_stamp) invalidate_route();
    }
    if (!r.valid()) {
        RouteStatus st = calc_route_to_goal(feet, f.cel);
        if (st != RouteStatus::Following && st != RouteStatus::Approximate && st != RouteStatus::Straight) {
            out.status = st;
            out.distance = net_->route_distance(r, feet);
            return out;
        }
    }
    float d = net_->route_distance(r, feet);
    r.distance = d;
    out.distance = d;
    out.waypoint = r.waypoint.pos;
    if (d < arrive_radius_) {
        out.status = RouteStatus::Arrived;
        return out;
    }
    RouteStatus st = net_->follow_route(r, f, &out);
    out.waypoint = r.waypoint.pos;
    if (st == RouteStatus::Arrived) out.status = RouteStatus::Arrived;
    else if (st == RouteStatus::CreepFailed) out.status = RouteStatus::CreepFailed;
    else if (st == RouteStatus::NoPath) out.status = RouteStatus::NoPath;
    else out.status = RouteStatus::Following;
    return out;
}

std::uint16_t NavAgent::nearest_node(const Vec3& feet, int cel) {
    if (nearest_cache_ == kNoNode && path_ >= 0)
        nearest_cache_ = net_->nearest_node({feet, cel == kNoCel ? net_->find_cel(feet) : cel}, path_);
    return nearest_cache_;
}

bool NavAgent::distance_to_emitter(const Vec3& feet, const NavEmitter& e, float* out) {
    return net_->distance_to_emitter(net_->locate(feet), path_, nearest_cache_, e, out);
}

// ---- patrol / mission routes --------------------------------------------------------------------

bool NavAgent::assign_ai_path(const Vec3& feet, int cel, std::uint32_t path_mask) {
    // NDrone2_AssignAIPath @0x154190 + NDrone2_InitAIPath @0x153720.
    if (cel == kNoCel) cel = net_->find_cel(feet);
    if (cel == kNoCel) return false;
    NodeRef best;
    float best_d = 0.3f;
    for (NodeRef ref : net_->cels()[std::size_t(cel)].nodes) {
        const NavPath& p = net_->paths()[ref.path];
        if (!(p.flags & path_mask)) continue;
        float d = dist2d(feet, p.nodes[ref.node].pos);
        if (d < best_d) {
            best_d = d;
            best = ref;
        }
    }
    if (!best.valid()) return false;

    const NavPath& p = net_->paths()[best.path];
    NavRoute& r = mission_route_;
    net_->reset_route(r);
    std::uint16_t keep80 = r.flags & NavRoute::kSkipStraight;
    r.flags = std::uint16_t(keep80 | ((p.flags & pathflag::kPatrol) ? 2 : 1));
    if (p.flags & pathflag::kRouteFlag20) r.flags |= NavRoute::kFlag20;
    r.path = p.index;

    // Walk the chain from the start node (InitAIPath): follow the degree-2 links away from the node we came from.
    std::uint16_t prev = kNoNode;
    std::uint16_t cur = best.node;
    for (std::size_t i = 0; i < p.nodes.size(); ++i) {
        r.nodes.push_back(cur);
        const NavNode& n = p.nodes[cur];
        if (n.adj_count == 2) {
            const NavLink& l1 = p.links[p.adjacency[n.adj_first]];
            const NavLink& l2 = p.links[p.adjacency[n.adj_first + 1]];
            const NavLink* use = &l1;
            if (l1.a == prev || l1.b == prev) {
                use = &l2;                          // the first link leads back: take the other
            } else if ((p.flags & pathflag::kPatrol) && prev == kNoNode && !(p.flags & pathflag::kRouteFlag20)) {
                use = &l2;                          // patrol start: the second link unless path flag 0x10
            }
            prev = cur;
            cur = use->a == cur ? use->b : use->a;
        } else if (p.flags & pathflag::kMission) {
            if (n.adj_count == 1 && r.nodes.size() == 1) {   // mission path starting at an end
                const NavLink& l = p.links[p.adjacency[n.adj_first]];
                prev = cur;
                cur = l.a == cur ? l.b : l.a;
            } else {
                break;                              // mission paths stop at the far end / a junction
            }
        }
        // other degree: the original re-adds the same node until `nnodes` entries exist
    }
    r.index = 0;
    r.start = {feet, cel};
    if (r.nodes.size() < 2) {
        r.goal = {p.nodes[best.node].pos, p.nodes[best.node].cel};
    } else {
        const Vec3& last = p.nodes[r.nodes[r.nodes.size() - 1]].pos;
        Vec3 dir = last - p.nodes[r.nodes[r.nodes.size() - 2]].pos;
        float len = length(dir);
        if (len > 0) dir = dir * (0.5f / len);
        r.goal = {last + dir, p.nodes[r.nodes.back()].cel};
        r.radius = 0.5f;
    }
    if (r.nodes.size() < 2) r.radius = 0.5f;   // drone+0x9fc = 0.5 (NDrone2_DefaultInit)
    r.target_stamp = net_->frame();
    r.flags |= NavRoute::kValid;
    r.status = RouteStatus::Following;
    return net_->link_creep_calc(r, false);
}

NavMove NavAgent::follow_ai_path(const Vec3& feet, int cel) {
    // NDrone2_FollowRoute @0x155be0 on the mission route.
    NavMove out;
    NavRoute& r = mission_route_;
    CelPos f{feet, cel == kNoCel ? net_->find_cel(feet) : cel};
    RouteStatus st = net_->follow_route(r, f, &out);
    out.waypoint = r.waypoint.pos;
    out.distance = r.distance;
    if (st == RouteStatus::Following && r.nodes.empty()) st = RouteStatus::Straight;
    out.status = st;
    return out;
}

float NavAgent::mission_path_distance(const CelPos& feet, float radius) const {
    const NavRoute& r = mission_route_;
    if (r.nodes.empty() || r.path < 0) return 0;
    const NavPath& p = net_->paths()[std::size_t(r.path)];
    int best = -1;
    float best_sq = radius * radius;
    for (std::size_t i = 0; i < r.nodes.size(); ++i) {
        const NavNode& n = p.nodes[r.nodes[i]];
        float d = sqdist2d(n.pos, feet.pos);
        if (best >= 0 && !(d < best_sq)) continue;
        if (net_->move_test(feet, {n.pos, n.cel}) == 1) {
            best = int(i);
            best_sq = d;
        }
    }
    return best < 0 ? 0.0f : net_->route_distance_between(r, r.index, best);
}

NodeRef NavAgent::mission_node() const {
    const NavRoute& r = mission_route_;
    if (r.path < 0 || r.nodes.empty()) return {};
    int idx = std::clamp(r.index, 0, int(r.nodes.size()) - 1);
    return {std::uint16_t(r.path), r.nodes[std::size_t(idx)]};
}

}  // namespace nf

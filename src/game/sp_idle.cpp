#include <cmath>

#include "game/sp_idle.hpp"

namespace nf::sp {

using namespace nf::drone;

bool stand_idle_anim(Drone& d, bool timed) {
    // DroneAnim_SetStandIdleAnim 0x13cad0
    IdleSlot& s = sx(d).slot<IdleSlot>();
    if (timed && d.now() < s.next_idle_anim) return false;
    int dasc = kStandIdle1;   // 0x24
    std::uint32_t delay = 0;
    const int cur = d.smi.cur;
    if (cur == kStIdle) {
        dasc = kStandIdle1 + int(d.sys->rand_int(3));
        delay = d.seconds(float(5 + d.sys->rand_int(5)));
    } else if (cur == kStPartyGirl && d.has_beh(0x47)) {
        dasc = kStandIdle1 + int(d.sys->rand_int(4));
        delay = d.seconds(float(5 + d.sys->rand_int(5)));
    }
    if (d.char_class == 0x10) dasc = kStandIdle3;   // 0x26
    if (dasc != kStandIdle1 && !anim_can_do(d, dasc)) dasc = kStandIdle1;
    anim_call(d, 0, dasc);
    s.next_idle_anim = d.now() + delay;
    return true;
}

bool walk_idle_anim(Drone& d, bool timed) {
    // DroneAnim_SetWalkIdleAnim 0x13cc30
    IdleSlot& s = sx(d).slot<IdleSlot>();
    if (timed && d.now() < s.next_idle_anim) return false;
    int dasc = kWalkIdle1;   // 0x37
    if (d.smi.cur == kStPatrol) dasc = kWalkIdle1 + int((d.id + 1) % 3);
    if (d.sys->config().level_id == 0x7000014) dasc = kWalkIdle3;   // 0x39
    if (dasc != kWalkIdle1 && !anim_can_do(d, dasc)) dasc = kWalkIdle1;
    anim_call(d, 0, dasc);
    s.next_idle_anim = d.now();
    return true;
}

int update_patrol_route(Drone& d) {
    // NDrone2_UpdatePatrolRoute 0x1541?: FollowRoute on the mission route, then the walk animation
    const int status = follow_ai_path(d);
    const bool alert_walk = d.has_beh(beh::kPatrolAlert) && d.alertness >= 0.66f;
    if (status == int(RouteStatus::NoPath)) {
        stand_idle_anim(d, true);
    } else if (status == int(RouteStatus::Following)) {
        if (!alert_walk) walk_idle_anim(d, true);
        else anim_call(d, 0, kWalkAlert);
    } else {
        if (!alert_walk) walk_idle_anim(d, true);
        else anim_call(d, 0, kStandAlert);
        return 1;
    }
    return status;
}

void patrol_talk(Drone& d) {
    // NDrone2_PatrolTalk 0x1?: guards of class 3 mutter now and then (rate limited by NPCGlobals+0x1b4)
    if (d.health <= 0 || d.sys->config().multiplayer) return;
    IdleSlot& s = sx(d).slot<IdleSlot>();
    (void)s;
    if (d.char_class != 3) return;
    static thread_local std::uint32_t next_talk = 0;   // NPCGlobals+0x1b4: one voice line per 10 s over all drones
    if (d.now() < next_talk) return;
    if (d.sys->rand_int(2) != 0) {
        next_talk = d.now() + d.seconds(5.0f);
        return;
    }
    next_talk = d.now() + d.seconds(10.0f);
    talk(d, Speech::Patrol);
}

void play_script(Drone& d, std::uint32_t script, int dasc, bool loop, int end_msg) {
    Drone::Anim& a = d.anim;
    a.req_state = 0;
    a.cur_state = dasc;
    a.script = script;
    a.loop = loop;
    a.applied = true;
    a.end_state = 0;
    a.end_msg = loop ? 0 : end_msg;
    const bool ok = d.character && d.character->blend_to(script, 8.0f, loop);
    a.clip_running = ok && !loop;
    if (!ok) anim_call(d, 0, dasc, 0, 0, end_msg);   // script not in this skeleton's bank: stand in the requested pose
}


void set_ai_goal(Drone& d, const Vec3& pos, float radius) {
    // NDrone2_SetSoundAlertRoute 0x1541c0: AI goal = alert position; the movement route restarts.
    SpExt::AiGoal& g = sx(d).ai_goal;
    g.set = true;
    g.pos = pos;
    g.radius = radius <= 0.0f ? 2.0f : radius;
    if (d.nav) {
        d.nav->set_goal_position(pos, g.radius);
        d.nav->invalidate_route();
    }
    d.mv.goal_is_object = false;
}

const SpExt::AiGoal& ai_goal(const Drone& d) {
    return static_cast<const SpExt*>(d.ext.get())->ai_goal;
}

int move_to_ai_goal(Drone& d) {
    // NDrone2_MoveToGoalPosition on the stored AI goal (GoToGoalPosition / AlertToPosition TICK).
    const SpExt::AiGoal& g = ai_goal(d);
    if (!g.set) return int(RouteStatus::CannotCalc);
    return move_to_goal_position(d, g.pos, g.radius);
}

float distance_to_ai_point(const Drone& d) {
    // NDrone2_DistanceToAIPoint: 2-D feet distance to the AI goal.
    const SpExt::AiGoal& g = ai_goal(d);
    if (!g.set) return 1e9f;
    const Vec3 f = d.nav_pos();
    const float dx = f[0] - g.pos[0], dz = f[2] - g.pos[2];
    return std::sqrt(dx * dx + dz * dz);
}

RefindChoice choose_mission_node(const std::vector<Vec3>& nodes, const Vec3& feet, bool stationary, bool has_opp,
                                 const Vec3& opp, const std::function<int(const Vec3&)>& find_cel,
                                 const std::function<int(const CelPos&, const CelPos&)>& move_test,
                                 const std::function<int(const Vec3&, float)>& move_to_goal) {
    // NDrone2_ReFindMissionPath 0x1542c0 decision core: full scan over the mission-route nodes (the
    // original never early-exits: every node gets a MoveTest unless the drone is stationary). Empty route
    // = opponent goal with no MoveToGoal call. The setup path (nothing reachable) runs MoveToGoalPosition
    // on the nearest node: verdicts 0-3 take SetState(99,0), 4+ falls through to the opponent goal.
    RefindChoice out;
    if (nodes.empty()) {
        if (has_opp) {
            out.pos = opp;
            out.radius = 2.0f;
            out.has_choice = true;
        }
        return out;
    }
    const CelPos from{feet, find_cel(feet)};
    float reach_d2 = 1e18f, near_d2 = 1e18f;
    std::size_t reach_pos = 0, near_pos = 0;
    Vec3 reach{}, near{};
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const Vec3& n = nodes[i];
        const float dx = feet[0] - n[0], dz = feet[2] - n[2];
        const float d2 = dx * dx + dz * dz;
        if (d2 < near_d2) {
            near_d2 = d2;
            near_pos = i;
            near = n;
        }
        if (stationary) continue;
        if (move_test(from, {n, find_cel(n)}) == 1 && d2 < reach_d2) {
            reach_d2 = d2;
            reach_pos = i;
            reach = n;
        }
    }
    if (reach_d2 < 1e18f) {
        // Move path: the route is (re)built by MoveToGoalPosition; its verdict is ignored, never SetState.
        out.move_goal_called = true;
        move_to_goal(reach, 2.0f);
        out.found = true;
        out.selected = true;
        out.has_choice = true;
        out.position = reach_pos;
        out.pos = reach;
        out.radius = 2.0f;
        return out;
    }
    out.move_goal_called = true;
    if (move_to_goal(near, 1.0f) <= 3) {
        out.selected = true;
        out.has_choice = true;
        out.position = near_pos;
        out.pos = near;
        out.radius = 1.0f;
        out.goto_goal_state = true;
        return out;
    }
    if (has_opp) {
        out.pos = opp;
        out.radius = 2.0f;
        out.has_choice = true;
    }
    return out;
}

void refind_mission_path(Drone& d) {
    // NDrone2_ReFindMissionPath 0x1542c0: re-anchor the mission route at the nearest reachable node and
    // point the AI goal at it (radius 2.0), so the GoToGoalPosition transition walks back onto the path.
    // With no reachable node the goal is the nearest node anyway (radius 1.0) plus the GoToGoalPosition
    // state when the route rebuild succeeds (verdict 0-3), else the player position (radius 2.0). With no
    // mission route at all the goal is the player position. The mission index tracks the selected array
    // position (EE +0x9f0); with no selection it is left alone.
    if (!d.nav || !d.sys->nav()) return;
    NavNetwork& net = *d.sys->nav();
    const NavRoute& mr = d.nav->mission_route();
    if (!mr.valid() || mr.path < 0 || std::size_t(mr.path) >= net.paths().size() || mr.nodes.empty()) {
        if (d.opponent.valid()) set_ai_goal(d, target_pos(*d.sys, d.opponent), 2.0f);
        return;
    }
    const NavPath& path = net.paths()[std::size_t(mr.path)];
    std::vector<Vec3> nodes;
    nodes.reserve(mr.nodes.size());
    for (const std::uint16_t id : mr.nodes) {
        if (id >= path.nodes.size()) continue;
        nodes.push_back(path.nodes[id].pos);
    }
    const Vec3 f = d.nav_pos();
    const bool stationary = (d.flags & flag::kStationary) != 0;
    const bool has_opp = d.opponent.valid();
    const Vec3 opp = has_opp ? target_pos(*d.sys, d.opponent) : Vec3{};
    RefindChoice c = choose_mission_node(
        nodes, f, stationary, has_opp, opp, [&](const Vec3& p) { return net.find_cel(p); },
        [&](const CelPos& a, const CelPos& b) { return net.move_test(a, b); },
        [&](const Vec3& p, float r) { return move_to_goal_position(d, p, r); });
    if (!c.has_choice) return;
    if (c.selected) d.nav->mission_route().index = int(c.position);
    set_ai_goal(d, c.pos, c.radius);
    if (c.goto_goal_state) d.set_state(st::kStGoToGoalPosition, 0);
}
}  // namespace nf::sp

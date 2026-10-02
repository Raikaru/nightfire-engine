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
        dasc = kStandIdle1 + int(d.sys->rand() % 3);
        delay = d.seconds(float(5 + d.sys->rand() % 5));
    } else if (cur == kStPartyGirl && d.has_beh(0x47)) {
        dasc = kStandIdle1 + int(d.sys->rand() % 4);
        delay = d.seconds(float(5 + d.sys->rand() % 5));
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
    if (d.sys->rand() % 2 != 0) {
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

void refind_mission_path(Drone& d) {
    // NDrone2_ReFindMissionPath: re-anchor the patrol at the mission-route node nearest the drone and
    // make it the AI goal, so the GoToGoalPosition transition walks back onto the path.
    if (!d.nav || !d.sys->nav()) return;
    const NavNetwork& net = *d.sys->nav();
    const NavRoute& mr = d.nav->mission_route();
    if (!mr.valid() || mr.path < 0 || std::size_t(mr.path) >= net.paths().size()) return;
    const NavPath& path = net.paths()[std::size_t(mr.path)];
    const Vec3 f = d.nav_pos();
    const NavNode* best = nullptr;
    float best_d2 = 1e18f;
    for (const std::uint16_t id : mr.nodes) {
        if (id >= path.nodes.size()) continue;
        const NavNode& n = path.nodes[id];
        const float dx = f[0] - n.pos[0], dz = f[2] - n.pos[2];
        const float d2 = dx * dx + dz * dz;
        if (d2 < best_d2) {
            best_d2 = d2;
            best = &n;
        }
    }
    if (best) set_ai_goal(d, best->pos, 2.0f);
}
}  // namespace nf::sp

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

}  // namespace nf::sp

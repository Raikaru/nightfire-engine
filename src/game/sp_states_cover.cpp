// Cover states: RunForCover, UnderCover* (docs/spec-arena-ai.md Part 3 §7).
// Drone_ProcessCoverNodes / Drone_IsCoverNodeUsable gate nodes on the switch channels;
// NDrone2_FindCover 0x152548 picks the free node with the minimum path cost.
#include <cmath>

#include "game/sp_civilian_util.hpp"
#include "game/sp_common.hpp"
#include "game/sp_idle.hpp"
#include "game/sp_states.hpp"

namespace nf::sp {

using namespace nf::drone;

namespace {

int skeleton(Drone& d, const Msg& m) { return skel_common(d, m, kStAttack) ? 1 : 0; }


bool at_cover(const Drone& d) {
    const int node = static_cast<const SpExt*>(d.ext.get())->cover_node;
    if (node < 0) return false;
    const Vec3& p = sp_of(const_cast<Drone&>(d)).level().cover_nodes[std::size_t(node)].pos;
    const Vec3 f = d.feet();
    return std::hypot(f[0] - p[0], f[2] - p[2]) < 1.5f;
}

// ---- RunForCover 0x8c (0x169150) -------------------------------------------------------------
int state_run_for_cover(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.flags |= flag::kAware;
        if (!find_cover(d)) {   // no free node: stand and fight
            d.set_state(kStCombatNoMove);
            return 1;
        }
        anim_call(d, 0, kRun);
        return 1;
    case kMsgTick: {
        const int next = enemy_look_for_opponent(d);
        if (next != 0 && (d.sight_flags & sight::kSeen)) {
            // Seen while running: keep running (the original does not divert here).
        }
        (void)next;
        const int status = move_to_ai_goal(d);
        set_angle_to_dest(d);
        if (status == int(RouteStatus::Following) || status == int(RouteStatus::Straight)) {
            anim_call(d, 0, kRun);
            return 1;
        }
        d.set_state(kStUnderCoverInit);
        return 1;
    }
    default:
        return skeleton(d, m);
    }
}

// ---- UnderCoverInit 0x93: crouch into the node ---------------------------------------------------
int state_under_cover_init(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.flags |= flag::kAware;
        anim_call(d, 0, kCrouchCover, 0, kStUnderCoverIdle);
        return 1;
    case kMsgTick:
        return 1;
    default:
        return skeleton(d, m);
    }
}

// ---- UnderCoverIdle 0x94: head down, pop up on a timer ----------------------------------------------
int state_under_cover_idle(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.flags |= flag::kAware;
        anim_call(d, 0, kCrouchCover);
        d.timer1 = {d.now() + d.seconds(2.0f), 0};
        return 1;
    case kMsgTimer1:
        d.set_state(at_cover(d) ? kStUnderCoverAim : kStUnderCoverLeave);
        return 1;
    case kMsgTick:
        if (d.opponent.valid() && (d.sight_flags & sight::kSeen) && d.opp_dist < 4.0f)
            d.set_state(kStUnderCoverLeaveNow);   // flushed out at close range
        return 1;
    default:
        return skeleton(d, m);
    }
}

// ---- UnderCoverAim 0x95 / UnderCoverFire 0x96 ----------------------------------------------------------
int state_under_cover_aim(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.flags |= flag::kAware;
        set_angle_to_obj(d, d.opponent, 0.0f, true);
        anim_call(d, 0, kAimCrouch);
        d.timer1 = {d.now() + d.seconds(1.0f), 0};
        return 1;
    case kMsgTimer1:
        d.set_state(kStUnderCoverFire);
        return 1;
    case kMsgTick:
        set_angle_to_obj(d, d.opponent, 0.0f);
        return 1;
    default:
        return skeleton(d, m);
    }
}

int state_under_cover_fire(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.flags |= flag::kAware;
        set_angle_to_obj(d, d.opponent, 0.0f, true);
        anim_call(d, 0, kAimCrouch);
        return 1;
    case kMsgTick:
        set_angle_to_obj(d, d.opponent, 0.0f);
        d.fire_requested = !d.fire_lock;   // DroneWeap_HandleFiring fires post-move, like the original
        if (d.burst_done) {
            d.burst_done = false;
            d.set_state(kStUnderCoverIdle);   // back down after the burst
        }
        return 1;
    default:
        return skeleton(d, m);
    }
}

// ---- UnderCoverSniperFire 0x97 / SniperReload 0x98: the long-gun variant ----------------------------------
int state_under_cover_sniper_fire(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.flags |= flag::kAware;
        set_angle_to_obj(d, d.opponent, 0.0f, true);
        anim_call(d, 0, kShoot);
        return 1;
    case kMsgAnimResume:
        d.set_state(kStUnderCoverSniperReload);
        return 1;
    case kMsgTick:
        d.fire_requested = !d.fire_lock;   // DroneWeap_HandleFiring fires post-move, like the original
        return 1;
    default:
        return skeleton(d, m);
    }
}

int state_under_cover_sniper_reload(Drone& d, const Msg& m) {
    if (m.id == kMsgNone) return 1;
    if (m.id == kMsgEnter) {
        anim_call(d, 0, kReload, 0, kStUnderCoverIdle);
        return 1;
    }
    if (m.id == kMsgTick) return 1;
    return skeleton(d, m);
}

// ---- UnderCoverReturn 0x99: back to the node after TypeChange -----------------------------------------------
int state_under_cover_return(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        anim_call(d, 0, kCrouchCover, 0, kStUnderCoverIdle);
        return 1;
    case kMsgTick:
        return 1;
    default:
        return skeleton(d, m);
    }
}

// ---- UnderCoverTypeChange 0x9a: corner <-> low stance ----------------------------------------------------------
int state_under_cover_type_change(Drone& d, const Msg& m) {
    if (m.id == kMsgNone) return 1;
    if (m.id == kMsgEnter) {
        anim_call(d, 0, kCrouchCoverLook, 0, kStUnderCoverReturn);
        return 1;
    }
    if (m.id == kMsgTick) return 1;
    return skeleton(d, m);
}

// ---- UnderCoverLeave 0x9b / LeaveNow 0x9c --------------------------------------------------------------------------
int state_under_cover_leave(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        release_cover(d);
        anim_call(d, 0, kCStand, 0, kStCombat);
        return 1;
    case kMsgTick:
        return 1;
    default:
        return skeleton(d, m);
    }
}

int state_under_cover_leave_now(Drone& d, const Msg& m) {
    if (m.id == kMsgNone) return 1;
    if (m.id == kMsgEnter || m.id == kMsgTick) {
        release_cover(d);
        d.set_state(kStCombatNoMove);
        return 1;
    }
    return skeleton(d, m);
}

}  // namespace

void register_cover_states() {
    using drone::register_state;
    register_state(kStRunForCover, "RunForCover", state_run_for_cover);
    register_state(kStUnderCoverInit, "UnderCoverInit", state_under_cover_init);
    register_state(kStUnderCoverIdle, "UnderCoverIdle", state_under_cover_idle);
    register_state(kStUnderCoverAim, "UnderCoverAim", state_under_cover_aim);
    register_state(kStUnderCoverFire, "UnderCoverFire", state_under_cover_fire);
    register_state(kStUnderCoverSniperFire, "UnderCoverSniperFire", state_under_cover_sniper_fire);
    register_state(kStUnderCoverSniperReload, "UnderCoverSniperReload", state_under_cover_sniper_reload);
    register_state(kStUnderCoverReturn, "UnderCoverReturn", state_under_cover_return);
    register_state(kStUnderCoverTypeChange, "UnderCoverTypeChange", state_under_cover_type_change);
    register_state(kStUnderCoverLeave, "UnderCoverLeave", state_under_cover_leave);
    register_state(kStUnderCoverLeaveNow, "UnderCoverLeaveNow", state_under_cover_leave_now);
}

}  // namespace nf::sp

// NDrone2_DSTATE_Bot* (docs/spec-arena-ai.md Part 2A §2): the bot state handlers 0xc3..0xf9 registered with the
// shared drone state machine (game/drone_sm.hpp). Addresses in the comments are the original functions.

#include <cmath>

#include "game/bot_brain.hpp"
#include "game/drone_sm.hpp"

namespace nf::bots {

namespace {

using drone::Drone;
using drone::Msg;
using namespace drone;   // kMsg* ids

constexpr float kHalfPi = 1.5707964f;
constexpr float kPi = 3.14159265f;

BotBrain& B(Drone& d) { return *d.ext_as<BotBrain>(); }

void go(Drone& d, int state) {
    if (state != 0) d.set_state(state);
}
void set_timer1(Drone& d, std::uint32_t ticks) { d.timer1 = {d.now() + ticks, 0}; }
void set_timer2(Drone& d, std::uint32_t ticks) { d.timer2 = {d.now() + ticks, 0}; }
std::uint32_t seconds(Drone& d, float s) { return d.seconds(s); }

float wrap_pi(float a) {
    while (a > kPi) a -= 2 * kPi;
    while (a < -kPi) a += 2 * kPi;
    return a;
}

bool never_moves(const Drone& d) {   // Drone+0x4f8 & 0x10 (stationary) or behaviour property 0x40
    return (d.flags & flag::kStationary) != 0 || d.has_beh(beh::kNeverMovesInCombat);
}

bool bot_dead(const Drone& d) {
    return d.health <= 0 || (d.flags & flag::kDeadMask) != 0;
}

bool opponent_in_front(const Drone& d) { return std::fabs(d.opp_facing_b) < kHalfPi; }

// The "move towards the opponent" block shared by the attack states: skipped while recovering from a hit.
int chase_opponent(BotBrain& b) {
    if (b.v.bits & bitflag::kRecovering) return 0;
    return b.body->move_to_participant(b.opponent(), 2.0f, true);
}

// Result handling used by BotAttackFire / BotAttackCrouch after MoveToObject. Returns true when handled.
void move_result_or_combat_move(Drone& d, BotBrain& b, int r, bool arrived_goes_attack) {
    if (!b.validate_route(r)) {
        go(d, st::kAttackNoRoute);
        return;
    }
    if (r == moveres::kArrived) {
        b.body->invalidate_attack_route();
        if (arrived_goes_attack) {
            go(d, st::kAttack);
            return;
        }
    } else if (r < moveres::kArrived) {
        go(d, st::kAttackRun);
        return;
    } else if (r == moveres::kDoor) {
        b.door_return_state_ = st::kAttack;
        d.set_state(st::kDoorOpen);
        return;
    }
    go(d, b.choose_combat_move());
}

// ---------------------------------------------------------------------------------------------------------
// 0xc3 BotInit / 0xc4 BotRespawn / 0xc6..0xce personality stubs

int state_init(Drone& d, const Msg& m) {                 // NDrone2_DSTATE_BotInit @0x1748c8
    if (m.id == kMsgEnter) {
        BotBrain& b = B(d);
        b.body->invalidate_nearest_node();
        b.v.state_type = 0;
        go(d, st::kIdle);
    }
    return m.id == kMsgNone || m.id == kMsgEnter;
}

int state_respawn(Drone& d, const Msg& m) {              // NDrone2_DSTATE_BotRespawn @0x174938 -> BOT_respawn
    if (m.id == kMsgEnter) {
        BotBrain& b = B(d);
        if (b.on_respawn_request) b.on_respawn_request(b);
    }
    return m.id == kMsgNone || m.id == kMsgEnter;
}

int state_collector(Drone& d, const Msg& m) {            // NDrone2_DSTATE_BotCollector @0x174980 (never entered)
    if (m.id == kMsgEnter) {
        BotBrain& b = B(d);
        b.set_goal_pick_prefs(1, 1, 1, 0, 0, 0, 0);
        go(d, b.pick_goal(0) ? st::kGotoGoal : st::kIdle);
    }
    return m.id == kMsgNone || m.id == kMsgEnter;
}

int state_to_idle(Drone& d, const Msg& m) {              // BotGuardian .. BotAssassin, BotStuck, BotAttackNoOpponent, BotAttackNoRoute
    if (m.id == kMsgEnter) go(d, st::kIdle);
    return m.id == kMsgNone || m.id == kMsgEnter;
}

int state_no_route(Drone& d, const Msg& m) {             // NDrone2_DSTATE_BotAttackNoRoute @0x174c68
    if (m.id == kMsgEnter) {
        B(d).body->invalidate_attack_route();
        go(d, st::kIdle);
    }
    return m.id == kMsgNone || m.id == kMsgEnter;
}

int state_to_attack(Drone& d, const Msg& m) {            // BotSeenOpponent @0x175268, BotSeenDroneShot @0x1752b0, cover stubs
    if (m.id == kMsgEnter) go(d, st::kAttack);
    return m.id == kMsgNone || m.id == kMsgEnter;
}

int state_heard_noise(Drone&, const Msg& m) { return m.id == kMsgNone; }   // @0x1752f8

// ---------------------------------------------------------------------------------------------------------
// 0xf9 BotIdle @0x171b80

int state_idle(Drone& d, const Msg& m) {
    BotBrain& b = B(d);
    BotVars& v = b.v;
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        if (b.has_opponent() && (d.sight_flags & sight::kSeen)) {
            b.combat_weapon_change_choice(true, false);
            go(d, st::kAttack);
            return 1;
        }
        b.set_opponent(-1);
        set_timer1(d, 2);
        return 1;
    case kMsgTick: {
        v.trait_opponent = b.preferred_trait_opponent();
        if (v.trait_opponent != -1) {
            b.set_goal_pick_prefs(0, 0, 0, 0, 0, 0, 0);
            if (b.pick_goal(0)) {
                go(d, st::kGotoGoal);
                return 1;
            }
        }
        const std::uint32_t sc = b.env->scenario();
        if (v.in_zone && (sc == scenario::kProtection || sc == scenario::kDemolition)) {
            v.in_zone = b.env->in_defended_zone(v.slot);
            if (v.in_zone) v.alerted = true;
            return 1;
        }
        b.set_goal_pick_prefs(0, 0, 0, 1.0f, 1, 0, 0);
        if (b.pick_goal(1)) go(d, st::kGotoGoal);
        return 1;
    }
    case kMsgTimer1:
        d.call_anim(0, 0x24);
        return 1;
    default:
        return 0;
    }
}

// ---------------------------------------------------------------------------------------------------------
// 0xeb BotGotoGoalPosition @0x171240, 0xea BotAlertToPosition @0x171048, 0xf6 BotDoorOpen @0x1716d0

int state_goto_goal(Drone& d, const Msg& m) {
    BotBrain& b = B(d);
    BotVars& v = b.v;
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.call_anim(0, 0x1e);
        return 1;
    case kMsgTick: {
        const int ag = v.active_goal;
        if (ag < 0 || v.goal[std::size_t(ag)].type == 0) {
            if (v.pending_state != 0) {
                d.set_state(v.pending_state);
                v.pending_state = 0;
            } else {
                go(d, st::kIdle);
            }
            return 1;
        }
        BotGoal& g = v.goal[std::size_t(ag)];
        const int r = b.body->move_to_goal_position(b.speed_mul());
        g.last_result = r;
        if (!b.validate_route(r)) {
            if (g.type == goaltype::kPickup) b.set_pickup_visit_time(g.target);
            go(d, st::kIdle);
            return 1;
        }
        if (r == moveres::kArrived) return 1;   // BotGlobal.processGoals handles arrival
        if (r < moveres::kArrived) {
            d.call_anim(0, 0x1e);
        } else if (r == moveres::kDoor) {
            b.door_return_state_ = st::kGotoGoal;
            d.set_state(st::kDoorOpen);
        }
        return 1;
    }
    default:
        return 0;
    }
}

int state_alert_to_position(Drone& d, const Msg& m) {
    BotBrain& b = B(d);
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter: {
        std::uint32_t unit;
        if (b.body->can_do_anim_state(8)) {
            d.call_anim(0, 8);
            unit = 60;
        } else {
            d.call_anim(0, 0x1e);
            unit = seconds(d, 1.0f);
        }
        set_timer1(d, 5 * unit);
        return 1;
    }
    case kMsgTick: {
        if (d.sight_flags & sight::kSeen) {
            go(d, st::kAttack);
            return 1;
        }
        const int r = b.body->move_to_alert_position(1.0f);
        if (!b.validate_route(r)) {
            go(d, st::kAttackNoRoute);
        } else if (r == moveres::kArrived) {
            b.body->invalidate_attack_route();
            go(d, st::kAttack);
        } else if (r < moveres::kArrived) {
            go(d, st::kAttackRun);   // state 208 in the original; run towards the alert position
        } else if (r == moveres::kDoor) {
            b.door_return_state_ = st::kAttack;
            d.set_state(st::kDoorOpen);
        }
        return 1;
    }
    case kMsgTimer1:
        d.call_anim(0, 0x1e);
        return 1;
    case kMsgPatrolObstructed:
        go(d, st::kAttack);
        return 1;
    default:
        return 0;
    }
}

int state_door_open(Drone& d, const Msg& m) {
    BotBrain& b = B(d);
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        if (b.body->door_is_open()) {
            go(d, b.door_return_state_);
            return 1;
        }
        d.call_anim(0, d.alertness >= 1.0f ? 0x21 : 0x4b, d.alertness >= 1.0f ? 0 : 1);
        b.body->open_door();
        set_timer1(d, 2 * seconds(d, 1.0f));
        return 1;
    case kMsgTick:
        if (b.body->door_is_open()) go(d, b.door_return_state_);
        return 1;
    case kMsgTimer1:
        b.body->open_door();
        set_timer1(d, seconds(d, 1.0f));
        return 1;
    case kMsgAnimEvent:
        b.body->open_door();
        return 1;
    default:
        return 0;
    }
}

// ---------------------------------------------------------------------------------------------------------
// 0xf7 BotGuardFriendIdle @0x175680 / 0xf8 BotGuardFriendFollow @0x1718e0

int state_guard_idle(Drone& d, const Msg& m) {
    BotBrain& b = B(d);
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.call_anim(0, 0x22);
        return 1;
    case kMsgTick: {
        const int f = b.v.friend_slot;
        if (f < 0 || !b.env->participant(f).valid) {
            go(d, st::kIdle);
            return 1;
        }
        const OtherInfo& o = b.v.other_info(f);
        if ((o.flags & otherflag::kValid) && o.sq_dist > 20.25f) go(d, st::kGuardFriendFollow);
        return 1;
    }
    default:
        return 0;
    }
}

int state_guard_follow(Drone& d, const Msg& m) {
    BotBrain& b = B(d);
    BotVars& v = b.v;
    const auto run_or_walk = [&] {
        if (!d.call_anim(0, 8)) d.call_anim(0, 0x1e);
    };
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        b.init_goal(0, goaltype::kPlayer, nullptr, v.friend_slot, 0);
        b.goto_goal(0, st::kGuardFriendIdle);
        run_or_walk();
        return 1;
    case kMsgTick: {
        const int f = v.friend_slot;
        if (f < 0 || !b.env->participant(f).valid || v.active_goal < 0) {
            go(d, st::kIdle);
            return 1;
        }
        BotGoal& g = v.goal[std::size_t(v.active_goal)];
        const int r = b.body->move_to_goal_position(b.speed_mul());
        g.last_result = r;
        if (!b.validate_route(r)) return 1;
        if (r < moveres::kArrived) {
            run_or_walk();
            return 1;
        }
        if (r != moveres::kArrived) return 1;
        const OtherInfo& o = v.other_info(f);
        if (!(o.flags & otherflag::kValid) || o.sq_dist <= 20.25f) {
            go(d, st::kGuardFriendIdle);
            return 1;
        }
        b.body->invalidate_attack_route();
        b.init_goal(0, goaltype::kPlayer, nullptr, f, 0);
        b.goto_goal(0, st::kGuardFriendIdle);
        return 1;
    }
    default:
        return 0;
    }
}

// ---------------------------------------------------------------------------------------------------------
// Attack family 0xcf..0xdf

int state_attack(Drone& d, const Msg& m) {               // NDrone2_DSTATE_BotAttack @0x16f198
    BotBrain& b = B(d);
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.call_anim(0, 0x0a);
        b.body->invalidate_attack_route();
        if (b.env->rand(3) == 0) go(d, b.choose_combat_move());
        return 1;
    case kMsgTick: {
        if (!b.has_opponent()) {
            go(d, st::kAttackNoOpponent);
            return 1;
        }
        const bool stay = never_moves(d) || (d.lost_frames < 0x10 && d.opp_dist < d.engage_dist);
        if (!stay) {
            const int r = chase_opponent(b);
            if (!b.validate_route(r)) {
                go(d, st::kAttackNoRoute);
                return 1;
            }
            if (r == moveres::kArrived) {
                b.body->invalidate_attack_route();
                go(d, st::kAttackFire);
                return 1;
            }
            if (r < moveres::kArrived) {
                go(d, st::kAttackRun);
                return 1;
            }
            if (r == moveres::kDoor) {
                b.door_return_state_ = st::kAttack;
                d.set_state(st::kDoorOpen);
                return 1;
            }
        }
        if (d.sight_flags & sight::kSeen) go(d, b.react_to_opponent_sighted(st::kAttackFire));
        return 1;
    }
    default:
        return 0;
    }
}

int state_attack_run(Drone& d, const Msg& m) {           // @0x16f390
    BotBrain& b = B(d);
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        if (b.body->can_do_anim_state(8)) d.call_anim(0, 8); else d.call_anim(0, 0x1e);
        b.body->invalidate_attack_route();
        return 1;
    case kMsgTick: {
        if (!b.has_opponent()) {
            go(d, st::kAttackNoOpponent);
            return 1;
        }
        const int r = chase_opponent(b);
        if (!b.validate_route(r)) {
            go(d, st::kAttackNoRoute);
            return 1;
        }
        if (r == moveres::kArrived) {
            b.body->invalidate_attack_route();
        } else if (r < moveres::kArrived) {
            if (d.opp_dist <= d.range_ec && b.body->can_backoff()) {
                go(d, st::kBackoff);
                return 1;
            }
            b.body->anim_for_route_distance();
        } else if (r == moveres::kDoor) {
            b.door_return_state_ = st::kAttack;
            d.set_state(st::kDoorOpen);
            return 1;
        }
        if (d.opp_dist < d.engage_dist && (d.sight_flags & sight::kSeen)) go(d, b.react_to_opponent_sighted(st::kAttack));
        return 1;
    }
    default:
        return 0;
    }
}

int state_attack_fire(Drone& d, const Msg& m) {          // @0x16f5e8
    BotBrain& b = B(d);
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.call_anim(0, 0x0a);
        b.body->invalidate_attack_route();
        if (!b.body->fire_requested()) b.body->fire(1);
        return 1;
    case kMsgTick: {
        if (b.has_opponent()) b.body->set_angle_to_participant(b.opponent(), -0.04f);
        if (!b.has_opponent()) {
            go(d, st::kAttackNoOpponent);
            return 1;
        }
        if (!b.body->burst_done() && b.body->fire_requested()) {
            if (b.env->rand(3) != 0) return 1;
            go(d, b.choose_combat_move());
            return 1;
        }
        if (never_moves(d)) return 1;
        if (d.opp_dist <= d.range_ec && b.body->can_backoff()) {
            go(d, st::kBackoff);
            return 1;
        }
        if (d.opp_dist < d.max_combat_dist && d.lost_frames < 0x10) {
            go(d, b.choose_combat_move());
            return 1;
        }
        move_result_or_combat_move(d, b, chase_opponent(b), false);
        return 1;
    }
    default:
        return 0;
    }
}

int state_strafe(Drone& d, const Msg& m, bool left) {    // @0x16f850 / @0x16f9b8
    BotBrain& b = B(d);
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.call_anim(0, left ? 0x0c : 0x0d);
        if (!b.body->fire_requested()) b.body->fire(1);
        set_timer1(d, (b.env->rand(8) + 2) * seconds(d, 1.0f));
        return 1;
    case kMsgTick:
        if (!b.has_opponent()) {
            go(d, st::kAttackNoOpponent);
            return 1;
        }
        b.body->aim_at_opponent();
        if (!(left ? b.body->can_strafe_left() : b.body->can_strafe_right())) {
            const int mv = b.choose_combat_move();
            go(d, mv ? mv : st::kAttack);
        } else if (d.lost_frames >= 0x10) {
            go(d, st::kAttackRun);
        }
        return 1;
    case kMsgTimer1:
        go(d, st::kAttack);
        return 1;
    default:
        return 0;
    }
}
int state_strafe_left(Drone& d, const Msg& m) { return state_strafe(d, m, true); }
int state_strafe_right(Drone& d, const Msg& m) { return state_strafe(d, m, false); }

int state_run_change_position(Drone& d, const Msg& m) {  // @0x16fb20
    BotBrain& b = B(d);
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        b.body->invalidate_attack_route();
        d.call_anim(0, 0x1e);
        set_timer1(d, b.env->rand(3) * seconds(d, 1.0f));
        return 1;
    case kMsgTick:
        if (b.body->at_dest() || b.body->near_drone(6.0f)) go(d, st::kAttack);
        else b.body->set_angle_to_dest();
        return 1;
    case kMsgTimer1:
    case kMsgPatrolObstructed:
        go(d, st::kAttack);
        return 1;
    default:
        return 0;
    }
}

int state_backoff(Drone& d, const Msg& m) {              // @0x16fc30
    BotBrain& b = B(d);
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        if (b.body->can_backoff()) {
            d.call_anim(0, 4);
            set_timer1(d, (b.env->rand(1) + 2) * seconds(d, 1.0f));
            if (!b.body->fire_requested()) b.body->fire(1);
            return 1;
        }
        if (opponent_in_front(d)) {
            if (b.v.has_flag(botflag::kMelee) || b.arm.current() == weap::kFists) {
                go(d, st::kUnarmed);
                return 1;
            }
            if (b.env->rand(8) == 7) {
                go(d, st::kUnarmed);
                return 1;
            }
        }
        {
            const int mv = b.really_want_combat_move(3);
            go(d, mv ? mv : st::kAttack);
        }
        return 1;
    case kMsgTick: {
        if (!b.has_opponent()) {
            go(d, st::kAttackNoOpponent);
            return 1;
        }
        if (d.engage_dist < d.opp_dist || !b.body->can_backoff()) {
            go(d, st::kAttack);
            return 1;
        }
        const int e = b.evasive_move_state();
        if (e != 0) {
            go(d, e);
            return 1;
        }
        b.body->aim_at_opponent();
        if ((b.body->burst_done() || !b.body->fire_requested()) && (b.env->rand(4) & 1)) go(d, b.choose_combat_move());
        return 1;
    }
    case kMsgTimer1: {
        const int mv = b.choose_combat_move();
        go(d, mv ? mv : st::kAttack);
        return 1;
    }
    default:
        return 0;
    }
}

int state_crouch(Drone& d, const Msg& m) {               // @0x16fe80
    BotBrain& b = B(d);
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.call_anim(0, 5);
        b.body->invalidate_attack_route();
        if (!b.body->fire_requested()) b.body->fire(1);
        return 1;
    case kMsgTick: {
        if (b.has_opponent()) b.body->set_angle_to_participant(b.opponent(), -0.1f);
        if (!never_moves(d) && d.opp_dist <= d.range_ec && b.body->can_backoff()) {
            go(d, st::kBackoff);
            return 1;
        }
        if (!b.has_opponent()) {
            go(d, st::kAttackNoOpponent);
            return 1;
        }
        if (!b.body->burst_done() && b.body->fire_requested()) return 1;
        const int e = b.evasive_move_state();
        if (e != 0) {
            go(d, e);
            return 1;
        }
        if (!never_moves(d)) {
            if (d.opp_dist < d.max_combat_dist && d.lost_frames < 0x10) {
                go(d, b.choose_combat_move());
                return 1;
            }
            const int r = chase_opponent(b);
            if (!b.validate_route(r)) {
                go(d, st::kAttackNoRoute);
            } else if (r == moveres::kArrived) {
                b.body->invalidate_attack_route();
                go(d, st::kAttack);
            } else if (r < moveres::kArrived) {
                go(d, st::kAttackRun);
            } else if (r != moveres::kDoor) {
                go(d, b.choose_combat_move());
            }
            return 1;
        }
        if ((b.env->rand(3) == 0 || d.lost_frames >= 0x10) && b.body->can_do_anim_state(5)) go(d, st::kAttack);
        return 1;
    }
    case 0x1c:
        go(d, st::kRunChangePosition);
        return 1;
    default:
        return 0;
    }
}

int state_roll(Drone& d, const Msg& m, bool left) {      // @0x174d18 / @0x174dd0
    BotBrain& b = B(d);
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.call_anim(0, left ? 0x53 : 0x54, 0, st::kCrouch);
        return 1;
    case kMsgTick:
        b.body->aim_at_opponent();
        return 1;
    case kMsgAnimResume:
        go(d, b.has_opponent() ? st::kCrouch : st::kAttackNoOpponent);
        return 1;
    default:
        return 0;
    }
}
int state_roll_left(Drone& d, const Msg& m) { return state_roll(d, m, true); }
int state_roll_right(Drone& d, const Msg& m) { return state_roll(d, m, false); }

int state_step(Drone& d, const Msg& m, bool left) {      // @0x174e88 / @0x174fa8
    BotBrain& b = B(d);
    const int anim = left ? 0x5a : 0x5b;
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.call_anim(0, anim, 0, st::kAttackFire);
        if (!b.body->fire_requested()) b.body->fire(1);
        return 1;
    case kMsgTick:
        if (!b.has_opponent()) {
            go(d, st::kAttackNoOpponent);
            return 1;
        }
        if (!b.body->can_do_anim_state(anim)) {
            go(d, st::kAttackFire);
            return 1;
        }
        b.body->aim_at_opponent();
        if (b.env->rand(5) == 3) go(d, b.choose_combat_move());
        return 1;
    default:
        return 0;
    }
}
int state_step_left(Drone& d, const Msg& m) { return state_step(d, m, true); }
int state_step_right(Drone& d, const Msg& m) { return state_step(d, m, false); }

int state_reload(Drone& d, const Msg& m) {               // @0x170190
    BotBrain& b = B(d);
    if (m.id != kMsgEnter) return m.id == kMsgNone;
    b.body->reset_firing();
    const bool back = d.opp_dist <= d.range_ec && b.body->can_backoff();
    if (b.arm.reload(false)) {
        if (!BotArmoury::reload_anim_for_weapon(b.arm.current())) {
            go(d, back ? st::kBackoff : st::kAttack);
        } else {
            d.call_anim(0, 0x52, 0, back ? st::kBackoff : st::kAttack);
        }
        b.log_event("reload", std::to_string(b.arm.current()));
    } else {
        d.initial_state = st::kAttack;
        d.set_state(st::kChangeWeapon);
    }
    return 1;
}

int state_change_weapon(Drone& d, const Msg& m) {        // @0x1750c8
    BotBrain& b = B(d);
    if (m.id != kMsgEnter) return m.id == kMsgNone;
    b.body->reset_firing();
    const int id = b.change_weapon(b.v.desired_weapon, true, false);
    if (id != 0) d.broadcast(botmsg::kTeammateWeapon, b.arm.current(), int(seconds(d, 1.0f)));
    d.initial_state = st::kAttack;
    go(d, st::kAttack);
    return 1;
}

int state_unarmed(Drone& d, const Msg& m) {              // @0x175188
    BotBrain& b = B(d);
    if (m.id != kMsgEnter) return m.id == kMsgNone;
    b.body->reset_firing();
    if (d.opp_dist <= 1.5f) {
        if (b.choose_unarmed_attack_anim() == 0) go(d, st::kBackoff);
    } else {
        go(d, st::kAttack);
    }
    return 1;
}

// ---------------------------------------------------------------------------------------------------------
// Impacts, stun, death

int state_impact_unreachable(Drone&, const Msg& m) { return m.id == kMsgNone; }   // 0xef..0xf1: rejected by validateStateChange

int state_stun_grenade(Drone& d, const Msg& m) {         // @0x175418
    BotBrain& b = B(d);
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        d.call_anim(0, 0x2d);
        set_timer1(d, std::uint32_t(float(stun_ticks(b.v.stats.recovery_rate)) * d.rate() / 30.0f));
        return 1;
    case kMsgTimer1:
        go(d, st::kIdle);
        return 1;
    default:
        return 0;
    }
}

int state_death_anim(Drone& d, const Msg& m) {           // @0x175508
    BotBrain& b = B(d);
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        b.sound_effect(1);
        b.set_opponent(-1);
        if (b.v.active_goal >= 0) b.uninit_goal(b.v.active_goal);
        b.log_event("died", std::string(character_name(b.v.character)));
        if (b.on_died) b.on_died(b);   // MP_PlayerKilled
        b.body->location_death_anim(st::kDead);
        return 1;
    case kMsgTick:
        b.body->drop_weapon();
        return 1;
    default:
        return 0;
    }
}

int state_death_by_explosion(Drone& d, const Msg& m) {   // @0x1755a8
    BotBrain& b = B(d);
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        b.sound_effect(1);
        b.body->explosion_death_anim(st::kDead, d.smi.arg);
        b.set_opponent(-1);
        if (b.v.active_goal >= 0) b.uninit_goal(b.v.active_goal);
        b.log_event("died", std::string(character_name(b.v.character)) + " (explosion)");
        if (b.on_died) b.on_died(b);
        b.body->drop_weapon();
        return 1;
    default:
        return 0;
    }
}

int state_dead(Drone& d, const Msg& m) {                 // @0x171518
    BotBrain& b = B(d);
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        b.env->reset_pickup_visits(b.v.bot_index);
        b.set_opponent(-1);
        if (b.v.active_goal >= 0) b.uninit_goal(b.v.active_goal);
        d.flags |= flag::kDeathProcessed;   // NDrone2_SetAsDead
        set_timer1(d, 3 * seconds(d, 1.0f));
        if (d.has_beh(0x31)) set_timer2(d, seconds(d, 1.0f));
        return 1;
    case kMsgTimer1:
        go(d, st::kRespawn);
        return 1;
    default:
        return 0;
    }
}

// ---------------------------------------------------------------------------------------------------------
// 0xc5 BotGlobal @0x16e060

// The attack-state part of the per-tick brain (spec §2.3 step 12).
void global_combat(Drone& d, BotBrain& b) {
    BotVars& v = b.v;
    const int cur = d.state();
    const int weapon = b.arm.current();
    const bool melee = v.has_flag(botflag::kMelee);
    const bool missile = b.opponent_is_missile();
    const bool busy = cur == st::kReload || cur == st::kChangeWeapon;

    const auto switch_to = [&](int id) {
        v.desired_weapon = id;
        d.set_state(st::kChangeWeapon);
    };
    const auto weapon_logic = [&] {
        if (!b.has_opponent()) return;
        if (b.in_hill()) {
            int mask = 0;
            switch (cur) {
            case st::kStrafeAimLeft: case st::kRollLeftCrouch: case st::kStepAimLeft: mask = 2; break;
            case st::kStrafeAimRight: case st::kRollRightCrouch: case st::kStepAimRight: mask = 1; break;
            default: break;
            }
            if (mask == 0 || b.check_attack_move(cur) != 0) return;
            const int mv = b.really_want_combat_move(mask);
            d.set_state(mv ? mv : st::kAttack);
            return;
        }
        if (busy) return;
        if (b.arm.table().weapon(weapon).category != 4) {
            const int id = b.arm.loaded_explosive_for_range(d.opp_dist);
            if (id != 0) switch_to(id);
        } else if (b.arm.too_close_for_weapon(weapon, d.opp_dist, v.personality(), missile)) {
            switch_to(b.change_weapon(0, true, true));
        }
    };

    if (busy) {
        weapon_logic();
        return;
    }
    if (b.arm.clip_mirror() <= 0) {
        if (b.arm.weapon_has_ammo(weapon)) {
            d.set_state(st::kReload);
        } else {
            switch_to(b.change_weapon(0, true, true));
        }
        return;
    }
    if (cur != st::kUnarmed) {
        if (!missile && d.opp_dist <= 1.5f && opponent_in_front(d)) {
            if (melee) {
                if (weapon != weap::kFists) switch_to(weap::kFists); else d.set_state(st::kUnarmed);
                return;
            }
            if (weapon == weap::kFists) {
                d.set_state(st::kUnarmed);
                return;
            }
            if (b.env->rand(200) == 199 || BotArmoury::punch_is_better_if_close(weapon)) {
                switch_to(weap::kFists);
                return;
            }
        }
        if (weapon == weap::kFists && d.opp_dist > 1.5f) {
            switch_to(b.change_weapon(0, true, true));
            return;
        }
    }
    if (!missile && !melee) {
        if (cur != st::kBackoff && d.opp_dist <= d.range_ec && b.body->can_backoff()) {
            d.set_state(st::kBackoff);
            return;
        }
        if (d.opp_dist < 3.0f) {
            switch (cur) {
            case st::kStrafeAimLeft: case st::kStrafeAimRight: case st::kRunChangePosition: case st::kBackoff:
            case st::kRollLeftCrouch: case st::kRollRightCrouch: case st::kStepAimLeft: case st::kStepAimRight:
            case st::kUnarmed:
                break;
            default: {
                int mv = b.really_want_combat_move(-1);
                if (mv == 0 && d.opp_dist <= 1.5f && opponent_in_front(d)) mv = st::kUnarmed;
                if (mv != 0) {
                    d.set_state(mv);
                    return;
                }
            }
            }
        }
    }
    weapon_logic();
}

// One tick of BotGlobal. Returns after possibly requesting a state change.
int global_tick(Drone& d, BotBrain& b) {
    BotVars& v = b.v;
    const int cur = d.state();
    if (cur == 0) {
        d.set_state(st::kIdle);
        return 1;
    }
    const int type = state_type(cur);
    v.state_type = type;

    if (bot_dead(d)) {
        if (type == 10) return 0;   // the death states run their own tick
        d.set_state(st::kDeathAnim);
        return 1;
    }
    b.set_other_player_info();
    b.update_opponent_tracking();

    if (v.pending_state != 0) {
        if (v.pending_state != cur) {
            const int p = v.pending_state;
            v.pending_state = 0;
            d.set_state(p);
            return 1;
        }
        v.pending_state = 0;
    }
    d.flags |= flag::kAware;   // enables the opponent-history logic

    if (b.has_opponent() && !b.env->participant(b.opponent()).valid) b.set_opponent(-1);

    if (v.bits & bitflag::kRecovering) {
        if (v.recovery_end >= b.env->tick()) {
            if (b.has_opponent()) {
                d.flags &= ~flag::kAware;
                d.sight_flags &= ~sight::kSeen;
                for (OtherInfo& o : v.other) o.flags &= ~(otherflag::kValid | otherflag::kVisible);
            }
        } else {
            v.bits &= ~bitflag::kRecovering;
        }
    }

    // Oddjob: the thrown hat grows back after 10 s.
    if (v.character == kOddjob && v.hat_tick != 0) {
        const std::uint32_t now = b.env->tick();
        const std::uint32_t age = now > v.hat_tick ? now - v.hat_tick : v.hat_tick - now;
        if (age >= seconds(d, 10.0f)) {
            b.arm.add_round(weap::kOddjobHat);
            v.hat_tick = 0;
            v.desired_weapon = b.change_weapon(0, true, true);
            d.set_state(st::kChangeWeapon);
            return 1;
        }
    }
    // Samedi: aware of anyone he can see.
    if (!(d.sight_flags & sight::kSeen) && v.has_flag(botflag::kAware) && b.has_opponent() &&
        (v.other_info(b.opponent()).flags & 6) == 6)
        d.sight_flags |= sight::kSeen;
    // Samedi: +5 health each second up to the starting health.
    if (v.has_flag(botflag::kRegen) && b.env->tick() >= v.next_regen) {
        d.health = std::min(d.health + 5.0f, float(v.stats.health));
        b.set_health(d.health);
        v.next_regen = b.env->tick() + seconds(d, 1.0f);
    }
    if (!b.opponent_seen()) b.increase_distraction(-2.0f);

    if (type == 5) {
        const std::uint32_t sc = b.env->scenario();
        if (sc == scenario::kProtection || sc == scenario::kDemolition) {
            v.in_zone = b.env->in_defended_zone(v.slot);
            if (v.in_zone) {
                d.set_state(st::kIdle);
                return 1;
            }
        }
    }
    if (type < 3 || type == 10) return 1;

    // Stuck watchdog (not in the original, which trusts the route failure counter): a bot walking to a goal that
    // has not moved 0.75 units in four seconds gives the goal up.
    if (cur == st::kGotoGoal || cur == st::kGuardFriendFollow) {
        const std::uint32_t now = b.env->tick();
        if (b.stuck_ref_tick_ == 0 || now - b.stuck_ref_tick_ >= seconds(d, 4.0f)) {
            if (b.stuck_ref_tick_ != 0 && length(d.pos - b.stuck_ref_) < 0.75f && v.active_goal >= 0) {
                BotGoal& g = v.goal[std::size_t(v.active_goal)];
                b.log_event("stuck", "goal kind " + std::to_string(g.kind) + " target " + std::to_string(g.target));
                b.body->invalidate_attack_route();
                if (g.type == goaltype::kPickup) b.set_pickup_visit_time(g.target);
                b.set_state_change(g.return_state);
                b.uninit_goal(v.active_goal);
            }
            b.stuck_ref_ = d.pos;
            b.stuck_ref_tick_ = now;
        }
    } else {
        b.stuck_ref_tick_ = 0;
    }

    b.body->invalidate_nearest_node();
    b.process_goals();

    if (type == 4) {
        if (b.has_opponent()) global_combat(d, b);
        return 1;
    }
    // Not fighting: an opponent seen in the last five seconds keeps the alert bit and pulls the bot into combat.
    if (b.has_opponent() && b.env->tick() - d.last_seen_time < seconds(d, 5.0f)) d.sight_flags |= sight::kSeen;
    if (d.sight_flags & sight::kSeen) go(d, b.react_to_opponent_sighted(st::kSeenOpponent));
    return 1;
}

int state_global(Drone& d, const Msg& m) {
    BotBrain& b = B(d);
    const auto dead_type = [&] { return state_type(d.state()) == 10; };
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgTick:
        return global_tick(d, b);
    case kMsgPunch:
    case kMsgBullet:
    case kMsgExplosive:
        if (dead_type()) return 1;
        b.body->impact_reaction(m.id);
        b.v.last_hit_tick = b.env->tick();
        b.start_recovery();
        if (m.id == kMsgExplosive && bot_dead(d)) d.set_state(st::kDeathByExplosion, std::intptr_t(m.ptr));
        return 1;
    case kMsgShoutFirstSight: case kMsgShoutHurt: case kMsgShoutType3: case kMsgSoundAlert: case kMsgShoutType5:
    case kMsgShoutAttack: case kMsgDroneAlert: {
        const int t = state_type(d.state());
        if (t == 0 || t == 1 || t == 2 || t == 4 || t == 10) return 1;
        // NDrone2_ReactToDroneAlertMsg (MP): an alert from a teammate / anyone in FFA makes the bot accept the next
        // visible target without the view-cone test.
        if (!b.has_opponent()) b.v.alerted = true;
        return 1;
    }
    case kMsgStunGrenade:
        if (!dead_type()) d.set_state(st::kImpactStunGrenade);
        return 1;
    case kMsgStateChanged: {
        const int type = state_type(d.state());
        if (type != 13 && d.state() != st::kGotoGoal) b.v.friend_slot = -1;
        float alertness = 1.0f;
        AlertStatus status = AlertStatus::Alert;
        if (type == 1 || type == 10) {
            alertness = 0.0f;
            status = AlertStatus::Dead;
        }
        d.alertness = alertness;
        d.set_alert_status(status);
        d.flags |= flag::kAware;
        b.v.state_type = type;
        return 1;
    }
    case botmsg::kObjectiveChanged:
        if (state_type(d.state()) == 4 || b.has_opponent()) return 1;
        b.set_goal_pick_prefs(0, 0, 0, 1.0f, 1, 0, 0);
        if (b.pick_goal(1)) go(d, st::kGotoGoal);
        return 1;
    case botmsg::kGoalComplete:
        b.set_goal_complete(int(m.arg));
        return 1;
    case botmsg::kUninitGoal:
        b.uninit_goal(int(m.arg));
        return 1;
    case botmsg::kCancelGoalToObj:
        b.cancel_goal_to_obj(int(m.arg), false, st::kIdle);
        return 1;
    case botmsg::kWeaponChange:
        b.v.desired_weapon = int(m.arg);
        d.initial_state = d.state();
        d.set_state(st::kChangeWeapon);
        return 1;
    case botmsg::kPlayerDied: {
        if (bot_dead(d)) return 1;
        const int victim = int(m.arg);
        if (b.opponent() == victim) b.set_opponent(-1);
        if (b.v.friend_slot == victim) b.v.friend_slot = -1;
        b.cancel_goal_to_obj(victim, true, st::kIdle);
        if (state_type(d.state()) == 13) {
            go(d, st::kIdle);
        } else if (b.v.trait_opponent == victim) {
            b.v.trait_opponent = -1;
        }
        return 1;
    }
    case botmsg::kTeammateWeapon:
        if (m.sender != d.id && !bot_dead(d) && b.has_opponent()) b.combat_weapon_change_choice(true, false);
        return 1;
    case botmsg::kOpponentSet:
        b.combat_weapon_change_choice(true, false);
        b.default_combat_range();
        if (b.has_opponent() && d.state() == st::kIdle && b.v.in_zone) d.set_state(st::kAttack);
        return 1;
    default:
        return 0;
    }
}

}  // namespace

// NDrone2_ReactToOpponentSighted (MP bots reach the "return the requested state" tail: alertness is already 1.0).
int BotBrain::react_to_opponent_sighted(int state) {
    if (!has_opponent()) return 0;
    if (self->flags & flag::kCoverClaimed) return 0;
    self->alertness = 1.0f;
    self->sight_flags |= 0x20;
    return state;
}

// Per-tick upkeep of the opponent fields the drone code reads (+0x1a0 dist, +0x1c4/+0x1d4 bearing, +0x238 last
// seen, +0x270/+0x274 seen/lost counters).
void BotBrain::update_opponent_tracking() {
    if (!has_opponent()) {
        self->opp_dist = 1e9f;
        self->opp_visible = false;
        return;
    }
    const Participant me = env->participant(v.slot);
    const Participant opp = env->participant(opponent_slot_);
    if (!opp.valid) return;
    const Vec3 delta = opp.pos - me.pos;
    self->opp_vec = delta;
    self->opp_dist = length(delta);
    const float bearing = std::atan2(delta[0], delta[2]);
    self->opp_bearing = bearing;
    self->opp_facing_b = wrap_pi(bearing - self->yaw);
    const bool visible = (v.other_info(opponent_slot_).flags & otherflag::kVisible) != 0;
    self->opp_visible = visible;
    if (visible) {
        self->lost_frames = 0;
        ++self->seen_frames;
        self->last_seen_time = self->now();
    } else {
        self->seen_frames = 0;
        ++self->lost_frames;
    }
}

void register_bot_states() {
    using drone::register_state;
    register_state(st::kInit, "BotInit", state_init);
    register_state(st::kRespawn, "BotRespawn", state_respawn);
    register_state(st::kGlobal, "BotGlobal", state_global);
    register_state(0xc6, "BotCollector", state_collector);
    static const char* const kStubNames[] = {"BotGuardian", "BotTeamPlayer", "BotBully", "BotBerserker",
                                             "BotGreedy",   "BotVengeful",   "BotJudge", "BotAssassin"};
    for (int i = 0; i < 8; ++i) register_state(0xc7 + i, kStubNames[i], state_to_idle);
    register_state(st::kAttack, "BotAttack", state_attack);
    register_state(st::kAttackRun, "BotAttackRun", state_attack_run);
    register_state(st::kAttackNoRoute, "BotAttackNoRoute", state_no_route);
    register_state(st::kAttackFire, "BotAttackFire", state_attack_fire);
    register_state(st::kAttackNoOpponent, "BotAttackNoOpponent", state_to_idle);
    register_state(st::kStrafeAimLeft, "BotAttackStrafeAimLeft", state_strafe_left);
    register_state(st::kStrafeAimRight, "BotAttackStrafeAimRight", state_strafe_right);
    register_state(st::kRunChangePosition, "BotAttackRunChangePosition", state_run_change_position);
    register_state(st::kBackoff, "BotAttackBackoff", state_backoff);
    register_state(st::kCrouch, "BotAttackCrouch", state_crouch);
    register_state(st::kRollLeftCrouch, "BotAttackRollLeftCrouch", state_roll_left);
    register_state(st::kRollRightCrouch, "BotAttackRollRightCrouch", state_roll_right);
    register_state(st::kStepAimLeft, "BotAttackStepAimLeft", state_step_left);
    register_state(st::kStepAimRight, "BotAttackStepAimRight", state_step_right);
    register_state(st::kReload, "BotAttackReload", state_reload);
    register_state(st::kChangeWeapon, "BotAttackChangeWeapon", state_change_weapon);
    register_state(st::kUnarmed, "BotAttackUnarmed", state_unarmed);
    // Cover states (type 12) are rejected by BOT_validateStateChange in multiplayer; they exist in the table and
    // fall back to the attack state like their neutral siblings.
    static const char* const kCoverNames[] = {"BotCoverRunTo", "BotCoverInit",       "BotCoverIdle",
                                              "BotCoverAim",   "BotCoverFire",       "BotCoverReturn",
                                              "BotCoverTypeChange", "BotCoverLeave", "BotCoverLeaveNow"};
    for (int i = 0; i < 9; ++i) register_state(0xe0 + i, kCoverNames[i], state_to_attack);
    register_state(st::kStuck, "BotStuck", state_to_idle);
    register_state(st::kAlertToPosition, "BotAlertToPosition", state_alert_to_position);
    register_state(st::kGotoGoal, "BotGotoGoalPosition", state_goto_goal);
    register_state(st::kSeenOpponent, "BotSeenOpponent", state_to_attack);
    register_state(st::kSeenDroneShot, "BotSeenDroneShot", state_to_attack);
    register_state(st::kHeardNoise, "BotHeardNoise", state_heard_noise);
    register_state(st::kImpactBullet, "BotImpactBullet", state_impact_unreachable);
    register_state(st::kImpactExplosive, "BotImpactExplosive", state_impact_unreachable);
    register_state(st::kImpactPunch, "BotImpactPunch", state_impact_unreachable);
    register_state(st::kDeathAnim, "BotDeathAnim", state_death_anim);
    register_state(st::kDeathByExplosion, "BotDeathByExplosion", state_death_by_explosion);
    register_state(st::kDead, "BotDead", state_dead);
    register_state(st::kImpactStunGrenade, "BotImpactStunGrenade", state_stun_grenade);
    register_state(st::kDoorOpen, "BotDoorOpen", state_door_open);
    register_state(st::kGuardFriendIdle, "BotGuardFriendIdle", state_guard_idle);
    register_state(st::kGuardFriendFollow, "BotGuardFriendFollow", state_guard_follow);
    register_state(st::kIdle, "BotIdle", state_idle);
}

}  // namespace nf::bots

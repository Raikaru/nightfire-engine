// NDrone2 locomotion (docs/spec-arena-ai.md Part 2B §6.5, Part 3 §10).
#include "game/drone_move.hpp"

#include <algorithm>
#include <cmath>

#include <cstdio>
#include "game/drone_anim.hpp"
#include "game/drone_system.hpp"
#include "game/drone_vision.hpp"
#include "game/drone_weap.hpp"
#include "game/ee_math.hpp"

namespace nf::drone {

namespace {

// +x of the object frame is the character's LEFT (the original's convention: CanStrafeLeft moves by +x).
Vec3 to_world(const Vec3& local, float yaw) {
    const float s = std::sin(yaw), c = std::cos(yaw);
    return {local[0] * c + local[2] * s, local[1], -local[0] * s + local[2] * c};
}

float dist2d(const Vec3& a, const Vec3& b) { return std::hypot(a[0] - b[0], a[2] - b[2]); }

void publish_move(Drone& d, const NavMove& m) {
    // NDrone2_FollowRoute tail: waypoint -> Drone::mv (+0x670/+0x660/+0x694).
    d.mv.route_status = m.status;
    d.mv.route_distance = m.distance;
    const bool moving = m.status == RouteStatus::Following || m.status == RouteStatus::Approximate ||
                        m.status == RouteStatus::Straight;
    d.mv.have_dest = moving;
    if (moving) {
        d.mv.dest = m.waypoint;
        d.mv.arrive_radius = 0.3f;
        const Vec3 f = d.nav_pos();
        d.mv.dest_dist = dist2d(f, m.waypoint);
        d.mv.dest_angle = atan2_approx(m.waypoint[0] - d.pos[0], m.waypoint[2] - d.pos[2]);
    }
}

// Straight-line fallback for levels without a navigation network.
int straight_move(Drone& d, const Vec3& goal, float radius) {
    NavMove m;
    const Vec3 f = d.nav_pos();
    const float dist = dist2d(f, goal);
    m.distance = dist;
    m.waypoint = goal;
    m.status = dist <= std::max(radius, 0.3f) ? RouteStatus::Arrived : RouteStatus::Straight;
    publish_move(d, m);
    return int(m.status);
}

}  // namespace

// ---- route movement ---------------------------------------------------------------------------------------------------
int move_to_goal_position(Drone& d, const Vec3& goal, float radius, bool follow) {
    if (!d.nav) return straight_move(d, goal, radius);
    NavAgent& a = *d.nav;
    const Vec3 f = d.nav_pos();
    // The original keeps the route until a state invalidates it; a goal that moved is treated as a new goal.
    if (!a.has_goal() || dist2d(a.goal().pos, goal) > 1.0f) {
        a.set_goal_position(goal, radius);
        a.invalidate_route();
    }
    if (!follow) {
        a.calc_route_to_goal(f);
        return int(a.route().status);
    }
    NavMove m = a.move_to_goal(f);
    publish_move(d, m);
    return int(m.status);
}

int move_to_object(Drone& d, const TargetRef& target, float radius, bool follow) {
    if (!target.valid()) return int(RouteStatus::NoTargetNodes);
    const Vec3 tp = target_pos(*d.sys, target);
    if (!d.nav) return straight_move(d, tp, radius);
    NavAgent& a = *d.nav;
    const Vec3 f = d.nav_pos();
    if (!a.has_goal() || !d.mv.goal_is_object || !(d.mv.goal_target == target)) {
        if (target == TargetRef::player(0)) a.set_goal_player(tp, radius);
        else a.set_goal_object(tp, radius);
        d.mv.goal_is_object = true;
        d.mv.goal_target = target;
        a.invalidate_route();
    } else {
        a.update_goal_object(tp);
    }
    NavMove m = a.move_to_goal(f);
    if (!follow && m.status == RouteStatus::Following) return int(m.status);
    publish_move(d, m);
    return int(m.status);
}

bool assign_ai_path(Drone& d, std::uint32_t path_mask) {
    if (!d.nav || !d.sys->nav()) return false;
    return d.nav->assign_ai_path(d.nav_pos(), d.sys->nav()->find_cel(d.nav_pos()), path_mask);
}

int follow_ai_path(Drone& d) {
    if (!d.nav) return int(RouteStatus::NoPath);
    NavMove m = d.nav->follow_ai_path(d.nav_pos());
    publish_move(d, m);
    return int(m.status);
}

void invalidate_attack_route(Drone& d) {
    if (d.nav) d.nav->invalidate_route();
    d.mv.goal_is_object = false;
}

void stop(Drone& d) {
    d.mv.have_dest = false;
    d.mv.route_status = RouteStatus::Reset;
    if (d.nav) d.nav->invalidate_route();
}

// ---- facing -----------------------------------------------------------------------------------------------------------
void set_angle_to_dest(Drone& d, float offset) {
    if (d.flags & flag::kSawPlayer) return;
    d.mv.dest_angle = atan2_approx(d.mv.dest[0] - d.pos[0], d.mv.dest[2] - d.pos[2]) + offset;
}

void set_angle_to_obj(Drone& d, const TargetRef& t, float offset, bool force) {
    if (!t.valid()) return;
    if ((d.flags & flag::kSawPlayer) && !force) return;
    const Vec3 tp = target_pos(*d.sys, t);
    d.mv.dest_angle = atan2_approx(tp[0] - d.pos[0], tp[2] - d.pos[2]) + offset;
}

void face_yaw(Drone& d, float yaw) { d.mv.dest_angle = yaw; }

void snap_yaw(Drone& d, float yaw) { d.yaw = d.mv.dest_angle = yaw; }

// ---- move tests -----------------------------------------------------------------------------------------------------------
namespace {

bool can_move_to_relative_impl(Drone& d, const Vec3& local_offset) {
    const Vec3 from = d.nav_pos();
    const Vec3 to = from + to_world(local_offset, d.yaw);
    d.mv.reach_check_pos = to;   // DroneReachCheckPos side effect (BOTSTATE_checkAttackMove KOTH veto reads it)
    if (NavNetwork* nav = d.sys->nav()) return nav->move_test(nav->locate(from), nav->locate(to)) == 1;
    // No nav network: a knee-high ray.
    const Vec3 up{0, 0.5f, 0};
    return d.sys->collision().line_of_sight(from + up, to + up, 0xa27);
}

bool can_move_to_relative_impl(Drone& d, const Vec3& local);

bool gate(Drone& d, int behaviour_id, int dasc, int dasc2 = -1) {
    if (!d.has_beh(behaviour_id) || (d.flags & flag::kStationary)) return false;
    if (!anim_can_do(d, dasc)) return false;
    return dasc2 < 0 || anim_can_do(d, dasc2);
}

float bot_speed_mul(const Drone& d) { return d.speed_scale; }

}  // namespace

bool can_move_to_relative(Drone& d, const Vec3& local) { return can_move_to_relative_impl(d, local); }

bool can_strafe_left(Drone& d) {
    if (!gate(d, beh::kStrafe, kAimStrafeLeft)) return false;
    const float dist = (d.sub_class == 6 || d.sub_class == 9) ? 1.8f : 4.0f;
    return can_move_to_relative_impl(d, {dist * bot_speed_mul(d), 0, 0});
}
bool can_strafe_right(Drone& d) {
    if (!gate(d, beh::kStrafe, kAimStrafeLeft, kAimStrafeRight)) return false;
    const float dist = (d.sub_class == 6 || d.sub_class == 9) ? 1.8f : 4.0f;
    return can_move_to_relative_impl(d, {-dist * bot_speed_mul(d), 0, 0});
}
bool can_strafe_dodge_left(Drone& d) {
    if (!gate(d, beh::kStrafeDodge, kStrafeDodgeLeft)) return false;
    return can_move_to_relative_impl(d, {3.6f * bot_speed_mul(d), 0, 0});
}
bool can_strafe_dodge_right(Drone& d) {
    if (!gate(d, beh::kStrafeDodge, kStrafeDodgeLeft, kStrafeDodgeRight)) return false;
    return can_move_to_relative_impl(d, {-3.6f * bot_speed_mul(d), 0, 0});
}
bool can_step_left(Drone& d) {
    if (!gate(d, beh::kStep, 0x5a)) return false;
    return can_move_to_relative_impl(d, {1.8f * bot_speed_mul(d), 0, 0});
}
bool can_step_right(Drone& d) {
    if (!gate(d, beh::kStep, 0x5a, 0x5b)) return false;
    return can_move_to_relative_impl(d, {-1.8f * bot_speed_mul(d), 0, 0});
}
bool can_roll_left(Drone& d) {
    if (!gate(d, beh::kRoll, 0x53)) return false;
    return can_move_to_relative_impl(d, {5.2f * bot_speed_mul(d), 0, 0});
}
bool can_roll_right(Drone& d) {
    if (!gate(d, beh::kRoll, 0x53)) return false;
    return can_move_to_relative_impl(d, {-5.2f * bot_speed_mul(d), 0, 0});
}
bool can_backoff(Drone& d) {
    if (!gate(d, 6, kAimBackoff)) return false;
    return can_move_to_relative_impl(d, {0, 0, -1.8f * bot_speed_mul(d)});
}
bool can_aim_crouch(Drone& d) {
    if (!d.has_beh(beh::kAimCrouch) || !anim_can_do(d, kAimCrouch)) return false;
    return d.opp_dist >= 6.0f;
}

bool opponent_facing_me(const Drone& d, float degrees) {
    if (!d.opponent.valid()) return false;
    return std::fabs(d.opp_facing_b) < degrees * (kPi / 180.0f);
}

bool opponent_is_aiming_at_me(const Drone& d, float degrees) {
    // DroneWeap_OpponentIsAimingAtMe: the opponent is armed and either just shot at us or (MP) faces us.
    if (!d.opponent.valid()) return false;
    if (d.shot_at) return true;
    return d.sys->config().multiplayer && opponent_facing_me(d, degrees);
}

int evasive_move(Drone& d) {
    // NDrone2_EvasiveMove 0x1506d8
    if ((d.flags & flag::kStationary) || !opponent_is_aiming_at_me(d)) return 0;
    int choices[4];
    int n = 0;
    if (gate(d, beh::kRoll, 0x53)) choices[n++] = 1;
    if (gate(d, beh::kStrafe, kAimStrafeLeft)) choices[n++] = 2;
    if (gate(d, beh::kStrafeDodge, kStrafeDodgeLeft)) choices[n++] = 3;
    if (gate(d, beh::kStep, 0x5a)) choices[n++] = 4;
    if (n == 0) return 0;
    DroneSystem& sys = *d.sys;
    auto coin = [&] { return sys.rand_int(2) == 0; };
    int state = 0;
    switch (choices[sys.rand_int(unsigned(n))]) {
        case 1:   // roll
            if (d.has_beh(beh::kRoll)) {
                if (coin()) {
                    if (can_roll_right(d)) state = 0x7a;
                    else if (can_roll_left(d)) state = 0x79;
                } else {
                    if (can_roll_left(d)) state = 0x79;
                    else if (can_roll_right(d)) state = 0x7a;
                }
            }
            [[fallthrough]];
        case 2:   // strafe (also the fallback of a failed roll)
            if (state == 0 && d.has_beh(beh::kStrafe)) {
                if (coin()) {
                    if (can_strafe_right(d)) state = 0x76;
                    else if (can_strafe_left(d)) state = 0x75;
                } else {
                    if (can_strafe_left(d)) state = 0x75;
                    else if (can_strafe_right(d)) state = 0x76;
                }
            }
            break;
        case 3:   // dodge
            if (d.has_beh(beh::kStrafeDodge)) {
                if (coin()) {
                    if (can_strafe_dodge_right(d)) state = 0x78;
                    else if (can_strafe_dodge_left(d)) state = 0x77;
                } else {
                    if (can_strafe_dodge_left(d)) state = 0x77;
                    else if (can_strafe_dodge_right(d)) state = 0x78;
                }
            }
            break;
        case 4:   // step
            if (coin()) {
                if (can_step_right(d)) state = 0x74;
                else if (can_step_left(d)) state = 0x73;
            } else {
                if (can_step_left(d)) state = 0x73;
                else if (can_step_right(d)) state = 0x74;
            }
            break;
        default: break;
    }
    if (d.dtype != kDtypeBot && state != 0) d.set_state(state);
    return state;
}
int choose_combat_move(Drone& d, const CombatStubs& st) {
    // NDrone2_ChooseCombatMove: behaviour- and anim-gated pick list (1 dodge / 2 strafe / 3 roll / 4 step /
    // 5 crouch-advance / 6 fallback), Rand-picked, then the leg for the pick. Pure selector: returns the
    // state (0 = none); the caller enters it. Draw order is load-bearing (diff-combat rand column).
    const bool stationary = (d.flags & flag::kStationary) != 0;
    auto rand_draw = [&](std::uint32_t n) -> std::uint32_t {
        if (st.rand_draw) return st.rand_draw(n);
        return n == 0 ? 0 : d.sys->rand_int(n);
    };
    auto anim_ok = [&](int dasc) { return st.anim_ok ? st.anim_ok(dasc) : anim_can_do(d, dasc); };
    if (stationary || d.has_beh(beh::kNeverMovesInCombat)) return 0;
    const bool bunched = d.mv.bunched;   // +0x17: a moving drone is in the way
    const bool aiming = opponent_is_aiming_at_me(d);
    const std::uint32_t pick15 = rand_draw(15);
    if (pick15 >= 7 && !aiming && !bunched) return 0;
    // Gated pick list (auStack_80).
    int codes[6] = {0, 0, 0, 0, 0, 0};
    int n = 0;
    if (aiming) {
        if (d.has_beh(beh::kStrafeDodge) && !stationary && anim_ok(0x29)) codes[n++] = 1;
    }
    if (d.has_beh(beh::kStrafe) && !stationary && anim_ok(0xc)) codes[n++] = 2;
    if (aiming) {
        if (d.has_beh(beh::kRoll) && anim_ok(0x53)) codes[n++] = 3;
    }
    if (d.has_beh(beh::kStep) && anim_ok(0x5a)) codes[n++] = 4;
    if (!aiming) {
        if (d.has_beh(beh::kAimCrouch) && anim_ok(5) && d.opp_dist >= 6.0f && rand_draw(2) == 0) codes[n++] = 5;
        codes[n++] = 6;
    }
    if (n == 0) return 0;   // Rand_Rand(0) is undefined; no pick list was built (all-gated aiming)
    // Legs: each picks a state or falls into the next (dodge -> strafe -> roll -> step -> 0x6f -> 0x67).
    const auto dodge_pick = [&] {
        if (rand_draw(2) == 0) {
            if (can_strafe_dodge_right(d)) return 0x78;
            if (can_strafe_dodge_left(d)) return 0x77;
            return 0;
        }
        if (can_strafe_dodge_left(d)) return 0x77;
        return 0;
    };
    const auto strafe_pick = [&] {
        if (rand_draw(2) == 0) {
            if (can_strafe_right(d)) return 0x76;
            if (can_strafe_left(d)) return 0x75;
            return 0;
        }
        if (can_strafe_left(d)) return 0x75;
        return 0;
    };
    const auto roll_pick = [&] {
        if (rand_draw(2) == 0) {
            if (can_roll_right(d)) return 0x7a;
            if (can_roll_left(d)) return 0x79;
            return 0;
        }
        if (can_roll_left(d)) return 0x79;
        return 0;
    };
    const auto step_pick = [&] {
        if (!d.has_beh(beh::kStep) || !anim_ok(0x5a)) return 0;
        if (rand_draw(2) == 0) {
            if (can_step_right(d)) return 0x74;
            if (can_step_left(d)) return 0x73;
            return 0;
        }
        if (can_step_left(d)) return 0x73;
        if (can_step_right(d)) return 0x74;
        return 0;
    };
    // Leg dispatch with fallthrough (dodge -> strafe -> roll -> step -> 0x6f -> 0x67), then the tail
    // adjustments (0x67 refused in 0x68, 0x6f refused in 0x70; picked == current returns 0).
    int picked = 0;
    switch (codes[rand_draw(std::uint32_t(n))] - 1) {
    case 0:   // dodge (needs aiming)
        if (aiming && d.has_beh(beh::kStrafeDodge)) {
            picked = dodge_pick();
            if (picked != 0) goto tail;
        } else if (!aiming) {
            break;   // post-switch step leg
        }
        [[fallthrough]];
    case 1:   // strafe (needs aiming)
        if (aiming && d.has_beh(beh::kStrafe)) {
            picked = strafe_pick();
            if (picked != 0) goto tail;
        } else if (!aiming) {
            break;   // post-switch step leg
        }
        [[fallthrough]];
    case 2:   // roll (needs aiming)
        if (aiming && d.has_beh(beh::kRoll)) {
            picked = roll_pick();
            if (picked != 0) goto tail;
        }
        break;   // post-switch step leg
    case 3:  // (code 4) post-switch step leg directly
        break;
    case 4:  // (code 5) 0x11 re-gate directly
        goto regate;
    case 5:  // (code 6) Rand(4) leg directly
        goto rand4;
    default: // uninitialized slot (pick beyond the list)
        return 0;
    }
    picked = step_pick();
    if (picked != 0) goto tail;
regate:
    if (d.has_beh(beh::kAimCrouch) && anim_ok(5) && d.opp_dist >= 6.0f && rand_draw(2) == 0) {
        picked = 0x6f;
        goto tail;
    }
rand4:
    picked = rand_draw(4) != 0 ? 0x67 : 0;
tail:
    if (picked == 0x67 && d.smi.cur == 0x68) picked = 0;
    if (picked == 0x6f && d.smi.cur == 0x70) picked = 0;
    return picked != d.smi.cur ? picked : 0;
}

void anim_for_dist(Drone& d, float dist) {
    // NDrone2_AnimForDist 0x1512e0
    int state = kRun;
    bool aiming = false;
    if (d.dtype == kDtypeBot) {
        state = kAimRun;
        if (!anim_can_do(d, kAimRun)) state = kRun;
    }
    const bool seen = (d.sight_flags & sight::kSeen) != 0;
    if (seen) {
        if (dist < d.engage_dist * 0.5f) {
            state = kAimStand;
            aiming = true;
        } else if (d.opp_dist < d.engage_dist) {
            const bool walk = (d.has_beh(0) || d.has_beh(2)) && anim_can_do(d, kAimWalk);
            if (walk) {
                state = kAimWalk;
                aiming = true;
            }
        }
    }
    if (!aiming && d.opp_dist < d.engage_dist * 1.5f) {
        if (d.has_beh(1) && !d.has_beh(2) && anim_can_do(d, kAimRun)) {
            state = kAimRun;
            aiming = true;
        }
    }
    if (d.anim.cur_state != state) anim_call(d, 8, state);
    if (aiming) {
        if (seen && std::fabs(d.opp_facing_b) < 0.7853982f) d.fire_requested = true;
    } else {
        d.fire_requested = false;
    }
}

bool set_combat_move_anim(Drone& d, float dist) {
    // DroneAnim_SetCombatMoveAnim 0x13cfb0 (move quad selection + the anim gates; see docs/ai-drone-core.md).
    if (!d.opponent.valid() || !d.have_dest_or_target()) {
        d.fire_requested = false;
        anim_call(d, 8, kRun);
        return true;
    }
    const float to_opp = heading_of(d.opp_pos[0] - d.pos[0], d.opp_pos[2] - d.pos[2]);
    const float to_dest = heading_of(d.mv.dest[0] - d.pos[0], d.mv.dest[2] - d.pos[2]);
    const float ang = angle_diff(to_opp, to_dest);   // 0 = moving toward the target
    // DroneAnim_GetMoveQuad with hysteresis on the previous quad (Drone+0x3c6)
    int quad;
    const float a = std::fabs(ang);
    switch (d.mv.move_quad) {
        case 1: quad = a > 2.495821f ? 2 : (ang > 0.7853982f ? 3 : (ang < -0.9250245f ? 1 : 0)); break;
        case 3: quad = a > 2.495821f ? 2 : (ang > 0.9250245f ? 3 : (ang < -0.7853982f ? 1 : 0)); break;
        case 2: quad = a > 2.2165682f ? 2 : (a > 0.7853982f ? (ang > 0 ? 3 : 1) : 0); break;
        default: quad = a > 2.3561947f ? 2 : (a > 0.9250245f ? (ang > 0 ? 3 : 1) : 0); break;
    }
    int state = kRun;
    bool aiming = false;
    const bool seen = d.lost_frames < d.seconds(3.0f);
    switch (quad) {
        case 0:   // forward
            if ((d.has_beh(1) || d.has_beh(0)) && anim_can_do(d, kAimRun)) { state = kAimRun; aiming = seen; }
            else if (d.has_beh(2) && anim_can_do(d, kAimWalk)) { state = kAimWalk; aiming = seen; }
            break;
        case 1:   // target on the other side: strafe left keeps the gun on target
            if ((d.has_beh(0) || d.has_beh(beh::kStrafe)) && anim_can_do(d, kAimStrafeLeft)) { state = kAimStrafeLeft; aiming = seen; }
            else if (d.has_beh(beh::kStep) && anim_can_do(d, 0x5a)) { state = 0x5a; aiming = false; }
            break;
        case 2:   // backing away
            if ((d.has_beh(0) || d.has_beh(6)) && anim_can_do(d, kAimBackoff)) { state = kAimBackoff; aiming = seen; }
            break;
        default:  // 3: mirror of 1
            if ((d.has_beh(0) || d.has_beh(beh::kStrafe)) && anim_can_do(d, kAimStrafeRight)) { state = kAimStrafeRight; aiming = seen; }
            else if (d.has_beh(beh::kStep) && anim_can_do(d, 0x5b)) { state = 0x5b; aiming = false; }
            break;
    }
    d.mv.move_quad = quad;
    (void)dist;
    if (d.anim.cur_state != state) anim_call(d, 8, state);
    d.fire_requested = aiming;
    return true;
}

// ---- per tick -------------------------------------------------------------------------------------------------------------
void move_step(Drone& d) {
    // NDrone2_Move 0x14fe?: steering, then the anim root motion moves the object (AnimObjectUpdate + AnimSeqTick).
    const float rec = d.sys->timing().rec();
    if (!d.mv.disabled) {
        // Steer: heading error turned by a fraction each tick (0.1 per 60 Hz frame, DefaultInit +0x4a0).
        const float err = angle_diff(d.yaw, d.mv.dest_angle);
        const float k = std::min(1.0f, d.mv.turn_rate * 60.0f / d.rate());
        d.yaw = wrap_pi(d.yaw + err * k);
    } else if (d.mv.fly) {
        // Scripted flight (abseil / astronaut): accelerate toward the destination, face it.
        d.fly_velocity += (d.mv.dest - d.pos) * (rec * d.mv.fly_speed);
        const float err = angle_diff(d.yaw, d.mv.dest_angle);
        d.yaw = wrap_pi(d.yaw + err * std::min(1.0f, 0.1f * 60.0f / d.rate()));
    }
    if (!d.mv.disabled) {
        // DroneMove_SetBoundryFlags: which sides are blocked within 0.5 m.
        std::uint32_t f = 0;
        if (!can_move_to_relative_impl(d, {0, 0, 0.5f})) f |= 2;
        if (!can_move_to_relative_impl(d, {0, 0, -0.5f})) f |= 4;
        if (!can_move_to_relative_impl(d, {0.5f, 0, 0})) f |= 8;
        if (!can_move_to_relative_impl(d, {-0.5f, 0, 0})) f |= 0x10;
        d.mv.boundary_flags = f;
    }
    const Vec3 before = d.pos;
    const Vec3 rm = d.mv.root_motion;
    const bool source_gate = d.anim.source_gate_valid;
    const bool source_fallback =
        source_gate && !d.anim.source_update_due && !d.mv.disabled && d.anim.step > 0.0f;
    Vec3 w = source_gate ? Vec3{} : to_world({rm[0], d.mv.disabled ? rm[1] : 0.0f, rm[2]}, d.yaw);
    if (source_fallback) {
        const float sin_yaw = weap::ps2_sin(d.yaw);
        const float cos_yaw = weap::ps2_sin(nf::game::ee_math::add(d.yaw, 1.5707964f));
        w = {nf::game::ee_math::mul(sin_yaw, d.anim.step), 0.0f,
             nf::game::ee_math::mul(cos_yaw, d.anim.step)};
    } else if (!source_gate && !d.mv.disabled && d.mv.have_dest && (w[0] == 0.0f && w[2] == 0.0f) &&
               d.anim.step > 0.0f) {
        // Legacy/non-v5 no-root fallback; v5 uses the source Drone_Control gate above.
        w = {std::sin(d.yaw) * d.anim.step * d.sys->timing().FRAME_RATE_MUL, 0.0f,
             std::cos(d.yaw) * d.anim.step * d.sys->timing().FRAME_RATE_MUL};
    }
    if (source_fallback) {
        d.pos[0] = nf::game::ee_math::add(d.pos[0], w[0]);
        d.pos[2] = nf::game::ee_math::add(d.pos[2], w[2]);
    } else {
        d.pos += w;
    }
    d.mv.speed = dist2d(before, d.pos);
    d.velocity = (d.pos - before) * d.rate();
    // DroneMove_NoBunching: keep drones from stacking on one spot.
    d.mv.bunched = false;
    for (const auto& o : d.sys->drones()) {
        if (o.get() == &d || !o->alive()) continue;
        const float dx = d.pos[0] - o->pos[0], dz = d.pos[2] - o->pos[2];
        const float dist = std::hypot(dx, dz);
        const float min_dist = d.radius + o->radius;
        if (dist < min_dist && std::fabs(d.pos[1] - o->pos[1]) < 1.5f) {
            if (o->mv.speed > 1e-3f) d.mv.bunched = true;
            const float push = (min_dist - dist) * 0.25f;
            if (dist > 1e-4f) {
                d.pos[0] += dx / dist * push;
                d.pos[2] += dz / dist * push;
            } else {
                d.pos[0] += push;
            }
        }
    }
}
void apply_animation_root_motion(Drone& d, const Vec3& root_delta, const std::array<Vec3, 3>& basis) {
    const Vec3 world_delta = basis[0] * root_delta[0] + basis[1] * root_delta[1] + basis[2] * root_delta[2];
    d.pos += world_delta;
}

CollisionStepState collision_pre_root_step(Drone& d) {
    if (d.anim.source_collision_supported) {
        bool near_player = false;
        for (int slot = 0; slot < 4; ++slot) {
            const Player* player = d.sys->world().player(slot);
            if (!player) continue;
            const Vec3 delta = player->pos - d.pos;
            near_player |= std::sqrt(delta[0] * delta[0] + delta[1] * delta[1] + delta[2] * delta[2]) < 2.0f;
        }
        const std::uint32_t flags = d.flags;
        const bool mode_allows_collision = (flags & 0x200u) == 0 || d.sys->config().level_id == 0x700004au;
        const bool collision_predicate =
            (flags & 0x20u) != 0 &&
            (d.char_class == 0x0c ||
             (d.char_class != 0x13 &&
              (d.anim.source_force_anim || near_player || d.anim.source_object_anim || (d.anim.cur_flags & 2u) == 0)));
        d.anim.source_collision_due = mode_allows_collision && collision_predicate;
        d.anim.source_gravity_due =
            (flags & 0x80u) != 0 &&
            ((flags & 0x4u) != 0 || (mode_allows_collision && collision_predicate));
        d.anim.source_collision_valid = true;
    }

    const bool source_collision_valid = d.anim.source_collision_valid;
    const bool source_collision_due = d.anim.source_collision_due;
    d.radius = d.char_class == 0x0c ? 0.55f : 0.4f;

    // NDrone2_Collision applies the AI-boundary push before AnimFrameResolve.
    if (!source_collision_valid || source_collision_due) {
        if (NavNetwork* nav = d.sys->nav()) {
            Vec3 push{};
            if (nav->bounds_push_vector(0.4f, nav->locate(d.pos), push)) {
                d.pos[0] += push[0];
                d.pos[2] += push[2];
            }
        }
    }
    CollisionStepState state;
    state.source_collision_valid = source_collision_valid;
    state.source_collision_due = source_collision_due;
    state.source_gravity_due = d.anim.source_gravity_due;
    if (!source_collision_valid || source_collision_due) {
        const float feet_height =
            d.anim.source_collision_valid && d.anim.source_callback_height_valid
                ? d.anim.source_callback_height
                : d.stand_height;
        CylinderQuery q;
        if (d.is_bot() && d.character) {
            q.a = {d.pos[0], d.pos[1] + d.radius, d.pos[2]};
            q.b = {d.pos[0], d.pos[1] + d.radius - feet_height, d.pos[2]};
            q.radius = d.radius;
        } else {
            q.a = {d.pos[0], d.pos[1] - d.stand_height + 1.4f, d.pos[2]};
            q.b = {d.pos[0], d.pos[1] - d.stand_height + d.radius, d.pos[2]};
            q.radius = d.radius;
        }
        state.capsule_a = q.a;
        state.capsule_b = q.b;
        state.capsule_radius = q.radius;
    }
    return state;
}

void collision_post_root_step(Drone& d, const CollisionStepState& state) {
    const bool source_collision_valid = state.source_collision_valid;
    const bool source_collision_due = state.source_collision_due;
    const bool source_gravity_due = state.source_gravity_due;
    const CollisionWorld& world = d.sys->collision();
    const FrameTiming timing = d.sys->timing();
    const bool diag_source_feet = d.anim.source_gate_supported && d.now() == 14740 &&
                                  std::fabs(d.pos[0] - 15.085f) < 0.1f;
    if (diag_source_feet)
        std::fprintf(stderr, "feet-enter y=%.8f stand=%.8f applied=%.8f cap-b=%.8f radius=%.8f\\n",
                     d.pos[1], d.stand_height, d.mv.applied_height, state.capsule_b[1], state.capsule_radius);
    // Feet stay planted while the pose height changes (crouch, roll).
    d.pos[1] += d.stand_height - d.mv.applied_height;
    d.mv.applied_height = d.stand_height;
    if (d.mv.disabled) {
        // Drone_CollisionHandler flight path: no gravity; the flight / knock velocity moves the body.
        d.fall_velocity = {};
        d.pos = d.pos + d.fly_velocity * timing.rec();
        if (d.flags & 0x40) d.fly_velocity = d.fly_velocity * 0.9f;
    }

    float source_feet_delta = 1.0f;
    bool source_hit_list_present = false;
    bool source_feet_delta_valid = false;
    const float h =
        source_collision_valid && d.anim.source_callback_height_valid
            ? d.anim.source_callback_height
            : d.stand_height;
    if (!source_collision_valid || source_collision_due) {
        const CylinderResult capsule =
            world.cylinder({state.capsule_a, state.capsule_b, state.capsule_radius});
        const FeetResult feet =
            world.feet_on_point(d.pos, state.capsule_b, {0, 1, 0}, h, capsule.contact);
        d.on_ground = feet.on_ground;
        source_hit_list_present = !capsule.hits.empty();
        if (source_collision_valid && source_collision_due && !d.mv.disabled) {
            d.on_ground = capsule.contact != 0;
            source_feet_delta_valid = true;
            if (feet.nearest) source_feet_delta = (d.pos[1] - h) - feet.nearest->point[1];
            if (source_feet_delta < 0.2f) d.on_ground = true;
            if (diag_source_feet)
                std::fprintf(stderr, "feet-probe delta=%.8f nearest=%d nearest-y=%.8f contact=%u on=%u ground=%.8f hits=%u\\n",
                             source_feet_delta, feet.nearest.has_value(),
                             feet.nearest ? feet.nearest->point[1] : 0.0f,
                             capsule.contact, unsigned(d.on_ground), feet.ground_normal_y,
                             unsigned(source_hit_list_present));
        }
        d.ground_normal_y = feet.ground_normal_y;
        if (source_hit_list_present) d.pos += capsule.push_out;
    } else {
        // NDrone2_DoCollision returned false: source skips the feet probe and hit push.
        d.on_ground = false;
        d.ground_normal_y = 1.0f;
    }
    // Drone_CollisionHandler applies NDrone2_DoGravity after collision and feet resolution.
    if (!d.mv.disabled && (!source_collision_valid || source_gravity_due)) {
        if (!d.on_ground ||
            (!(source_collision_valid && source_collision_due) && d.ground_normal_y < 0.5f)) {
            d.fall_velocity[1] -= 9.8f * timing.rec();
        } else {
            d.fall_velocity = {};
        }
        d.fall_velocity[1] = std::clamp(d.fall_velocity[1], -45.0f, 45.0f);
        d.pos = d.pos + d.fall_velocity * timing.rec();
    }
    // The source push branch and the FeetOnPoint snap are mutually exclusive.
    if (source_feet_delta_valid && !source_hit_list_present && d.on_ground && source_feet_delta > -0.1f) {
        d.pos[1] -= source_feet_delta * 0.125f;
    }
    if (diag_source_feet)
        std::fprintf(stderr, "feet-exit y=%.8f delta=%.8f cap-push=%.8f\\n",
                     d.pos[1], source_feet_delta, state.capsule_b[1]);
}

void place_on_floor(Drone& d) {
    const CollisionWorld& world = d.sys->collision();
    const Vec3 f = d.feet();
    if (const auto hit = world.point_on_floor({f[0], f[1] + 0.5f, f[2]}, 6.0f)) {
        d.pos[1] = (*hit)[1] + d.stand_height;
        d.on_ground = true;
        d.mv.applied_height = d.stand_height;
    }
}

}  // namespace nf::drone

// Reference content layer for the drone core (see drone_demo.hpp).
#include "game/drone_demo.hpp"

#include <cstdio>

#include "game/drone_anim.hpp"
#include "game/drone_impact.hpp"
#include "game/drone_move.hpp"
#include "game/drone_vision.hpp"
#include "game/drone_weap.hpp"

namespace nf::drone {

namespace {

constexpr int kStateAttack = 0x56;
constexpr int kStateGoTo = 0x63;

// The dispatch skeleton every non-scripted state shares (spec Part 3 §3.2): impacts, alerts.
int skeleton(Drone& d, const Msg& m, bool& handled) {
    handled = true;
    switch (m.id) {
        case kMsgPunch: case kMsgStunElectric: case kMsgBullet: case kMsgExplosive:
        case kMsgGas: case kMsgStunGrenade: case kMsgStunDart:
            handle_impact(d, m, kStateAttack, m.id != kMsgPunch);
            return 1;
        case kMsgShoutFirstSight: case kMsgShoutHurt: case kMsgSoundAlert: case kMsgShoutType3: case kMsgShoutType5:
        case kMsgShoutAttack: case kMsgDroneAlert: {
            const int next = enemy_alerts(d, m);
            if (next) d.set_state(next);
            return 1;
        }
        default:
            handled = false;
            return 0;
    }
}

int state_idle(Drone& d, const Msg& m) {
    bool handled;
    skeleton(d, m, handled);
    if (handled) return 1;
    switch (m.id) {
        case kMsgEnter:
            d.flags |= flag::kAware;                 // watching (Drone+0x4f8 & 0x10000)
            d.fire_requested = false;
            d.mv.disabled = false;
            stop(d);
            anim_call(d, 8, kStandAlert);
            return 1;
        case kMsgTick: {
            const int next = enemy_look_for_opponent(d);
            if (next) d.set_state(next);
            return 1;
        }
        case kMsgNone: case kMsgLeave: return 1;
        default: return 0;
    }
}

int state_attack(Drone& d, const Msg& m) {
    bool handled;
    skeleton(d, m, handled);
    if (handled) return 1;
    switch (m.id) {
        case kMsgEnter:
            d.flags |= flag::kAware;
            d.alertness = 1.0f;
            d.first_attack_time = d.now();
            d.alert_flags |= 0x10;   // attack shout bit: other drones that see us join in
            invalidate_attack_route(d);
            return 1;
        case kMsgLeave:
            d.fire_requested = false;
            return 1;
        case kMsgTick: {
            if (!d.opponent.valid()) {
                d.set_state(kStateIdle);
                return 1;
            }
            const bool seen = (d.sight_flags & 2) != 0;
            if (d.lost_frames > d.seconds(8.0f)) {   // lost the opponent for good
                d.flags &= ~std::uint32_t(flag::kAlertedByNoise | flag::kSawPlayer);
                d.set_state(kStateIdle);
                return 1;
            }
            if (d.opp_dist > d.engage_dist || !seen) {
                move_to_object(d, d.opponent, d.engage_dist * 0.8f);
                set_angle_to_dest(d);
                set_combat_move_anim(d, d.mv.route_distance);
            } else {
                // in range and visible: stand and shoot
                stop(d);
                set_angle_to_obj(d, d.opponent, 0.0f, true);
                if (d.anim.cur_state != kAimStand) anim_call(d, 8, kAimStand);
                weap::fire(d, true);
            }
            return 1;
        }
        case kMsgNone: return 1;
        default: return 0;
    }
}

int state_goto(Drone& d, const Msg& m) {
    bool handled;
    skeleton(d, m, handled);
    if (handled) return 1;
    DemoExt* ext = d.ext_as<DemoExt>();
    switch (m.id) {
        case kMsgEnter:
            d.flags |= flag::kAware;
            anim_call(d, 8, kWalk);
            if (ext) ext->arrived_logged = false;
            return 1;
        case kMsgTick: {
            const int next = enemy_look_for_opponent(d);
            if (next) {
                d.set_state(next);
                return 1;
            }
            if (!ext || !ext->has_goal) {
                d.set_state(kStateIdle);
                return 1;
            }
            const int status = move_to_goal_position(d, ext->goal, 1.0f);
            if (status == int(RouteStatus::Arrived)) {
                std::printf("drone %d arrived at goal (%.2f %.2f %.2f) after %u ticks\n", d.id, d.pos[0], d.pos[1],
                            d.pos[2], d.now() - d.smi.entry_time);
                d.set_state(kStateIdle);
            } else if (d.mv.have_dest) {
                set_angle_to_dest(d);
                if (d.anim.cur_state != kRun) anim_call(d, 8, kRun);
            }
            return 1;
        }
        case kMsgLeave: stop(d); return 1;
        case kMsgNone: return 1;
        default: return 0;
    }
}

}  // namespace

void register_demo_states() {
    register_state_if_free(kStateIdle, "DemoIdle", state_idle);
    register_state_if_free(kStateAttack, "DemoAttack", state_attack);
    register_state_if_free(kStateGoTo, "DemoGoTo", state_goto);
}

Drone& spawn_demo_drone(DroneSystem& sys, const Vec3& feet, float yaw, std::uint32_t skin_hash, int sub_class,
                        int weapon, float health, const Vec3* goal) {
    register_demo_states();
    SpawnInfo info;
    info.feet = feet;
    info.yaw = yaw;
    info.skin_hash = skin_hash;
    info.sub_class = sub_class;
    info.weapon = weapon;
    info.health = health;
    info.dtype = 0;
    info.side = kSideEnemy;
    info.initial_state = goal ? kStateGoTo : kStateIdle;
    for (int b : std::initializer_list<int>{beh::kSeesOpponents, beh::kHearsNoise, beh::kNoticeBelowAware, 0, beh::kStrafe, beh::kStep,
                  beh::kShoutsOnFirstSight, beh::kShoutReactFirstSight, beh::kShoutReactHurt})
        info.behaviour.set(b);
    auto ext = std::make_unique<DemoExt>();
    if (goal) {
        ext->goal = *goal;
        ext->has_goal = true;
    }
    info.ext = std::move(ext);
    Drone& d = sys.spawn(std::move(info));
    d.alertness = d.alertness_floor = 0.0f;
    return d;
}

}  // namespace nf::drone

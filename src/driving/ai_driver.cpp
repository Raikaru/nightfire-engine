#include "driving/ai_driver.hpp"

#include <algorithm>
#include <cmath>
#include "core/rng.hpp"  // process-global game Rand stream (AI MG accuracy rolls)

namespace nf::driving {
namespace {

float yaw_of_forward(const Vec3& f) { return std::atan2(f[0], f[2]); }

// Shortest signed angle from a to b.
float angle_diff(float a, float b) {
    float d = b - a;
    while (d > 3.14159265f) d -= 6.2831853f;
    while (d < -3.14159265f) d += 6.2831853f;
    return d;
}

}  // namespace

AiDriver::AiDriver(const VehicleParams& params, const PhysicsGlobals& globals, const Vec3& half_extents,
                   const WeaponSpec& weapons, AiRole role, float max_health, AiDynamics dyn)
    : weapons_(weapons, max_health), role_(role), dyn_(dyn) {
    const float top = params.gear_limit[5] > 0 ? params.gear_limit[5] : 40.0f;
    if (dyn == AiDynamics::Car) vehicle_ = std::make_unique<Vehicle>(params, globals, half_extents);
    if (dyn == AiDynamics::Sled) sled_ = std::make_unique<Snowmobile>(params, top);
    if (dyn == AiDynamics::Sub) sub_ = std::make_unique<Submarine>(params, top);
}

void AiDriver::reset(const Vec3& pos, float yaw, const TrackCollision& collision) {
    if (dyn_ == AiDynamics::Heli) {
        heli_pos_ = pos;
        heli_yaw_ = yaw;
        heli_anchor_ = pos;
        return;
    }
    if (dyn_ == AiDynamics::Sub) {
        sub_->reset(pos, yaw);
        return;
    }
    Vec3 p = pos;
    GroundHit g;
    if (collision.ground_below({p[0], p[1] + 3.0f, p[2]}, g)) p[1] = g.point[1];
    p[1] += dyn_ == AiDynamics::Sled ? 0.5f : 0.6f;
    if (dyn_ == AiDynamics::Sled) sled_->reset(p, yaw);
    else vehicle_->reset(p, yaw);
    stuck_timer_ = 0;
    unstuck_phase_ = 0;
    // fire_timer_ keeps its spawn value (8 s grace: no spawn-camping).
}

Vec3 AiDriver::position() const {
    if (dyn_ == AiDynamics::Heli) return heli_pos_;
    if (dyn_ == AiDynamics::Sled) return sled_->position();
    if (dyn_ == AiDynamics::Sub) return sub_->position();
    return vehicle_->body().position();
}

Vec3 AiDriver::forward() const {
    if (dyn_ == AiDynamics::Heli) return {std::sin(heli_yaw_), 0, std::cos(heli_yaw_)};
    if (dyn_ == AiDynamics::Sled) return sled_->forward();
    if (dyn_ == AiDynamics::Sub) return sub_->forward();
    return vehicle_->body().axes().forward;
}

Mat4 AiDriver::body_matrix() const {
    if (dyn_ == AiDynamics::Sled) return sled_->model_matrix();
    if (dyn_ == AiDynamics::Sub) return sub_->model_matrix();
    if (dyn_ == AiDynamics::Heli) {
        const float c = std::cos(heli_yaw_), s = std::sin(heli_yaw_);
        return {c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, heli_pos_[0], heli_pos_[1], heli_pos_[2], 1};
    }
    return vehicle_->model_matrix();
}

void AiDriver::nudge(const Vec3& dp) {
    if (dyn_ == AiDynamics::Sled) sled_->nudge(dp);
    else if (dyn_ == AiDynamics::Sub) sub_->nudge(dp);
    else if (dyn_ == AiDynamics::Heli) heli_pos_ += dp;
    else vehicle_->nudge(dp);
}

float AiDriver::speed() const {
    if (dyn_ == AiDynamics::Sled) return sled_->speed();
    if (dyn_ == AiDynamics::Sub) return sub_->speed();
    if (dyn_ == AiDynamics::Heli || !vehicle_) return 0;
    return vehicle_->speed();
}

bool AiDriver::awake(const Vec3& player_pos) const { return length(position() - player_pos) < wake_range_; }

DriveInput AiDriver::lane_drive(const Vec3& from, float yaw_now, const Vec3& target, float target_speed) {
    DriveInput in;
    const float want = yaw_of_forward(target - from);
    const float d = angle_diff(yaw_now, want);
    in.steer = std::clamp(d * 2.5f, -1.0f, 1.0f);
    const float dist = length(target - from);
    const float spd = dyn_ == AiDynamics::Sled ? sled_->speed()
                                              : (vehicle_ ? vehicle_->forward_speed() : 0);
    if (std::abs(d) > 1.2f && spd > 4.0f) {
        in.brake = 0.6f;  // hairpin: slow before turning (DriveToPointTraffic)
    } else if (spd < target_speed && dist > 3.0f) {
        in.gas = 1.0f;
    } else if (spd > target_speed + 2.0f) {
        in.brake = 0.5f;
    } else if (dist > 3.0f) {
        in.gas = 0.5f;
    }
    return in;
}

AiEvents AiDriver::step(const Vec3& player_pos, const Vec3& player_vel, const RoadNetwork& road, int node,
                        const TrackCollision& collision, const std::vector<Vec3>& blockers,
                        std::vector<Projectile>& projectiles, std::vector<HazardZone>& zones,
                        std::vector<SoundEvent>& sfx, float now) {
    AiEvents ev;
    weapons_.update(kTickDt);
    if (!weapons_.alive()) return ev;  // wreck: no drive, no fire
    if (dyn_ == AiDynamics::Heli) {
        // Kinematic scripted flight (AIHelicopter_*): ease toward the mission's anchor, face the
        // player, fire rockets/bullets. Altitude comes from the anchor (rspath_* heights).
        const Vec3 to = heli_anchor_ - heli_pos_;
        const float dist = length(to);
        if (dist > 2.0f) heli_pos_ += to * (std::min(18.0f, dist * 0.8f) * kTickDt / dist);
        heli_yaw_ = yaw_of_forward(player_pos - heli_pos_);
        fire_timer_ -= kTickDt;
        if (fire_timer_ <= 0) {
            const float pd = length(player_pos - heli_pos_);
            if (pd < 150.0f && pd > 8.0f) {
                // Line of sight: hold fire through terrain (canopy/buildings block AI gunners).
                SegmentHit blocked;
                if (collision.segment_hit(heli_pos_, player_pos, blocked) &&
                    blocked.t * pd < pd - 4.0f) {
                    fire_timer_ = 1.0f;
                    return ev;
                }
                const Vec3 dir = (player_pos - heli_pos_) * (1.0f / pd);
                const Vec3 muzzle = heli_pos_ + dir * 4.0f;
                if (weapons_.spec().machine_guns)
                    if (weapons_.fire_primary(now, muzzle, dir, 0.4f, sfx)) ++ev.shots_fired;
                if (!weapons_.secondaries().empty())
                    if (weapons_.fire_secondary(now, muzzle, dir, false, projectiles, sfx)) ++ev.shots_fired;
                fire_timer_ = 4.0f;
            }
        }
        return ev;
    }
    const Vec3 pos = position();
    const Vec3 fwd = forward();
    const float yaw = yaw_of_forward(fwd);

    // EMP hold / smoke blindness (DisableSteering / GetBeenInSmoke equivalents).
    bool held = weapons_.emp_held();
    if (weapons_.in_smoke) smoke_blind_ = 4.0f;
    smoke_blind_ = std::max(0.0f, smoke_blind_ - kTickDt);
    weapons_.in_smoke = weapons_.in_oil = false;  // re-evaluated by the mission every tick

    // CancelStuck: almost no wheel speed while demanding drive -> reverse out briefly.
    const float spd = dyn_ == AiDynamics::Sled ? std::abs(sled_->speed())
                                               : std::abs(vehicle_->forward_speed());
    if (role_ != AiRole::Parked && dyn_ == AiDynamics::Car) {
        if (spd < 1.0f) stuck_timer_ += kTickDt;
        else stuck_timer_ = 0;
    }
    DriveInput in;
    Vec3 aim = pos + fwd * 10.0f;
    float fire_range = 0;
    if (held) {
        in = DriveInput{};  // DisableSteering: coast
    } else if (unstuck_phase_ > 0) {
        unstuck_phase_ -= kTickDt;
        in.brake = 1.0f;  // reverse (BRAKE at standstill selects reverse in Vehicle)
        in.steer = -1.0f;
    } else if (stuck_timer_ > 2.0f) {
        stuck_timer_ = 0;
        unstuck_phase_ = 1.5f;
    } else {
        switch (role_) {
            case AiRole::Parked: in.handbrake = true; break;
            case AiRole::Traffic: {
                // Cruise the lane; brake for blockers (CheckTrafficCollision). At dead ends
                // (intersection gaps) keep pushing straight onto the next span.
                Vec3 target = pos + fwd * 30.0f;
                if (node >= 0 && !road.empty()) {
                    const int nx = road.successor(node, fwd);
                    if (nx >= 0) {
                        target = road.node(std::size_t(nx)).pos;
                        const int nx2 = road.successor(nx, road.node(std::size_t(nx)).dir);
                        if (nx2 >= 0) target = target + (road.node(std::size_t(nx2)).pos - target) * 0.4f;
                    }
                }
                in = lane_drive(pos, yaw, target, cruise_speed_);
                for (const Vec3& b : blockers) {
                    const Vec3 d = b - pos;
                    if (dot(d, fwd) > 0 && length(d) < 14.0f) {
                        in.gas = 0;
                        in.brake = 0.8f;
                        break;
                    }
                }
                break;
            }
            case AiRole::Fleeing: {
                // Run away from the player along the lanes.
                Vec3 target = pos + (pos - player_pos);
                if (node >= 0 && !road.empty()) {
                    const int nx = road.successor(node, fwd);
                    Vec3 away = road.node(std::size_t(node)).pos - player_pos;
                    away[1] = 0;
                    target = pos + away * 0.1f + fwd * 15.0f;
                    if (nx >= 0) {
                        const Vec3 cand = road.node(std::size_t(nx)).pos - player_pos;
                        if (length(cand) > length(away)) target = road.node(std::size_t(nx)).pos;
                    }
                }
                in = lane_drive(pos, yaw, target, cruise_speed_ + 8.0f);
                break;
            }
            case AiRole::Pursuer: {
                // Chase + ram (DoAttackMode); fire when roughly behind/in front in range.
                const Vec3 lead = player_pos + player_vel * 0.5f;
                in = lane_drive(pos, yaw, lead, cruise_speed_ + 14.0f);
                aim = player_pos;
                fire_range = 55.0f;
                const float dist = length(player_pos - pos);
                if (dist < 6.0f) ev.rammed_player = true;
                break;
            }
            case AiRole::Heli: break;  // handled by the kinematic branch above
        }
    }

    if (dyn_ == AiDynamics::Car) {
        if (weapons_.boost_now()) in.gas = 1.0f;  // EnableRocketBoost throttle
        vehicle_->step(in, collision);
    } else if (dyn_ == AiDynamics::Sled) {
        sled_->step(in, collision);
    } else if (dyn_ == AiDynamics::Sub) {
        // Depth-aware pursuit: steer toward the mark, planes toward its depth.
        Vec3 mark = aim;
        if (role_ == AiRole::Traffic && node >= 0 && !road.empty())
            mark = road.node(std::size_t(node)).pos;  // swim paths run at depth
        const float want_yaw = yaw_of_forward(mark - pos);
        const float depth_err = std::clamp((pos[1] - mark[1]) * 0.3f, -1.0f, 1.0f);
        const FlightInput fi{std::clamp(angle_diff(yaw, want_yaw) * 2.0f, -1.0f, 1.0f), depth_err, 1.0f, 0.0f};
        sub_->step(held ? FlightInput{} : fi, collision);
    }

    // Return fire (FireBullets/Missiles/Rockets with accuracy-scaled aim).
    fire_timer_ -= kTickDt;
    if (fire_range > 0 && fire_timer_ <= 0 && !held && smoke_blind_ <= 0) {
        const Vec3 to = aim - pos;
        const float dist = length(to);
        if (dist < fire_range && dist > 4.0f && dot(to * (1.0f / dist), fwd) > 0.65f) {
            // Line of sight: no shooting through hills/buildings.
            SegmentHit blocked;
            if (collision.segment_hit(pos + Vec3{0, 1.0f, 0}, aim, blocked) &&
                blocked.t * dist < dist - 3.0f) {
                fire_timer_ = 1.0f;
                return ev;
            }
            const Vec3 dir = to * (1.0f / dist);
            const Vec3 muzzle = pos + dir * 3.0f + Vec3{0, 1.0f, 0};
            if (weapons_.spec().machine_guns) {
                if (weapons_.fire_primary(now, muzzle, dir, 1.0f - accuracy_, sfx)) ++ev.shots_fired;
                // Accuracy roll: some bursts chip the player (mission applies 2 hp).
                if (dist < 45.0f && nf::game_rng().frand(1.0f) < accuracy_ * 0.5f) ev.mg_hit = true;
            }
            if (!weapons_.secondaries().empty() && dist < 50.0f && dist > 10.0f) {
                if (weapons_.fire_secondary(now, muzzle, dir, false, projectiles, sfx)) ++ev.shots_fired;
            }
            fire_timer_ = 2.5f + (1.0f - accuracy_) * 3.0f;
        }
    }
    (void)zones;
    return ev;
}

}  // namespace nf::driving

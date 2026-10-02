#include "driving/vehicle.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace nf::driving {

namespace {

constexpr float kTwoPi = 6.2831855f;  // 0x40C90FDB (sub_213618)

// The engine's own sin/cos (sub_2CBD50 / sub_2CBEB0) take angles in turns (one revolution = 1.0).
float turns_to_rad(float turns) { return turns * kTwoPi; }

// Move `cur` towards `target` by at most `step` (sub_2104A8).
float slew(float cur, float target, float step) {
    if (target < cur) return (cur - step < target) ? target : cur - step;
    return (target < cur + step) ? target : cur + step;
}

// Value clamp of sub_210510: beyond +-limit the replacement magnitude is used (here the same number).
float clamp_limit(float v, float limit) { return v > limit ? limit : (v < -limit ? -limit : v); }

// sub_213618: height jitter of rough surfaces, added to the wheel-to-ground distance.
float surface_bump(Surface s, const Vec3& p) {
    float freq = 2.0f, amp = 0;
    switch (s) {
        case Surface::Gravel:
        case Surface::Grass:
        case Surface::Dirt:
        case Surface::PavedRough:
            amp = 0.01f;
            break;
        case Surface::Cobble:
        case Surface::Wood:
            amp = 0.015f;
            break;
        case Surface::Water:
            freq = 0;
            amp = -10.0f;
            break;
        default:
            return 0;
    }
    return (std::sin(p[2] * freq * kTwoPi) + std::cos(p[0] * freq * kTwoPi)) * amp;
}

// Corner i (0..7) of the collision box in body space (sub_1F8CF0 stack sequence).
Vec3 box_corner(int i, const Vec3& h) {
    static const float sx[8] = {-1, 1, 1, -1, -1, 1, 1, -1};
    static const float sy[8] = {-1, -1, -1, -1, 1, 1, 1, 1};
    static const float sz[8] = {-1, -1, 1, 1, 1, 1, -1, -1};
    return {sx[i] * h[0], sy[i] * h[1], sz[i] * h[2]};
}

}  // namespace

// Parameters of the four tyres for one tick: the array a2 of sub_18C1B0 (S2+0x170..).
struct Vehicle::DriveCommand {
    std::array<float, 4> grip{};   // a2[0]: lateral stiffness (front/rear share * TYREGRIPFACTOR)
    std::array<float, 4> drive{};  // a2[1]: drive (+) or brake (-) acceleration along the wheel
    std::array<float, 4> limit{};  // a2[2]: friction limit
    std::array<float, 4> hbf{};    // a2[3]: locked-wheel (handbrake) drag
    bool wheelspin = false;        // S2+0x445
    Vec3 roll_drag{};              // S2+0x160: horizontal velocity * ROLLING_RESISTANCE
    Vec3 velocity{};               // body velocity the tyre model reads
};

Vehicle::Vehicle(const VehicleParams& params, const PhysicsGlobals& globals, const Vec3& half_extents)
    : params_(params), g_(globals), body_(params.mass, half_extents) {
    const Vec3& h = half_extents;
    const float x = params.wheel_x_offset, zf = params.wheel_zf_offset, zr = params.wheel_zr_offset;
    // sub_1F72D8: the first four levers are the wheels, on the bottom face of the box.
    wheel_local_[0] = {x - h[0], -h[1], h[2] + zf};
    wheel_local_[1] = {h[0] - x, -h[1], h[2] + zf};
    wheel_local_[2] = {x - h[0], -h[1], zr - h[2]};
    wheel_local_[3] = {h[0] - x, -h[1], zr - h[2]};
    reset({0, 0, 0}, 0);
}

void Vehicle::reset(const Vec3& position, float yaw) {
    body_.reset(position, axes_from_yaw(yaw));
    wheel_ = {};
    steer_raw_ = steer_ = 0;
    gas_raw_ = gas_event_ = brake_raw_ = gas_ = brake_ = 0;
    handbrake_ = handbrake_reverse_ = handbrake_input_ = burnout_ = false;
    speed_h_ = steer_angle_ = wheel_spin_ = phase_front_ = phase_rear_ = 0;
    fric_mod_ = 1;
    grounded_ = 0;
    drive_state_ = 0;
    reverse_ = braking_forward_ = counter_steering_ = collided_ = false;
    reverse_timer_ = 0;
    air_ticks_ = 0;
    gear_ = 1;
    prev_gear_ = -1;
    shift_timer_ = 0;
    rpm_ = 0;
    skid_wheelspin_ = 0;
    boost_timer_ = 0;
    since_object_hit_ = 0;
    slip_out_ = {};
    const Axes& ax = body_.axes();
    for (int i = 0; i < 4; ++i) {
        wheel_[i].position = position + ax.to_world(wheel_local_[i]);
        wheel_poses_[i] = {};
        wheel_poses_[i].position = wheel_[i].position;
    }
}

bool Vehicle::enable_rocket_boost() {
    // sub_18B768: allowed once the previous boost is past its first phase (BOOST_TIME ticks).
    if (3 * params_.boost_time < boost_timer_) return false;
    boost_timer_ = 4 * params_.boost_time;
    return true;
}

CameraTarget Vehicle::camera_target() const {
    const Axes& a = body_.axes();
    CameraTarget t;
    t.position = body_.position();
    t.right = a.right;
    t.up = a.up;
    t.forward = a.forward;
    t.velocity = body_.velocity();
    t.speed = length(t.velocity);
    return t;
}

Mat4 Vehicle::model_matrix() const {
    const Axes& a = body_.axes();
    const Vec3& p = body_.position();
    return {a.right[0],   a.right[1],   a.right[2],   0, a.up[0],      a.up[1],      a.up[2],      0,
            a.forward[0], a.forward[1], a.forward[2], 0, p[0],         p[1],         p[2],         1};
}

void Vehicle::step(const DriveInput& input, const CollisionWorld& world) {
    update_controls(input);
    // sub_1F7C60: gravity joins the force accumulator (the original adds it at the end of the previous step).
    body_.add_accel({0, g_.gravity, 0});
    car_forces(world);
    collide_world(world);
    body_.integrate(kTickDt, g_.m_limit, g_.am_limit);
    if (since_object_hit_ < 0xFFFF) ++since_object_hit_;

    for (int i = 0; i < 4; ++i) {
        WheelPose& p = wheel_poses_[i];
        p.position = wheel_[i].position;
        p.compression = wheel_[i].compression;
        p.steer_angle = i < 2 ? steer_angle_ : 0;
        p.spin_phase = i < 2 ? phase_front_ : phase_rear_;
        p.slip = std::max((slip_out_[i] - 0.3f) * 1.4285715f, 0.0f);
        p.in_contact = wheel_[i].contact;
        p.surface = wheel_[i].surface;
    }
}

// PBondCar::GetControllerInput (sub_18ADD8), player branch.
void Vehicle::update_controls(const DriveInput& in) {
    // The action queue only delivers a value when the device scalar changes; the level is what matters.
    if (in.gas != gas_event_) {
        gas_raw_ = in.gas;
        gas_event_ = in.gas;
    }
    brake_raw_ = in.brake;
    steer_raw_ = in.steer;
    if (in.handbrake != handbrake_input_) {
        handbrake_input_ = in.handbrake;
        handbrake_ = in.handbrake;
        if (in.handbrake) handbrake_reverse_ = dot(body_.velocity(), body_.axes().forward) < 0.0f;
    }
    // GAMEACTION_GAS is ramped: doubled every tick up to 1 (a1+0x278), so any press becomes full throttle.
    gas_raw_ = std::min(gas_raw_ + gas_raw_, 1.0f);

    const bool steer_big = std::fabs(steer_raw_) > 0.9f;
    const float ws_limit = burnout_ ? params_.ws_speed * 4.0f : params_.ws_speed;
    if (grounded_ >= 3) {
        burnout_ = gas_raw_ > 0.9f ? (steer_big && speed_h_ < ws_limit) : false;
    } else {
        burnout_ = false;
    }
    gas_ = gas_raw_;
    brake_ = brake_raw_;
    if (brake_ >= gas_) gas_ = 0;
    else brake_ = 0;
    if (handbrake_) gas_ = brake_ = 0;

    if (-g_.pad_dead_zone < steer_raw_ && steer_raw_ < g_.pad_dead_zone) steer_raw_ = 0;
    steer_ = slew(steer_, steer_raw_, 0.25f);
}

void Vehicle::query_wheel(int i, const CollisionWorld& world) {
    WheelRuntime& w = wheel_[i];
    const Vec3 p = w.position;
    GroundHit hit;
    bool found = world.ground_below(p, hit);
    const bool had_ground = w.cache_valid && w.cache.surface != Surface::NoDrive;
    if (!found) {
        if (had_ground) {
            // sub_1F6AE0 retries three times, 0.1 m further along +X each time (a crack between triangles).
            for (int k = 1; k <= 3 && !found; ++k) {
                Vec3 q = p;
                q[0] += 0.1f * static_cast<float>(k);
                found = world.ground_below(q, hit);
            }
            if (!found) hit = w.cache;  // keep the last known triangle
        } else {
            hit = GroundHit{p, {0, 1, 0}, Surface::Paved};  // sub_232228: flat probe triangle at the wheel
        }
    }
    w.cache = hit;
    w.cache_valid = true;
    Vec3 n = hit.normal;
    if (n[1] < 0) n = vneg(n);
    if (n[1] > 0.9999f) n[1] = 0.9999f;
    w.ground_normal = n;
    w.ground_w = dot(hit.point - p, n) + surface_bump(hit.surface, p);
    w.surface = hit.surface;
}

// PBondCar::ProcessPhysics (sub_18CEF8) for a player-controlled car.
void Vehicle::car_forces(const CollisionWorld& world) {
    const VehicleParams& P = params_;
    const Axes ax = body_.axes();
    const Vec3 vel = body_.velocity();
    const Vec3 vel_h{vel[0], 0, vel[2]};
    const Vec3 pos = body_.position();

    speed_h_ = length(vel_h);
    const float v_fwd = dot(vel_h, ax.forward);
    skid_wheelspin_ = 0;

    DriveCommand cmd;
    cmd.velocity = vel;
    cmd.roll_drag = vel_h * g_.rolling_resistance;

    // Reverse: braking from (nearly) standstill engages it after a delay.
    reverse_ = false;
    braking_forward_ = false;
    float front_share = 0;
    if (brake_ == 0) {
        reverse_timer_ = 30;
    } else if (v_fwd >= 2.5f) {
        reverse_timer_ = 0;
        braking_forward_ = true;
    } else if (reverse_timer_ >= 30) {
        if (v_fwd <= 0.0f) {
            front_share = P.reverse_frw_ratio;
            drive_state_ = 3;
            reverse_ = true;
        }
    } else {
        ++reverse_timer_;
        if (!(v_fwd >= 0.5f)) brake_ = 0;
    }

    if (!reverse_) {
        if (g_.min_button_value >= brake_) {
            if (g_.min_button_value >= gas_) {
                drive_state_ = 0;
                front_share = P.coast_frw_ratio;
            } else {
                drive_state_ = 1;
                front_share = P.accel_frw_ratio;
            }
        } else {
            drive_state_ = 2;
            front_share = P.brake_frw_ratio;
        }
    }
    float rear_share = 1.0f - front_share;

    // Handbrake.
    float hb_force = 0;
    if (handbrake_) {
        if (speed_h_ >= 1.0f) {
            hb_force = P.handbrake_force;
        } else if (ax.up[1] <= 0.707f) {
            hb_force = speed_h_ + speed_h_ + 1.0f;
        } else {
            const Vec3& p = body_.momentum();
            body_.set_momentum({0, p[1], 0});  // parked: horizontal momentum is dropped
        }
        if (handbrake_reverse_) {
            drive_state_ = 5;
            front_share = P.rev_handbrake_frw_ratio * 0.5f;
            rear_share = (1.0f - front_share) * 0.5f;
            front_share *= P.total_handbrake_friction_scale;
        } else {
            drive_state_ = 4;
            front_share = P.handbrake_frw_ratio;
            rear_share = (1.0f - front_share) * P.handbrake_rw_fric_scale;
            front_share *= P.total_handbrake_friction_scale;
        }
        rear_share *= P.total_handbrake_friction_scale;
        if (speed_h_ < 5.0f) front_share = rear_share = 0.5f;
    }
    if (drive_state_ == 2) {
        front_share *= P.braking_friction;
        rear_share *= P.braking_friction;
    }
    cmd.wheelspin = burnout_ && !handbrake_;
    if (drive_state_ == 1 && wheel_spin_ < 0.0f) {
        const float k = std::min(1.0f, P.braking_friction + P.braking_friction);
        if (P.is_rally) front_share *= k;
        rear_share *= k;
        if (gas_ > 0.75f) cmd.wheelspin = true;
    }
    if (cmd.wheelspin) skid_wheelspin_ = 1.0f;

    // Counter-steer grip boost.
    float counter = 1.0f;
    counter_steering_ = false;
    if (P.counter_steer_scale != 0) {
        const float yaw_rate = body_.angular_velocity()[1];
        if ((yaw_rate > 0 && steer_ < 0) || (yaw_rate < 0 && steer_ > 0)) counter_steering_ = true;
        if (counter_steering_) {
            counter = std::fabs(yaw_rate) * P.counter_steer_scale * std::fabs(steer_);
            if (drive_state_ == 1) counter *= 1.2f;
            counter = std::max(std::min(counter, P.counter_steer_scale_limit), 1.0f);
        }
    }

    // Rocket boost timer: extra acceleration in the first BOOST_TIME ticks.
    float boost_acc = 0;
    if (boost_timer_ != 0) {
        const int bt3 = 3 * P.boost_time;
        --boost_timer_;
        if (bt3 < boost_timer_) boost_acc = P.boost_extra_acc;
    }

    if (gear_ != prev_gear_) {
        prev_gear_ = gear_;
        shift_timer_ = 15;
    }
    const float max_acc = P.max_acc * P.gear_ratio[gear_];
    float rev_acc = P.max_rev_acc;
    if (reverse_) {
        const float excess = speed_h_ - P.speed_cutoff_reverse;
        if (excess > 0) rev_acc = std::max(rev_acc - excess * P.speed_cutoff_rate_reverse, 0.0f);
    }
    float drive_cmd;
    if (reverse_ && boost_timer_ == 0) drive_cmd = (boost_acc + gas_ * max_acc) - brake_ * rev_acc;
    else drive_cmd = (boost_acc + gas_ * max_acc) - brake_ * P.max_brake;
    if (extra_drag_ != 0) drive_cmd -= extra_drag_ * (speed_h_ >= 0 ? 1.0f : -1.0f);

    // Free-spin rate of the wheels in revolutions per tick.
    float rev_rate = std::sqrt(vel_h[0] * vel_h[0] + vel_h[2] * vel_h[2]) / (P.wheel_radius * 6.32f * kTickHz);
    if (rev_rate < 0.01f && handbrake_) rev_rate = 0;
    rev_rate = std::min(rev_rate, 1.0f);
    if (grounded_ == 0 || ax.up[1] <= 0.1f) {
        wheel_spin_ *= braking_forward_ ? 0.25f : 0.95f;
    } else {
        const float s = std::min(rev_rate, g_.max_wheel_spin_rate);
        wheel_spin_ = v_fwd < 0 ? -s : s;
    }

    // Steering angle and steered axle direction.
    float steer_mod = 1.0f;
    if (P.mod_steer_speed < speed_h_) steer_mod = 1.0f - std::min(1.0f, (speed_h_ - P.mod_steer_speed) / 25.0f);
    steer_mod = std::max(steer_mod, P.min_steer);
    steer_angle_ = turns_to_rad(steer_ * P.max_steering * steer_mod);
    const Axes steered_axes{ax.right, ax.up, ax.forward * std::cos(steer_angle_) + ax.right * std::sin(steer_angle_)};

    // Boost raises the friction limits while it lasts.
    float boost_grip = 1.0f;
    if (boost_timer_ != 0) {
        const int bt3 = 3 * P.boost_time;
        boost_grip = (bt3 >= boost_timer_) ? (static_cast<float>(boost_timer_) / static_cast<float>(bt3)) * 2.0f + 1.0f
                                           : 3.0f;
    }
    const float limit_front = P.friction_limit_front * boost_grip * grip_front_;
    const float limit_rear = P.friction_limit_rear * boost_grip * counter * grip_rear_;

    for (int i = 0; i < 4; ++i) {
        wheel_[i].position = pos + ax.to_world(wheel_local_[i]);
        query_wheel(i, world);
        const bool rear = i >= 2;
        float drive;
        if (rear) {
            cmd.grip[i] = rear_share * P.tyre_grip_factor * counter;
            drive = cmd.wheelspin ? drive_cmd * P.ws_accel : (drive_cmd >= 0 ? drive_cmd : 0.0f);
            cmd.limit[i] = limit_rear;
        } else {
            cmd.grip[i] = front_share * P.tyre_grip_factor;
            drive = drive_cmd < 0 ? drive_cmd : (P.is_rally ? drive_cmd : 0.0f);
            cmd.limit[i] = limit_front;
        }
        drive *= cmd.grip[i];
        cmd.hbf[i] = hb_force;
        const float f = g_.friction[static_cast<int>(wheel_[i].surface)];
        cmd.limit[i] *= f;
        cmd.drive[i] = drive * f;
    }

    // Lateral grip falls with speed.
    if (P.fric_mod_speed >= speed_h_) {
        fric_mod_ = 1.0f;
    } else {
        float f = std::min(speed_h_ - P.fric_mod_speed, P.fric_mod_range);
        f = 1.0f - f / P.fric_mod_range;
        fric_mod_ = std::max(f, P.fric_mod_min);
    }

    grounded_ = add_wheel_forces(cmd, steered_axes);

    // Parked with the handbrake on and no throttle: stop dead.
    if (grounded_ == 4 && handbrake_ && gas_ < 0.1f && length(vel) < 0.08f) {
        body_.set_momentum({0, vel[1] * body_.mass(), 0});
        const Vec3& f = body_.force();
        body_.set_force({0, f[1], 0});
        const Vec3& t = body_.torque();
        body_.set_torque({t[0], 0, t[2]});
        body_.set_angular_momentum({0, 0, 0});
    }

    // Wheel rotation for the renderer.
    if (!braking_forward_ || brake_ <= 0.5f) {
        if (cmd.wheelspin) {
            phase_front_ += wheel_spin_ * 0.1f;
        } else {
            phase_front_ += wheel_spin_;
            if (!handbrake_) phase_rear_ += wheel_spin_;
        }
    }
    if (cmd.wheelspin) phase_rear_ += g_.max_wheel_spin_rate;
    for (float* ph : {&phase_front_, &phase_rear_}) {
        if (*ph > 1.0f) *ph -= static_cast<float>(static_cast<int>(*ph));
        else if (*ph < 0.0f) *ph -= static_cast<float>(static_cast<int>(*ph - 1.0f));
    }

    // Airborne: level the car out (sub_18BB90).
    if (!collided_ && since_object_hit_ > 50 && grounded_ == 0) improve_landing();

    air_ticks_ = grounded_ != 0 ? 0 : std::min(air_ticks_ + 1, 255);
    if (shift_timer_ != 0) --shift_timer_;
    update_gear_and_rpm(speed_h_);
}

// The tyre model, PBondCar::AddWheelForces (sub_18C1B0). Returns the number of wheels in contact.
int Vehicle::add_wheel_forces(const DriveCommand& cmd_in, const Axes& steered) {
    DriveCommand cmd = cmd_in;
    const VehicleParams& P = params_;
    const Axes ax = body_.axes();
    const Vec3 pos = body_.position();
    const Vec3 omega = body_.angular_velocity();
    const Vec3 fwd_flat = vnormalized({ax.forward[0], 0, ax.forward[2]});  // S1+0x2F0
    const bool body_collided = collided_;                                    // rec+0x4FD / 0x4FE
    int contact_count = 0;
    float lift = 0;

    for (int i = 0; i < 4; ++i) {
        WheelRuntime& w = wheel_[i];
        const Vec3& n = w.ground_normal;
        const bool rear = i >= 2;
        float c = w.ground_w + P.spring_rest_length;

        // Gear change squat of the front axle.
        if (gear_ > 0 && shift_timer_ != 0 && !rear && grounded_ == 4 && c > 0.02f) c -= 0.02f;

        const bool was_loaded = w.compression != 0.0f;  // IsWheelInContact (vtable +0x214)
        if (!was_loaded) lift = std::max(lift, c);      // fresh touchdown pushes the body out of the ground
        c = std::min(c, P.spring_compression_limit);
        if (c < 0) c = 0;

        bool in_contact = c > 0 || w.compression > 0;
        if (in_contact) ++contact_count;
        else in_contact = grounded_ > 0;
        w.contact = c > 0;

        const float upright = ax.up[1] * n[1];
        if (in_contact && upright > g_.enable_roll_stops_threshold) {
            // Suspension.
            float spring = 0;
            if (c > 0) {
                const float stiff = rear ? P.spring_stiffness_rear : P.spring_stiffness_front;
                const float damp = rear ? P.spring_damping_rear : P.spring_damping_front;
                spring = c * stiff + (c - w.compression) * damp;
            }

            // Wheel frame on the ground plane: d = rolling direction, lat = sideways.
            const Vec3& axle_fwd = rear ? ax.forward : steered.forward;
            const Vec3 t1 = vnormalized(cross(axle_fwd, n));
            Vec3 d = vnormalized(cross(n, t1));
            Vec3 lat = vnormalized(cross(n, d));

            Vec3 r = w.position - pos;
            const Vec3 v_wheel = cross(omega, r) + cmd.velocity;
            if (cmd.hbf[i] != 0 && rear) lat = vneg(vnormalized(v_wheel));

            float grip = cmd.grip[i], limit = cmd.limit[i], drive = cmd.drive[i], hbf = cmd.hbf[i];
            if (P.is_rally) {
                const float k = std::max(1.0f, 5.0f - speed_h_);
                grip *= k;
                limit *= k;
            }
            float f_lat = dot(v_wheel, lat) * grip * fric_mod_;

            if (w.surface == Surface::Ice) {
                if (drive < 0 || hbf != 0) {
                    drive *= 0.5f;
                    hbf *= 0.5f;
                }
                limit *= 0.6f;
            }
            bool skidding = false;
            float excess = 0;
            if (std::fabs(f_lat) > limit) {
                excess = std::fabs(f_lat) - limit;
                f_lat = f_lat < 0 ? -limit : limit;
                skidding = true;
            }
            if (cmd.wheelspin && rear) {
                f_lat *= P.handbrake_rw_fric_scale;
                d = d * P.handbrake_rw_fric_scale;
            }

            Vec3 force{0, 0, 0};
            const bool axle_loaded = was_loaded || wheel_[i ^ 1].compression != 0.0f;
            if (axle_loaded) force += d * drive;

            if (body_collided) r[1] = 0;
            else r[1] *= P.wheel_force_app_scale;

            if (skidding && !cmd.wheelspin) {
                const float s = std::min(excess / limit, 1.0f);
                const float k = rear ? P.rear_slipping_scale : P.front_slipping_scale;
                f_lat *= 1.0f - (1.0f - k) * s;
            }
            const Vec3 lateral_force = lat * f_lat;
            Vec3 support = n * spring;

            // Slope: only a fraction of the sideways part of the support force survives on inclines.
            float slope = std::fabs(dot(vnormalized({n[0], 0, n[2]}), fwd_flat));
            if (!(0.70710f < slope)) slope *= 0.2f;
            slope = std::max(slope, 0.05f);
            if (!(P.is_rally && speed_h_ < 5.0f && hbf != 0)) {
                support[0] *= P.slope_scale * slope;
                support[2] *= P.slope_scale * slope;
            }
            if (hbf != 0) support -= vnormalized(v_wheel) * hbf;
            if (hbf == 0 || speed_h_ > 1.0f) support -= lateral_force;
            support -= cmd.roll_drag;
            force += support;

            slip_out_[i] = std::min(std::fabs(f_lat) / limit, 1.0f);

            // Tyres slide sooner on loose surfaces once the car is moving.
            if (speed_h_ > 10.0f) {
                float loss = g_.lateral_loss[static_cast<int>(w.surface)];
                if (loss != 1.0f) {
                    if (!rear) loss += (1.0f - loss) * 0.5f;
                    if (loss * limit < std::fabs(f_lat)) {
                        Vec3 local = ax.to_local(force);
                        local[0] *= (loss * limit) / std::fabs(f_lat);
                        force = ax.to_world(local);
                    }
                }
            }
            body_.add_accel(force);
            body_.add_torque_accel(cross(r, force));
        } else if (c == 0.0f) {
            slip_out_[i] = 0;
            if (grounded_ > 0 && upright > g_.enable_roll_stops_threshold && !body_collided) {
                // A wheel hovering just off the ground while the car rises on it: pull it down.
                const Vec3 r = w.position - pos;
                if (cross(omega, r)[1] > 0) body_.add_torque_accel(cross(r, {0, -50.0f, 0}));
            }
        }
        w.compression = c;
    }

    body_.set_position(body_.position() + Vec3{0, lift, 0});

    if (P.yaw_stability_factor != 0) {
        const float yaw_local = ax.to_local(omega)[1];
        float torque = -yaw_local * P.yaw_stability_factor;
        if (counter_steering_) torque *= 8.0f;
        body_.add_torque_accel(ax.to_world({0, torque, 0}));
    }
    return contact_count;
}

// sub_18BB90: blends the orientation towards level with a small nose-down pitch.
void Vehicle::improve_landing() {
    const Axes cur = body_.axes();
    if (cur.up[1] < 0.4f) return;
    const Vec3 fwd = vnormalized(cross(cur.right, {0, 1, 0}));
    const Vec3 right = cross({0, 1, 0}, fwd);
    const float nose = turns_to_rad(params_.nose_jump_angle);
    const float cs = std::cos(nose), sn = std::sin(nose);
    const Vec3 world_up{0, 1, 0};
    const Axes t{right, world_up * cs + fwd * sn, fwd * cs - world_up * sn};

    const float n = static_cast<float>(params_.num_blend_steps);
    const float inv = 1.0f / n;
    auto blend = [&](const Vec3& a, const Vec3& b) { return (a * (n - 1.0f) + b) * inv; };
    Axes blended{blend(cur.right, t.right), blend(cur.up, t.up), blend(cur.forward, t.forward)};
    body_.set_orientation(blended);
    if (air_ticks_ > 10) {
        // The original re-applies SetAngularMomentum with the world vector scaled by 0.8 (sub_1F8588 reads
        // its argument as a body-space vector), so the momentum is also rotated by the body matrix.
        body_.set_angular_momentum(body_.axes().to_world(body_.angular_momentum() * 0.8f));
    }
}

// sub_18AA88: gear from horizontal speed, engine revs.
void Vehicle::update_gear_and_rpm(float speed) {
    const VehicleParams& P = params_;
    if (grounded_ != 0) {
        gear_ = 1;
        float limit = P.gear_limit[1];
        if (speed >= limit) {
            for (;;) {
                ++gear_;
                if (gear_ >= 5) break;  // the limit of gear 4 stays the divisor in gear 5
                limit = P.gear_limit[gear_];
                if (speed < limit) break;
            }
        }
        if (gear_ < prev_gear_ && shift_timer_ != 0) {
            gear_ = prev_gear_;
            limit = P.gear_limit[gear_];
        }
        rpm_ = speed * 6000.0f / limit;
    } else {
        rpm_ = gas_ * 7000.0f;
    }
    rpm_ += g_.wheel_spin_extra_rpm * skid_wheelspin_;
}

// RigidBody::CollideWithWorld (sub_1FC2F0) for a car: eleven probe segments from the body centre to the
// eight box corners (widened by 0.18 on X), the nose and the two sides, each advanced by one tick of velocity.
void Vehicle::collide_world(const CollisionWorld& world) {
    collided_ = false;
    const Axes ax = body_.axes();
    const Vec3 pos = body_.position();
    const Vec3 vel = body_.velocity();
    const Vec3& h = body_.half_extents();
    const Vec3 hw{h[0] + 0.18f, h[1], h[2]};
    const float speed = length(vel);

    for (int i = 0; i < 11; ++i) {
        Vec3 off;
        if (i < 8) off = ax.to_world(box_corner(i, hw));
        else if (i == 8) off = ax.forward * (h[2] + 0.05f);
        else if (i == 9) off = ax.right * h[0];
        else off = ax.right * (-h[0]);
        const Vec3 end = pos + off + vel * kTickDt;

        SegmentHit hit;
        if (!world.segment_hit(pos, end, hit)) continue;
        if (hit.surface == Surface::NoCollide) continue;

        float depth = length(end - hit.point);
        float scale = 1.0f;
        if (i >= 9) {
            depth *= 0.25f;
            scale = 0.25f;
        }
        const Vec3 n = vnormalized(hit.normal);

        // The contact height is pulled towards the centre of gravity (CAR_COLLIDE_* bias of sub_1FC2F0).
        float bias;
        if (speed >= 5.0f) {
            const float k = std::max(0.5f, (speed - 5.0f) / 10.0f);
            bias = std::fabs(dot(n, ax.forward)) <= 0.8191f ? 0.0f : k * -0.11f;
        } else {
            bias = -0.11f;
        }
        Vec3 point = hit.point;
        point[1] = (point[1] - dot(end - pos, ax.up)) + bias;
        world_contact(point, n, depth, scale);
    }
}

// RigidBody::ResolveWorldOBBCollision (sub_1FB318): one impulse at `point` along `normal`.
void Vehicle::world_contact(const Vec3& point, const Vec3& n, float depth, float scale) {
    collided_ = true;
    const Vec3 pos = body_.position();
    const Axes ax = body_.axes();
    Vec3 r = point - pos;
    Vec3 vp = cross(body_.angular_velocity(), r) + body_.velocity();
    const float vn = dot(vp, n) * scale;
    if (vn > 0.0f) return;  // already separating

    const Vec3 t = body_.inv_inertia_world(cross(r, n));
    const float angular = dot(cross(t, r), n);
    const float f1 = std::max(r[0] * r[0] + r[2] * r[2] - std::fabs(r[0] * n[0] + r[2] * n[2]), 0.0f);
    const float lever = f1 > 0 ? std::sqrt(f1) / body_.half_extents()[0] : 0.0f;
    const float denom = angular + lever;
    float j = 0;
    if (denom != 0.0f) j = -(vn - depth * g_.pf_scale) / denom;
    j = std::max(-g_.building_force_limit, std::min(g_.building_force_limit, j));

    Vec3 friction = vp * g_.building_friction_factor;
    for (float& c : friction) c = clamp_limit(c, g_.building_friction_limit);
    Vec3 impulse = (n * j - friction) * body_.mass();

    // Reversing hard against a wall beside the car: slide instead of spinning (sub_1FB318 v41).
    bool assist = false;
    if (reverse_ && std::fabs(steer_) > 0.75f && dot(r, ax.forward) < 0.0f && std::fabs(dot(n, ax.forward)) < 0.707f) {
        assist = true;
        r[1] = 0;
    }
    Vec3 torque = cross(r, impulse);
    Vec3 local = ax.to_local(torque);
    local[2] *= 0.2f;
    if (assist) local[1] *= 0.425f;
    local[1] *= 0.2f;
    torque = ax.to_world(local);

    body_.add_momentum(impulse);
    body_.add_angular_momentum(torque);
}

}  // namespace nf::driving

#include "driving/vehicle_params.hpp"

#include <string>

#include "assets/reader.hpp"

namespace nf::driving {

PhysicsGlobals PhysicsGlobals::defaults() {
    PhysicsGlobals g;
    // Tables at 0x3319D8 / 0x331A18 (16 float slots each); every slot not listed is 1.0.
    g.friction.fill(1.0f);
    g.lateral_loss.fill(1.0f);
    g.friction[static_cast<int>(Surface::Grass)] = 0.8f;
    g.friction[static_cast<int>(Surface::Dirt)] = 0.9f;
    g.lateral_loss[static_cast<int>(Surface::Grass)] = 0.75f;
    g.lateral_loss[static_cast<int>(Surface::Dirt)] = 0.9f;
    g.lateral_loss[static_cast<int>(Surface::Ice)] = 0.6f;
    return g;
}

PhysicsGlobals PhysicsGlobals::load(const Attributes& rigid, const Attributes& physical) {
    PhysicsGlobals g = defaults();
    g.gravity = rigid.get_float("GRAVITY", g.gravity);
    g.m_limit = rigid.get_float("M_LIMIT", g.m_limit);
    g.am_limit = rigid.get_float("AM_LIMIT", g.am_limit);
    g.natural_angular_damping = rigid.get_float("NATURAL_ANGULAR_DAMPING", g.natural_angular_damping);
    g.pf_scale = rigid.get_float("PF_SCALE", g.pf_scale);
    g.building_force_limit = rigid.get_float("BUILDING_FORCE_LIMIT", g.building_force_limit);
    g.building_friction_factor = rigid.get_float("BUILDING_FRICTION_FACTOR", g.building_friction_factor);
    g.building_friction_limit = rigid.get_float("BUILDING_FRICTION_LIMIT", g.building_friction_limit);

    g.pad_dead_zone = physical.get_float("PAD_DEAD_ZONE", g.pad_dead_zone);
    g.rolling_resistance = physical.get_float("ROLLING_RESISTANCE", g.rolling_resistance);
    g.enable_roll_stops_threshold = physical.get_float("ENABLE_ROLL_STOPS_THRESHOLD", g.enable_roll_stops_threshold);
    g.min_button_value = physical.get_float("MIN_BUTTON_VALUE", g.min_button_value);
    g.wheel_spin_extra_rpm = physical.get_float("WHEEL_SPIN_EXTRA_RPM", g.wheel_spin_extra_rpm);
    g.max_wheel_spin_rate = physical.get_float("MAX_WHEEL_SPIN_RATE", g.max_wheel_spin_rate);
    return g;
}

VehicleParams VehicleParams::load(const Attributes& a) {
    VehicleParams p;
    p.is_4x4 = a.get_int("IS_4X4") != 0;
    p.is_rally = a.get_int("IS_RALLY") != 0;
    p.speed_cutoff_reverse = a.get_float("SPEED_CUTOFF_REVERSE");
    p.speed_cutoff_rate_reverse = a.get_float("SPEED_CUTOFF_RATE_REVERSE");
    p.wheel_force_app_scale = a.get_float("WHEEL_FORCE_APP_SCALE");
    p.max_acc = a.get_float("MAXACC");
    p.max_rev_acc = a.get_float("MAXREVACC");
    p.max_brake = a.get_float("MAXBRAKE");
    p.max_steering = a.get_float("MAXSTEERING");
    p.mod_steer_speed = a.get_float("MODSTEERSPEED");
    p.min_steer = a.get_float("MINSTEER");
    p.fric_mod_speed = a.get_float("FRIC_MOD_SPEED");
    p.fric_mod_range = a.get_float("FRIC_MOD_RANGE");
    p.fric_mod_min = a.get_float("FRIC_MOD_MIN");
    p.yaw_stability_factor = a.get_float("YAW_STABILITY_FACTOR");
    p.nose_jump_angle = a.get_float("NOSE_JUMP_ANGLE");
    p.friction_limit_front = a.get_float("FRICTIONLIMITFRONT");
    p.friction_limit_rear = a.get_float("FRICTIONLIMITREAR");
    p.tyre_grip_factor = a.get_float("TYREGRIPFACTOR");
    p.slope_scale = a.get_float("SLOPE_SCALE");
    p.spring_stiffness_front = a.get_float("SPRING_STIFFNESS_FRONT");
    p.spring_stiffness_rear = a.get_float("SPRING_STIFFNESS_REAR");
    p.spring_damping_front = a.get_float("SPRING_DAMPING_FRONT");
    p.spring_damping_rear = a.get_float("SPRING_DAMPING_REAR");
    p.spring_rest_length = a.get_float("SPRING_REST_LENGTH");
    p.spring_compression_limit = a.get_float("SPRING_COMPRESSION_LIMIT");
    p.spring_compression_limit_draw = a.get_float("SPRING_COMPRESSION_LIMIT_DRAW");
    p.wheel_radius = a.get_float("WHEEL_RADIUS");
    p.braking_friction = a.get_float("BRAKING_FRICTION");
    p.accel_frw_ratio = a.get_float("ACCEL_FRW_RATIO");
    p.coast_frw_ratio = a.get_float("COAST_FRW_RATIO");
    p.brake_frw_ratio = a.get_float("BRAKE_FRW_RATIO");
    p.reverse_frw_ratio = a.get_float("REVERSE_FRW_RATIO");
    p.handbrake_frw_ratio = a.get_float("HANDBRAKE_FRW_RATIO");
    p.rev_handbrake_frw_ratio = a.get_float("REV_HANDBRAKE_FRW_RATIO");
    p.front_slipping_scale = a.get_float("FRONT_SLIPPING_SCALE");
    p.rear_slipping_scale = a.get_float("REAR_SLIPPING_SCALE");
    p.handbrake_force = a.get_float("HANDBRAKE_FORCE");
    p.total_handbrake_friction_scale = a.get_float("TOTAL_HANDBRAKE_FRICTION_SCALE");
    p.handbrake_rw_fric_scale = a.get_float("HANDBRAKE_RW_FRIC_SCALE");
    p.ws_speed = a.get_float("WS_SPEED");
    p.ws_accel = a.get_float("WS_ACCEL");
    p.counter_steer_scale = a.get_float("COUNTER_STEER_SCALE");
    p.counter_steer_scale_limit = a.get_float("COUNTER_STEER_SCALE_LIMIT");
    p.num_blend_steps = a.get_int("NUM_BLEND_STEPS");

    static const char* const kGear[6] = {"R", "1", "2", "3", "4", "5"};
    for (int i = 0; i < 6; ++i) {
        p.gear_limit[i] = a.get_float(std::string("GEAR_LIMIT") + kGear[i]);
        p.gear_ratio[i] = a.get_float(std::string("GEAR_RATIO") + kGear[i]);
    }

    p.mass = a.get_float("MASS");
    p.wheel_x_offset = a.get_float("CAR_WHEEL_X_OFFSET");
    p.wheel_zf_offset = a.get_float("CAR_WHEEL_ZF_OFFSET");
    p.wheel_zr_offset = a.get_float("CAR_WHEEL_ZR_OFFSET");
    p.tyre_radius = a.get_float("TYRE_RADIUS");
    p.boost_extra_acc = a.get_float("BOOST_EXTRA_ACC");
    p.boost_time = a.get_int("BOOST_TIME");

    // The driving code divides by these; the shipped data always sets them.
    if (!(p.mass > 0)) throw FormatError("vehicle attributes: MASS missing or not positive");
    if (!(p.wheel_radius > 0)) throw FormatError("vehicle attributes: WHEEL_RADIUS missing or not positive");
    if (!(p.fric_mod_range > 0)) throw FormatError("vehicle attributes: FRIC_MOD_RANGE missing or not positive");
    if (p.num_blend_steps < 1) throw FormatError("vehicle attributes: NUM_BLEND_STEPS missing or below 1");
    for (float lim : p.gear_limit)
        if (!(lim > 0)) throw FormatError("vehicle attributes: GEAR_LIMIT* missing or not positive");
    return p;
}

}  // namespace nf::driving

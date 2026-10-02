#pragma once

#include <array>

#include "driving/attributes.hpp"
#include "driving/world_query.hpp"

namespace nf::driving {

// Fixed simulation step. The physics runs from the "Schedule_SimRate" task group, once per vertical
// blank (60 Hz NTSC): the scheduler sub_179D40 runs (frames elapsed * rate scale 1.0) steps per call, the
// vblank handler sub_1799A0 increments the frame counter dword_32F2A8, and the per-step game clock
// advances by 0.016666668 s (sub_159D10). ESetSimRate (sub_158D28) only rescales the step count.
// Speeds and accelerations in all data files are metres per second (per second squared).
constexpr float kTickHz = 60.0f;
constexpr float kTickDt = 1.0f / kTickHz;

// The global tuning of the three "Physics:*" tuning sections (`tuning/physics/{rigid,physical,friction}
// /default.tun`, registered by sub_1F6600 and sub_185668). Only values the ported code paths read are
// kept; defaults are the initialised values in DRIVING.ELF .data.
struct PhysicsGlobals {
    // Physics:Rigid (sub_1F6600; addresses are the float globals)
    float gravity = -34.0f;                    // GRAVITY 0x3390D0, world units/s^2 along Y (sub_1F7C60)
    float m_limit = 100.0f;                    // M_LIMIT 0x339108: |v| limit per axis (VU0 integrator)
    float am_limit = 10.0f;                    // AM_LIMIT 0x339104: |omega| limit per axis (rad/s)
    float natural_angular_damping = 0.995f;    // NATURAL_ANGULAR_DAMPING 0x3390FC (sub_1F8088, non-car bodies)
    float pf_scale = 10.0f;                    // PF_SCALE 0x339158: penetration recovery speed per unit depth
    float building_force_limit = 10.0f;        // BUILDING_FORCE_LIMIT 0x339148: max impulse per contact
    float building_friction_factor = 0.0045f;  // BUILDING_FRICTION_FACTOR 0x33913C
    float building_friction_limit = 0.125f;    // BUILDING_FRICTION_LIMIT 0x339140

    // Physics:Physical (sub_185668)
    float pad_dead_zone = 0.2f;                // PAD_DEAD_ZONE 0x331988 (sub_18ADD8)
    float rolling_resistance = 0.1f;           // ROLLING_RESISTANCE 0x3319A4 (sub_18CEF8)
    float enable_roll_stops_threshold = 0.2f;  // ENABLE_ROLL_STOPS_THRESHOLD 0x331970 (sub_18C1B0)
    float min_button_value = 0.1f;             // MIN_BUTTON_VALUE 0x3319A8 (sub_18CEF8)
    float wheel_spin_extra_rpm = 4000.0f;      // WHEEL_SPIN_EXTRA_RPM 0x3319AC (sub_18AA88)
    float max_wheel_spin_rate = 0.05f;         // MAX_WHEEL_SPIN_RATE 0x3319C0, revolutions per tick

    // Physics:Friction: the tables the code reads are `friction[kXXX]` (0x3319D8) and
    // `lateralLoss[kXXX]` (0x331A18), indexed by the wheel's surface byte. The shipped .tun files name their
    // entries `frictionCoefficients[WSurface_kXXX]`/`lateralLossCoefficients[...]`, which match no
    // registered variable, so the tuning loader never applies them: the ELF defaults are what runs.
    std::array<float, kSurfaceCount> friction;
    std::array<float, kSurfaceCount> lateral_loss;

    // ELF defaults.
    static PhysicsGlobals defaults();

    // Applies `rigid/default.tun` and `physical/default.tun` (flat attribute view of each file) on top of
    // the defaults. Keys the original registers are the only ones honoured.
    static PhysicsGlobals load(const Attributes& rigid_tun, const Attributes& physical_tun);
};

// The `CarPhysics` attribute record of a PVehicle (sub_1968F8 registers every key with its struct
// offset; the record is zero-initialised, so a key missing from both `default.atr` and the car's own
// section reads as 0). Keys are listed with the record offset they land at.
struct VehicleParams {
    // sub_1968F8 "CarPhysics" record.
    bool is_4x4 = false;                 // IS_4X4 +0x00 (read by AI physics only)
    bool is_rally = false;               // IS_RALLY +0x04: front wheels also driven, low-speed grip boost
    float speed_cutoff_reverse = 0;      // SPEED_CUTOFF_REVERSE +0x08
    float speed_cutoff_rate_reverse = 0; // SPEED_CUTOFF_RATE_REVERSE +0x0C
    float wheel_force_app_scale = 0;     // WHEEL_FORCE_APP_SCALE +0x10
    float max_acc = 0;                   // MAXACC +0x14
    float max_rev_acc = 0;               // MAXREVACC +0x18
    float max_brake = 0;                 // MAXBRAKE +0x1C
    float max_steering = 0;              // MAXSTEERING +0x20 (turns: 1.0 = 360 degrees of front wheel angle at full lock)
    float mod_steer_speed = 0;           // MODSTEERSPEED +0x24
    float min_steer = 0;                 // MINSTEER +0x28
    float fric_mod_speed = 0;            // FRIC_MOD_SPEED +0x2C
    float fric_mod_range = 0;            // FRIC_MOD_RANGE +0x30
    float fric_mod_min = 0;              // FRIC_MOD_MIN +0x34
    float yaw_stability_factor = 0;      // YAW_STABILITY_FACTOR +0x38
    float nose_jump_angle = 0;           // NOSE_JUMP_ANGLE +0x3C (turns)
    float friction_limit_front = 0;      // FRICTIONLIMITFRONT +0x40
    float friction_limit_rear = 0;       // FRICTIONLIMITREAR +0x44
    float tyre_grip_factor = 0;          // TYREGRIPFACTOR +0x48
    float slope_scale = 0;               // SLOPE_SCALE +0x4C
    float spring_stiffness_front = 0;    // SPRING_STIFFNESS_FRONT +0x50
    float spring_stiffness_rear = 0;     // SPRING_STIFFNESS_REAR +0x54
    float spring_damping_front = 0;      // SPRING_DAMPING_FRONT +0x58
    float spring_damping_rear = 0;       // SPRING_DAMPING_REAR +0x5C
    float spring_rest_length = 0;        // SPRING_REST_LENGTH +0x60
    float spring_compression_limit = 0;  // SPRING_COMPRESSION_LIMIT +0x64
    float spring_compression_limit_draw = 0;  // SPRING_COMPRESSION_LIMIT_DRAW +0x68 (renderer)
    float wheel_radius = 0;              // WHEEL_RADIUS +0x6C
    float braking_friction = 0;          // BRAKING_FRICTION +0x70
    float accel_frw_ratio = 0;           // ACCEL_FRW_RATIO +0x74: front share of the tyre grip, per drive state
    float coast_frw_ratio = 0;           // COAST_FRW_RATIO +0x78
    float brake_frw_ratio = 0;           // BRAKE_FRW_RATIO +0x7C
    float reverse_frw_ratio = 0;         // REVERSE_FRW_RATIO +0x80
    float handbrake_frw_ratio = 0;       // HANDBRAKE_FRW_RATIO +0x84
    float rev_handbrake_frw_ratio = 0;   // REV_HANDBRAKE_FRW_RATIO +0x88
    float front_slipping_scale = 0;      // FRONT_SLIPPING_SCALE +0x8C
    float rear_slipping_scale = 0;       // REAR_SLIPPING_SCALE +0x90
    float handbrake_force = 0;           // HANDBRAKE_FORCE +0x98
    float total_handbrake_friction_scale = 0;  // TOTAL_HANDBRAKE_FRICTION_SCALE +0x9C
    float handbrake_rw_fric_scale = 0;   // HANDBRAKE_RW_FRIC_SCALE +0xA0
    float ws_speed = 0;                  // WS_SPEED +0xA8: wheel-spin (burnout) speed threshold
    float ws_accel = 0;                  // WS_ACCEL +0xAC
    float counter_steer_scale = 0;       // COUNTER_STEER_SCALE +0xB0
    float counter_steer_scale_limit = 0; // COUNTER_STEER_SCALE_LIMIT +0xB4
    int num_blend_steps = 0;             // NUM_BLEND_STEPS +0xB8 (in-air levelling)
    std::array<float, 6> gear_limit{};   // GEAR_LIMITR, GEAR_LIMIT1..5 +0xD0..: speed at which the gear ends
    std::array<float, 6> gear_ratio{};   // GEAR_RATIOR, GEAR_RATIO1..5 +0xE8..

    // Records read by other loaders.
    float mass = 0;                      // MASS (sub_169D80 on the PVehicle attribute set)
    float wheel_x_offset = 0;            // CAR_WHEEL_X_OFFSET (sub_1F72D8): wheel inset from the box side
    float wheel_zf_offset = 0;           // CAR_WHEEL_ZF_OFFSET: front axle offset from the box front (negative = inside)
    float wheel_zr_offset = 0;           // CAR_WHEEL_ZR_OFFSET: rear axle offset from the box rear
    float tyre_radius = 0;               // TYRE_RADIUS (renderer)
    float boost_extra_acc = 0;           // BOOST_EXTRA_ACC (sub_18CEF8)
    int boost_time = 0;                  // BOOST_TIME (sub_18B768, sub_18CEF8), in ticks

    // `car` must already be the effective attribute view: `default.atr` overlaid with the car's section.
    static VehicleParams load(const Attributes& car);
};

}  // namespace nf::driving

#pragma once

#include <array>

#include "core/math.hpp"
#include "driving/camera_target.hpp"
#include "driving/pad_controls.hpp"
#include "driving/rigid_body.hpp"
#include "driving/vehicle_params.hpp"
#include "driving/world_query.hpp"

namespace nf::driving {

// Everything a renderer needs about one wheel (all wheels index 0/1 = front left/right, 2/3 = rear).
struct WheelPose {
    Vec3 position{};        // world position of the suspension attach point (bottom corner of the body box)
    float compression = 0;  // spring compression in metres after this tick (0 = fully extended)
    float steer_angle = 0;  // front wheel yaw relative to the body, radians (positive turns towards +X)
    float spin_phase = 0;   // wheel rotation in revolutions, wrapped to [0,1)
    float slip = 0;         // tyre saturation 0..1 (skid intensity, original a1+544..)
    bool in_contact = false;
    Surface surface = Surface::Paved;
};

// A player/AI car simulated the way PBondCar::ProcessPhysics does (sub_18CEF8 with the tyre model
// sub_18C1B0), inside the rigid-body step of the "Simulation" (sub_1FF3D8): controller input -> wheel
// ray casts, spring/tyre/drive forces -> world collision probes (sub_1FC2F0) -> integration.
//
// Frame: body +X = right, +Y = up, +Z = forward; the body origin is the centre of the bounding box; world
// units are metres, m/s, Y up.
class Vehicle {
public:
    // `half_extents` are the half sizes of the car model's bounding box (the original derives them from the
    // render model with sub_1C9778); the wheels are attached to the box's bottom corners inset by the
    // CAR_WHEEL_* offsets and the inertia is derived from box and mass exactly like sub_210938.
    Vehicle(const VehicleParams& params, const PhysicsGlobals& globals, const Vec3& half_extents);

    // Puts the car at rest with its body centre at `position` facing yaw (forward = (sin yaw, 0, cos yaw)).
    // Also the original's InitializeCarVariables (sub_187350): gear 1, timers cleared, no ground cached.
    void reset(const Vec3& position, float yaw);

    // Advances one fixed 60 Hz tick (kTickDt) with the controller state of this tick.
    void step(const DriveInput& input, const CollisionWorld& world);

    // PBondCar::EnableRocketBoost (sub_18B768). False while a boost is still in its first phase.
    bool enable_rocket_boost();

    // --- state for camera / renderer / audio ---
    const RigidBody& body() const { return body_; }
    CameraTarget camera_target() const;
    Mat4 model_matrix() const;                      // body-origin to world, column-major
    const std::array<WheelPose, 4>& wheels() const { return wheel_poses_; }
    float front_steer_angle() const { return steer_angle_; }
    float speed() const { return length(body_.velocity()); }
    float forward_speed() const { return dot(body_.velocity(), body_.axes().forward); }
    int gear() const { return gear_; }              // 0 = reverse gear slot, 1..5
    float rpm() const { return rpm_; }              // engine revs (sub_18AA88)
    int wheels_in_contact() const { return grounded_; }
    bool reversing() const { return reverse_; }
    bool handbrake() const { return handbrake_; }
    float throttle() const { return gas_; }         // effective gas/brake after the input rules
    float brake() const { return brake_; }
    float steer() const { return steer_; }          // slewed steering, -1..1
    int boost_ticks_left() const { return boost_timer_; }
    const VehicleParams& params() const { return params_; }

private:
    struct WheelRuntime {
        bool cache_valid = false;
        GroundHit cache;
        float compression = 0;  // previous tick (a1+0x210+4i)
        Vec3 ground_normal{0, 1, 0};
        float ground_w = 0;
        Vec3 position{};
        Surface surface = Surface::Paved;
        bool contact = false;
    };

    struct DriveCommand;  // per-tick tyre parameters, see vehicle.cpp

    void update_controls(const DriveInput& in);
    void car_forces(const CollisionWorld& world);
    void query_wheel(int i, const CollisionWorld& world);
    int add_wheel_forces(const DriveCommand& cmd, const Axes& steered);
    void improve_landing();
    void update_gear_and_rpm(float speed_h_at_start);
    void collide_world(const CollisionWorld& world);
    void world_contact(const Vec3& point, const Vec3& normal, float depth, float normal_scale);

    VehicleParams params_;
    PhysicsGlobals g_;
    RigidBody body_;
    std::array<Vec3, 4> wheel_local_{};
    std::array<WheelRuntime, 4> wheel_{};
    std::array<WheelPose, 4> wheel_poses_{};

    // Input state (sub_18ADD8).
    float steer_raw_ = 0, steer_ = 0;
    float gas_raw_ = 0, gas_event_ = 0, brake_raw_ = 0, gas_ = 0, brake_ = 0;
    bool handbrake_ = false, handbrake_reverse_ = false, handbrake_input_ = false;
    bool burnout_ = false;

    // Drive state (sub_18CEF8).
    float speed_h_ = 0;         // a1+648: horizontal speed
    float steer_angle_ = 0;     // a1+652
    float fric_mod_ = 1;        // a1+744
    float wheel_spin_ = 0;      // a1+628, revolutions per tick
    float phase_front_ = 0, phase_rear_ = 0;  // a1+560/564
    int grounded_ = 0;          // a1+665
    int drive_state_ = 0;       // a1+689: 0 coast, 1 accelerate, 2 brake, 3 reverse, 4 handbrake, 5 reverse handbrake
    bool reverse_ = false;      // a1+668
    bool braking_forward_ = false;  // a1+664
    int reverse_timer_ = 0;     // a1+666
    bool counter_steering_ = false;  // a1+730
    int air_ticks_ = 0;         // a1+731
    int gear_ = 1, prev_gear_ = 255, shift_timer_ = 0;  // a1+728, 729, 727
    float rpm_ = 0;
    float skid_wheelspin_ = 0;  // a1+568
    int boost_timer_ = 0;       // a1+660
    bool collided_ = false;     // rec+1277
    int since_object_hit_ = 0;  // rec+1246, only object contacts (not ported) reset it
    std::array<float, 4> slip_out_{};
};

}  // namespace nf::driving

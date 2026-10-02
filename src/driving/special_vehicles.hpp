#pragma once

#include "core/math.hpp"
#include "driving/camera_target.hpp"
#include "driving/pad_controls.hpp"  // DriveInput (snowmobile shares the car input)
#include "driving/track_collision.hpp"
#include "driving/vehicle_params.hpp"

namespace nf::driving {

// Non-car player vehicles of the driving missions, in the spirit of PBondCar's sibling physics
// paths (ProcessSubmarinePhysics/RollSub, the IS_FLYING ultralight flight with NO_WORLD_COLLISIONS,
// ProcessSnowmobilePhysics/AddSnowmobileForces sleds). Wheeled cars, trucks, jeeps, tanks and
// boats run the shared Vehicle model from their .atr data; submarines (IS_SUB/SUB_PHYSICS/
// NUM_WHEELS=0), snowmobiles (IS_SNOWMOBILE) and flying ultralights (IS_FLYING) have their own
// arcade dynamics here, with original-derived numbers where the data gives them (MAXACC/BOOST_*,
// GEAR_LIMIT top speed). Tuned constants are marked [INFERENCE].

struct FlightInput {
    float steer = 0;    // LX: roll / turn, -1..1
    float pitch = 0;    // LY (STERVERTICAL): climb/dive or dive planes, -1..1
    float gas = 0;      // cross: throttle 0..1
    float brake = 0;    // square: brake/reverse 0..1
};

// Player submarine (ProcessSubmarinePhysics/RollSub equivalent).
class Submarine {
public:
    Submarine(const VehicleParams& params, float top_speed);

    void reset(const Vec3& pos, float yaw);
    // One 60 Hz tick. Depth is positive-down keel clearance: the boat floats toward periscope
    // depth on its own (buoyancy) and answers dive planes + throttle; the seafloor/ceiling from
    // the collision mesh clamp it (NO beaching through the floor).
    void step(const FlightInput& in, const TrackCollision& collision);

    Vec3 position() const { return pos_; }
    float yaw() const { return yaw_; }
    float pitch() const { return pitch_; }
    float speed() const { return speed_; }
    float rpm() const;
    Vec3 forward() const;
    CameraTarget camera_target() const;
    Mat4 model_matrix() const;
    void set_boost(float seconds) { boost_ = seconds; }  // gadget rocket boost (Submarine)
    void nudge(const Vec3& dp) { pos_ += dp; }          // car-car separation

private:
    float boost_ = 0;
    float top_speed_ = 20.0f, accel_ = 2.75f;
    Vec3 pos_{};
    float yaw_ = 0, pitch_ = 0, speed_ = 0, throttle_ = 0;
};

// Player ultralight (IS_FLYING/NO_WORLD_COLLISIONS equivalent): throttle + pitch + banking turns,
// stall sink below fly speed, terrain clamp (never through the ground), soft ceiling.
class Ultralight {
public:
    Ultralight(const VehicleParams& params, float top_speed);

    void reset(const Vec3& pos, float yaw);
    void step(const FlightInput& in, const TrackCollision& collision);

    Vec3 position() const { return pos_; }
    float yaw() const { return yaw_; }
    float pitch() const { return pitch_; }
    float roll() const { return roll_; }
    float speed() const { return speed_; }
    float rpm() const;
    Vec3 forward() const;
    CameraTarget camera_target() const;
    Mat4 model_matrix() const;
    void set_boost(float seconds) { boost_ = seconds; }  // gadget rocket boost (Ultralight)
    void nudge(const Vec3& dp) { pos_ += dp; }          // car-car separation

private:
    float boost_ = 0;
    float top_speed_ = 40.0f, accel_ = 3.0f;
    Vec3 pos_{};
    float yaw_ = 0, pitch_ = 0, roll_ = 0, speed_ = 0, throttle_ = 0;
};

// Player/AI snowmobile (IS_SNOWMOBILE, PBondCar_ProcessSnowmobilePhysics/AddSnowmobileForces
// equivalent): ski steering + track drive as an arcade ground-follower. No wheels (NUM_WHEELS=0
// in the data: supersnow, small_snowmobile); wall contact stops it via a segment probe so it
// cannot tunnel through mountains. Tuned constants [INFERENCE].
class Snowmobile {
public:
    Snowmobile(const VehicleParams& params, float top_speed);

    void reset(const Vec3& pos, float yaw);
    void step(const DriveInput& in, const TrackCollision& collision);

    Vec3 position() const { return pos_; }
    float yaw() const { return yaw_; }
    float speed() const { return speed_; }
    float rpm() const;
    Vec3 forward() const;
    CameraTarget camera_target() const;
    Mat4 model_matrix() const;
    void set_boost(float seconds) { boost_ = seconds; }  // gadget rocket boost (Snowmobile)
    void nudge(const Vec3& dp) { pos_ += dp; }          // car-car separation

private:
    float boost_ = 0;
    float top_speed_ = 40.0f, accel_ = 3.0f;
    Vec3 pos_{};
    float yaw_ = 0, speed_ = 0, throttle_ = 0;
};

}  // namespace nf::driving

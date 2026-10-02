#pragma once

#include <array>
#include <memory>
#include <string>

#include "driving/camera_ini.hpp"
#include "driving/chase_camera.hpp"
#include "driving/driving_level.hpp"
#include "driving/special_vehicles.hpp"
#include "driving/track_collision.hpp"
#include "driving/track_model.hpp"
#include "driving/vehicle.hpp"
#include "game/input.hpp"

namespace nf::driving {

// One running driving mission: track collision, the player's car and its chase camera, advanced at the
// simulation rate (kTickHz = 60, sub_1FEDA0). Rendering data (car meshes) is exposed for the app.
// Player kinds (PBondCar physics paths): Car covers wheeled vehicles (cars, trucks, tanks,
// boats: all run the shared Vehicle model from their .atr), Sub the IS_SUB/SUB_PHYSICS
// submarines (ProcessSubmarinePhysics/RollSub), Fly the IS_FLYING ultralights
// (NO_WORLD_COLLISIONS flight) and Sled the IS_SNOWMOBILE snowmobiles
// (ProcessSnowmobilePhysics/AddSnowmobileForces).
enum class PlayerKind { Car, Sub, Fly, Sled };

class DriveSession {
public:
    DriveSession(DrivingLevel& level, const std::string& car);
    ~DriveSession();

    PlayerKind kind() const { return kind_; }
    const std::string& car() const { return car_; }

    // Puts the vehicle at `position` (x, z used; height from the track/depth datum) facing `yaw`.
    void place_at_start(const Vec3& position, float yaw);
    // Recovery drop (cars only): places 2 m above the walk node WITHOUT grounding, so the
    // car falls in and settles on its wheels. A grounded placement can balance on marginal
    // support and hover; falling in punches through. Starts use place_at_start instead.
    void place_dropped(const Vec3& position, float yaw);

    // One simulation tick with this controller sample.
    void tick(const PadState& pad);

    // Player dynamics (kind-dispatched; Mission uses these for weapons/HUD/audio).
    Vec3 player_position() const;
    Vec3 player_forward() const;
    float player_speed() const;
    float player_rpm() const;
    CameraTarget player_camera_target() const;
    Mat4 player_body_matrix() const;
    bool has_wheels() const;
    const VehicleParams& player_params() const { return params_; }
    Submarine& sub();
    Ultralight& fly();
    Snowmobile& sled();
    // Battle damage (car only) + rocket boost (all kinds).
    void set_player_damage(float grip_front, float grip_rear, float drag);
    void trigger_player_boost();

    const Vehicle& vehicle() const { return *vehicle_; }  // valid for Car kind
    const CameraPose& camera_pose() const { return camera_pose_; }
    const TrackCollision& collision() const { return collision_; }
    std::size_t tick_count() const { return ticks_; }

    // Car meshes in car space (origin at the body box centre) and the shape library of the car.
    const SceneMesh& body_mesh() const { return body_; }
    const std::array<SceneMesh, 4>& wheel_meshes() const { return wheels_; }
    const SshFile& car_shapes() const { return car_shapes_; }

    // Model matrices (column-major, model -> world) for the body and each wheel (front left/right, rear
    // left/right), including suspension travel, steering and spin.
    Mat4 body_matrix() const;
    Mat4 wheel_matrix(int wheel) const;

private:
    class Rays;
    void floor_start(Vec3& pos) const;

    PlayerKind kind_ = PlayerKind::Car;
    std::string car_;
    TrackCollision collision_;
    std::unique_ptr<Rays> rays_;
    CameraIni camera_ini_;
    std::unique_ptr<ChaseCamera> camera_;
    std::unique_ptr<Vehicle> vehicle_;  // Car kind only
    std::unique_ptr<Submarine> sub_;
    std::unique_ptr<Ultralight> fly_;
    std::unique_ptr<Snowmobile> sled_;
    VehicleParams params_;
    CameraPose camera_pose_;
    PadHistory pad_;
    SceneMesh body_;
    std::array<SceneMesh, 4> wheels_;
    SshFile car_shapes_;
    Vec3 half_{};                    // half extents of the body box
    Vec3 body_center_{};             // model-space centre of the body box
    float wheel_radius_ = 0.35f;
    std::size_t ticks_ = 0;
};

}  // namespace nf::driving

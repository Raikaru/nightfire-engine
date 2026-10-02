#pragma once

#include <array>
#include <memory>
#include <string>

#include "driving/camera_ini.hpp"
#include "driving/chase_camera.hpp"
#include "driving/driving_level.hpp"
#include "driving/track_collision.hpp"
#include "driving/track_model.hpp"
#include "driving/vehicle.hpp"
#include "game/input.hpp"

namespace nf::driving {

// One running driving mission: track collision, the player's car and its chase camera, advanced at the
// simulation rate (kTickHz = 60, sub_1FEDA0). Rendering data (car meshes) is exposed for the app.
class DriveSession {
public:
    DriveSession(DrivingLevel& level, const std::string& car);
    ~DriveSession();

    // Puts the car on the ground at `position` (x, z used; the height comes from the track) facing `yaw`.
    void place_at_start(const Vec3& position, float yaw);

    // One simulation tick with this controller sample.
    void tick(const PadState& pad);

    const Vehicle& vehicle() const { return *vehicle_; }
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

    TrackCollision collision_;
    std::unique_ptr<Rays> rays_;
    CameraIni camera_ini_;
    std::unique_ptr<ChaseCamera> camera_;
    std::unique_ptr<Vehicle> vehicle_;
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

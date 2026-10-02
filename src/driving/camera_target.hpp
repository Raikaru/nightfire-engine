#pragma once

#include "core/math.hpp"

namespace nf::driving {

// What the chase camera needs to know about the car it follows. World frame: Y up (same axes as the
// track data). `right`, `up`, `forward` are the car body's unit axes in world space.
struct CameraTarget {
    Vec3 position;       // car body origin (world)
    Vec3 right, up, forward;
    Vec3 velocity;       // world units / second
    float speed = 0;     // |velocity| (world units / second)
    // Additive fields (DRIVING.ELF sub_1D5E38 / sub_1AA890, sub_1B3CE0; see docs/driving-camera.md):
    bool tumbling = false;    // vehicle tumble counter (vehicle +0x3E8, sub_188200) != 0: up.y < 0.3 with the collision-state flag set
    float steer_input = 0;    // left stick X, -1..1, positive = right (GAMEACTION_CAMROTATEX); dashboard glance only
};

// Ray query against the track collision, used to stop the camera clipping through walls.
class RayCaster {
public:
    virtual ~RayCaster() = default;
    // Nearest hit along from -> to. Returns true and the hit fraction t in [0,1] of the segment.
    virtual bool segment_hit(const Vec3& from, const Vec3& to, float& t) const = 0;
};

// Output of the camera: a look-at pose plus the field of view of the original projection.
// The original passes the horizontal view angle 2*33 deg with aspect 4:3 (sub_1D4878); fovy is the
// vertical angle that yields the same frustum at `aspect`.
struct CameraPose {
    Vec3 eye;
    Vec3 target;
    Vec3 up{0, 1, 0};
    float fovy = 1.0f;                     // vertical FOV, radians
    float fovx = 1.0f;                     // horizontal FOV, radians
    float aspect = 4.0f / 3.0f;            // width / height the FOV is defined for
    float znear = 0.17f, zfar = 3000.0f;   // RViewCamera defaults (sub_1D40F0)
};

}  // namespace nf::driving

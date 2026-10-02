#pragma once

#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "core/math.hpp"

namespace nf::driving {

// `data\render\camera.ini`, read by RCameraIniLoader (sub_1A49E0). A file is a `[Global]` section, one
// section per camera (`[VanquishHeliCam0]`) and one `[<camera>:Arm<n>]` section per arm. The camera
// *kind* comes from a case-sensitive substring of the section name ("Heli", "Bumper", "Dashboard"...).
// Missing float keys read 0, missing integer keys their code default. Key names are case-insensitive.
struct CameraIni {
    enum class Kind { Heli, Bumper, Dashboard, Fixed, Spline, Ellipse, Tumble, WorldAnim, RelativeAnim, AIPathAnim, Collision, AutoDrive };

    // Defaults are the literals passed by RCameraIniLoader (sub_1A49E0 0x1a678c-0x1a6bec) when a key is missing.
    struct Global {
        int max_tumble = 100;                 // kMaxTumble: frames the tumble camera holds after the last tumble tick
        int max_collision = 50;               // kMaxCollision
        int default_transition = 60;          // kDefualtTransition (sic)
        float trans_rate = 0.03f;             // kTransRate: blend rate at the start of a smooth transition
        float trans_rate_lerp_rate = 0.02f;   // kTransRateLerpRate: per tick approach of the blend rate to its steady value
        float bumper_y_lerp_rate = 0.4f;      // kBumperYLerpRate
        float collide_radius = 1.0f;          // kCollideRadius (not read by any code in DRIVING.ELF)
        float spline_offset_lerp = 0.01f;
        float explosion_scale = 0.035f;
        float explosion_max_shake = 0.25f;
        int explosion_shake_period = 8;
        float explosion_after_shock = 0.1f;
        float explosion_time_scale = 1.0f;
        Vec3 camera_object_radius_ex{1.675f, 1.925f, 1.325f};
        float camera_object_sphere_rad = 2.875f;
        float collision_min_rate = 0.04f;
        float collision_max_rate = 0.13f;
        float collision_rate_diff = 0.008f;
        float zoom_inc_speed = 1.5f;
        float centering_speed = 0.5f;
        int auto_drive_latency = 0;
        int weapon_arm_change_latency = 60;   // kWeaponArmChangeLatency: transition frames when the weapon changes the arm
        int missile_cam_latency = 40;
        float weapon_animation_length = 60.0f;
        float weapon_animation_amplitude = 1.0f;
        float max_autoaim_distance = 300.0f;
    };

    struct Anchor {  // sub_1D69C8
        Vec3 offset{0, 0, 0};  // Anchor_X/Y/Z, in the car frame
        float slide_dist = 0, slide_rate = 0, slide_rec_rate = 0;
    };

    struct Arm {  // `[<cam>:Arm<n>]`, heli cameras
        float sideways = 0, height = 0, distance = 0;  // Heli_Sideways / Heli_Height / Heli_Distance
        int transition = 0;                            // armTransition (default: the camera's smoothTrans value)
        Anchor anchor;
        std::string weapons;                           // `weapons` (raw list; the arm is used when the secondary weapon matches)
    };

    struct Heli {
        float min_rate = 0, max_rate = 0, speed_rate_diff = 0, height_factor = 0;
        float fallback_factor = 0, max_fallback = 0;
        float vertigo_lerp = 0, tumble_arm_scale = 0;
        float max_vertigo_downhill = 0, max_vertigo_uphill = 0;  // clamped to [0,1] by the loader
        bool rigid_arm = false;
        bool check_collisions = true;
        float noise_pace = 0, noise_amount = 0, noise_frequency = 0, up_rate = 0, look_up = 0;
        bool cinematic = false;
        std::vector<Arm> arms;  // Arm0..; up to 32
    };

    struct Bumper {  // sub_1A49E0 Bumper branch: (x, y, z, panUp)
        std::array<float, 4> forward{}, backward{};
        Anchor anchor;
    };

    struct Dashboard {
        Vec3 forward_arm{0, 0, 0}, backward_arm{0, 0, 0};
        Vec3 force_scale{0, 0, 0}, force_max{0, 0, 0}, torque_scale{0, 0, 0}, torque_max{0, 0, 0};
        float force_pace = 0, torque_pace = 0, forward_pitch = 0, forward_yaw = 0;
        float inertia_scale = 0, inertia_min = 0, inertia_max = 0;  // `intertia*` (sic)
        float steer_scale = 0, steer_max = 0, steer_pace = 0;
        float glance_scale = 0, glance_max = 0, glance_pace = 0;
        float noise_amount = 0, noise_frequency = 0;
        float vertigo_lerp = 0, max_vertigo_downhill = 0, max_vertigo_uphill = 0;
        Anchor anchor;
    };

    struct TumbleCam {
        float rel_pos_lerp = 0, vector_lerp = 0;
        Anchor anchor;
    };

    struct Camera {
        std::string name;              // section name, original case
        Kind kind = Kind::Heli;
        std::string car;               // `car`: comma/space separated list; empty + !has_car = every car
        bool has_car = false;
        bool tumble = false;           // switches to the tumble camera while the car tumbles
        bool shake = false;            // explosion shake enabled
        int smooth_trans = 0;          // smoothTrans (signed byte; > 0 = blends into / out of this camera)
        bool look_back = false;        // L2 flips the arm
        bool selectable = false;       // reachable with GAMEACTION_CHANGECAMERA
        bool lerp_rotation = false;
        bool interior_view = false;
        int cam_id = 0;                // camID (used to request a camera by id, not for the cycle order)
        bool default_camera = false;   // defaultCamera
        float default_fov = 33.0f;     // defaultFov (degrees, default 33 = half the horizontal view angle; applied when > 2)
        float explosion_shake_scale = 1.0f;
        Heli heli;                     // kind == Heli
        Bumper bumper;                 // kind == Bumper
        Dashboard dashboard;           // kind == Dashboard
        TumbleCam tumble_cam;          // kind == Tumble
    };

    Global global;
    std::vector<Camera> cameras;  // in file order; `[Global]`, `*Debug*` and `*:Arm*` sections are not cameras

    // Throws nf::FormatError on malformed text (an unclosed `[section`).
    static CameraIni parse(std::string_view text);

    // The cameras RCameraIniLoader keeps for the car called `car_name`, in cycle order (= file order):
    //  * the `car` list must contain the name as a token prefix (sub_1A6CF8), sections without `car` match every car;
    //  * `*AnyCar*` Heli/Dashboard sections are only kept when no car specific one of that kind matched;
    //  * only the first AutoDrive camera is kept.
    std::vector<const Camera*> cameras_for_car(std::string_view car_name) const;
};

}  // namespace nf::driving

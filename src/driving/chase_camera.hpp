#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "driving/camera_ini.hpp"
#include "driving/camera_target.hpp"

namespace nf::driving {

// The player camera of DRIVING.ELF (RPlayerCamera), reduced to the cameras a driven car uses: the
// `Heli` chase cameras (sub_1B2B48), `Bumper` (sub_1B3B40), `Dashboard` (sub_1B3CE0) and the `Tumble`
// camera (sub_1B3788). All rates are per simulation tick: the original updates the camera once per
// sim tick and the tick rate is 60 Hz on NTSC (sub_2D6B90 table, sub_1FEDA0). Call update() at 60 Hz.
//
// The CameraIni passed to the constructor must outlive the camera.
class ChaseCamera {
public:
    static constexpr float kTickRate = 60.0f;

    // Keeps the cameras camera.ini defines for `car_name` (the vehicle attribute name, e.g. "vanquish").
    // Throws nf::FormatError when the car has no usable camera (no selectable camera, or a Heli camera
    // without `:Arm0`).
    ChaseCamera(const CameraIni& ini, std::string_view car_name);

    // Puts the camera on the default camera (`defaultCamera = 1`) and snaps it to the target.
    void reset(const CameraTarget& target);

    // GAMEACTION_CHANGECAMERA (Triangle) / CHANGECAMERADOWN: next / previous `selectable` camera in file order.
    void cycle_view();
    void cycle_view_back();

    // GAMEACTION_CAMLOOKBACK (L2 pressed) / CAMLOOKBACKRELEASE. Only cameras with `lookBack = 1` react.
    void set_look_back(bool on);

    // Selected secondary weapon (the name `weapons =` lists in the `:Arm<n>` sections match against, e.g.
    // "PROX MINE"); an empty string selects Arm0. Switching arms blends over kWeaponArmChangeLatency ticks.
    void set_weapon(std::string_view name);

    // One simulation tick. `rays` may be null (no world collision).
    CameraPose update(const CameraTarget& target, const RayCaster* rays);

    CameraPose pose() const;

    // Number of `selectable` cameras (what the cycle visits) and the position of the active one among
    // them (view_count() while a non-selectable camera such as the tumble camera is active).
    std::size_t view_count() const;
    std::size_t view_index() const;
    const CameraIni::Camera& camera() const { return *cams_[cur_]; }

private:
    struct Frame {
        Vec3 right, up, forward;
    };

    static constexpr std::size_t kNone = static_cast<std::size_t>(-1);

    std::size_t find_arm(const CameraIni::Camera& cam) const;
    void switch_to(std::size_t index, int transition_frames);
    bool smoothing_allowed() const;
    void update_anchor(const Frame& m, const Vec3& position, const Vec3& anchor_offset, bool smooth);
    void update_heli(const CameraIni::Camera& cam, const Frame& m, const CameraTarget& t, const RayCaster* rays);
    void update_bumper(const CameraIni::Camera& cam, const Frame& m, const CameraTarget& t);
    void update_dashboard(const CameraIni::Camera& cam, const Frame& m, const CameraTarget& t);
    void update_tumble(const CameraIni::Camera& cam, const Frame& m, const CameraTarget& t, const RayCaster* rays);
    void resolve_collision(const Frame& m, const Vec3& car_position, const RayCaster* rays, bool heli_mode, bool noisy, bool ceiling);
    void finish_view(const Vec3& up, float look_x, float look_y);
    void step_fov(float target_deg, bool snap);

    CameraIni::Global global_;
    std::vector<const CameraIni::Camera*> cams_;
    std::size_t default_ = 0, tumble_ = kNone;
    std::size_t cur_ = 0, prev_ = kNone, last_selectable_ = kNone;

    int flags_ = 0;  // bit 0: camera changed this tick, bit 1: look-back toggled (RPlayerCamera +224)
    std::size_t arm_ = 0;
    std::string weapon_;
    bool look_back_ = false;  // RPlayerCamState +72

    // Smooth transition between two cameras / arms (+300, +636, +637, +620).
    int transition_left_ = 0, transition_request_ = 0;
    bool transition_active_ = false, transition_first_ = false;

    float dist_ = 0, step_ = 0, pitch_ = 0, rate_ = 0, glance_ = 0;  // +540, +544, +600, +604, +592
    int tumble_left_ = 0;                                           // +304
    Vec3 eye_{0, 0, 0}, anchor_{0, 0, 0}, anchor_lag_{0, 0, 0}, offset_{0, 0, 0}, target_{0, 0, 1};
    Vec3 up_{0, 1, 0};                                              // +704
    Vec3 cam_x_{1, 0, 0}, cam_y_{0, 1, 0}, cam_z_{0, 0, 1};         // last view basis (RCamera matrix rows)
    float fov_deg_ = 33.0f;                                         // RCamera +164
    float heading_x_ = 0, heading_z_ = 1;

    int ceiling_count_ = 0;  // dword_333A38: frames the roof probe has been blocked (0..80)
    Vec3 ceiling_hit_{0, 0, 0};
};

}  // namespace nf::driving

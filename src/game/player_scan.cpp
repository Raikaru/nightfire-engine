#include "game/player_scan.hpp"

#include <algorithm>
#include <cmath>

#include "game/player.hpp"

namespace nf {

namespace {

constexpr float kMinFov = 0.2f, kMaxFov = 3.14f;   // Player_SSScanMode's clamp of viewer+0x118
constexpr int kActZoom = 6;                        // D-pad up/down

}  // namespace

void Player::update_scan(const ActionInput& input, FrameTiming timing) {
    // Player_SSScanMode: the debug free-fly camera.
    const float mul = timing.mul(), rec = timing.rec();
    if ((body_flags & body::kZoomed) != 0) {
        const float step = input.actionf(kActZoom) * rec;
        if (step != 0.0f) scan.fov = std::clamp(scan.fov + step, kMinFov, kMaxFov);
    } else {
        velocity[1] = 0.0f;
        look_state_ = 2;
        yaw_step_ = -input.actionf(kActTurn) * 3.1415927f * rec;
        const float pitch_delta = -(input.actionf(kActLookPitch) * 3.1415927f * rec);
        // Strafe up to 0.225 and forward up to 0.5 per FRAME_RATE_MUL, eased by 20% of the gap.
        const float strafe = 0.075f * mul * 3.0f, forward = 0.1f * mul * 5.0f;
        velocity[0] = ease_toward(prev_velocity_[0], input.actionf(kActStrafe) * 0.22500001f * mul, mul, 0.2f, -strafe,
                                  mul * 0.22500001f);
        velocity[2] = ease_toward(prev_velocity_[2], input.actionf(kActForward) * 0.5f * mul, mul, 0.2f, -forward,
                                  mul * 0.5f);
        // Yaw turns about the world's up axis, pitch about the body's own left axis.
        body_ = basis_mul(rot_x(pitch_delta), basis_mul(body_, yaw_basis(yaw_step_)));
        // Jump / crouch fly up and down at 3 units per second.
        pos[1] += float(int(input.held(kActJump)) - int(input.held(kActCrouch))) * rec * 3.0f;
        settled_pos[1] = pos[1];
    }
    fall_velocity = {};
}

void Player::set_scan_mode(bool on) {
    if (on) {
        set_substate(SubState::Scan, timing_);
        return;
    }
    // Player_Update's toggle: the heading of the free camera becomes the body's yaw (Vec_Cartesian_2_Spherical of
    // its forward axis, pitch and roll dropped), the walking substate and the default field of view return.
    const Vec3 dir = body_[2];
    yaw = std::atan2(dir[0], dir[2]);
    set_substate(SubState::Walk, timing_);
    scan.fov = kDefaultFov;
}

float Player::matrix_view_pitch() const { return std::asin(std::clamp(view_axes()[2][1], -1.0f, 1.0f)); }

float Player::matrix_view_yaw() const {
    const Vec3 dir = view_axes()[2];
    return std::atan2(dir[0], dir[2]);
}

}  // namespace nf

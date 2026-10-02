#include "game/player_zerog.hpp"

#include <algorithm>
#include <cmath>

#include "game/player.hpp"

namespace nf {

namespace {

constexpr int kActLevel = 14;   // Cross: Input_Action(pad, 14, 4) starts the roll correction

struct Quat {
    float w, x, y, z;
};

// Quat_MatrixToQuaternion / Quat_QuaternionToMatrix: any pair that inverts each other and maps rotations onto
// unit quaternions works for the slerp, this is the textbook one (basis rows as the matrix rows).
Quat quat_from_basis(const Basis& m) {
    const float trace = m[0][0] + m[1][1] + m[2][2];
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        return {s * 0.25f, (m[2][1] - m[1][2]) / s, (m[0][2] - m[2][0]) / s, (m[1][0] - m[0][1]) / s};
    }
    if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        const float s = std::sqrt(1.0f + m[0][0] - m[1][1] - m[2][2]) * 2.0f;
        return {(m[2][1] - m[1][2]) / s, s * 0.25f, (m[0][1] + m[1][0]) / s, (m[0][2] + m[2][0]) / s};
    }
    if (m[1][1] > m[2][2]) {
        const float s = std::sqrt(1.0f + m[1][1] - m[0][0] - m[2][2]) * 2.0f;
        return {(m[0][2] - m[2][0]) / s, (m[0][1] + m[1][0]) / s, s * 0.25f, (m[1][2] + m[2][1]) / s};
    }
    const float s = std::sqrt(1.0f + m[2][2] - m[0][0] - m[1][1]) * 2.0f;
    return {(m[1][0] - m[0][1]) / s, (m[0][2] + m[2][0]) / s, (m[1][2] + m[2][1]) / s, s * 0.25f};
}

Basis basis_from_quat(const Quat& q) {
    const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    return {Vec3{1 - 2 * (yy + zz), 2 * (xy - wz), 2 * (xz + wy)}, Vec3{2 * (xy + wz), 1 - 2 * (xx + zz), 2 * (yz - wx)},
            Vec3{2 * (xz - wy), 2 * (yz + wx), 1 - 2 * (xx + yy)}};
}

// Quat_Slerp_Acc(t, a, b): spherical interpolation over the short arc; nearly parallel quaternions are lerped.
Quat slerp_acc(float t, const Quat& a, const Quat& b) {
    float cosine = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
    const bool flip = cosine < 0.0f;
    if (flip) cosine = -cosine;
    float wa, wb = t;
    if (1.0f - cosine > 0.01f) {
        const float angle = std::acos(cosine);
        const float inv = 1.0f / std::sin(angle);
        wb = std::sin(angle * t) * inv;
        wa = std::sin(angle - angle * t) * inv;
    } else {
        wa = 1.0f - t;
    }
    if (flip) wa = -wa;
    return {wa * a.w + wb * b.w, wa * a.x + wb * b.x, wa * a.y + wb * b.y, wa * a.z + wb * b.z};
}

}  // namespace

void Player::update_zerog(const ActionInput& input, FrameTiming timing) {
    // Player_ZeroG (substates 8 and 9; 9 only adds the walk animation blend, which this port does not have).
    const float mul = timing.mul(), rec = timing.rec();
    fall_velocity = {};
    look_state_ = 2;
    pitch = 0.0f;

    float pitch_delta;
    if ((body_flags & body::kZoomed) != 0) {
        yaw_step_ = (-input.actionf(kActLookX) * kZeroGTurnRate * rec * 0.75f) / zoom;
        pitch_delta = (-input.actionf(kActLookY) * kZeroGTurnRate * rec * 0.75f) / zoom;
    } else {
        yaw_step_ = -input.actionf(kActTurn) * kZeroGTurnRate * rec;
        pitch_delta = -input.actionf(kActLookPitch) * kZeroGTurnRate * rec;
    }

    // Each thrust axis eases towards 0.1 * stick by 4% of the gap per FRAME_RATE_MUL, at most 0.05 * FRAME_RATE_MUL.
    const float limit = mul * 0.05f;
    velocity[0] = ease_toward(prev_velocity_[0], input.actionf(kActStrafe) * 0.1f, mul, 0.040000003f, -limit, limit);
    velocity[2] = ease_toward(prev_velocity_[2], input.actionf(kActForward) * 0.1f, mul, 0.040000003f, -limit, limit);
    const float vertical = float(int(input.held(kActJump)) - int(input.held(kActCrouch))) * 0.1f;
    velocity[1] = ease_toward(prev_velocity_[1], vertical, mul, 0.040000003f, -limit, limit);

    // The turn deltas rotate the body about its own axes: RotMatrix(pitch, yaw, 0) applied first.
    body_ = basis_mul(rot_euler({pitch_delta, yaw_step_, 0.0f}), body_);

    if (input.pressed(kActLevel)) zerog.level_timer = std::int16_t(int(timing.rate * 1.5f));
    if (zerog.level_timer != 0) {
        --zerog.level_timer;
        // Roll the body upright: face the horizontal projection of its heading (straight up or down: along its
        // own up axis, mirrored) with the world up above it, and slerp there at 3 * REC_FRAME_RATE per frame.
        const Vec3 dir = body_[2], up = body_[1];
        const float d = dir[1];
        Vec3 heading;
        if (d > 0.995f) heading = up * -1.0f;
        else if (d < -0.995f) heading = up;
        else heading = dir + Vec3{0.0f, 1.0f, 0.0f} * -d;
        const Basis target = align_to_up({0.0f, 1.0f, 0.0f}, heading);
        body_ = normalised(basis_from_quat(slerp_acc(rec * 3.0f, quat_from_basis(body_), quat_from_basis(target))));
    }
}

}  // namespace nf

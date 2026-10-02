#include "driving/special_vehicles.hpp"

#include <algorithm>
#include <cmath>

namespace nf::driving {
namespace {

constexpr float kDt = 1.0f / 60.0f;

Mat4 mat_from_axes(const Vec3& x, const Vec3& y, const Vec3& z, const Vec3& t) {
    return {x[0], y[0], z[0], 0, x[1], y[1], z[1], 0, x[2], y[2], z[2], 0, t[0], t[1], t[2], 1};
}

float floor_at(const TrackCollision& c, const Vec3& p, float fallback) {
    GroundHit g;
    if (c.ground_below({p[0], p[1] + 50.0f, p[2]}, g)) return g.point[1];
    return fallback;
}

}  // namespace

Submarine::Submarine(const VehicleParams& params, float top_speed)
    : top_speed_(top_speed), accel_(params.max_acc > 0 ? params.max_acc : 2.75f) {}

void Submarine::reset(const Vec3& pos, float yaw) {
    pos_ = pos;
    yaw_ = yaw;
    pitch_ = speed_ = throttle_ = 0;
}

Vec3 Submarine::forward() const {
    const float cp = std::cos(pitch_);
    return {std::sin(yaw_) * cp, std::sin(pitch_), std::cos(yaw_) * cp};
}

void Submarine::step(const FlightInput& in, const TrackCollision& collision) {
    boost_ = std::max(boost_ - kDt, 0.0f);
    // Throttle toward demand; drag grows quadratically (water).
    throttle_ += std::clamp(in.gas - in.brake - throttle_, -2.0f * kDt, 2.0f * kDt);
    const float target = throttle_ * top_speed_ * (boost_ > 0 ? 1.5f : 1.0f);
    const float rate = (target > speed_ ? accel_ : accel_ * 2.0f) * kDt;
    speed_ += std::clamp(target - speed_, -rate, rate);
    speed_ -= speed_ * speed_ * 0.002f * kDt * 60.0f * 0.016f;
    yaw_ += in.steer * (0.5f + 0.5f * std::min(1.0f, speed_ / 8.0f)) * 0.9f * kDt;
    // Dive planes: pitch follows LY with depth-rate damping; auto-level near the surface.
    const float want_pitch = std::clamp(-in.pitch * 0.5f, -0.5f, 0.5f);
    pitch_ += std::clamp(want_pitch - pitch_, -0.8f * kDt, 0.8f * kDt);
    pos_ += forward() * (speed_ * kDt);
    // Buoyancy: drift up toward periscope depth (3 m under the surface datum y=0 of uw_mis11).
    const float floor = floor_at(collision, pos_, pos_[1] - 30.0f);
    const float surface = 0.0f;  // uw_mis11 water datum [INFERENCE: tracks sampled near y=0]
    const float keel = 2.0f;
    if (speed_ < 3.0f) pos_[1] += ((surface - 3.0f) - pos_[1]) * 0.2f * kDt;
    pos_[1] = std::clamp(pos_[1], floor + keel, surface - 1.0f);
    (void)collision;
}

float Submarine::rpm() const { return 800.0f + std::abs(throttle_) * 5000.0f; }

CameraTarget Submarine::camera_target() const {
    CameraTarget t;
    t.position = pos_;
    t.forward = forward();
    t.right = {std::cos(yaw_), 0, -std::sin(yaw_)};
    t.up = {0, 1, 0};
    t.velocity = forward() * speed_;
    t.speed = std::abs(speed_);
    return t;
}

Mat4 Submarine::model_matrix() const {
    const Vec3 f = forward();
    const Vec3 r = {std::cos(yaw_), 0, -std::sin(yaw_)};
    const Vec3 u = cross(f, r);
    return mat_from_axes(r, u, f, pos_);
}

Ultralight::Ultralight(const VehicleParams& params, float top_speed)
    : top_speed_(top_speed), accel_(params.max_acc > 0 ? params.max_acc : 3.0f) {}

void Ultralight::reset(const Vec3& pos, float yaw) {
    pos_ = pos;
    yaw_ = yaw;
    pitch_ = roll_ = throttle_ = 0;
    speed_ = top_speed_ * 0.5f;
}

Vec3 Ultralight::forward() const {
    const float cp = std::cos(pitch_);
    return {std::sin(yaw_) * cp, std::sin(pitch_), std::cos(yaw_) * cp};
}

void Ultralight::step(const FlightInput& in, const TrackCollision& collision) {
    boost_ = std::max(boost_ - kDt, 0.0f);
    throttle_ += std::clamp(in.gas - in.brake - throttle_, -1.5f * kDt, 1.5f * kDt);
    const float target = (0.25f + 0.75f * throttle_) * top_speed_ * (boost_ > 0 ? 1.4f : 1.0f);
    const float rate = accel_ * kDt;
    speed_ += std::clamp(target - speed_, -rate * 0.5f, rate);
    // Banking turn: roll follows stick, yaw follows roll.
    roll_ += std::clamp(-in.steer * 0.7f - roll_, -2.0f * kDt, 2.0f * kDt);
    yaw_ += -roll_ * (0.4f + 0.6f * std::min(1.0f, speed_ / top_speed_)) * 1.2f * kDt;
    const float want_pitch = std::clamp(-in.pitch * 0.45f, -0.45f, 0.45f);
    pitch_ += std::clamp(want_pitch - pitch_, -1.0f * kDt, 1.0f * kDt);
    // Stall: below fly speed the nose drops and she sinks [INFERENCE].
    const float fly = top_speed_ * 0.45f;
    float sink = 0;
    if (speed_ < fly) sink = (fly - speed_) * 0.6f;
    pos_ += forward() * (speed_ * kDt);
    pos_[1] -= sink * kDt;
    // NO_WORLD_COLLISIONS: only the terrain clamp (never through the ground) + soft ceiling.
    const float floor = floor_at(collision, pos_, pos_[1] - 100.0f);
    pos_[1] = std::clamp(pos_[1], floor + 2.5f, floor + 220.0f);
}

float Ultralight::rpm() const { return 1200.0f + std::abs(throttle_) * 4800.0f; }

CameraTarget Ultralight::camera_target() const {
    CameraTarget t;
    t.position = pos_;
    t.forward = forward();
    t.right = {std::cos(yaw_), 0, -std::sin(yaw_)};
    t.up = {0, 1, 0};
    t.velocity = forward() * speed_;
    t.speed = std::abs(speed_);
    return t;
}

Mat4 Ultralight::model_matrix() const {
    const float cr = std::cos(roll_), sr = std::sin(roll_);
    const Vec3 f = forward();
    Vec3 r = {std::cos(yaw_), 0, -std::sin(yaw_)};
    Vec3 u = cross(f, r);
    // Apply roll about the forward axis.
    r = r * cr + u * sr;
    u = cross(f, r);
    return mat_from_axes(r, u, f, pos_);
}

Snowmobile::Snowmobile(const VehicleParams& params, float top_speed)
    : top_speed_(top_speed), accel_(params.max_acc > 0 ? params.max_acc : 2.5f) {}

void Snowmobile::reset(const Vec3& pos, float yaw) {
    pos_ = pos;
    yaw_ = yaw;
    speed_ = throttle_ = 0;
}

Vec3 Snowmobile::forward() const { return {std::sin(yaw_), 0, std::cos(yaw_)}; }

void Snowmobile::step(const DriveInput& in, const TrackCollision& collision) {
    boost_ = std::max(boost_ - kDt, 0.0f);
    if (in.handbrake) {
        speed_ = std::max(0.0f, speed_ - 25.0f * kDt);  // track brake digs in
    } else {
        throttle_ += std::clamp(in.gas - in.brake - throttle_, -2.0f * kDt, 2.0f * kDt);
        const float target = throttle_ * top_speed_ * (boost_ > 0 ? 1.4f : 1.0f);
        const float rate = (target > speed_ ? accel_ : accel_ * 3.0f) * kDt;
        speed_ += std::clamp(target - speed_, -rate, rate);
    }
    // Ski steering: agile at crawl, lazy at speed; slides wide when steered hard and fast.
    const float grip = std::min(1.0f, 12.0f / (4.0f + speed_));
    yaw_ += in.steer * (0.5f + 1.6f * grip) * kDt;
    const Vec3 step = forward() * (speed_ * kDt);
    // Wall probe so the sled cannot tunnel through mountainsides.
    SegmentHit hit;
    if (speed_ > 1.0f && collision.segment_hit(pos_ + Vec3{0, 0.5f, 0}, pos_ + step + Vec3{0, 0.5f, 0}, hit) &&
        hit.normal[1] < 0.5f) {
        speed_ *= 0.2f;
    } else {
        pos_ += step;
    }
    // Ground following (0.35 m ride height); falls back to the current height over voids.
    GroundHit g;
    if (collision.ground_below({pos_[0], pos_[1] + 3.0f, pos_[2]}, g)) pos_[1] = g.point[1] + 0.35f;
}

float Snowmobile::rpm() const { return 900.0f + std::abs(throttle_) * 4500.0f + speed_ * 20.0f; }

CameraTarget Snowmobile::camera_target() const {
    CameraTarget t;
    t.position = pos_;
    t.forward = forward();
    t.right = {std::cos(yaw_), 0, -std::sin(yaw_)};
    t.up = {0, 1, 0};
    t.velocity = forward() * speed_;
    t.speed = std::abs(speed_);
    return t;
}

Mat4 Snowmobile::model_matrix() const {
    const Vec3 f = forward();
    const Vec3 r = {std::cos(yaw_), 0, -std::sin(yaw_)};
    const Vec3 u = cross(f, r);
    return mat_from_axes(r, u, f, pos_);
}

}  // namespace nf::driving

#include "driving/rigid_body.hpp"

#include <algorithm>
#include <cmath>

namespace nf::driving {

Axes axes_from_quat(const Quat& q) {
    const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    Axes a;
    a.right = {1 - 2 * (yy + zz), 2 * (xy + wz), 2 * (xz - wy)};
    a.up = {2 * (xy - wz), 1 - 2 * (xx + zz), 2 * (yz + wx)};
    a.forward = {2 * (xz + wy), 2 * (yz - wx), 1 - 2 * (xx + yy)};
    return a;
}

Quat quat_from_axes(const Axes& a) {
    // Rotation matrix with the axes as columns.
    const float m00 = a.right[0], m10 = a.right[1], m20 = a.right[2];
    const float m01 = a.up[0], m11 = a.up[1], m21 = a.up[2];
    const float m02 = a.forward[0], m12 = a.forward[1], m22 = a.forward[2];
    Quat q;
    const float tr = m00 + m11 + m22;
    if (tr > 0) {
        const float s = std::sqrt(tr + 1) * 2;
        q = {(m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25f * s};
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1 + m00 - m11 - m22) * 2;
        q = {0.25f * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s};
    } else if (m11 > m22) {
        const float s = std::sqrt(1 + m11 - m00 - m22) * 2;
        q = {(m01 + m10) / s, 0.25f * s, (m12 + m21) / s, (m02 - m20) / s};
    } else {
        const float s = std::sqrt(1 + m22 - m00 - m11) * 2;
        q = {(m02 + m20) / s, (m12 + m21) / s, 0.25f * s, (m10 - m01) / s};
    }
    const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return {q.x / n, q.y / n, q.z / n, q.w / n};
}

Axes axes_from_yaw(float yaw) {
    const float s = std::sin(yaw), c = std::cos(yaw);
    Axes a;
    a.right = {c, 0, -s};
    a.up = {0, 1, 0};
    a.forward = {s, 0, c};
    return a;
}

Vec3 inverse_inertia(float m, const Vec3& h) {
    const float a = h[0], b = h[1], c = h[2];
    if (!(c > 4.0f)) {
        const float r2 = a * a + b * b;
        const float perp = 0.5f / ((m * 0.08333f) * ((c + c) * (c + c)) + (m * 0.25f) * r2);
        return {perp, perp, 0.5f / ((m * 0.5f) * r2)};
    }
    const float k = m * 0.3333f;
    return {1.0f / (k * (b * b + c * c)), 1.0f / (k * (a * a + c * c)), 1.0f / (k * (a * a + b * b))};
}

RigidBody::RigidBody(float mass, const Vec3& half_extents)
    : mass_(mass), half_extents_(half_extents), inv_i_(inverse_inertia(mass, half_extents)) {
    reset({0, 0, 0}, Axes{});
}

void RigidBody::reset(const Vec3& position, const Axes& axes) {
    pos_ = position;
    set_orientation(axes);
    p_ = l_ = vel_ = omega_ = force_ = torque_ = {0, 0, 0};
}

void RigidBody::set_orientation(const Axes& a) {
    q_ = quat_from_axes(a);
    axes_ = axes_from_quat(q_);
    refresh();
}

Vec3 RigidBody::inv_inertia_world(const Vec3& v) const {
    return axes_.to_world(vscale(axes_.to_local(v), inv_i_));
}

void RigidBody::refresh() {
    vel_ = p_ * (1.0f / mass_);
    omega_ = inv_inertia_world(l_);
}

void RigidBody::set_momentum(const Vec3& p) {
    p_ = p;
    refresh();
}
void RigidBody::add_momentum(const Vec3& dp) { set_momentum(p_ + dp); }
void RigidBody::set_angular_momentum(const Vec3& l) {
    l_ = l;
    refresh();
}
void RigidBody::add_angular_momentum(const Vec3& dl) { set_angular_momentum(l_ + dl); }

void RigidBody::apply_angular_damping(float factor) { set_angular_momentum(l_ * factor); }

void RigidBody::integrate(float dt, float m_limit, float am_limit) {
    const float p_lim = m_limit * mass_;
    for (float& c : p_) c = std::clamp(c, -p_lim, p_lim);
    p_ += force_ * dt;
    l_ += torque_ * dt;
    force_ = torque_ = {0, 0, 0};

    vel_ = p_ * (1.0f / mass_);
    omega_ = inv_inertia_world(l_);
    for (float& c : omega_) c = std::clamp(c, -am_limit, am_limit);
    // Angular momentum is rebuilt from the limited angular velocity (I * omega, world frame).
    const Vec3 inertia{1.0f / inv_i_[0], 1.0f / inv_i_[1], 1.0f / inv_i_[2]};
    l_ = axes_.to_world(vscale(axes_.to_local(omega_), inertia));

    pos_ += vel_ * dt;

    // q += 0.5 * dt * (omega (x) q), omega as a pure world-frame quaternion.
    const Vec3 wdt = omega_ * dt;
    const Vec3 qv{q_.x, q_.y, q_.z};
    const Vec3 dv = cross(wdt, qv) + wdt * q_.w;
    const float dw = -dot(wdt, qv);
    q_.x += 0.5f * dv[0];
    q_.y += 0.5f * dv[1];
    q_.z += 0.5f * dv[2];
    q_.w += 0.5f * dw;
    const float n = std::sqrt(q_.x * q_.x + q_.y * q_.y + q_.z * q_.z + q_.w * q_.w);
    q_ = {q_.x / n, q_.y / n, q_.z / n, q_.w / n};
    axes_ = axes_from_quat(q_);
}

}  // namespace nf::driving

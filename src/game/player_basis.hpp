#pragma once

#include <array>
#include <cmath>

#include "core/math.hpp"

namespace nf {

// The rotation part of the original's MATRIX (obj+0x90): row 0 = left, row 1 = up, row 2 = forward, in world
// space. Row-vector convention like the PS2 code: `v * M = v.x * row0 + v.y * row1 + v.z * row2`.
using Basis = std::array<Vec3, 3>;

// A yaw-only body: forward = (sin yaw, 0, cos yaw), RotMatrixY(yaw).
inline Basis yaw_basis(float yaw) {
    const float s = std::sin(yaw), c = std::cos(yaw);
    return {Vec3{c, 0.0f, -s}, Vec3{0.0f, 1.0f, 0.0f}, Vec3{s, 0.0f, c}};
}

// RotMatrixX: row1 = (0, cos, sin), row2 = (0, -sin, cos); looking up is a negative angle.
inline Basis rot_x(float a) {
    const float s = std::sin(a), c = std::cos(a);
    return {Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, c, s}, Vec3{0.0f, -s, c}};
}

// RotMatrix(&(x, y, z)): the composite the zero-G and scan handlers feed their per-frame turn deltas to.
inline Basis rot_euler(const Vec3& v) {
    const float cx = std::cos(v[0]), sx = std::sin(v[0]);
    const float cy = std::cos(v[1]), sy = std::sin(v[1]);
    const float cz = std::cos(v[2]), sz = std::sin(v[2]);
    return {Vec3{cy * cz, sx * sy * cz + cx * sz, -cx * sy * cz + sx * sz},
            Vec3{cy * -sz, cx * cz - sx * sy * sz, sx * cz + cx * sy * sz},
            Vec3{sy, -sx * cy, cx * cy}};
}

// `a` then `b` in the row-vector convention: Concat(b, a) leaves this in `a`.
inline Basis basis_mul(const Basis& a, const Basis& b) {
    Basis r{};
    for (int i = 0; i < 3; ++i) r[std::size_t(i)] = b[0] * a[std::size_t(i)][0] + b[1] * a[std::size_t(i)][1] + b[2] * a[std::size_t(i)][2];
    return r;
}

// Vec_Normalise: a zero vector stays zero.
inline Vec3 normalised(const Vec3& v) {
    const float len = length(v);
    return len == 0.0f ? Vec3{0, 0, 0} : v * (1.0f / len);
}

// Mat_Normalize: forward and up are kept (normalised), left = up x forward, up = forward x left.
inline Basis normalised(const Basis& m) {
    const Vec3 dir = normalised(m[2]);
    const Vec3 left = normalised(cross(normalised(m[1]), dir));
    return {left, cross(dir, left), dir};
}

// Mat_Align2Up(&out, up, dir): the level frame that keeps `up` and faces along `dir` (projected onto the plane
// normal to `up`): left = up x dir, forward = left x up.
inline Basis align_to_up(const Vec3& up, const Vec3& dir) {
    const Vec3 left = normalised(cross(up, dir));
    return {left, up, normalised(cross(left, up))};
}

// The velocity easing of the flying substates (zero-G, scan mode): `previous` moves by `gain * mul` of the gap
// towards `target`, never past it, then is clamped to [lo, hi].
inline float ease_toward(float previous, float target, float mul, float gain, float lo, float hi) {
    const float gap = target - previous;
    float v = previous + gap * mul * gain;
    if (gap >= 0.0f ? target < v : v < target) v = target;
    return v < lo ? lo : (hi < v ? hi : v);
}

}  // namespace nf

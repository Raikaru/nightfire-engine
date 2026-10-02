#pragma once

#include <array>
#include <cmath>

namespace nf {

// Game coordinates: Y up. The original's forward for yaw θ is (sin θ, 0, cos θ).
using Vec3 = std::array<float, 3>;
using Mat4 = std::array<float, 16>;  // column-major

inline Vec3 operator+(const Vec3& a, const Vec3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
inline Vec3 operator-(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
inline Vec3 operator*(const Vec3& a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }
inline Vec3& operator+=(Vec3& a, const Vec3& b) { return a = a + b; }
inline Vec3& operator-=(Vec3& a, const Vec3& b) { return a = a - b; }
inline float dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
inline float length(const Vec3& a) { return std::sqrt(dot(a, a)); }

// Emotion Engine single-precision multiply: every operation rounds toward zero (docs/ee.md, src/ee/ps2float.hpp).
// Host IEEE rounds to nearest, which is off by one ulp exactly when the product sits just below a float
// (e.g. 0.7f * 20.0f is 13.9999998 on the EE but 14.0f on the host) — enough to flip integer boundaries like
// the FindOpponent lost-sight timeout (839 vs 840 frames). Only for normal-range positive products.
inline float ee_trunc_mul(float a, float b) {
    const double exact = double(a) * double(b);
    float r = float(exact);
    if ((exact > 0.0 && r > exact) || (exact < 0.0 && r < exact)) r = std::nextafter(r, 0.0f);
    return r;
}

inline Mat4 identity() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; }

inline Mat4 mul(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row)
            for (int k = 0; k < 4; ++k) r[c * 4 + row] += a[k * 4 + row] * b[c * 4 + k];
    return r;
}

inline Vec3 transform_point(const Mat4& m, const Vec3& p) {
    return {m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12], m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13],
            m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14]};
}

inline Mat4 perspective(float fovy, float aspect, float znear, float zfar) {
    float f = 1.0f / std::tan(fovy / 2);
    Mat4 m{};
    m[0] = f / aspect;
    m[5] = f;
    m[10] = (zfar + znear) / (znear - zfar);
    m[11] = -1;
    m[14] = 2 * zfar * znear / (znear - zfar);
    return m;
}

}  // namespace nf

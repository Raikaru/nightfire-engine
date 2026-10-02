#pragma once

#include "core/math.hpp"

namespace nf::driving {

inline Vec3 vscale(const Vec3& a, const Vec3& b) { return {a[0] * b[0], a[1] * b[1], a[2] * b[2]}; }
inline Vec3 vneg(const Vec3& a) { return {-a[0], -a[1], -a[2]}; }
inline Vec3 vnormalized(const Vec3& a) {
    const float l = length(a);
    return l > 0 ? a * (1.0f / l) : Vec3{0, 0, 0};
}

struct Quat {
    float x = 0, y = 0, z = 0, w = 1;
};

// Orientation as the body's three axes expressed in world coordinates. These are the rows of the
// original's orientation matrix (record +0x00/+0x10/+0x20): local +X = right, +Y = up, +Z = forward.
// The original's coordinates are left-handed (right x up = forward); every formula here is
// handedness-agnostic, so the same numbers are used unchanged.
struct Axes {
    Vec3 right{1, 0, 0}, up{0, 1, 0}, forward{0, 0, 1};

    Vec3 to_world(const Vec3& l) const { return right * l[0] + up * l[1] + forward * l[2]; }
    Vec3 to_local(const Vec3& w) const { return {dot(w, right), dot(w, up), dot(w, forward)}; }
};

Axes axes_from_quat(const Quat& q);
Quat quat_from_axes(const Axes& a);
// Forward (sin yaw, 0, cos yaw); positive yaw turns towards +X.
Axes axes_from_yaw(float yaw);

// Principal inverse moments of inertia the original derives from the mass and the model's bounding box
// half extents (sub_210938, called by PBondCar::PBondCar sub_1862C0 with the sub_1C9778 half extents):
// a "cylinder-like" formula (doubled) while the half length is at most 4, an ordinary box beyond.
Vec3 inverse_inertia(float mass, const Vec3& half_extents);

// One rigid body integrated exactly like the VU0 "Timestep" micro-program (.vutext, entered from
// sub_1F89E0): state is position, orientation quaternion, linear momentum and angular momentum; forces
// and torques accumulate as (acceleration * mass) and are consumed by integrate().
class RigidBody {
public:
    RigidBody(float mass, const Vec3& half_extents);

    // Places the body at rest.
    void reset(const Vec3& position, const Axes& axes);

    float mass() const { return mass_; }
    // Accumulators as left by add_accel()/add_torque_accel() this tick (force = acceleration * mass).
    const Vec3& force() const { return force_; }
    const Vec3& torque() const { return torque_; }
    void set_force(const Vec3& f) { force_ = f; }
    void set_torque(const Vec3& t) { torque_ = t; }

    const Vec3& half_extents() const { return half_extents_; }
    const Vec3& position() const { return pos_; }
    const Axes& axes() const { return axes_; }
    const Vec3& velocity() const { return vel_; }
    const Vec3& angular_velocity() const { return omega_; }
    const Vec3& momentum() const { return p_; }
    const Vec3& angular_momentum() const { return l_; }

    // sub_1F8830 / sub_1F88C8: `accel` (or angular acceleration) times the mass joins the accumulator.
    void add_accel(const Vec3& accel) { force_ += accel * mass_; }
    void add_torque_accel(const Vec3& t) { torque_ += t * mass_; }

    void set_position(const Vec3& p) { pos_ = p; }
    void set_orientation(const Axes& a);

    // Direct momentum edits (sub_1FB318 world collision, handbrake stop in sub_18CEF8); the derived
    // velocities follow immediately.
    void set_momentum(const Vec3& p);
    void add_momentum(const Vec3& dp);
    void set_angular_momentum(const Vec3& l);
    void add_angular_momentum(const Vec3& dl);

    // World inverse inertia applied to a world vector.
    Vec3 inv_inertia_world(const Vec3& v) const;

    // RigidBody::ApplyAngularDamping (sub_1F8088): angular momentum *= factor. The original applies it
    // to non-car bodies only.
    void apply_angular_damping(float factor);

    // One tick: momentum limit, force/torque integration, angular velocity limit, position and
    // orientation update; clears the accumulators.
    void integrate(float dt, float m_limit, float am_limit);

private:
    void refresh();

    float mass_;
    Vec3 half_extents_;
    Vec3 inv_i_;  // principal inverse inertia
    Vec3 pos_{0, 0, 0};
    Quat q_;
    Axes axes_;
    Vec3 p_{0, 0, 0}, l_{0, 0, 0};
    Vec3 vel_{0, 0, 0}, omega_{0, 0, 0};
    Vec3 force_{0, 0, 0}, torque_{0, 0, 0};
};

}  // namespace nf::driving

#include "driving/chase_camera.hpp"

#include <algorithm>
#include <cmath>

#include "assets/reader.hpp"

namespace nf::driving {
namespace {

using Kind = CameraIni::Kind;

constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;

// RPlayerCamera::UpdateCurrentArm (sub_1D6498): the per tick approach of the anchor offset.
constexpr float kAnchorLerp = 0.05f;
// Camera roof probe (sub_1B9198): hit counter range and the drop of the eye per counted frame.
constexpr int kCeilingMax = 80;
constexpr float kCeilingDrop = 0.0125f;
// Dashboard vertigo clamp (sub_1B3CE0).
constexpr float kDashboardVertigoMax = 0.05f;
// Default camera height above the eye of the tumble camera when it starts without smoothing (sub_1B3788).
constexpr float kTumbleStartLift = 2.0f;
// Arm of the tumble camera when the previous camera was not a Heli camera (sub_1B3788).
constexpr Vec3 kTumbleFallbackArm{0.0f, 3.0f, -6.0f};

Vec3 normalized(const Vec3& v, const Vec3& fallback) {
    const float l = length(v);
    return l > 1e-8f ? v * (1.0f / l) : fallback;
}

Vec3 approach(const Vec3& current, const Vec3& target, float rate) { return current + (target - current) * rate; }

float horizontal_speed(const CameraTarget& t) { return std::hypot(t.velocity[0], t.velocity[2]); }

bool supported(Kind k) { return k == Kind::Heli || k == Kind::Bumper || k == Kind::Dashboard || k == Kind::Tumble; }

// sub_1B9020: is `p` inside the triangle (a, b, c)? The test drops the axis whose normal component is
// largest (signed comparison, as in the original) and checks the three edge signs in the other two.
bool inside_triangle(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c) {
    const Vec3 n = cross(a - b, c - b);
    int axis = 2;
    if (n[0] >= n[1]) {
        axis = n[0] < n[2] ? 2 : 0;
    } else {
        axis = n[1] < n[2] ? 2 : 1;
    }
    // sub_1B8F80: 2D orientation of (x, y, z) in the plane picked by `axis` (0: y,z  1: z,x  2: x,y).
    auto edge = [axis](const Vec3& x, const Vec3& y, const Vec3& z) {
        const int u = axis == 2 ? 0 : (axis == 1 ? 2 : 1);
        const int v = axis == 2 ? 1 : (axis == 1 ? 0 : 2);
        return (y[v] - z[v]) * (x[u] - z[u]) - (x[v] - z[v]) * (y[u] - z[u]);
    };
    const float e1 = edge(p, a, b);
    const float e2 = edge(p, c, b);
    const float e3 = edge(p, b, a);
    if (e1 > 0.0f) return e2 >= 0.0f && e3 >= 0.0f;
    return e2 <= 0.0f && e3 <= 0.0f;
}

}  // namespace

ChaseCamera::ChaseCamera(const CameraIni& ini, std::string_view car_name) : global_(ini.global) {
    for (const CameraIni::Camera* cam : ini.cameras_for_car(car_name)) {
        if (!supported(cam->kind)) continue;
        if (cam->kind == Kind::Heli && cam->heli.arms.empty())
            throw FormatError("camera.ini: heli camera " + cam->name + " has no :Arm0 section");
        cams_.push_back(cam);
    }
    bool have_default = false;
    std::size_t first_selectable = kNone;
    for (std::size_t i = 0; i < cams_.size(); ++i) {
        if (cams_[i]->kind == Kind::Tumble && tumble_ == kNone) tumble_ = i;
        if (cams_[i]->default_camera && !have_default) {  // byte_333A12: the first `defaultCamera` wins
            default_ = i;
            have_default = true;
        }
        if (cams_[i]->selectable && first_selectable == kNone) first_selectable = i;
    }
    if (first_selectable == kNone) throw FormatError("camera.ini: no selectable camera for car " + std::string(car_name));
    if (!have_default) default_ = first_selectable;
    cur_ = default_;
}

std::size_t ChaseCamera::view_count() const {
    return static_cast<std::size_t>(std::count_if(cams_.begin(), cams_.end(), [](const auto* c) { return c->selectable; }));
}

std::size_t ChaseCamera::view_index() const {
    if (!cams_[cur_]->selectable) return view_count();
    std::size_t n = 0;
    for (std::size_t i = 0; i < cur_; ++i) n += cams_[i]->selectable ? 1 : 0;
    return n;
}

void ChaseCamera::reset(const CameraTarget& target) {
    // RPlayerCamera reset (sub_1BBC90): identity view, no offset, +224 = 1 (camera changed).
    cur_ = default_;
    prev_ = last_selectable_ = kNone;
    flags_ = 1;
    arm_ = cams_[cur_]->kind == Kind::Heli ? find_arm(*cams_[cur_]) : 0;
    look_back_ = false;
    transition_left_ = transition_request_ = 0;
    transition_active_ = transition_first_ = false;
    dist_ = step_ = pitch_ = rate_ = glance_ = 0;
    tumble_left_ = 0;
    eye_ = anchor_ = anchor_lag_ = offset_ = {0, 0, 0};
    target_ = {0, 0, 1};
    up_ = {0, 1, 0};
    cam_x_ = {1, 0, 0};
    cam_y_ = {0, 1, 0};
    cam_z_ = {0, 0, 1};
    fov_deg_ = 33.0f;  // RViewCamera constructor, sub_1D40F0
    heading_x_ = 0;
    heading_z_ = 1;
    ceiling_count_ = 0;
    update(target, nullptr);
}

std::size_t ChaseCamera::find_arm(const CameraIni::Camera& cam) const {  // FindHeliArmInd, sub_1BADD0
    if (weapon_.empty()) return 0;
    for (std::size_t i = 0; i < cam.heli.arms.size(); ++i)
        if (cam.heli.arms[i].weapons.find(weapon_) != std::string::npos) return i;
    return 0;
}

void ChaseCamera::switch_to(std::size_t index, int transition_frames) {  // sub_1BA7A0
    const CameraIni::Camera& next = *cams_[index];
    if (next.kind == Kind::Heli) {
        const std::size_t arm = find_arm(next);
        if (index == cur_ && arm == arm_) return;  // same camera, same arm: nothing to do
        arm_ = arm;
    }
    const CameraIni::Camera& old = *cams_[cur_];
    if (old.kind != Kind::Tumble) {  // the tumble camera never becomes the camera to return to
        prev_ = cur_;
        if (old.selectable) last_selectable_ = cur_;
    }
    pitch_ = 0;
    cur_ = index;
    flags_ |= 1;
    transition_request_ = transition_frames;
}

void ChaseCamera::cycle_view() {  // sub_1BA5C0
    if (!cams_[cur_]->selectable) return;
    std::size_t next = (cur_ + 1) % cams_.size();
    for (std::size_t n = 0; n < cams_.size() && !cams_[next]->selectable; ++n) next = (next + 1) % cams_.size();
    switch_to(next, 0);
}

void ChaseCamera::cycle_view_back() {  // sub_1BA6B0
    if (!cams_[cur_]->selectable) return;
    std::size_t next = (cur_ + cams_.size() - 1) % cams_.size();
    for (std::size_t n = 0; n < cams_.size() && !cams_[next]->selectable; ++n) next = (next + cams_.size() - 1) % cams_.size();
    switch_to(next, 0);
}

void ChaseCamera::set_look_back(bool on) {  // SetCameraLookBack, sub_1BC048
    if (!cams_[cur_]->look_back) return;
    look_back_ = on;
    flags_ |= 2;
}

void ChaseCamera::set_weapon(std::string_view name) {  // sub_1BB010
    weapon_ = name;
    if (cams_[cur_]->kind == Kind::Heli) switch_to(cur_, global_.weapon_arm_change_latency);
}

bool ChaseCamera::smoothing_allowed() const {  // sub_1BBF70: both the previous and the current camera blend
    return prev_ != kNone && cams_[prev_]->smooth_trans > 0 && cams_[cur_]->smooth_trans > 0;
}

void ChaseCamera::update_anchor(const Frame& m, const Vec3& position, const Vec3& anchor_offset, bool smooth) {
    const Vec3 local = m.right * anchor_offset[0] + m.up * anchor_offset[1] + m.forward * anchor_offset[2];
    anchor_lag_ = smooth ? approach(anchor_lag_, local, kAnchorLerp) : local;
    anchor_ = anchor_lag_ + position;
}

// RCamera view angle easing (sub_1B8740 case 1 / sub_1D6720): the angle moves towards the camera's
// defaultFov by kZoomIncSpeed degrees per tick; `snap` sets it at once. Values <= 2 are ignored.
void ChaseCamera::step_fov(float target_deg, bool snap) {
    float v = target_deg;
    if (!snap) {
        const float s = global_.zoom_inc_speed;
        v = fov_deg_ + s < target_deg ? fov_deg_ + s : std::max(target_deg, fov_deg_ - s);
    }
    if (v > 2.0f) fov_deg_ = v;
}

// sub_1D5700 / sub_1D5610: the view direction is (anchor - eye) plus optional look offsets along the
// up vector and the previous right axis; the right axis is up x direction.
void ChaseCamera::finish_view(const Vec3& up, float look_x, float look_y) {
    const Vec3 raw = anchor_ - eye_ + up * look_y + cam_x_ * look_x;
    target_ = eye_ + raw;
    const Vec3 z = normalized(raw, cam_z_);
    const Vec3 x = normalized(cross(up, z), cam_x_);
    cam_z_ = z;
    cam_x_ = x;
    cam_y_ = cross(z, x);
}

// Keeps the eye out of the world (RPlayerCamera::ResolveAllCollisions, sub_1B9198). Three probe rays
// run from the car to slightly beyond the eye (spread up/down and left/right in the last view basis);
// the eye is pulled in to the nearest blocked fraction. In heli mode a roof probe between eye and car
// lowers the eye smoothly while the view is squeezed under geometry.
void ChaseCamera::resolve_collision(const Frame& m, const Vec3& car, const RayCaster* rays, bool heli_mode, bool noisy, bool ceiling) {
    if (!rays) return;
    const Vec3 d = eye_ - car;
    float ref = heli_mode && !noisy ? 2.5f : 2.0f;
    const float push = noisy ? 0.1f : 0.5f;
    if (!heli_mode) ref = std::fabs(dot(cam_z_, m.forward)) * (ref - 1.1f) + 1.1f;

    float fraction = 1.0f;
    bool clear = true;
    auto probe = [&](const Vec3& to) {
        float t = 0;
        if (!rays->segment_hit(car, to, t)) return;
        const Vec3 hit = car + (to - car) * t;
        const float hit_dist = length(hit - car);
        if (hit_dist <= 1.1f) return;  // hits right at the car are ignored
        const float h = std::max(hit_dist, ref);
        fraction = std::min(fraction, h / length(to - car));
        if (h - ref < 1.5f) clear = false;
    };
    Vec3 to = car + d * 1.075f - cam_y_ * 0.5f;
    probe(to);
    to = to + cam_y_ * 1.0f - cam_x_ * 0.4f;
    probe(to);
    to = to + cam_x_ * 0.8f;
    probe(to);

    if (fraction < 1.0f) {
        eye_ = car + d * fraction;
        if (heli_mode) eye_ += cam_y_ * (push * (1.0f - fraction));
    }

    if (!(ceiling && clear)) return;
    const float len = length(eye_ - car);
    const Vec3 q0 = eye_ + m.forward * (len * 0.125f);
    const Vec3 q1 = eye_ + m.forward * (len * 0.975f);
    float t = 0;
    if (rays->segment_hit(q0, q1, t)) {
        ceiling_hit_ = q0 + (q1 - q0) * t;
        if (ceiling_count_ < kCeilingMax) ++ceiling_count_;
    }
    if (ceiling_count_ > 0) {
        if (inside_triangle(ceiling_hit_, eye_, q1, car)) {
            if (ceiling_count_ < kCeilingMax) ++ceiling_count_;
        } else {
            --ceiling_count_;
        }
        eye_ += cam_y_ * (static_cast<float>(ceiling_count_) * -kCeilingDrop);
    }
}

void ChaseCamera::update_heli(const CameraIni::Camera& cam, const Frame& m, const CameraTarget& t, const RayCaster* rays) {
    const CameraIni::Heli& h = cam.heli;
    const CameraIni::Arm& arm = h.arms[arm_];
    const float speed = horizontal_speed(t);

    // Desired arm (sideways, height, distance) in the car frame.
    Vec3 arm_vec{arm.sideways, arm.height, arm.distance};
    if (look_back_ && cam.look_back)
        arm_vec[2] = -arm_vec[2];  // view from the front
    else
        arm_vec[2] -= std::min(speed * h.fallback_factor, h.max_fallback);  // pulled back with speed

    // Desired eye offset from the anchor.
    Vec3 desired;
    const Vec3 world_up{0, 1, 0};
    const auto rigid_up = [&]() {  // the world up blended towards the car's up while it is pitched/rolled
        const float fy = std::fabs(m.forward[1]);
        const float lean = std::max(-m.up[1], 0.0f);
        const float blend = std::min(1.0f, fy + (1.0f - fy) * lean * (1.0f - std::fabs(m.right[1])));
        return normalized(approach(world_up, m.up, blend), world_up);
    };
    if (h.rigid_arm) {
        Frame basis = m;
        if (h.up_rate > 0.0f) {
            const Vec3 x = normalized(cross(rigid_up(), m.forward), m.right);
            basis = {x, cross(m.forward, x), m.forward};
        }
        desired = basis.right * arm_vec[0] + basis.up * arm_vec[1] + basis.forward * arm_vec[2];
    } else {
        // Vertigo: the (height, distance) pair rotates with the car's pitch.
        const float f = m.forward[1] < 0.0f ? h.max_vertigo_downhill : h.max_vertigo_uphill;
        pitch_ += (m.forward[1] * f - pitch_) * h.vertigo_lerp;
        const float c = std::sqrt(std::max(0.0f, 1.0f - pitch_ * pitch_));
        const float y = c * arm_vec[1] + pitch_ * arm_vec[2];
        const float z = c * arm_vec[2] - pitch_ * arm_vec[1];
        // Yaw only: the arm follows the car's heading on the ground plane.
        const float n = std::hypot(m.forward[0], m.forward[2]);
        if (n > 1e-6f) {
            heading_x_ = m.forward[0] / n;
            heading_z_ = m.forward[2] / n;
        }
        desired = {-heading_z_ * arm_vec[0] + heading_x_ * z, y, heading_x_ * arm_vec[0] + heading_z_ * z};
    }

    bool snap = false;
    if (flags_ != 0) {
        const bool from_dashboard = last_selectable_ != kNone && cams_[last_selectable_]->kind == Kind::Dashboard;
        if (smoothing_allowed() && !(flags_ & 2) && !from_dashboard) {
            // Blend from the previous camera's offset: distance interpolates linearly, the direction
            // follows the arm with a rate that starts at kTransRate and grows.
            update_anchor(m, t.position, arm.anchor.offset, true);
            transition_left_ = transition_request_ != 0 ? transition_request_ : arm.transition;
            transition_active_ = true;
            transition_first_ = true;
            rate_ = global_.trans_rate;
            dist_ = length(offset_);
        } else {
            update_anchor(m, t.position, arm.anchor.offset, false);
            offset_ = desired;
            dist_ = length(offset_);
            snap = true;
        }
    } else {
        update_anchor(m, t.position, arm.anchor.offset, true);
        const float previous_dist = dist_;
        dist_ = length(desired);
        const float rate = std::max(std::min(speed * h.speed_rate_diff, h.max_rate), h.min_rate);
        const bool from_dashboard = last_selectable_ != kNone && cams_[last_selectable_]->kind == Kind::Dashboard;
        if (transition_left_ <= 0 || from_dashboard) {
            rate_ = rate;
        } else if (transition_active_) {
            if (transition_first_) step_ = arm.transition > 0 ? (dist_ - previous_dist) / static_cast<float>(arm.transition) : 0.0f;
            dist_ -= step_ * static_cast<float>(transition_left_);
            transition_first_ = false;
            --transition_left_;
            rate_ += (rate - rate_) * global_.trans_rate_lerp_rate;
        }
        // Lag: the direction of the offset eases towards the desired one, its length is set at once.
        offset_ = normalized(offset_, desired) * dist_;
        offset_ = approach(offset_, desired, rate_);
    }
    eye_ = anchor_ + offset_;

    if (h.check_collisions) resolve_collision(m, t.position, rays, true, h.noise_amount > 0.0f, true);

    if (h.up_rate > 0.0f) up_ = normalized(approach(up_, rigid_up(), h.up_rate), world_up);
    finish_view(up_, 0.0f, h.look_up);
    step_fov(cam.default_fov, snap);
}

void ChaseCamera::update_bumper(const CameraIni::Camera& cam, const Frame& m, const CameraTarget& t) {
    const auto& arm = (look_back_ && cam.look_back) ? cam.bumper.backward : cam.bumper.forward;
    const Vec3 off = m.right * arm[0] + m.up * arm[1] + m.forward * arm[2];
    Vec3 eye = t.position + off;
    eye[1] += arm[3];  // panUp
    if (flags_ != 0) {
        eye_ = eye;
    } else {
        eye_[0] = eye[0];
        eye_[2] = eye[2];
        eye_[1] += (eye[1] - eye_[1]) * global_.bumper_y_lerp_rate;
    }
    anchor_ = eye_ + off;  // looks along the arm, so the back arm looks backwards
    finish_view(m.up, 0.0f, 0.0f);
    step_fov(cam.default_fov, true);
}

void ChaseCamera::update_dashboard(const CameraIni::Camera& cam, const Frame& m, const CameraTarget& t) {
    const CameraIni::Dashboard& d = cam.dashboard;
    const bool back = look_back_ && cam.look_back;
    if (back) {
        const Vec3 off = m.right * d.backward_arm[0] + m.up * d.backward_arm[1] + m.forward * d.backward_arm[2];
        const Vec3 eye = t.position + off;
        if (flags_ != 0) {
            eye_ = eye;
        } else {
            eye_[0] = eye[0];
            eye_[2] = eye[2];
            eye_[1] += (eye[1] - eye_[1]) * global_.bumper_y_lerp_rate;
        }
        anchor_ = eye_ + off;
        finish_view(m.up, 0.0f, 0.0f);
        step_fov(cam.default_fov, true);
        return;
    }

    Vec3 arm = d.forward_arm;
    if (flags_ == 0) {
        // Vertigo: the eye height follows the car's pitch a little.
        const float fy = m.forward[1];
        const float f = fy >= 0.0f ? -d.max_vertigo_uphill : d.max_vertigo_downhill;
        const float v = std::clamp(f * arm[2] * fy, -kDashboardVertigoMax, kDashboardVertigoMax);
        pitch_ += (v - pitch_) * d.vertigo_lerp;
        arm[1] += pitch_;
    }
    const Vec3 desired = m.right * arm[0] + m.up * arm[1] + m.forward * arm[2];
    if (flags_ != 0) {
        offset_ = desired;
    } else {
        // Inertia: the cockpit eye lags the body more the slower the car goes.
        rate_ = std::max(std::min(horizontal_speed(t) * d.inertia_scale, d.inertia_max), d.inertia_min);
        dist_ = length(desired);
        offset_ = approach(offset_, desired, rate_);
    }
    eye_ = t.position + offset_;
    anchor_ = eye_ + m.forward;

    float look_x = 0.0f;
    if (d.steer_scale != 0.0f) {  // glance into the corner
        const float wanted = std::clamp(t.steer_input * d.steer_scale, -d.steer_max, d.steer_max);
        glance_ += (wanted - glance_) * d.steer_pace;
        look_x = glance_;
    }
    finish_view(m.up, look_x, 0.0f);
    step_fov(cam.default_fov, true);
}

void ChaseCamera::update_tumble(const CameraIni::Camera& cam, const Frame& m, const CameraTarget& t, const RayCaster* rays) {
    const CameraIni::TumbleCam& tc = cam.tumble_cam;
    const CameraIni::Camera* from = prev_ != kNone ? cams_[prev_] : nullptr;
    Vec3 arm_vec = kTumbleFallbackArm;
    Vec3 anchor_offset = tc.anchor.offset;
    if (from && from->kind == Kind::Heli) {
        const CameraIni::Arm& arm = from->heli.arms[std::min(arm_, from->heli.arms.size() - 1)];
        arm_vec = Vec3{arm.sideways, arm.height, arm.distance} * from->heli.tumble_arm_scale;
        anchor_offset = arm.anchor.offset;
    }
    update_anchor(m, t.position, anchor_offset, true);
    dist_ = length(arm_vec);

    // View heading: the current view direction pulled towards the car's velocity.
    Vec3 dir = anchor_ - eye_;
    const Vec3 velocity = horizontal_speed(t) == 0.0f ? dir : t.velocity;
    dir = approach(dir, velocity, tc.vector_lerp);
    const float n = std::hypot(dir[0], dir[2]);
    if (n > 1e-6f) {
        heading_x_ = dir[0] / n;
        heading_z_ = dir[2] / n;
    }
    Vec3 desired{-heading_z_ * arm_vec[0] + heading_x_ * arm_vec[2], arm_vec[1], heading_x_ * arm_vec[0] + heading_z_ * arm_vec[2]};
    desired = normalized(desired, desired) * dist_;

    if (flags_ & 1) {
        if (smoothing_allowed()) {
            offset_ = eye_ - anchor_;
        } else {
            offset_ = eye_ + Vec3{0, kTumbleStartLift, 0} - anchor_;
        }
    }
    offset_ = approach(offset_, desired, tc.rel_pos_lerp);
    eye_ = anchor_ + offset_;
    resolve_collision(m, t.position, rays, false, false, false);
    --tumble_left_;
    finish_view({0, 1, 0}, 0.0f, 0.0f);
    step_fov(cam.default_fov, false);
}

CameraPose ChaseCamera::update(const CameraTarget& t, const RayCaster* rays) {
    const Frame m{t.right, t.up, t.forward};

    // Director (sub_1AA890): a tumbling car takes the camera to the tumble camera, which keeps it for
    // kMaxTumble ticks after the last tumbling tick and then returns to the previous camera.
    if (cams_[cur_]->tumble && t.tumbling && tumble_ != kNone) {
        tumble_left_ = global_.max_tumble;
        if (cur_ != tumble_) switch_to(tumble_, 0);
    }
    if (cams_[cur_]->kind == Kind::Tumble && (tumble_left_ <= 0 || (flags_ & 2)) && prev_ != kNone) switch_to(prev_, 0);

    const CameraIni::Camera& cam = *cams_[cur_];
    switch (cam.kind) {
        case Kind::Heli: update_heli(cam, m, t, rays); break;
        case Kind::Bumper: update_bumper(cam, m, t); break;
        case Kind::Dashboard: update_dashboard(cam, m, t); break;
        case Kind::Tumble: update_tumble(cam, m, t, rays); break;
        default: break;
    }
    flags_ = 0;
    return pose();
}

CameraPose ChaseCamera::pose() const {
    CameraPose p;
    p.eye = eye_;
    p.target = target_;
    p.up = cam_y_;
    // sub_1D4878: view angle = 2 * fov (degrees, horizontal), aspect 4/3; sub_295910 builds the projection
    // with x scale 1/tan(angle/2) and y scale aspect times that.
    p.aspect = 4.0f / 3.0f;
    p.fovx = 2.0f * fov_deg_ * kDegToRad;
    p.fovy = 2.0f * std::atan(std::tan(p.fovx * 0.5f) / p.aspect);
    return p;
}

}  // namespace nf::driving

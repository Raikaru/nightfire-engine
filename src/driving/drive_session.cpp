#include "driving/drive_session.hpp"

#include <cmath>

namespace nf::driving {

namespace {

// Part 0 of a vehicle model is the exterior shell. The Vanquish keeps its roof/glass panels and bonnet in
// separate parts (0x0D, 0x0E, 0x13); wheels are recognised by shape (see `is_wheel`).
constexpr int kBodyPart = 0x00;
constexpr int kVanquishExtraParts[] = {0x0D, 0x0E, 0x13};

// A wheel part is a small, roughly round mesh centred on the model origin (wheel-local space): extents
// about the wheel diameter in Y and Z, a tyre width in X.
bool is_wheel(const SceneMesh& m) {
    Vec3 size{}, centre{};
    for (int a = 0; a < 3; ++a) size[a] = m.max[a] - m.min[a], centre[a] = (m.max[a] + m.min[a]) / 2;
    return std::abs(centre[0]) < 0.1f && std::abs(centre[1]) < 0.1f && std::abs(centre[2]) < 0.1f && size[1] > 0.4f &&
           size[1] < 1.6f && std::abs(size[1] - size[2]) < 0.15f * size[1] && size[0] > 0.1f && size[0] < 0.7f;
}

Mat4 translation(const Vec3& t) { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, t[0], t[1], t[2], 1}; }

// Rotation about the Y axis (yaw) and X axis (roll of a wheel), column-major.
Mat4 rot_y(float a) {
    const float c = std::cos(a), s = std::sin(a);
    return {c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, 0, 0, 0, 1};
}
Mat4 rot_x(float a) {
    const float c = std::cos(a), s = std::sin(a);
    return {1, 0, 0, 0, 0, c, s, 0, 0, -s, c, 0, 0, 0, 0, 1};
}

}  // namespace

// Camera collision rays go through the track collision.
class DriveSession::Rays final : public RayCaster {
public:
    explicit Rays(const TrackCollision& c) : collision_(c) {}
    bool segment_hit(const Vec3& from, const Vec3& to, float& t) const override {
        SegmentHit h;
        if (!collision_.segment_hit(from, to, h)) return false;
        t = h.t;
        return true;
    }

private:
    const TrackCollision& collision_;
};

// Missions without a usable road network (boat and tank courses): start on the floor triangle closest to
// the centroid of all floor triangles.
void DriveSession::floor_start(Vec3& pos) const {
    Vec3 mean{0, 0, 0};
    std::size_t n = 0;
    for (const Tri& t : collision_.triangles())
        if (t.solid() && t.normal[1] > 0.95f) {
            mean += (t.v[0] + t.v[1] + t.v[2]) * (1.0f / 3.0f);
            ++n;
        }
    if (!n) throw FormatError("track has no floor triangles");
    mean = mean * (1.0f / float(n));
    float best = 1e30f;
    for (const Tri& t : collision_.triangles()) {
        if (!t.solid() || t.normal[1] <= 0.95f) continue;
        const Vec3 c = (t.v[0] + t.v[1] + t.v[2]) * (1.0f / 3.0f);
        const Vec3 d = c - mean;
        const float dist = dot(d, d);
        if (dist < best) best = dist, pos = c;
    }
}

// The first record of the road network (`rs` members of the RNgp group, sub_220C28): segment start (+0x00),
// end (+0x10). The mission start places the player on it, driving towards the end.
static bool road_start(const CarpFile& carp, Vec3& pos, float& yaw) {
    for (const CarpEntry& e : carp.entries())
        if (!e.is_head && e.tag == "rs" && e.index == 0 && e.size >= 0x24) {
            const Bytes b = carp.payload(e);
            pos = {load<float>(b, 0), load<float>(b, 4), load<float>(b, 8)};
            const Vec3 end{load<float>(b, 16), load<float>(b, 20), load<float>(b, 24)};
            yaw = std::atan2(end[0] - pos[0], end[2] - pos[2]);
            return true;
        }
    return false;
}

DriveSession::DriveSession(DrivingLevel& level, const std::string& car)
    : collision_(TrackCollision::load(level.carp())),
      rays_(std::make_unique<Rays>(collision_)),
      camera_ini_(CameraIni::parse(level.read_text("data\\render\\camera.ini"))) {
    // Vehicle model.
    const Attributes attrs = level.vehicle_attributes(car);
    const std::string render_name = attrs.get_string("render_filename", car + ".crp");
    std::string stem = render_name.substr(0, render_name.rfind('.'));
    const auto model = level.read_file("data\\car\\model\\" + stem + ".crp");
    const CarpFile carp(model);
    const ElfImage elf = carp.load_elf();
    car_shapes_ = parse_ssh(level.read_file("data\\car\\model\\" + stem + ".ssh"));
    auto parts = build_vehicle_parts(carp, elf);
    Vec3 lo{}, hi{};
    const bool vanquish = stem.rfind("vanquish", 0) == 0 && stem != "vanquishsub";
    int wheel_count = 0;
    for (VehiclePart& p : parts) {
        if (p.id == kBodyPart) {
            body_ = p.mesh;
            lo = p.mesh.min;
            hi = p.mesh.max;
        } else if (wheel_count < 4 && is_wheel(p.mesh)) {
            wheels_[wheel_count++] = std::move(p.mesh);
        }
    }
    if (body_.batches.empty()) throw FormatError("vehicle model " + stem + " has no body mesh");
    if (vanquish)
        for (const VehiclePart& p : parts)
            for (int id : kVanquishExtraParts)
                if (p.id == id) body_.batches.insert(body_.batches.end(), p.mesh.batches.begin(), p.mesh.batches.end());
    Vec3 half{};
    for (int a = 0; a < 3; ++a) body_center_[a] = (lo[a] + hi[a]) / 2, half[a] = (hi[a] - lo[a]) / 2;

    const PhysicsGlobals globals = PhysicsGlobals::load(
        Attributes::parse_flat(level.read_text("data\\tuning\\physics\\rigid\\default.tun")),
        Attributes::parse_flat(level.read_text("data\\tuning\\physics\\physical\\default.tun")));
    const VehicleParams params = VehicleParams::load(attrs);
    wheel_radius_ = params.wheel_radius;
    vehicle_ = std::make_unique<Vehicle>(params, globals, half);
    camera_ = std::make_unique<ChaseCamera>(camera_ini_, car);
    half_ = half;

    Vec3 start{0, 0, 0};
    float yaw = 0;
    GroundHit ground;
    if (!road_start(level.carp(), start, yaw) || !collision_.ground_below({start[0], start[1] + 3.0f, start[2]}, ground))
        floor_start(start);
    place_at_start(start, yaw);
}

DriveSession::~DriveSession() = default;

Mat4 DriveSession::body_matrix() const { return mul(vehicle_->model_matrix(), translation(body_center_ * -1.0f)); }

Mat4 DriveSession::wheel_matrix(int w) const {
    const WheelPose& p = vehicle_->wheels()[w];
    // Hub: the suspension attachment point (bottom of the body box), moved by the spring extension.
    const Vec3 attach = vehicle_->body().axes().to_local(p.position - vehicle_->body().position());
    const Vec3 hub{attach[0], attach[1] + wheel_radius_ - (vehicle_->params().spring_rest_length - p.compression), attach[2]};
    const Mat4 body_rot = vehicle_->model_matrix();   // body origin -> world (box centre)
    Mat4 m = mul(body_rot, translation(hub));
    if (w < 2) m = mul(m, rot_y(p.steer_angle));
    m = mul(m, rot_x(p.spin_phase * 6.2831853f));
    return m;
}

void DriveSession::tick(const PadState& pad) {
    pad_.push(pad);
    const PadActions actions = actions_from_pad(pad_);
    vehicle_->step(actions.drive, collision_);
    if (actions.change_camera) camera_->cycle_view();
    camera_->set_look_back(actions.look_back);
    camera_pose_ = camera_->update(vehicle_->camera_target(), rays_.get());
    ++ticks_;
}

void DriveSession::place_at_start(const Vec3& position, float yaw) {
    Vec3 p = position;
    GroundHit ground;
    if (collision_.ground_below({p[0], p[1] + 3.0f, p[2]}, ground)) p[1] = ground.point[1];
    p[1] += half_[1] + vehicle_->params().spring_rest_length + 0.2f;
    vehicle_->reset(p, yaw);
    camera_->reset(vehicle_->camera_target());
    camera_pose_ = camera_->update(vehicle_->camera_target(), rays_.get());
}

}  // namespace nf::driving

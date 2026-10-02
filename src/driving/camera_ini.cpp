#include "driving/camera_ini.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <unordered_map>

#include "assets/reader.hpp"
#include "driving/attributes.hpp"

namespace nf::driving {
namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

std::string lower(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

bool contains(std::string_view s, std::string_view needle) { return s.find(needle) != std::string_view::npos; }

// One `[section]` with its keys. Lookups are case-insensitive (the original compares with a folding
// strcmp, sub_2A9198) and the first definition of a key wins (sub_2D9AF0 scans from the start).
struct RawSection {
    std::string name;
    Attributes attributes;
};

std::vector<RawSection> read_sections(std::string_view text) {
    std::vector<RawSection> out;
    std::vector<AttributeSection> raw;
    std::vector<std::string> names;
    while (!text.empty()) {
        const auto eol = text.find('\n');
        std::string_view line = text.substr(0, eol);
        text = eol == std::string_view::npos ? std::string_view{} : text.substr(eol + 1);
        if (const auto c = line.find("//"); c != std::string_view::npos) line = line.substr(0, c);
        line = trim(line);
        if (line.empty()) continue;
        if (line.front() == '[') {
            if (line.back() != ']') throw FormatError("camera.ini: unterminated section header: " + std::string(line));
            names.emplace_back(trim(line.substr(1, line.size() - 2)));
            raw.push_back({lower(names.back()), {}});
        } else if (const auto eq = line.find('='); eq != std::string_view::npos && !raw.empty()) {
            raw.back().values.emplace(lower(trim(line.substr(0, eq))), std::string(trim(line.substr(eq + 1))));
        }
    }
    out.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) out.push_back({names[i], Attributes::from_section(raw[i])});
    return out;
}

const RawSection* find_section(const std::vector<RawSection>& sections, std::string_view name) {
    const std::string want = lower(name);
    for (const auto& s : sections)
        if (lower(s.name) == want) return &s;
    return nullptr;
}

CameraIni::Anchor read_anchor(const Attributes& a) {  // sub_1D69C8
    CameraIni::Anchor r;
    r.offset = {a.get_float("Anchor_X"), a.get_float("Anchor_Y"), a.get_float("Anchor_Z")};
    r.slide_dist = a.get_float("Anchor_Slide_Dist");
    r.slide_rate = a.get_float("Anchor_Slide_Rate");
    r.slide_rec_rate = a.get_float("Anchor_Slide_Rec_Rate");
    return r;
}

Vec3 read_vec(const Attributes& a, std::string_view base) {
    const std::string b(base);
    return {a.get_float(b + ".x"), a.get_float(b + ".y"), a.get_float(b + ".z")};
}

std::array<float, 4> read_arm4(const Attributes& a, std::string_view base) {
    const std::string b(base);
    return {a.get_float(b + ".x"), a.get_float(b + ".y"), a.get_float(b + ".z"), a.get_float(b + ".panUp")};
}

float clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }  // sub_1A49B0 with (0, 1)

// The loader's second pass tests the section name against these substrings in this order (sub_1A49E0).
std::optional<CameraIni::Kind> kind_of(std::string_view name) {
    using K = CameraIni::Kind;
    static constexpr std::pair<std::string_view, K> kTable[] = {
        {"Heli", K::Heli},         {"Spline", K::Spline},       {"Ellipse", K::Ellipse},
        {"Bumper", K::Bumper},     {"Dashboard", K::Dashboard}, {"Fixed", K::Fixed},
        {"Tumble", K::Tumble},     {"WorldAnim", K::WorldAnim}, {"RelativeAnim", K::RelativeAnim},
        {"AIPathAnim", K::AIPathAnim}, {"Collision", K::Collision}, {"AutoDrive", K::AutoDrive},
    };
    for (const auto& [sub, kind] : kTable)
        if (contains(name, sub)) return kind;
    return std::nullopt;
}

CameraIni::Global read_global(const Attributes& g) {
    CameraIni::Global r;
    r.max_tumble = g.get_int("kMaxTumble", r.max_tumble);
    r.max_collision = g.get_int("kMaxCollision", r.max_collision);
    r.default_transition = g.get_int("kDefualtTransition", r.default_transition);
    r.trans_rate = g.get_float("kTransRate", r.trans_rate);
    r.trans_rate_lerp_rate = g.get_float("kTransRateLerpRate", r.trans_rate_lerp_rate);
    r.bumper_y_lerp_rate = g.get_float("kBumperYLerpRate", r.bumper_y_lerp_rate);
    r.collide_radius = g.get_float("kCollideRadius", r.collide_radius);
    r.spline_offset_lerp = g.get_float("kSplineOffsetLerp", r.spline_offset_lerp);
    r.explosion_scale = g.get_float("kExplosionScale", r.explosion_scale);
    r.explosion_max_shake = g.get_float("kExplosionMaxShake", r.explosion_max_shake);
    r.explosion_shake_period = g.get_int("kExplosionShakePeriod", r.explosion_shake_period);
    r.explosion_after_shock = g.get_float("kExplosionAfterShock", r.explosion_after_shock);
    r.explosion_time_scale = g.get_float("kExplosionTimeScale", r.explosion_time_scale);
    r.camera_object_radius_ex = {g.get_float("kCameraObjectRadiusEx.x", r.camera_object_radius_ex[0]),
                                 g.get_float("kCameraObjectRadiusEx.y", r.camera_object_radius_ex[1]),
                                 g.get_float("kCameraObjectRadiusEx.z", r.camera_object_radius_ex[2])};
    r.camera_object_sphere_rad = g.get_float("kCameraObjectSphereRad", r.camera_object_sphere_rad);
    r.collision_min_rate = g.get_float("kCollisionMinRate", r.collision_min_rate);
    r.collision_max_rate = g.get_float("kCollisionMaxRate", r.collision_max_rate);
    r.collision_rate_diff = g.get_float("kCollisionRateDiff", r.collision_rate_diff);
    r.zoom_inc_speed = g.get_float("kZoomIncSpeed", r.zoom_inc_speed);
    r.centering_speed = g.get_float("kCenteringSpeed", r.centering_speed);
    r.auto_drive_latency = g.get_int("kAutoDriveLatency", r.auto_drive_latency);
    r.weapon_arm_change_latency = g.get_int("kWeaponArmChangeLatency", r.weapon_arm_change_latency);
    r.missile_cam_latency = g.get_int("kMissileCamLatency", r.missile_cam_latency);
    r.weapon_animation_length = g.get_float("kfWeaponAnimationLength", r.weapon_animation_length);
    r.weapon_animation_amplitude = g.get_float("kfWeaponAnimationAmplitude", r.weapon_animation_amplitude);
    r.max_autoaim_distance = g.get_float("kfMaxAutoaimDistance", r.max_autoaim_distance);
    return r;
}

CameraIni::Heli read_heli(const std::vector<RawSection>& sections, const RawSection& sec, int smooth_trans) {
    const Attributes& a = sec.attributes;
    CameraIni::Heli h;
    h.min_rate = a.get_float("Min_Rate");
    h.max_rate = a.get_float("Max_Rate");
    h.speed_rate_diff = a.get_float("Speed_Rate_Diff");
    h.height_factor = a.get_float("Height_Factor");
    h.fallback_factor = a.get_float("Fallback_Factor");
    h.max_fallback = a.get_float("Max_Fallback");
    h.vertigo_lerp = a.get_float("Vertigo_Lerp");
    h.tumble_arm_scale = a.get_float("Tumble_Arm_Scale");
    h.rigid_arm = a.get_int("rigidArm", 0) != 0;
    h.check_collisions = a.get_int("checkCollisions", 1) != 0;
    h.max_vertigo_downhill = clamp01(a.get_float("MaxVertigoDownhill"));
    h.max_vertigo_uphill = clamp01(a.get_float("MaxVertigoUphill"));
    h.noise_pace = a.get_float("noisePace");
    h.noise_amount = a.get_float("noiseAmount");
    h.noise_frequency = a.get_float("noiseFrequency");
    h.up_rate = a.get_float("upRate");
    h.look_up = a.get_float("lookUp");
    h.cinematic = a.get_int("Cinematic", 0) != 0;
    for (int i = 0; i < 32; ++i) {  // `%s:Arm%d` until the first missing one, at most 32
        const RawSection* arm = find_section(sections, sec.name + ":Arm" + std::to_string(i));
        if (!arm) break;
        const Attributes& aa = arm->attributes;
        CameraIni::Arm r;
        r.sideways = aa.get_float("Heli_Sideways");
        r.height = aa.get_float("Heli_Height");
        r.distance = aa.get_float("Heli_Distance");
        r.transition = aa.get_int("armTransition", smooth_trans);
        r.anchor = read_anchor(aa);
        r.weapons = aa.get_string("weapons");
        h.arms.push_back(std::move(r));
    }
    return h;
}

CameraIni::Bumper read_bumper(const Attributes& a) {
    CameraIni::Bumper b;
    b.forward = read_arm4(a, "forwardArm");
    b.backward = read_arm4(a, "backwardsArm");
    b.anchor = read_anchor(a);
    return b;
}

CameraIni::Dashboard read_dashboard(const Attributes& a) {
    CameraIni::Dashboard d;
    d.forward_arm = read_vec(a, "forwardArm");
    d.backward_arm = read_vec(a, "backwardsArm");
    d.force_scale = read_vec(a, "forceScale");
    d.force_max = read_vec(a, "forceMax");
    d.torque_scale = read_vec(a, "torqueScale");
    d.torque_max = read_vec(a, "torqueMax");
    d.force_pace = a.get_float("forcePace");
    d.torque_pace = a.get_float("torquePace");
    d.forward_pitch = a.get_float("forwardPitch");
    d.forward_yaw = a.get_float("forwardYaw");
    d.inertia_scale = a.get_float("intertiaScale");
    d.inertia_min = a.get_float("intertiaMin");
    d.inertia_max = a.get_float("intertiaMax");
    d.steer_scale = a.get_float("steerScale");
    d.steer_max = a.get_float("steerMax");
    d.steer_pace = a.get_float("steerPace");
    d.glance_scale = a.get_float("glanceScale");
    d.glance_max = a.get_float("glanceMax");
    d.glance_pace = a.get_float("glancePace");
    d.noise_amount = a.get_float("noiseAmount");
    d.noise_frequency = a.get_float("noiseFrequency");
    d.vertigo_lerp = a.get_float("Vertigo_Lerp");
    d.max_vertigo_downhill = clamp01(a.get_float("MaxVertigoDownhill"));
    d.max_vertigo_uphill = clamp01(a.get_float("MaxVertigoUphill"));
    d.anchor = read_anchor(a);
    return d;
}

// sub_1A6C68: `needle` must occur in `list` and be followed by a space, a comma or the end.
// The first occurrence decides (strstr), there is no check of the character before it.
bool car_token_matches(std::string_view list, std::string_view needle) {
    if (list.size() < needle.size()) return false;
    const auto pos = list.find(needle);
    if (pos == std::string_view::npos) return false;
    const std::size_t end = pos + needle.size();
    return end == list.size() || list[end] == ' ' || list[end] == ',';
}

}  // namespace

CameraIni CameraIni::parse(std::string_view text) {
    const std::vector<RawSection> sections = read_sections(text);
    CameraIni ini;
    if (const RawSection* g = find_section(sections, "Global")) ini.global = read_global(g->attributes);

    for (const RawSection& sec : sections) {
        // First loop of sub_1A49E0: these substrings exclude a section from being a camera.
        if (contains(sec.name, "Global") || contains(sec.name, "Debug") || contains(sec.name, ":Arm")) continue;
        const auto kind = kind_of(sec.name);
        if (!kind) continue;  // "Unknown camera type loaded" in the original, section ignored

        const Attributes& a = sec.attributes;
        Camera cam;
        cam.name = sec.name;
        cam.kind = *kind;
        cam.has_car = a.has("car");
        cam.car = a.get_string("car");
        cam.tumble = a.get_int("tumble", 0) != 0;
        cam.shake = a.get_int("shake", 0) != 0;
        cam.smooth_trans = static_cast<signed char>(a.get_int("smoothTrans", 0));
        cam.look_back = a.get_int("lookBack", 0) != 0;
        cam.selectable = a.get_int("selectable", 0) != 0;
        cam.lerp_rotation = a.get_int("lerpRotation", 0) != 0;
        cam.interior_view = a.get_int("interiorView", 0) != 0;
        cam.cam_id = static_cast<std::uint16_t>(a.get_int("camID", 0));
        cam.default_camera = a.get_int("defaultCamera", 0) != 0;
        cam.default_fov = a.get_float("defaultFov", 33.0f);                    // loader default: 33 degrees
        cam.explosion_shake_scale = a.get_float("explosionShakeScale", 1.0f);  // loader default: 1
        switch (cam.kind) {
            case Kind::Heli: cam.heli = read_heli(sections, sec, cam.smooth_trans); break;
            case Kind::Bumper: cam.bumper = read_bumper(a); break;
            case Kind::Dashboard: cam.dashboard = read_dashboard(a); break;
            case Kind::Tumble:
                cam.tumble_cam.rel_pos_lerp = a.get_float("relPosLerp");
                cam.tumble_cam.vector_lerp = a.get_float("vectorLerp");
                cam.tumble_cam.anchor = read_anchor(a);
                break;
            default: break;
        }
        ini.cameras.push_back(std::move(cam));
    }
    return ini;
}

std::vector<const CameraIni::Camera*> CameraIni::cameras_for_car(std::string_view car_name) const {
    const std::string name = lower(car_name);
    int heli = 0, any_heli = 0, dashboard = 0, any_dashboard = 0;
    bool autodrive = false;
    std::vector<const Camera*> out;
    for (const Camera& cam : cameras) {
        if (cam.has_car && !car_token_matches(lower(cam.car), name)) continue;
        const bool any_car = contains(cam.name, "AnyCar");
        switch (cam.kind) {
            case Kind::Heli:
                if (any_car) {
                    if (heli != any_heli) continue;
                    ++any_heli;
                }
                ++heli;
                break;
            case Kind::Dashboard:
                if (any_car) {
                    if (dashboard != any_dashboard) continue;
                    ++any_dashboard;
                }
                ++dashboard;
                break;
            case Kind::AutoDrive:
                if (autodrive) continue;
                autodrive = true;
                break;
            default: break;
        }
        out.push_back(&cam);
    }
    return out;
}

}  // namespace nf::driving

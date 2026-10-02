#include "game/objects.hpp"

#include <algorithm>
#include <cmath>

#include "assets/nav_data.hpp"
#include "game/damage.hpp"
#include "game/sp_common.hpp"  // SwitchChannels
#include "game/weapons.hpp"

namespace nf {

namespace {

// Param keys: Create functions read `level_tag+0x2c+4*key`, i.e. StaticInstance::param(key).
std::uint32_t uparam(const StaticInstance& s, std::int32_t key) { return s.param(key); }
std::uint16_t uparam16(const StaticInstance& s, std::int32_t key) {
    return std::uint16_t(s.param(key) & 0xFFFF);
}

float dist2(const std::array<float, 3>& a, const std::array<float, 3>& b) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz;
}

}  // namespace

SpObjects::SpObjects(Level& level, std::uint32_t level_id, sp::SwitchChannels& channels)
    : level_(level), level_id_(level_id), channels_(channels), trigger_volumes_(make_volumes(level)) {}

CollisionWorld SpObjects::make_volumes(Level& level) {
    // Volume classes for the touch tests below (model collision at the static transform).
    static constexpr std::uint32_t kVolumeClasses[] = {32,  40,  41,  46,  48,  49,  51,  219, 220, 225,
                                                       226, 228, 234, 235, 240, 249, 251, 254};
    return CollisionWorld::of_objects(level, std::span<const std::uint32_t>(kVolumeClasses, std::size(kVolumeClasses)));
}

const StaticInstance* SpObjects::statics(std::size_t placement) const {
    const MapChunk* map = level_.map() ? &level_.map()->chunk : nullptr;
    if (!map) return nullptr;
    const Placement& p = level_.placements()[placement];
    return p.instance < map->statics.size() ? &map->statics[p.instance] : nullptr;
}

std::array<float, 3> SpObjects::placement_pos(std::size_t placement) const {
    const Placement& p = level_.placements()[placement];
    return {p.transform[12], p.transform[13], p.transform[14]};
}

void SpObjects::build() {
    const MapChunk* map = level_.map() ? &level_.map()->chunk : nullptr;
    if (!map) return;
    for (std::size_t i = 0; i < level_.placements().size(); ++i) {
        const Placement& p = level_.placements()[i];
        if (p.instance >= map->statics.size()) continue;
        const StaticInstance& s = map->statics[p.instance];
        if ((s.flags & 0x8000) != 0) continue;  // world cel, not an object
        const std::uint32_t cls = s.object_class();
        switch (cls) {
        case 219: {  // Door_Create: p0 flags, p1 group, p2 unlock, p3 lock, p5/6/7 sounds
            DoorObject d;
            d.placement = i;
            d.flags = uparam16(s, 0);
            d.group = uparam16(s, 1);
            d.unlock_channel = uparam16(s, 2);
            d.lock_channel = uparam16(s, 3);
            d.open_sound = uparam16(s, 5);
            d.close_sound = uparam16(s, 6);
            d.locked_sound = uparam16(s, 7);
            if (d.open_sound == 0xFFFF) d.open_sound = 0;  // -1 = silent
            if (d.close_sound == 0xFFFF) d.close_sound = 0;
            if (d.locked_sound == 0xFFFF) d.locked_sound = 0;
            d.rate = 1.0f / 60.0f;  // full swing in ~1 s at 30 Hz [INFERENCE: param 4 is a header field]
            d.swing = ((uparam(s, 8) | uparam(s, 9)) >> 2 & 1) == 0;
            d.auto_door = (d.flags & 2) != 0;  // `obj+244 & 2`: proximity doors (data: p0 = 2)
            doors_.push_back(d);
            break;
        }
        case 220:
        case 234:
        case 235: {  // Trigger_Create / TouchOnce(→1) / Touch(→2)
            TriggerObject t;
            t.placement = i;
            t.type = cls == 234 ? 1 : 2;
            if (cls == 220) t.type = uparam16(s, 0);
            // Base triggers read out at key1; the Touch wrappers shuffle editor key0 there.
            t.out_channel = cls == 220 ? uparam(s, 1) : uparam(s, 0);
            for (int k = 0; k < 8; ++k) t.inputs[std::size_t(k)] = uparam16(s, 2 + k);
            t.gate_channel = uparam16(s, 10);
            t.extra = uparam16(s, 11);
            triggers_.push_back(t);
            break;
        }
        case 232: {  // Trigger_LoadLevelCreate (→ type 10)
            TriggerObject t;
            t.placement = i;
            t.type = 10;
            t.level_id = uparam(s, 0);  // editor's first param [INFERENCE: verify against .bins]
            t.out_channel = uparam(s, 1);
            for (int k = 0; k < 8; ++k) t.inputs[std::size_t(k)] = uparam16(s, 2 + k);
            t.gate_channel = uparam16(s, 10);
            triggers_.push_back(t);
            break;
        }
        case 244: {  // Trigger_MoviePlayer (→ type 13)
            TriggerObject t;
            t.placement = i;
            t.type = 13;
            t.script_hash = uparam(s, 0);  // [INFERENCE: verify against .bins]
            t.gate_channel = uparam16(s, 10);
            triggers_.push_back(t);
            break;
        }
        case 236:
        case 237:
        case 238:
        case 239: {  // Multiplex variants: out = param 0, inputs = params 1..
            TriggerObject t;
            t.placement = i;
            t.type = cls == 236 ? 3 : cls == 237 ? 4 : cls == 238 ? 5 : 12;
            t.out_channel = uparam(s, 0);
            for (int k = 0; k < 8; ++k) t.inputs[std::size_t(k)] = uparam16(s, 1 + k);
            multiplex_.push_back(t);
            break;
        }
        case 41: {  // Switch_Create: init channels[channel] = param 3
            Switch sw;
            sw.placement = i;
            sw.channel = uparam16(s, 0);
            sw.gate = uparam16(s, 5);
            if (sw.channel != 0) channels_.set(int(sw.channel), uparam16(s, 3) != 0);
            switches_.push_back(sw);
            break;
        }
        case 226: {  // SS_Create: out = p0, mask = p1, value = p3, sound = p4
            TriggerObject t;
            t.placement = i;
            t.type = 2;  // touch-driven (SS_Operate: touch bits & mask, or value channel)
            t.out_channel = uparam(s, 0);
            t.extra = uparam16(s, 1);  // class mask [INFERENCE: players always pass]
            t.value = uparam(s, 3);
            t.rate = float(uparam(s, 4));  // sound id (reuses the spline-rate slot: SS never roves)
            triggers_.push_back(t);
            break;
        }
        case 32: {  // Break_Create: hp = param 1, sound = param 2
            Breakable b;
            b.placement = i;
            b.hp = float(std::max<std::int32_t>(1, std::int32_t(uparam(s, 1))));
            b.sound = uparam(s, 2);
            b.center = placement_pos(i);
            breakables_.push_back(b);
            break;
        }
        case 225: {  // Destroy_Create: gate = param 1, out = param 0
            Breakable b;
            b.placement = i;
            b.hp = 1;  // smashes on the gate channel, not on damage
            b.gate = uparam16(s, 1);
            b.sound = uparam(s, 4);
            b.center = placement_pos(i);
            breakables_.push_back(b);
            break;
        }
        case 40: {  // Sensor_Create: alarm = param 3, gate = param 4
            Sensor se;
            se.placement = i;
            se.alarm_channel = uparam16(s, 3);
            se.gate_channel = uparam16(s, 4);
            se.range = float(std::max<std::int32_t>(1, std::int32_t(uparam(s, 2))));
            se.half_sin = std::sin(float(uparam(s, 1)) * 0.0087266f);
            sensors_.push_back(se);
            break;
        }
        case 222: {  // Searchlight_Create: alarm = p1 (long cone, sweep static v1 [INFERENCE])
            Sensor se;
            se.placement = i;
            se.alarm_channel = uparam16(s, 1);
            se.range = 60.0f;
            se.half_sin = 0.05f;
            sensors_.push_back(se);
            break;
        }
        case 224: {  // Copter / GunImp / Shooter / Creature: scripted shooters
            Turret t;
            t.placement = i;
            t.kind = cls;
            t.range = cls == 210 ? 40.0f : 30.0f;
            t.damage = cls == 47 ? 15.0f : 10.0f;
            t.period = 60.0f;
            t.timer = t.period;
            turrets_.push_back(t);
            break;
        }
        case 228: {  // Hurt volumes: damage = p0 per tick while inside (kill-planes: p0 = 6)
            Volume v;
            v.placement = i;
            v.kind = cls;
            v.damage = float(std::int32_t(uparam(s, 0)));
            if (!(v.damage > 0)) v.damage = 6.0f;
            v.radius = 2.0f;
            volumes_.push_back(v);
            break;
        }
        case 254: {  // Mine_Create: proximity explosive, sound = p5
            Volume v;
            v.placement = i;
            v.kind = cls;
            v.damage = 50.0f;
            v.radius = 2.5f;
            v.sound = uparam(s, 5);
            volumes_.push_back(v);
            break;
        }
        case 46:  // Lock_Create: use -> unlock channel p1 (gated by p0) [INFERENCE]
        case 48: {  // Monitor_Create: use -> channel p1 [INFERENCE]
            Switch sw;
            sw.placement = i;
            sw.gate = uparam16(s, 0);
            sw.out = uparam16(s, 1);
            switches_.push_back(sw);
            break;
        }
        case 49: {  // FuseBox_Create: spark script = p0, use powers p1 (no gate) [INFERENCE]
            Switch sw;
            sw.placement = i;
            sw.out = uparam16(s, 1);
            switches_.push_back(sw);
            break;
        }
        case 51: {  // Hint_Create: label = p0, sfx = p1, gate = p3
            Switch sw;
            sw.placement = i;
            sw.label = uparam16(s, 0);
            sw.sound = uparam16(s, 1);
            sw.gate = uparam16(s, 3);
            switches_.push_back(sw);
            break;
        }
        case 249: {  // SoundTrigger_Create: chA = p0, chB = p4, sfx = p2 (one-shot)
            Switch sw;
            sw.placement = i;
            sw.channel = uparam16(s, 0);
            sw.gate = uparam16(s, 4);
            sw.sound = uparam16(s, 2);
            switches_.push_back(sw);
            break;
        }
        case 251: {  // MusicTrigger_Create: gate = p1 (alt p0), event = p2
            Switch sw;
            sw.placement = i;
            sw.channel = uparam16(s, 1);
            sw.gate = uparam16(s, 0);
            sw.event = uparam(s, 2);
            switches_.push_back(sw);
            break;
        }
        case 231: {  // ThirdCam_Create: camera anchor, id = p0
            thirdcams_.push_back({i, uparam(s, 0)});
            break;
        }
        case 253: {  // CamSubject_Create: look-at anchor (recorded for the camera)
            thirdcams_.push_back({i, 0xFFFFFFFF});
            break;
        }
        case 217: {  // SP_CreateScriptPlayer: hash = p0, auto = (p2 == 1), trigger = p6
            ScriptPlayerAnchor sp;
            sp.placement = i;
            sp.hash = uparam(s, 0);
            sp.auto_play = uparam(s, 2) == 1;
            sp.trigger_channel = uparam16(s, 6);
            script_players_.push_back(sp);
            break;
        }
        case 113: {  // rotor_init: spinning rotor
            rotors_.push_back({i, 0});
            break;
        }
        case 218: {  // Car_Create: parked car, solid with zero displacement
            DoorObject d;
            d.placement = i;
            d.rate = 0;  // never moves (scripted cars are a Driving-slice concern)
            d.swing = true;
            doors_.push_back(d);
            break;
        }
        case 240: {  // Pickup_Create: kind = p0, id = p1, amount = p2, sound = p4, respawn = p6
            PickupObj pk;
            pk.placement = i;
            pk.kind = uparam16(s, 0);
            pk.arg0 = uparam(s, 1);
            pk.arg1 = uparam(s, 2);
            pk.sound = uparam(s, 4);
            if (pk.sound == 0xFFFF) pk.sound = 245;
            pk.respawn = uparam(s, 6);
            pickups_.push_back(pk);
            break;
        }
        default:
            statics_.push_back({i, cls});
            break;
        }
    }
    // Breakable bounds for the damage tests.
    // Door spline tracks (`static_path_refs` by static index; swing otherwise).
    const std::vector<PathTrack> tracks = parse_path_data(*map);
    for (const StaticPathRef& ref : static_path_refs(*map)) {
        if (ref.count == 0 || ref.path_index >= tracks.size()) continue;
        for (DoorObject& d : doors_) {
            const Placement& dp = level_.placements()[d.placement];
            if (dp.instance == ref.static_index && tracks[ref.path_index].keys.size() >= 2) {
                d.path = tracks[ref.path_index].keys;
                d.has_path = true;
            }
        }
    }
    // Cache world-space bounds per placement with collision (touch tests + movers run per tick).
    for (std::size_t i = 0; i < trigger_volumes_.solid_count(); ++i) {
        const auto solid = trigger_volumes_.solid(i);
        if (solid.collision.tris.empty()) continue;
        const bool fresh = bounds_.count(solid.placement) == 0;
        auto& slot = bounds_[solid.placement];
        bool init = fresh;
        for (const CollisionTri& tri : solid.collision.tris) {
            for (const Vec3& v : tri.v) {
                const Vec3 w = transform_point(solid.transform, v);
                if (init) {
                    slot.first = w;
                    slot.second = w;
                    init = false;
                } else {
                    for (int k = 0; k < 3; ++k) {
                        slot.first[std::size_t(k)] = std::min(slot.first[std::size_t(k)], w[std::size_t(k)]);
                        slot.second[std::size_t(k)] = std::max(slot.second[std::size_t(k)], w[std::size_t(k)]);
                    }
                }
            }
        }
    }
    for (Breakable& b : breakables_) {
        std::array<float, 3> mn{}, mx{};
        object_bounds(b.placement, mn, mx);
        b.center = {(mn[0] + mx[0]) * 0.5f, (mn[1] + mx[1]) * 0.5f, (mn[2] + mx[2]) * 0.5f};
        const float dx = mx[0] - mn[0], dy = mx[1] - mn[1], dz = mx[2] - mn[2];
        b.radius = 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz);
        if (!(b.radius > 0.01f)) b.radius = 1.0f;
    }
    // Model-space door bounds (rotated by the live pose each tick for correct swing AABBs).
    for (std::size_t i = 0; i < trigger_volumes_.solid_count(); ++i) {
        const auto solid = trigger_volumes_.solid(i);
        if (solid.collision.tris.empty()) continue;
        for (DoorObject& d : doors_) {
            if (d.placement != solid.placement) continue;
            for (const CollisionTri& tri : solid.collision.tris) {
                for (const Vec3& v : tri.v) {
                    if (!d.has_local) {
                        d.local_mn = v;
                        d.local_mx = v;
                        d.has_local = true;
                    } else {
                        for (int k = 0; k < 3; ++k) {
                            d.local_mn[std::size_t(k)] = std::min(d.local_mn[std::size_t(k)], v[std::size_t(k)]);
                            d.local_mx[std::size_t(k)] = std::max(d.local_mx[std::size_t(k)], v[std::size_t(k)]);
                        }
                    }
                }
            }
        }
    }
}
void SpObjects::object_bounds(std::size_t placement, std::array<float, 3>& mn,
                              std::array<float, 3>& mx) const {
    const auto it = bounds_.find(placement);
    if (it != bounds_.end()) {
        mn = it->second.first;
        mx = it->second.second;
        return;
    }
    mn = placement_pos(placement);  // no collision model: a 2 m box at the marker [INFERENCE]
    mx = mn;
    for (int k = 0; k < 3; ++k) {
        mn[std::size_t(k)] -= 1.0f;
        mx[std::size_t(k)] += 1.0f;
    }
}

bool SpObjects::touch_test(std::size_t placement, const Toucher& t) const {
    std::array<float, 3> mn{}, mx{};
    object_bounds(placement, mn, mx);
    // Horizontal closeness + vertical overlap (the capsule vs the volume's world AABB).
    const float cx = std::clamp(t.pos[0], mn[0], mx[0]);
    const float cz = std::clamp(t.pos[2], mn[2], mx[2]);
    const float dx = t.pos[0] - cx, dz = t.pos[2] - cz;
    if (dx * dx + dz * dz > (t.radius + 0.3f) * (t.radius + 0.3f)) return false;
    const float top = t.pos[1] + t.height, bottom = t.pos[1] - 0.5f;
    // Volumes are often flat floor plates: step tolerance in Y.
    return top + 0.3f >= mn[1] && bottom - 0.6f <= mx[1];
}

void SpObjects::door_pose(DoorObject& d, std::array<float, 16>& out) const {
    const StaticInstance* s = statics(d.placement);
    const Placement& p = level_.placements()[d.placement];
    if (d.swing || !d.has_path) {
        // `Door_SetupSwing`: yaw about the placement origin ([INFERENCE]: hinge approximated).
        const float ang = d.progress * 1.75f * d.direction;
        const float c = std::cos(ang), sn = std::sin(ang);
        Mat4 rot = identity();
        rot[0] = c;
        rot[2] = -sn;
        rot[8] = sn;
        rot[10] = c;
        std::array<float, 16> base{};
        if (s) base = instance_transform(*s);
        else base = p.transform;
        out = mul(base, rot);
        (void)p;
        return;
    }
    // Spline doors (`Door_Interp`): sample the static's path track by open progress.
    const float fi = std::clamp(d.progress, 0.0f, 1.0f) * float(d.path.size() - 1);
    const std::size_t i = std::min(d.path.size() - 2, std::size_t(fi));
    const float f = fi - float(i);
    const PathKey& a = d.path[i];
    const PathKey& b = d.path[i + 1];
    std::array<float, 3> pos;
    std::array<float, 4> q;
    for (int k = 0; k < 3; ++k) pos[std::size_t(k)] = a.pos[std::size_t(k)] + (b.pos[std::size_t(k)] - a.pos[std::size_t(k)]) * f;
    float dot = 0;
    for (int k = 0; k < 4; ++k) dot += a.quat[std::size_t(k)] * b.quat[std::size_t(k)];
    const float sgn = dot < 0 ? -1.0f : 1.0f;
    for (int k = 0; k < 4; ++k) q[std::size_t(k)] = a.quat[std::size_t(k)] * (1 - f) + sgn * b.quat[std::size_t(k)] * f;
    // Normalised lerp (keys are near-identity rotations; [INFERENCE] on quat order x,y,z,w).
    float len = 0;
    for (int k = 0; k < 4; ++k) len += q[std::size_t(k)] * q[std::size_t(k)];
    len = std::sqrt(std::max(1e-12f, len));
    for (int k = 0; k < 4; ++k) q[std::size_t(k)] /= len;
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    Mat4 m = identity();
    m[0] = 1 - 2 * (y * y + z * z);
    m[1] = 2 * (x * y + z * w);
    m[2] = 2 * (x * z - y * w);
    m[4] = 2 * (x * y - z * w);
    m[5] = 1 - 2 * (x * x + z * z);
    m[6] = 2 * (y * z + x * w);
    m[8] = 2 * (x * z + y * w);
    m[9] = 2 * (y * z - x * w);
    m[10] = 1 - 2 * (x * x + y * y);
    m[12] = pos[0];
    m[13] = pos[1];
    m[14] = pos[2];
    out = m;
}

bool SpObjects::use_door(DoorObject& d) {
    if (d.rate <= 0 || d.locked_shown) return false;
    const auto center = placement_pos(d.placement);
    if (d.lock_channel != 0 && channels_.on(int(d.lock_channel))) {
        texts_.push_back({0x02000003, 180, 1});
        if (d.locked_sound != 0) sounds_.push_back({d.locked_sound, center, true});
        d.locked_shown = true;
        return true;
    }
    d.locked_shown = false;
    d.opening = !d.opening;  // `Door_Activate` toggles
    return true;
}

bool SpObjects::use_switch(Switch& sw) {
    const StaticInstance* s = statics(sw.placement);
    const std::uint32_t cls = s ? s->object_class() : 0;
    if (cls == 41) {  // `Switch_Activate`: use toggles the channel (gated)
        if (sw.gate != 0 && channels_.on(int(sw.gate))) return false;
        channels_.set(int(sw.channel), !channels_.on(int(sw.channel)));
        return true;
    }
    if (cls == 46 || cls == 48 || cls == 49) {  // Lock/Monitor/FuseBox: use sets out
        if (sw.gate != 0 && !channels_.on(int(sw.gate))) return false;
        if (sw.out != 0) channels_.set(int(sw.out), true);
        return true;
    }
    return false;
}

bool SpObjects::activate_at(const Vec3& center, float radius) {
    // The probe tests against object volumes (`Collide_SphereIntersect` radius 1.0), not centers.
    const std::array<float, 3> c{center[0], center[1], center[2]};
    auto dist_to = [&](std::size_t placement) {
        std::array<float, 3> mn{}, mx{};
        object_bounds(placement, mn, mx);
        float d2 = 0;
        for (int k = 0; k < 3; ++k) {
            const float v = c[std::size_t(k)];
            const float lo = mn[std::size_t(k)] - radius, hi = mx[std::size_t(k)] + radius;
            if (v < lo) d2 += (lo - v) * (lo - v);
            else if (v > hi) d2 += (v - hi) * (v - hi);
        }
        return d2;
    };
    DoorObject* best_door = nullptr;
    float best_d = radius * radius;
    for (DoorObject& d : doors_) {
        const float dd = dist_to(d.placement);
        if (dd < best_d) {
            best_d = dd;
            best_door = &d;
        }
    }
    if (best_door) return use_door(*best_door);
    Switch* best_sw = nullptr;
    best_d = radius * radius;
    for (Switch& sw : switches_) {
        const StaticInstance* s = statics(sw.placement);
        const std::uint32_t cls = s ? s->object_class() : 0;
        if (cls != 41 && cls != 46 && cls != 48 && cls != 49) continue;
        const float dd = dist_to(sw.placement);
        if (dd < best_d) {
            best_d = dd;
            best_sw = &sw;
        }
    }
    if (best_sw) return use_switch(*best_sw);
    return false;
}
std::size_t SpObjects::door_count() const { return doors_.size(); }

bool SpObjects::any_door_open() const {
    for (const DoorObject& d : doors_)
        if (d.progress > 0.05f) return true;
    return false;
}

void SpObjects::break_near(const std::array<float, 3>& pos) {
    for (Breakable& b : breakables_) {
        if (b.broken) continue;
        if (dist2(b.center, pos) < (b.radius + 1.0f) * (b.radius + 1.0f)) {
            b.broken = true;
            if (b.sound != 0) sounds_.push_back({b.sound, b.center, true});
        }
    }
}

void SpObjects::set_link_byte(const std::array<float, 3>& pos, std::uint8_t value) {
    // Event 11 writes the linked door's lock byte: lock/unlock the nearest door [INFERENCE].
    DoorObject* best = nullptr;
    float best_d = 0;
    for (DoorObject& d : doors_) {
        const auto pp = placement_pos(d.placement);
        const float dd = dist2(pp, pos);
        if (!best || dd < best_d) {
            best = &d;
            best_d = dd;
        }
    }
    if (!best) return;
    if (value != 0) {
        best->locked_shown = true;  // freeze shut like `Door_Activate`'s denied path
        best->opening = false;
    } else {
        best->locked_shown = false;
    }
}

std::uint32_t SpObjects::take_load_level() {
    const std::uint32_t out = load_level_;
    load_level_ = 0;
    return out;
}

std::uint32_t SpObjects::take_movie() {
    const std::uint32_t out = movie_;
    movie_ = 0;
    return out;
}

std::vector<SoundRequest> SpObjects::take_sounds() {
    std::vector<SoundRequest> out;
    out.swap(sounds_);
    return out;
}

std::vector<std::uint32_t> SpObjects::take_music() {
    std::vector<std::uint32_t> out;
    out.swap(music_);
    return out;
}

std::vector<SpObjects::Text> SpObjects::take_texts() {
    std::vector<Text> out;
    out.swap(texts_);
    return out;
}

SpObjects::Census SpObjects::census() const {
    Census c;
    const MapChunk* map = level_.map() ? &level_.map()->chunk : nullptr;
    if (!map) {
        c.issues.push_back("no Map entry");
        return c;
    }
    for (std::size_t i = 0; i < level_.placements().size(); ++i) {
        const Placement& p = level_.placements()[i];
        if (p.instance >= map->statics.size()) continue;
        const StaticInstance& s = map->statics[p.instance];
        if ((s.flags & 0x8000) != 0) continue;
        const std::uint32_t cls = s.object_class();
        if (cls < 256) c.per_class[cls]++;
    }
    return c;
}


void SpObjects::tick(const std::vector<Toucher>& touchers, const std::vector<bool>& use_pressed,
                     const WeaponEvents& weapon_events, float dt_frames, WeaponSystem* weapons,
                     const HurtSink& hurt) {
    movers_.clear();
    draws_.clear();
    hides_.clear();
    ++tick_;
    auto rising = [&](int ch) { return ch > 0 && ch < 256 && channels_.on(ch) && !prev_[std::size_t(ch)]; };
    auto touching = [&](std::size_t placement, bool players_only) {
        for (const Toucher& t : touchers) {
            if (players_only && !t.is_player) continue;
            if (touch_test(placement, t)) return true;
        }
        return false;
    };
    auto use_near = [&](std::size_t placement, float radius) {
        const auto pp = placement_pos(placement);
        for (const Toucher& t : touchers) {
            if (!t.is_player || t.slot < 0 || std::size_t(t.slot) >= use_pressed.size()) continue;
            if (!use_pressed[std::size_t(t.slot)]) continue;
            if (dist2(pp, t.pos) < radius * radius) return true;
        }
        return false;
    };
    // ---- doors (`Door_Update`) ----
    for (DoorObject& d : doors_) {
        const auto center = placement_pos(d.placement);
        // Proximity scan (`Door_Update` sphere test): anyone within ~3 m of the panel.
        bool near = false;
        for (const Toucher& t : touchers) {
            if (dist2(center, t.pos) < (3.0f + t.radius) * (3.0f + t.radius)) {
                near = true;
                break;
            }
        }
        if (d.rate > 0) {
            if (d.locked_shown) {  // frozen by the denied path; the unlock channel releases it
                if (d.unlock_channel != 0 && channels_.on(int(d.unlock_channel))) {
                    d.locked_shown = false;
                    d.opening = true;
                }
            } else if (d.lock_channel != 0 && rising(int(d.lock_channel))) {
                // Lock edge: "locked" message + denied freeze (`Door_Update` LABEL_34).
                texts_.push_back({0x02000003, 180, 1});
                if (d.locked_sound != 0) sounds_.push_back({d.locked_sound, center, true});
                d.locked_shown = true;
                d.opening = false;
            } else {
                if (d.unlock_channel != 0 && channels_.on(int(d.unlock_channel))) {
                    d.locked_shown = false;
                    d.opening = true;
                } else if (d.auto_door && near) {
                    d.locked_shown = false;
                    d.opening = true;
                }
                if (use_near(d.placement, 2.5f)) use_door(d);
            }
            const bool was_open = d.progress > 0.02f;
            d.progress = std::clamp(d.progress + (d.opening ? d.rate * dt_frames : -d.rate * dt_frames), 0.0f, 1.0f);
            const bool is_open = d.progress > 0.02f;
            if (is_open && !was_open && d.open_sound != 0) sounds_.push_back({d.open_sound, center, true});
            if (!is_open && was_open && d.close_sound != 0) sounds_.push_back({d.close_sound, center, true});
            if (is_open && !near) {
                d.idle_frames += 1.0f;  // auto-close after ~6 s clear (`Door_Update` 6*FRAME_RATE_INT)
                if (d.auto_door && d.idle_frames > 180.0f) d.opening = false;
            } else {
                d.idle_frames = 0;
            }
            // `Door_SetState` reports the open state back on the unlock channel.
            if (d.unlock_channel != 0) channels_.set(int(d.unlock_channel), is_open);
        }
        // Publish the solid: the model-space box posed by the live matrix (correct for both
        // sliders and swung panels); falls back to the shifted closed box without local bounds.
        std::array<float, 16> pose{};
        door_pose(d, pose);
        Mat4 pm;
        for (int k = 0; k < 16; ++k) pm[std::size_t(k)] = pose[std::size_t(k)];
        Mover m;
        if (d.has_local) {
            bool init = false;
            for (int cx = 0; cx < 8; ++cx) {
                const Vec3 corner{(cx & 1) != 0 ? d.local_mx[0] : d.local_mn[0],
                                  (cx & 2) != 0 ? d.local_mx[1] : d.local_mn[1],
                                  (cx & 4) != 0 ? d.local_mx[2] : d.local_mn[2]};
                const Vec3 w = transform_point(pm, corner);
                if (!init) {
                    m.min = w;
                    m.max = w;
                    init = true;
                } else {
                    for (int k = 0; k < 3; ++k) {
                        m.min[std::size_t(k)] = std::min(m.min[std::size_t(k)], w[std::size_t(k)]);
                        m.max[std::size_t(k)] = std::max(m.max[std::size_t(k)], w[std::size_t(k)]);
                    }
                }
            }
        } else {
            std::array<float, 3> mn{}, mx{};
            object_bounds(d.placement, mn, mx);
            m.min = mn;
            m.max = mx;
        }
        m.id = std::uint32_t(d.placement);
        if (d.has_last) m.displacement = {m.max[0] - d.last_max[0], m.max[1] - d.last_max[1], m.max[2] - d.last_max[2]};
        d.last_max = m.max;
        d.has_last = true;
        movers_.push_back(m);
        DrawOverride dr;
        dr.placement = d.placement;
        dr.transform = pose;
        draws_.push_back(dr);
        hides_.push_back(d.placement);
    }
    // ---- touch triggers ----
    for (TriggerObject& t : triggers_) {
        if (t.gate_channel != 0 && !channels_.on(int(t.gate_channel))) continue;
        const bool touch = touching(t.placement, true);
        const bool was = t.touched;
        t.touched = touch;
        switch (t.type) {
        case 1:  // TouchOnce: latch once on the rising edge
            if (touch && !t.once_fired) {
                t.once_fired = true;
                if (t.out_channel != 0) channels_.set(int(t.out_channel), true);
            }
            break;
        case 2: {  // Touch / SS: latch while touched (SS also ORs its value channel)
            const bool ss = t.value != 0 || t.rate != 0;
            bool out = touch;
            if (ss && t.value != 0 && channels_.on(int(t.value))) out = true;
            if (out && t.out_channel != 0) channels_.set(int(t.out_channel), true);
            if (ss && out && !was && t.rate != 0) {
                const auto pp = placement_pos(t.placement);
                sounds_.push_back({std::uint32_t(t.rate), pp, true});
            }
            break;
        }
        case 10: {  // LoadLevel: full gate logic (`Trigger_Activate`), then exit
            if (t.cooldown > 0) {
                t.cooldown--;
                break;
            }
            if (!touch) break;
            const std::uint16_t blocker = t.inputs[1];  // editor param 3 [INFERENCE]
            if (blocker != 0 && !channels_.on(int(blocker))) {
                texts_.push_back({0x0200021, 180, 1});  // "you can't leave yet"
                t.cooldown = 90;
                break;
            }
            for (int k : {0, 2}) {  // completion channels (editor params 2/4) [INFERENCE]
                const std::uint16_t ch = t.inputs[std::size_t(k)];
                if (ch != 0) channels_.set(int(ch), true);
            }
            load_level_ = t.level_id;
            break;
        }
        case 13:  // MoviePlayer: touch or gate edge plays the script
            if ((touch && !was) || rising(int(t.gate_channel))) {
                if (t.script_hash != 0) movie_ = t.script_hash;
            }
            break;
        default:
            break;
        }
    }
    // ---- multiplex logic (no volumes) ----
    for (TriggerObject& m : multiplex_) {
        switch (m.type) {
        case 3: {  // AND
            bool any = false, all = true;
            for (std::uint16_t ch : m.inputs) {
                if (ch == 0) continue;
                any = true;
                if (!channels_.on(int(ch))) all = false;
            }
            if (any && m.out_channel != 0) channels_.set(int(m.out_channel), all);
            break;
        }
        case 12: {  // OR
            bool any = false;
            for (std::uint16_t ch : m.inputs) {
                if (ch != 0 && channels_.on(int(ch))) any = true;
            }
            if (m.out_channel != 0) channels_.set(int(m.out_channel), any);
            break;
        }
        case 5:  // fan-out: inputs follow the output
            for (std::uint16_t ch : m.inputs) {
                if (ch == 0) continue;
                channels_.set(int(ch), m.out_channel != 0 && channels_.on(int(m.out_channel)));
            }
            break;
        case 4: {  // sequence: inputs in order inside a 4-tick window
            for (int k = 0; k < 8; ++k) {
                const std::uint16_t ch = m.inputs[std::size_t(k)];
                if (ch == 0) continue;
                if (rising(int(ch))) {
                    if (k == m.seq_pos && float(tick_) - m.seq_tick <= 4.0f) {
                        m.seq_pos++;
                        m.seq_tick = float(tick_);
                    } else if (k == 0) {
                        m.seq_pos = 1;
                        m.seq_tick = float(tick_);
                    }
                }
            }
            int need = 0;
            for (std::uint16_t ch : m.inputs)
                if (ch != 0) need++;
            if (need > 0 && m.seq_pos >= need && m.out_channel != 0) channels_.set(int(m.out_channel), true);
            break;
        }
        default:
            break;
        }
    }
    tick_switches(touchers, use_pressed);
    tick_damage(touchers, weapon_events, dt_frames, weapons, hurt);
}
void SpObjects::tick_switches(const std::vector<Toucher>& touchers, const std::vector<bool>& use_pressed) {
    auto use_near = [&](std::size_t placement, float radius) {
        const auto pp = placement_pos(placement);
        for (const Toucher& t : touchers) {
            if (!t.is_player || t.slot < 0 || std::size_t(t.slot) >= use_pressed.size()) continue;
            if (!use_pressed[std::size_t(t.slot)]) continue;
            if (dist2(pp, t.pos) < radius * radius) return true;
        }
        return false;
    };
    for (Switch& sw : switches_) {
        const StaticInstance* s = statics(sw.placement);
        const std::uint32_t cls = s ? s->object_class() : 0;
        if (cls == 41) {  // Switch_Activate: use toggles the channel (gated)
            if (!use_near(sw.placement, 2.0f)) continue;
            use_switch(sw);
            continue;
        }
        if (cls == 46 || cls == 48 || cls == 49) {  // Lock/Monitor/FuseBox: use sets out
            if (!use_near(sw.placement, 2.0f)) continue;
            use_switch(sw);
            continue;
        }
        if (cls == 51) {  // Hint: gate edge shows the text once
            if (sw.gate != 0 && channels_.on(int(sw.gate)) && !sw.fired) {
                sw.fired = true;
                texts_.push_back({sw.label, 180, 1});
                if (sw.sound != 0 && sw.sound != 0xFFFF) {
                    const auto pp = placement_pos(sw.placement);
                    sounds_.push_back({sw.sound, pp, false});
                }
            }
            if (sw.gate != 0 && !channels_.on(int(sw.gate))) sw.fired = false;
            continue;
        }
        if (cls == 249) {  // SoundTrigger: rising edge on either channel plays once
            const bool a = sw.channel != 0 && channels_.on(int(sw.channel)) && !prev_[sw.channel];
            const bool b = sw.gate != 0 && channels_.on(int(sw.gate)) && !prev_[sw.gate];
            if ((a || b) && !sw.fired && sw.sound != 0 && sw.sound != 0xFFFF) {
                sw.fired = true;
                const auto pp = placement_pos(sw.placement);
                sounds_.push_back({sw.sound, pp, true});
            }
            if (!channels_.on(int(sw.channel)) && !channels_.on(int(sw.gate))) sw.fired = false;
            continue;
        }
        if (cls == 251) {  // MusicTrigger: held gate holds Music_Event(event, 2)
            const bool active = (sw.channel != 0 && channels_.on(int(sw.channel))) ||
                                (sw.gate != 0 && channels_.on(int(sw.gate)));
            if (active && sw.event != 0 && sw.event != 0xFFFF) music_.push_back(sw.event);
            continue;
        }
    }
}

void SpObjects::tick_damage(const std::vector<Toucher>& touchers, const WeaponEvents& weapon_events,
                            float dt_frames, WeaponSystem* weapons, const HurtSink& hurt) {
    // Breakables from bullets and blasts (`Break_Kill` / `Destroy_Smash` essence).
    for (Breakable& b : breakables_) {
        if (b.broken) continue;
        if (b.gate != 0) {  // Destroy: smash when the gate channel is set
            if (channels_.on(int(b.gate))) {
                b.broken = true;
                if (b.sound != 0 && b.sound != 0xFFFF) sounds_.push_back({b.sound, b.center, true});
            }
            continue;
        }
        for (const ImpactEvent& im : weapon_events.impacts) {
            if (dist2(im.point, b.center) < (b.radius + 0.5f) * (b.radius + 0.5f)) b.hp -= 25.0f;
        }
        for (const ExplosionEvent& ex : weapon_events.explosions) {
            if (dist2(ex.position, b.center) < (b.radius + ex.radius) * (b.radius + ex.radius)) b.hp -= 100.0f;
        }
        if (b.hp <= 0) {
            b.broken = true;
            if (b.sound != 0 && b.sound != 0xFFFF) sounds_.push_back({b.sound, b.center, true});
        }
    }
    for (Breakable& b : breakables_) {
        if (b.broken) hides_.push_back(b.placement);
    }
    // Sensors and searchlights: cone trip raises the alarm channel.
    for (Sensor& se : sensors_) {
        if (se.gate_channel != 0 && !channels_.on(int(se.gate_channel))) {
            if (se.tripped && se.alarm_channel != 0) channels_.set(int(se.alarm_channel), false);
            se.tripped = false;
            continue;
        }
        const Placement& p = level_.placements()[se.placement];
        std::array<float, 3> dir{p.transform[8], p.transform[9], p.transform[10]};
        const float dl = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
        if (dl > 1e-6f) {
            dir[0] /= dl;
            dir[1] /= dl;
            dir[2] /= dl;
        }
        const auto origin = placement_pos(se.placement);
        bool trip = false;
        for (const Toucher& t : touchers) {
            if (!t.is_player) continue;
            std::array<float, 3> to{t.pos[0] - origin[0], t.pos[1] + 0.5f - origin[1], t.pos[2] - origin[2]};
            const float dist = std::sqrt(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]);
            if (dist > se.range || dist < 1e-6f) continue;
            const float dot = (to[0] * dir[0] + to[1] * dir[1] + to[2] * dir[2]) / dist;
            if (dot >= std::sqrt(std::max(0.0f, 1.0f - se.half_sin * se.half_sin))) {
                trip = true;
                break;
            }
        }
        if (trip && !se.tripped) sounds_.push_back({1194, origin, true});  // trip beep
        if (!trip && se.tripped) sounds_.push_back({1245, origin, true});  // clear
        se.tripped = trip;
        if (se.alarm_channel != 0) channels_.set(int(se.alarm_channel), trip);
    }
    // Scripted shooters: range check, then hurt the player on a timer (no LOS yet [INFERENCE]).
    for (Turret& t : turrets_) {
        t.timer -= 1.0f;
        if (t.timer > 0) continue;
        const auto pp = placement_pos(t.placement);
        const Toucher* best = nullptr;
        float best_d = t.range * t.range;
        for (const Toucher& c : touchers) {
            if (!c.is_player || c.slot < 0) continue;
            const float dd = dist2(pp, c.pos);
            if (dd < best_d) {
                best_d = dd;
                best = &c;
            }
        }
        if (best && hurt) {
            hurt(best->slot, t.damage, DamageType::Bullet, pp);
            t.timer = t.period;
        } else {
            t.timer = 15.0f;  // re-scan soon when nobody is near
        }
    }
    // Hurt volumes and mines.
    for (Volume& v : volumes_) {
        if (v.kind == 254 && v.spent) continue;
        if (v.gate_channel != 0 && !channels_.on(int(v.gate_channel))) continue;
        const auto pp = placement_pos(v.placement);
        for (const Toucher& t : touchers) {
            if (!t.is_player || t.slot < 0) continue;
            if (dist2(pp, t.pos) > v.radius * v.radius) continue;
            if (!hurt) continue;
            if (v.kind == 254) {  // mine: one blast, then gone
                v.spent = true;
                hides_.push_back(v.placement);
                hurt(t.slot, v.damage, DamageType::Bullet, pp);
                if (v.sound != 0 && v.sound != 0xFFFF) sounds_.push_back({v.sound, pp, true});
            } else {  // hurt volume: damage per tick while inside
                hurt(t.slot, v.damage * 0.5f * dt_frames, DamageType::HurtVolume, pp);
            }
        }
    }
    // Pickups: walk-over grants with respawn.
    for (PickupObj& pk : pickups_) {
        if (pk.taken) {
            if (pk.respawn != 0) {
                pk.respawn_in -= 1.0f;
                if (pk.respawn_in <= 0) pk.taken = false;
                else hides_.push_back(pk.placement);
            } else {
                hides_.push_back(pk.placement);
            }
            continue;
        }
        if (!weapons) continue;
        const auto pp = placement_pos(pk.placement);
        bool granted = false;
        for (const Toucher& t : touchers) {
            if (!t.is_player || t.slot < 0) continue;
            if (dist2(pp, t.pos) > 1.5f * 1.5f) continue;
            const int id = int(pk.arg0), rounds = int(pk.arg1);
            if (pk.kind == 1) granted = weapons->give_weapon(t.slot, id, rounds);
            else if (pk.kind == 3) granted = weapons->give_ammo(t.slot, id, rounds);
            else if (pk.kind == 6) granted = weapons->give_weapon(t.slot, id, 0);
            else granted = weapons->give_weapon(t.slot, id, rounds) || weapons->give_ammo(t.slot, id, rounds);
            if (granted) {
                if (pk.sound != 0 && pk.sound != 0xFFFF) sounds_.push_back({pk.sound, pp, false});
                pk.taken = true;
                pk.respawn_in = float(pk.respawn);
            }
            break;
        }
    }
    // Rotors spin for the renderer.
    for (Rotor& r : rotors_) {
        r.angle += 0.1f * dt_frames;
        const Placement& p = level_.placements()[r.placement];
        const float c = std::cos(r.angle), s = std::sin(r.angle);
        Mat4 rot = identity();
        rot[0] = c;
        rot[2] = -s;
        rot[8] = s;
        rot[10] = c;
        DrawOverride dr;
        dr.placement = r.placement;
        dr.transform = mul(p.transform, rot);
        draws_.push_back(dr);
        hides_.push_back(r.placement);
    }
    // Snapshot channels for next tick's rising edges.
    for (int i = 0; i < 256; ++i) prev_[std::size_t(i)] = channels_.on(i) ? 1 : 0;
}

}  // namespace nf

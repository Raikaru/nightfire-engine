#include "game/script_player.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace nf {

namespace {

std::uint32_t payload_u32(const std::vector<std::uint8_t>& p, std::size_t o) {
    return std::uint32_t(p[o]) | (std::uint32_t(p[o + 1]) << 8) | (std::uint32_t(p[o + 2]) << 16) |
           (std::uint32_t(p[o + 3]) << 24);
}
std::uint16_t payload_u16(const std::vector<std::uint8_t>& p, std::size_t o) {
    return std::uint16_t(p[o] | (p[o + 1] << 8));
}
float payload_f32(const std::vector<std::uint8_t>& p, std::size_t o) {
    const std::uint32_t v = payload_u32(p, o);
    float f = 0;
    std::memcpy(&f, &v, 4);
    return f;
}

}  // namespace

CutscenePlayer::CutscenePlayer(const CutsceneBin* bin, std::uint32_t id, Host* host)
    : bin_(bin), id_(id), host_(host), end_time_(float(bin->d)) {
    for (const CutsceneScript& s : bin->scripts) {
        Stream st;
        st.cmds = decode_stream(s);
        streams_.push_back(std::move(st));
    }
}

void CutscenePlayer::play(bool fast) {
    (void)fast;  // `Script_FFwd` timescale: NIS skip is owned by the mission system (flag 0x80 paths)
    for (Stream& s : streams_) {
        s.cursor = 0;
        s.time = 0;
        s.next = 0;
        s.done = false;
        s.has_entity = false;
        s.has_camera = false;
        s.camera_live = false;
    }
    time_ = 0;
    playing_ = true;
}

void CutscenePlayer::stop() {
    playing_ = false;
    host_->set_scriptcam(0);
}

std::size_t CutscenePlayer::key_count() const { return bin_->keys.size() / 48; }

std::array<float, 3> CutscenePlayer::key_pos(std::size_t i) const {
    std::array<float, 3> out{};
    const std::size_t o = i * 48;
    for (int k = 0; k < 3; ++k) {
        std::uint32_t v = load<std::uint32_t>(bin_->keys, o + std::size_t(4 * k));
        std::memcpy(&out[std::size_t(k)], &v, 4);
    }
    return out;
}

std::array<float, 4> CutscenePlayer::key_quat(std::size_t i) const {
    std::array<float, 4> out{0, 0, 0, 1};
    const std::size_t o = i * 48 + 16;
    for (int k = 0; k < 4; ++k) {
        std::uint32_t v = load<std::uint32_t>(bin_->keys, o + std::size_t(4 * k));
        std::memcpy(&out[std::size_t(k)], &v, 4);
    }
    return out;
}

float CutscenePlayer::key_time(std::size_t i) const {
    std::uint32_t v = load<std::uint32_t>(bin_->keys, i * 48 + 44);
    float f = 0;
    std::memcpy(&f, &v, 4);
    return f;
}

void CutscenePlayer::interpolate(Stream& s) {
    // `Script_Interp` case 1 (entity) / camera: sample the key track at the stream time.
    const std::size_t n = key_count();
    if (n == 0) return;
    std::size_t begin = 0, end = n - 1;
    if (s.has_camera && s.key_begin <= s.key_end && s.key_end < n) {
        begin = s.key_begin;
        end = s.key_end;
    }
    if (begin >= end) {
        if (s.has_camera) {
            const auto p = key_pos(begin);
            const auto q = key_quat(begin);
            s.eye = p;
            s.forward = {2 * (q[0] * q[2] + q[3] * q[1]), 2 * (q[1] * q[2] - q[3] * q[0]),
                         1 - 2 * (q[0] * q[0] + q[1] * q[1])};
        }
        return;
    }
    const float t = std::clamp(s.time, key_time(begin), key_time(end));
    std::size_t i = begin;
    while (i + 1 < end && key_time(i + 1) < t) ++i;
    const float t0 = key_time(i), t1 = key_time(i + 1);
    float f = (t1 > t0) ? (t - t0) / (t1 - t0) : 0;
    f = std::clamp(f, 0.0f, 1.0f);
    const auto pa = key_pos(i), pb = key_pos(i + 1);
    auto qa = key_quat(i), qb = key_quat(i + 1);
    // Slerp with the short path (`Quat_Slerp_Acc`).
    float dot = qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3];
    if (dot < 0) {
        dot = -dot;
        qb = {-qb[0], -qb[1], -qb[2], -qb[3]};
    }
    float a = 1 - f, b = f;
    if (dot < 0.9995f) {
        const float th = std::acos(std::clamp(dot, -1.0f, 1.0f));
        const float s2 = std::sin(th);
        if (s2 > 1e-6f) {
            a = std::sin((1 - f) * th) / s2;
            b = std::sin(f * th) / s2;
        }
    }
    std::array<float, 3> p;
    std::array<float, 4> q;
    if (s.spline && i > begin && i + 2 <= end) {
        // Catmull-Rom through the neighbours ([INFERENCE]: `Script_CalculateSpline` not reversed).
        const auto p0 = key_pos(i - 1), p3 = key_pos(i + 2);
        for (int k = 0; k < 3; ++k)
            p[std::size_t(k)] = 0.5f * ((2 * pa[std::size_t(k)]) + (-p0[std::size_t(k)] + pb[std::size_t(k)]) * f +
                                        (2 * p0[std::size_t(k)] - 5 * pa[std::size_t(k)] + 4 * pb[std::size_t(k)] -
                                         p3[std::size_t(k)]) * f * f +
                                        (-p0[std::size_t(k)] + 3 * pa[std::size_t(k)] - 3 * pb[std::size_t(k)] +
                                         p3[std::size_t(k)]) * f * f * f);
    } else {
        for (int k = 0; k < 3; ++k) p[std::size_t(k)] = pa[std::size_t(k)] + (pb[std::size_t(k)] - pa[std::size_t(k)]) * f;
    }
    for (int k = 0; k < 4; ++k) q[std::size_t(k)] = qa[std::size_t(k)] * a + qb[std::size_t(k)] * b;
    if (s.has_camera) {
        s.eye = p;
        // Forward = quat applied to +Z (the placement convention: forward = (sin yaw, 0, cos yaw)).
        s.forward = {2 * (q[0] * q[2] + q[3] * q[1]), 2 * (q[1] * q[2] - q[3] * q[0]),
                     1 - 2 * (q[0] * q[0] + q[1] * q[1])};
        const std::uint32_t raw = load<std::uint32_t>(bin_->keys, i * 48 + 32);
        float fov = 60;
        std::memcpy(&fov, &raw, 4);
        if (fov > 10 && fov < 150) s.fov = fov;  // [INFERENCE]: +32 reads as the horizontal fov
    }
}

void CutscenePlayer::fire(const ScriptCommand& cmd, Stream& s) {
    const auto& p = cmd.payload;
    switch (CutsceneOp(cmd.op)) {
    case CutsceneOp::EntityStart: {
        // dword entity hash + b0 mode + b1 obj ref + b2 spline + b3 kind + b4 extra-dword count.
        s.has_entity = true;
        s.entity_hash = payload_u32(p, 0);
        s.obj_ref = p[2] != 0 ? int(p[3]) : -1;
        s.spline = p[4] != 0;
        s.entity_start = s.time;
        break;
    }
    case CutsceneOp::EntityEnd:
        s.has_entity = false;
        s.obj_ref = -1;
        break;
    case CutsceneOp::CameraStart: {
        // u8 mode + u16 key start + u16 key end (`ScriptCam` = this script while live).
        s.has_camera = true;
        s.camera_live = true;
        s.key_begin = payload_u16(p, 1);
        s.key_end = payload_u16(p, 3);
        host_->set_scriptcam(id_);
        host_->music(11, std::int32_t(id_));  // kNisStarted
        break;
    }
    case CutsceneOp::CameraEnd:
        s.camera_live = false;
        host_->set_scriptcam(0);
        host_->music(12, std::int32_t(id_));  // kNisEnded
        break;
    case CutsceneOp::TextStart:
        host_->text(payload_u32(p, 0), payload_u16(p, 4));
        messages_.push_back({payload_u32(p, 0), payload_u16(p, 4)});
        break;
    case CutsceneOp::SoundStart: {
        const std::uint32_t id = payload_u32(p, 0);
        const bool two_d = p[1] == 1;
        std::array<float, 3> at{};
        bool positional = false;
        if (!two_d && p[2] != 0) {
            // 3D at the addressed entity when it is up.
            for (const Stream& o : streams_) {
                if (o.has_entity && o.obj_ref == int(p[2])) {
                    at = key_pos(0);  // entity origin; poses refine in entities()
                    positional = true;
                    break;
                }
            }
        }
        host_->sound(id, at, positional);
        sounds_.push_back({id, at, positional});
        break;
    }
    case CutsceneOp::FadeStart: {
        float seconds = payload_f32(p, 0);
        host_->fade(seconds);
        break;
    }
    case CutsceneOp::Event: {
        const std::uint8_t ev = p[1];
        const std::size_t nargs = (p.size() - 3) / 4;
        auto arg = [&](std::size_t k) { return nargs > k ? payload_u32(p, 2 + 4 * k) : 0; };
        // The v14 object = this stream's entity position (0 when none).
        std::array<float, 3> at{};
        if (s.has_entity && key_count() > 0) at = key_pos(0);
        switch (ev) {
        case 3:  // loop/restart bookkeeping: nothing to present
            break;
        case 4:
            host_->disable_player(true);
            break;
        case 5:
            host_->enable_drones(false);  // `Drone_EnableAll(0, ...)`
            break;
        case 6:  // save anchor matrix: camera holds
            break;
        case 7:  // flag 0x80 (NIS skip arm): nothing to present
            break;
        case 8: {  // `Drone_CoderCreate` at the object: spawn a drone with 4 args
            const std::uint32_t args[4] = {arg(0), arg(1), arg(2), arg(3)};
            host_->spawn_drone(at, args);
            break;
        }
        case 9:
            host_->camera_mode(arg(0));  // `Player_Cam2Mode`
            break;
        case 10:
            host_->break_near(at);  // `Break_DoBreak`
            break;
        case 11:
            host_->set_link_byte(at, std::uint8_t(arg(0) & 0xFF));
            break;
        case 12:  // flag 0x10: nothing to present
        case 14:  // stream flag: nothing to present
        case 16:  // flag 0x20: nothing to present
            break;
        case 13:
            if (arg(0) != 0) host_->set_channel(std::uint16_t(arg(0) & 0xFFFF), std::uint8_t(arg(1) & 0xFF));
            break;
        case 15:
            host_->message_callback(std::uint16_t(arg(0) & 0xFFFF), arg(1));
            break;
        case 17: {
            const std::uint32_t ch = arg(0);
            const std::uint32_t src = host_->channel(std::uint16_t(ch & 0xFFFF)) ? arg(1) : arg(2);
            if (src != 0) host_->set_channel(std::uint16_t(src & 0xFFFF), std::uint8_t(arg(3) & 0xFF));
            break;
        }
        case 18:
            host_->ram_save();
            host_->load_level(arg(0));  // `ResetMap_LevelToLoad(arg0, 1, 0)`
            break;
        case 19:
            host_->set_scriptcam(id_);
            break;
        default:
            break;
        }
        break;
    }
    case CutsceneOp::AnimStart:
    case CutsceneOp::AnimEnd:
    case CutsceneOp::LightStart: {
        // `DynamicLights::create` at the entity position (Characters-2's map).
        Light l;
        l.intensity = p[1];
        l.type = p[2] == 1;
        if (key_count() > 0) {
            const float span = std::max(1.0f, end_time_ - s.entity_start);
            const float f = std::clamp((s.time - s.entity_start) / span, 0.0f, 1.0f);
            l.pos = key_pos(std::min(key_count() - 1, std::size_t(f * float(key_count() - 1))));
        }
        lights_.push_back(l);
        break;
    }
    case CutsceneOp::LightEnd:
    case CutsceneOp::SpriteStart:
    case CutsceneOp::SpriteEnd:
    case CutsceneOp::SoundEnd:
    case CutsceneOp::SubScriptStart:
    case CutsceneOp::SubScriptEnd:
    case CutsceneOp::StreamEnd:
    case CutsceneOp::Wait:
    case CutsceneOp::CondSkip:
        break;  // cosmetics / nestings owned by the mission system or out of scope (see below)
    }
}

void CutscenePlayer::tick(float frames) {
    if (!playing_) return;
    time_ += frames;
    for (Stream& s : streams_) {
        if (s.done) continue;
        s.time += frames;
        // Fire every due command (`Script_Run`: while time >= next).
        while (s.cursor < s.cmds.size()) {
            const ScriptCommand& cmd = s.cmds[s.cursor];
            if (cmd.op == 4) {  // StreamEnd: wait for its word, then the pass ends
                const float word = float(cmd.payload[0] | (cmd.payload[1] << 8));
                s.next = word;
                if (s.time > word) {
                    s.done = true;  // no loop-back in the travelled data; hold the last frame
                }
                break;
            }
            if (cmd.time > s.time) {
                s.next = cmd.time;
                break;
            }
            if (cmd.op == 5) {  // Wait moves `next`
                s.next = float(cmd.payload[0] | (cmd.payload[1] << 8));
                ++s.cursor;
                continue;
            }
            fire(cmd, s);
            ++s.cursor;
        }
        if (!s.done) interpolate(s);
    }
    if (time_ >= end_time_ + 60) stop();  // hold one extra second, then release the camera
}

std::optional<CutscenePlayer::Camera> CutscenePlayer::camera() const {
    if (!playing_) return std::nullopt;
    for (const Stream& s : streams_) {
        if (s.has_camera && s.camera_live && !s.done) {
            Camera c;
            c.eye = s.eye;
            c.forward = s.forward;
            c.fov = s.fov;
            return c;
        }
    }
    return std::nullopt;
}

std::vector<CutscenePlayer::EntityPose> CutscenePlayer::entities() const {
    std::vector<EntityPose> out;
    if (!playing_) return out;
    for (const Stream& s : streams_) {
        if (!s.has_entity || s.done || key_count() == 0) continue;
        EntityPose e;
        e.hash = s.entity_hash;
        // Whole-track mapping over the entity's active window [start, script end].
        const float span = std::max(1.0f, end_time_ - s.entity_start);
        const float f = std::clamp((s.time - s.entity_start) / span, 0.0f, 1.0f);
        const float fi = f * float(key_count() - 1);
        const std::size_t i = std::min(key_count() - 1, std::size_t(fi));
        e.pos = key_pos(i);
        e.quat = key_quat(i);
        out.push_back(e);
    }
    return out;
}

std::vector<CutscenePlayer::Sound> CutscenePlayer::take_sounds() {
    std::vector<Sound> out;
    out.swap(sounds_);
    return out;
}

std::vector<CutscenePlayer::Message> CutscenePlayer::take_messages() {
    std::vector<Message> out;
    out.swap(messages_);
    return out;
}

std::vector<CutscenePlayer::Light> CutscenePlayer::take_lights() {
    std::vector<Light> out;
    out.swap(lights_);
    return out;
}

std::vector<CutscenePlayer::Music> CutscenePlayer::take_music() {
    std::vector<Music> out;
    out.swap(music_);
    return out;
}

}  // namespace nf

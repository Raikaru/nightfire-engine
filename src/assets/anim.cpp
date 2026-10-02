#include "assets/anim.hpp"

#include <algorithm>
#include <cmath>

namespace nf {

namespace {

constexpr std::size_t kSkeletonHeader = 0x10;
constexpr std::size_t kSkinBoneBytes = 28;   // vec3 translation + quaternion
constexpr std::size_t kDatumBytes = 36;
constexpr std::size_t kSeqHeader = 0x40;
constexpr std::size_t kScriptHeader = 8;
constexpr std::size_t kMaxBones = 96;        // three u32 words of translation mask fit before the offsets

std::size_t align4(std::size_t v) { return (v + 3) & ~std::size_t(3); }

Vec3 load_vec3(Bytes d, std::size_t o) { return {load<float>(d, o), load<float>(d, o + 4), load<float>(d, o + 8)}; }
Quat load_quat(Bytes d, std::size_t o) {
    return {load<float>(d, o), load<float>(d, o + 4), load<float>(d, o + 8), load<float>(d, o + 12)};
}

Mat4 translation(const Vec3& t) {
    Mat4 m = identity();
    m[12] = t[0];
    m[13] = t[1];
    m[14] = t[2];
    return m;
}

}  // namespace

Mat4 quat_trans_to_mat(const Quat& q, const Vec3& t) {
    const float x2 = q.x + q.x, y2 = q.y + q.y, z2 = q.z + q.z;
    const float xx = x2 * q.x, xy = x2 * q.y, xz = x2 * q.z, xw = x2 * q.w;
    const float yy = y2 * q.y, yz = y2 * q.z, yw = y2 * q.w;
    const float zz = z2 * q.z, zw = z2 * q.w;
    return {1 - (yy + zz), xy + zw,       xz - yw,       0,  //
            xy - zw,       1 - (xx + zz), yz + xw,       0,  //
            xz + yw,       yz - xw,       1 - (xx + yy), 0,  //
            t[0],          t[1],          t[2],          1};
}

Quat slerp(const Quat& a, const Quat& b, float t) {
    float cosine = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    const float sign = cosine < 0 ? -1.0f : 1.0f;
    cosine *= sign;
    float wa = 1 - t, wb = t * sign;
    if (cosine < 0.9995f) {
        const float angle = std::acos(cosine), inv_sin = 1.0f / std::sin(angle);
        wa = std::sin((1 - t) * angle) * inv_sin;
        wb = std::sin(t * angle) * inv_sin * sign;
    }
    Quat r{wa * a.x + wb * b.x, wa * a.y + wb * b.y, wa * a.z + wb * b.z, wa * a.w + wb * b.w};
    const float len = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w);
    return {r.x / len, r.y / len, r.z / len, r.w / len};
}

const Datum* SkinDef::find_datum(std::int32_t id) const {
    for (const auto& d : datums)
        if (d.id == id) return &d;
    return nullptr;
}

float AnimChannel::evaluate(int frame) const {
    const float t = float(frame - 1);
    const int index = std::max(frame - 1, 0);
    const AnimSegment* seg = &segments.back();
    for (const auto& s : segments) {
        if (index <= s.end_frame) {
            seg = &s;
            break;
        }
    }
    // Powers as the VU0 code builds them; the sum runs c0 .. c5.
    const float t2 = t * t, t3 = t2 * t, t4 = t3 * t, t5 = t4 * t;
    const auto& c = seg->coeff;
    return c[0] + c[1] * t + c[2] * t2 + c[3] * t3 + c[4] * t4 + c[5] * t5;
}

std::vector<std::uint32_t> AnimScript::sequences() const {
    std::vector<std::uint32_t> out;
    for (const auto& c : cmds)
        if (c.op == 0) out.push_back(0x04000000u | c.words[2]);
    return out;
}

Skeleton parse_skeleton(Bytes d) {
    Skeleton s;
    s.id = load<std::uint16_t>(d, 0);
    s.bone_count = load<std::uint8_t>(d, 2);
    s.extra = load<std::uint8_t>(d, 3);
    if (s.bone_count > kMaxBones) throw FormatError("skeleton has more bones than its 96-bit translation mask covers");
    slice(d, kSkeletonHeader, std::size_t(s.bone_count) * 12);
    for (std::size_t i = 0; i < s.bone_count; ++i) {
        s.translation_animated.push_back((load<std::uint32_t>(d, 4 + (i / 32) * 4) >> (i % 32) & 1) != 0);
        s.offset.push_back(load_vec3(d, kSkeletonHeader + i * 12));
    }
    return s;
}

bool rig_compatible(const Skeleton& seq_skel, const Skeleton& skin_skel) {
    return seq_skel.bone_count == skin_skel.bone_count &&
           seq_skel.translation_animated == skin_skel.translation_animated;
}

SkinDef parse_skin(Bytes d, const Skeleton& skeleton) {
    SkinDef s;
    s.hash = load<std::uint32_t>(d, 0);
    s.scale = load_vec3(d, 4);
    const auto skinned_count = load<std::uint8_t>(d, 16), part_count = load<std::uint8_t>(d, 17);
    s.facial_count = load<std::uint8_t>(d, 18);
    const auto datum_count = load<std::uint8_t>(d, 19);
    s.skeleton = load<std::uint8_t>(d, 20);
    if (s.skeleton != skeleton.id) throw FormatError("skin names a different skeleton than the one supplied");

    const std::size_t bones = skeleton.bone_count;
    std::size_t p = 21;
    for (std::size_t i = 0; i < bones; ++i) {
        auto flag = load<std::uint8_t>(d, p + i);
        if ((flag & 0x80) && (flag & 0x7F) != 0x7F && (flag & 0x7F) >= i)
            throw FormatError("skin bone " + std::to_string(i) + " has a parent that is not before it");
        s.parent.push_back(flag);
    }
    p += bones;

    auto mesh_hashes = [&](std::size_t count, bool sleeves, std::vector<MeshRef>& out) {
        p = align4(p);
        for (std::size_t i = 0; i < count; ++i) {
            const auto hash = load<std::uint32_t>(d, p + i * 4);
            out.push_back({hash, sleeves && hash != 0xFFFFFFFFu && (hash & 0x100000) != 0});
        }
        p += count * 4;
    };
    if (part_count) {
        std::vector<std::uint8_t> part_bone;
        for (std::size_t i = 0; i < part_count; ++i) part_bone.push_back(load<std::uint8_t>(d, p + i));
        p += part_count;
        mesh_hashes(part_count, false, s.parts);
        for (std::size_t i = 0; i < part_count; ++i) s.parts[i].bone = part_bone[i];
    }
    if (skinned_count) {
        mesh_hashes(skinned_count, true, s.skinned);
        for (std::size_t i = 0; i < bones; ++i) {
            s.inverse_bind_translation.push_back(load_vec3(d, p));
            p += kSkinBoneBytes;
        }
    }
    for (std::size_t i = 0; i < datum_count; ++i, p += kDatumBytes)
        s.datums.push_back({load<std::int32_t>(d, p), load<std::int32_t>(d, p + 4), load_vec3(d, p + 8), load_quat(d, p + 0x14)});
    if (d.size() < p) throw FormatError("skin records overrun the file");
    return s;
}

AnimSeq parse_anim_seq(Bytes d) {
    AnimSeq s;
    s.hash = load<std::uint32_t>(d, 0);
    s.flags = load<std::uint8_t>(d, 12) & 0xF;
    for (std::size_t i = 0; i < 4; ++i) s.root_a[i] = load<float>(d, 0x10 + i * 4);
    for (std::size_t i = 0; i < 3; ++i) s.root_b[i] = load<float>(d, 0x20 + i * 4);
    for (std::size_t i = 0; i < 3; ++i) s.root_c[i] = load<float>(d, 0x2C + i * 4);
    s.frame_count = load<std::uint16_t>(d, 0x38);
    s.channel_count = load<std::uint16_t>(d, 0x3A);
    s.tag_3a = load<std::uint8_t>(d, 0x3A);
    s.skeleton = load<std::uint8_t>(d, 0x3C);
    s.tag_3e = load<std::uint8_t>(d, 0x3E);
    if (s.frame_count == 0 || s.frame_count > 0x7FFF) throw FormatError("animation has no frames");
    const int last = int(s.frame_count) - 1;

    std::size_t p = kSeqHeader;
    for (std::size_t c = 0; c < s.channel_count; ++c) {
        // Channel: u32 (length in bytes | first entry << 16), then entries packed as u16 pairs, each
        // entry (frames << 6 | coefficient mask) followed by its 4-byte coefficients after the pair.
        const auto header = load<std::uint32_t>(d, p);
        const std::size_t length = header & 0xFFFF;
        slice(d, p, std::max<std::size_t>(length, 4));
        std::size_t q = p + 4;
        std::uint32_t half = header >> 16;
        bool have_half = true;
        int cumulative = 0;
        AnimChannel channel;
        while (true) {
            std::uint32_t entry;
            if (have_half) {
                entry = half;
                have_half = false;
            } else {
                const auto word = load<std::uint32_t>(d, q);
                q += 4;
                entry = word & 0xFFFF;
                half = word >> 16;
                have_half = true;
            }
            cumulative += int(entry >> 6);
            AnimSegment seg{std::uint16_t(std::min(cumulative, 0xFFFF)), {}};
            for (unsigned bit = 0; bit < 6; ++bit) {
                if (!(entry >> bit & 1)) continue;
                seg.coeff[bit] = load<float>(d, q);
                q += 4;
            }
            channel.segments.push_back(seg);
            if (cumulative >= last) break;
            if (q > p + length) throw FormatError("animation channel overruns its length");
        }
        if (q > p + length) throw FormatError("animation channel overruns its length");
        s.channels.push_back(std::move(channel));
        p += length;
    }
    return s;
}

AnimScript parse_anim_script(Bytes d) {
    AnimScript s;
    s.hash = load<std::uint32_t>(d, 0);
    s.length = load<std::uint16_t>(d, 4);
    const auto word6 = load<std::uint16_t>(d, 6);
    const std::size_t count = word6 & 0x1FF;
    s.flags = word6 >> 9;
    std::size_t p = kScriptHeader;
    for (std::size_t i = 0; i < count; ++i) {
        ScriptCmd c;
        c.op = load<std::uint8_t>(d, p);
        c.extra = load<std::uint8_t>(d, p + 1);
        std::size_t words = c.extra;
        if (c.op == 0) words += 3;
        else if (c.op <= 4) words += 2;
        for (std::size_t w = 0; w < words; ++w) c.words.push_back(load<std::uint16_t>(d, p + 2 + w * 2));
        p += 2 + words * 2;
        s.cmds.push_back(std::move(c));
    }
    return s;
}

std::uint16_t anim_sound_id(std::uint16_t id, int weapon) {
    if (id == 1 && (weapon == 7 || weapon == 9)) return 2;
    if (id == 0x403 && (weapon == 3 || weapon == 5)) return 2;
    if (id == 0xFA && weapon == 0x14) return 0x1FB;
    if (id == 0x106 && weapon >= 0x1E && weapon < 0x24) return 0x284;
    if (id == 0x1E1 && weapon == 0x10) return 0x602;
    if (id == 4 && (weapon == 0x16 || weapon == 0x17)) return 0x604;
    return id;
}

Pose rest_pose(const Skeleton& skeleton, const SkinDef* skin) {
    Pose pose;
    const Vec3 scale = skin ? skin->scale : Vec3{1, 1, 1};
    for (std::size_t i = 0; i < skeleton.bone_count; ++i) {
        const Vec3& o = skeleton.offset[i];
        pose.translation.push_back({o[0] * scale[0], o[1] * scale[1], o[2] * scale[2]});
        pose.rotation.push_back({});
    }
    return pose;
}

Pose sample_seq(const AnimSeq& seq, const Skeleton& skeleton, const SkinDef* skin, int frame) {
    frame = std::clamp(frame, 1, int(seq.frame_count));
    Pose pose;
    if (seq.facial()) {
        for (std::size_t c = 0; c < seq.tag_3a && c < seq.channels.size(); ++c)
            pose.facial.push_back(seq.channels[c].evaluate(frame));
        return pose;
    }
    const Vec3 scale = skin ? skin->scale : Vec3{1, 1, 1};
    std::size_t ch = 0;
    auto value = [&](bool active) {
        if (ch >= seq.channels.size()) throw FormatError("animation has fewer channels than its skeleton needs");
        return active ? seq.channels[ch++].evaluate(frame) : (++ch, 0.0f);
    };
    for (std::size_t bone = 0; bone < skeleton.bone_count; ++bone) {
        const bool active = !skin || skin->bone_active(bone);
        Vec3 t{0, 0, 0};
        if (skeleton.translation_animated[bone]) {
            for (int k = 0; k < 3; ++k) t[k] = value(active) * scale[k];
        } else if (active) {
            for (int k = 0; k < 3; ++k) t[k] = skeleton.offset[bone][k] * scale[k];
        }
        // Rotation channels: x, y, and z with the sign of w folded in (z + 4 when w < 0).
        Quat q;
        if (active) {
            q.x = value(true);
            q.y = value(true);
            const float zc = value(true);
            const bool negative = zc >= 2.0f;
            q.z = negative ? zc - 4.0f : zc;
            const float w2 = 1.0f - (q.x * q.x + q.y * q.y + q.z * q.z);
            q.w = w2 > 0 ? std::sqrt(w2) : 0.0f;
            if (negative) q.w = -q.w;
        } else {
            if ((ch += 3) > seq.channels.size()) throw FormatError("animation has fewer channels than its skeleton needs");
        }
        pose.translation.push_back(t);
        pose.rotation.push_back(q);
    }
    // AnimFrameCopy walks exactly the skeleton's channels; a few sequences (skeletons 26 and 33) carry 30
    // further channels after them that nothing reads.
    return pose;
}

Pose blend_poses(const Pose& a, const Pose& b, float t) {
    Pose out;
    for (std::size_t i = 0; i < a.translation.size(); ++i) {
        out.translation.push_back(a.translation[i] + (b.translation.at(i) - a.translation[i]) * t);
        out.rotation.push_back(slerp(a.rotation[i], b.rotation.at(i), t));
    }
    for (std::size_t i = 0; i < a.facial.size(); ++i) out.facial.push_back(a.facial[i] + (b.facial.at(i) - a.facial[i]) * t);
    return out;
}

Pose sample_seq(const AnimSeq& seq, const Skeleton& skeleton, const SkinDef* skin, float frame) {
    const float base = std::floor(frame);
    const int f0 = int(base), f1 = std::min(f0 + 1, int(seq.frame_count));
    const float t = frame - base;
    if (t <= 0.0002f || f1 == f0) return sample_seq(seq, skeleton, skin, f0);
    return blend_poses(sample_seq(seq, skeleton, skin, f0), sample_seq(seq, skeleton, skin, f1), t);
}

DistanceTable make_distance_table(const AnimSeq& seq, const Skeleton& skeleton) {
    DistanceTable t;
    const int n = seq.frame_count;
    t.cumulative.push_back(0.0f);
    float total = 0;
    for (int step = 1; step <= n; ++step) {
        const int to = step + 1 <= n ? step + 1 : 2;
        const float dz = sample_seq(seq, skeleton, nullptr, to).translation.at(0)[2] -
                         sample_seq(seq, skeleton, nullptr, to - 1).translation.at(0)[2];
        float d = std::fabs(dz);
        if (d < 1e-5f) d = 0.01f;
        total += d;
        t.cumulative.push_back(total);
    }
    return t;
}

float DistanceTable::frame_at(float distance) const {
    const float total_distance = total();
    const int steps = int(cumulative.size()) - 1;
    if (steps < 1 || total_distance <= 0) return 1.0f;
    distance = std::fmod(distance, total_distance);
    if (distance < 0) distance += total_distance;
    int i = std::min(int(distance * float(steps) / total_distance), steps - 1);
    while (i > 0 && distance < cumulative[std::size_t(i)]) --i;
    while (i < steps - 1 && distance >= cumulative[std::size_t(i) + 1]) ++i;
    const float span = cumulative[std::size_t(i) + 1] - cumulative[std::size_t(i)];
    return float(i + 1) + (distance - cumulative[std::size_t(i)]) / span;
}

Palette build_palette(const SkinDef& skin, const Pose& pose) {
    const std::size_t bones = skin.parent.size();
    if (pose.translation.size() != bones) throw FormatError("pose bone count does not match the skin");
    Palette pal;
    pal.world.assign(bones, identity());
    pal.skin.assign(bones, identity());
    for (std::size_t i = 0; i < bones; ++i) {
        if (!skin.bone_active(i)) continue;
        const Mat4 local = quat_trans_to_mat(pose.rotation[i], pose.translation[i]);
        pal.world[i] = skin.bone_parent(i) == 0x7F ? local : mul(pal.world[skin.bone_parent(i)], local);
        // Rotation of the world matrix, translation R * inverse-bind + world translation.
        if (!skin.inverse_bind_translation.empty()) pal.skin[i] = mul(pal.world[i], translation(skin.inverse_bind_translation[i]));
    }
    return pal;
}

Palette bind_palette(const SkinDef& skin, const Skeleton& skeleton) {
    if (skin.inverse_bind_translation.empty()) return build_palette(skin, rest_pose(skeleton, &skin));
    Palette pal;
    pal.skin.assign(skin.parent.size(), identity());
    pal.world.assign(skin.parent.size(), identity());
    for (std::size_t i = 0; i < skin.inverse_bind_translation.size(); ++i) {
        const Vec3& t = skin.inverse_bind_translation[i];
        pal.world[i] = translation({-t[0], -t[1], -t[2]});
    }
    return pal;
}

Mat4 bone_world(const Palette& palette, std::size_t bone) { return palette.world.at(bone); }

Mat4 datum_world(const SkinDef& skin, const Palette& palette, std::int32_t id) {
    const Datum* d = skin.find_datum(id);
    if (!d) return identity();
    const Mat4 local = quat_trans_to_mat(d->rotation, d->translation);
    return d->bone < 0 ? local : mul(palette.world.at(std::size_t(d->bone)), local);
}

}  // namespace nf

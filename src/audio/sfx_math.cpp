#include "audio/sfx_math.hpp"

#include <algorithm>

namespace nf::audio {

namespace {
int clamp100(int v) { return std::clamp(v, 0, 100); }

int dot1000(const SfxPoint& a, const SfxPoint& b) { return (a.x * b.x + a.y * b.y + a.z * b.z) / 1000; }
SfxPoint neg(const SfxPoint& a) { return {-a.x, -a.y, -a.z}; }

// PS2_NormaliseVector: scale to length 1000 using ps2_sqrt; a zero vector is left alone.
SfxPoint normalise(SfxPoint v) {
    int len = ps2_sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (len == 0) return v;
    return {1000 * v.x / len, 1000 * v.y / len, 1000 * v.z / len};
}
}  // namespace

SfxPoint to_sfx_point(const Vec3& v) {
    return {static_cast<int>(v[0] * 1000.0f), static_cast<int>(v[1] * 1000.0f), static_cast<int>(v[2] * 1000.0f)};
}

int ps2_sqrt(int v) {
    if (v == 0) return 0;
    std::int64_t x = 1;
    for (int i = 0; i < 10; ++i) x -= (x * x - v) / (2 * x);
    return static_cast<int>(x);
}

int distance_units(const ListenerFrame& l, const SfxPoint& p) {
    int dx = (l.pos.x - p.x) / 100, dy = (l.pos.y - p.y) / 100, dz = (l.pos.z - p.z) / 100;
    return ps2_sqrt(dx * dx + dy * dy + dz * dz) / 10;
}

ChannelVolumes calculate_3d(const ListenerFrame& l, const SfxPoint& source, int inner, int outer, bool stereo) {
    SfxPoint d{(source.x - l.pos.x) / 100, (source.y - l.pos.y) / 100, (source.z - l.pos.z) / 100};
    int dist = ps2_sqrt(d.x * d.x + d.y * d.y + d.z * d.z);

    int over = std::max(dist - 10 * inner, 0);
    int span = std::max(10 * (outer - inner), 10);
    int attenuation = std::min(std::max(1000 - 1000 * over / span, 0) / 10, 100);
    int level = attenuation * attenuation / 100;

    SfxPoint dir = normalise(d);
    int left = (dot1000(l.norm, dir) + 1000) * level / 2000;
    int right = (dot1000(neg(l.norm), dir) + 1000) * level / 2000;
    int behind = (dot1000(neg(l.dir), dir) + 1000) * level;
    int front_scale = 100 - behind / 2000;
    left = left * front_scale / 100;
    right = right * front_scale / 100;
    int rear = 75 * (behind / 2000) / 100;

    if (!stereo) left = right = rear = (left + right + rear) / 3;
    return {clamp100(left), clamp100(right), clamp100(rear)};
}

ChannelVolumes head_locked(int volume) { return {volume / 2, volume / 2, 30 * volume / 100}; }

ChannelVolumes apply_volume(const ChannelVolumes& pan, int volume) {
    return {pan.left * volume / 100, pan.right * volume / 100, pan.rear * volume / 100};
}

ChannelVolumes pan_2d(int pan, int volume, bool stereo) {
    int left = clamp100((pan - 100) / -2 * volume / 100);
    int right = clamp100((pan + 100) / 2 * volume / 100);
    if (!stereo) left = right = (left + right) / 2;
    return {left, right, 0};
}

}  // namespace nf::audio

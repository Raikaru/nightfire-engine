#include "game/collision_world.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <span>
#include <utility>

namespace nf {

namespace {

// ---- constants lifted from ACTION.ELF (hex = the literal in the instruction stream) -------------

constexpr float kNoHit = 1e8f;                                    // 0x4CBEBC20: HITDATA+0xC "rejected"
constexpr float kSlabFar = std::bit_cast<float>(0x7F7FC99Eu);      // D_0030D128
constexpr float kSlabNear = std::bit_cast<float>(0xFDCCA14Bu);
constexpr float kParallelEps = std::bit_cast<float>(0x3951B717u);  // |n.seg| below this: axis parallel to plane
constexpr float kFacingCos = std::bit_cast<float>(0x3F7FBE77u);    // 0.999
constexpr float kTinyLengthSq = std::bit_cast<float>(0x1E3CE508u); // 1e-20
constexpr float kThird = std::bit_cast<float>(0x3EAAAAABu);
constexpr float kContactCos = std::bit_cast<float>(0x3F333333u);   // 0.7
constexpr float kBoxPad = std::bit_cast<float>(0x3DCCCCCDu);       // 0.1 added to the capsule radius
constexpr std::uint32_t kCelFlag = 0x8000;                        // map_data_static flags: instance is a cel
constexpr std::uint8_t kPassBits = 0xC0;                           // material bits 6 and 7

Vec3 rotate(const Mat4& m, const Vec3& v) {
    return {m[0] * v[0] + m[4] * v[1] + m[8] * v[2], m[1] * v[0] + m[5] * v[1] + m[9] * v[2],
            m[2] * v[0] + m[6] * v[1] + m[10] * v[2]};
}

// Affine inverse of a column-major model matrix (general 3x3 block: instances may carry scale).
bool invert_affine(const Mat4& m, Mat4& out) {
    const float a = m[0], b = m[4], c = m[8];
    const float d = m[1], e = m[5], f = m[9];
    const float g = m[2], h = m[6], i = m[10];
    const float c00 = e * i - f * h, c01 = f * g - d * i, c02 = d * h - e * g;
    const float det = a * c00 + b * c01 + c * c02;
    if (!(std::fabs(det) > 1e-12f)) return false;
    const float r = 1.0f / det;
    out = identity();
    out[0] = c00 * r;
    out[4] = (c * h - b * i) * r;
    out[8] = (b * f - c * e) * r;
    out[1] = c01 * r;
    out[5] = (a * i - c * g) * r;
    out[9] = (c * d - a * f) * r;
    out[2] = c02 * r;
    out[6] = (b * g - a * h) * r;
    out[10] = (a * e - b * d) * r;
    const Vec3 t = {m[12], m[13], m[14]};
    const Vec3 rt = rotate(out, t);
    out[12] = -rt[0];
    out[13] = -rt[1];
    out[14] = -rt[2];
    return true;
}

// Vec_Normalise: zero vector stays zero, otherwise v * (1/|v|).
Vec3 normalise(const Vec3& v) {
    const float len = std::sqrt(dot(v, v));
    if (len == 0.0f) return {0, 0, 0};
    return v * (1.0f / len);
}

// Intersect_BoxBox(a: min,max; b: min,max): strict-separation test, touching boxes overlap.
bool box_box(const Vec3& amin, const Vec3& amax, const Vec3& bmin, const Vec3& bmax) {
    if (amax[0] < bmin[0] || bmax[0] < amin[0]) return false;
    if (amax[2] < bmin[2] || bmax[2] < amin[2]) return false;
    return !(amax[1] < bmin[1] || bmax[1] < amin[1]);
}

// Intersect_RayBox / ASH_Intersect_RayBox: slab test of origin + t*dir against a box; `t` is the
// entry parameter, accepted when 0 <= t <= radius (HITTEST+0x8C, max(|dir|, 1) for rays).
bool ray_box(const Vec3& o, const Vec3& dir, float radius, const Vec3& bmin, const Vec3& bmax, float& t) {
    if (bmin[0] <= o[0] && o[0] <= bmax[0] && bmin[1] <= o[1] && o[1] <= bmax[1] && bmin[2] <= o[2] &&
        o[2] <= bmax[2]) {
        t = 0;
        return true;
    }
    float near_[3], far_[3];
    // The original walks x, z, y.
    for (int axis : {0, 2, 1}) {
        if (dir[axis] == 0.0f) {
            if (o[axis] < bmin[axis] || bmax[axis] < o[axis]) return false;
            near_[axis] = kSlabNear;
            far_[axis] = kSlabFar;
        } else {
            const float t0 = (bmin[axis] - o[axis]) / dir[axis];
            const float t1 = (bmax[axis] - o[axis]) / dir[axis];
            far_[axis] = std::max(t0, t1);
            near_[axis] = std::min(t0, t1);
            if (far_[axis] < 0.0f) return false;
        }
    }
    const float tnear = std::max({near_[0], near_[2], near_[1]});
    const float tfar = std::min({far_[0], far_[2], far_[1]});
    if (tfar < tnear) return false;
    t = tnear;
    return 0.0f <= tnear && tnear <= radius;
}

// Collide_Filter(material, hit flags, pick mask, &dist): true when the mask rejects this material.
bool filter_rejects(unsigned material, unsigned flags, unsigned pick, float& dist) {
    if ((material & 0x40) && (pick & 0x800)) dist = kNoHit;
    unsigned f = flags & 0xFFFF;
    if (material & 0x40) f |= 2;
    if (material & 0x80) f |= 2;
    if ((pick & 1) && ((material & 0x3F) - 0xD) < 2u) dist = kNoHit;
    if ((pick & 2) && (material & 0x3F) == 0x10) dist = kNoHit;
    if ((pick & 8) && (f & 2)) dist = kNoHit;
    return dist == kNoHit;
}

// HITTEST copy in the model space of one placement (Collide_Intersect's auStack_120).
struct Local {
    Vec3 a{}, b{};       // +0x20 / +0x30: ray from/to, capsule end points
    Vec3 dir{};          // +0x40: ray direction (to - from)
    float radius = 0;    // +0x8C
    unsigned pick = 0;   // +0x98
    unsigned hit = 0;    // +0x9A
    unsigned contact = 0;// +0x9C
};

// HITDATA fields in model space, before Collide_Intersect transforms them out.
struct Found {
    float dist = kNoHit;
    Vec3 normal{}, point{};
    std::uint8_t material = 0;
    std::uint8_t flags = 0;
    std::size_t tri = 0;
};

void gather_overlap(const Collision& c, std::size_t index, const Vec3& qmin, const Vec3& qmax,
                    std::vector<const CollisionBox*>& out) {
    const CollisionBox& box = c.boxes[index];
    if (!box_box(qmin, qmax, box.min, box.max)) return;
    if (box.leaf) {
        out.push_back(&box);
        return;
    }
    gather_overlap(c, std::size_t(box.child_a), qmin, qmax, out);
    gather_overlap(c, std::size_t(box.child_b), qmin, qmax, out);
}

void gather_ray(const Collision& c, std::size_t index, const Local& h, std::vector<const CollisionBox*>& out) {
    const CollisionBox& box = c.boxes[index];
    float t;
    if (!ray_box(h.a, h.dir, h.radius, box.min, box.max, t)) return;
    if (box.leaf) {
        out.push_back(&box);
        return;
    }
    gather_ray(c, std::size_t(box.child_a), h, out);
    gather_ray(c, std::size_t(box.child_b), h, out);
}

// Intersect_RayGeom. The ray only meets front faces (n . dir < 0); the nearest t in [0, 1) wins,
// ties go to the earlier triangle.
void ray_geom(const Collision& c, Local& h, Found& d, std::size_t& tested) {
    d.dist = kNoHit;
    std::vector<const CollisionBox*> leaves;
    if (c.boxes.empty()) return;
    gather_ray(c, 0, h, leaves);
    if (leaves.empty()) return;

    float best_t = 1.0f;
    bool have = false;
    std::size_t best_tri = 0;
    Vec3 best_point{};
    bool done = false;
    for (const CollisionBox* leaf : leaves) {
        for (std::size_t t = leaf->first_tri; t < leaf->end_tri; ++t) {
            ++tested;
            const CollisionTri& tri = c.tris[t];
            const Vec3& n = tri.normal;
            const float denom = n[0] * h.dir[0] + n[1] * h.dir[1] + n[2] * h.dir[2];
            const float num = n[0] * h.a[0] + n[1] * h.a[1] + n[2] * h.a[2] + tri.plane_d;
            if (0.0f <= denom) continue;
            const float u = (-num) / denom;
            if (u < 0.0f || 1.0f < u) continue;
            const Vec3 p = h.a + h.dir * u;
            if (!(u < best_t)) continue;

            const Vec3 &v0 = tri.v[0], &v1 = tri.v[1], &v2 = tri.v[2];
            bool inside = dot(p - v0, cross(v0 - v2, n)) <= 0.0f;
            inside = inside && dot(p - v1, cross(v1 - v0, n)) <= 0.0f;
            inside = inside && dot(p - v2, cross(v2 - v1, n)) <= 0.0f;
            if (!inside) continue;

            best_t = u;
            best_point = p;
            best_tri = t;
            have = true;
            d.point = p;
            if (h.hit & hitflag::kFirstHit) {
                float scratch = 0.0f;
                const std::uint8_t mat = tri.material;
                const bool rejected = filter_rejects(mat, d.flags, h.pick, scratch);
                if (!rejected || (mat & kPassBits)) {
                    done = true;
                    break;
                }
            }
        }
        if (done) break;
    }
    if (!have) return;
    const Vec3 delta = h.a - best_point;
    d.dist = std::sqrt(dot(delta, delta));
    d.material = c.tris[best_tri].material;
    d.normal = c.tris[best_tri].normal;
    d.point = best_point;
    d.tri = best_tri;
}

// ASH_vecutil_Dist2Tri: distance from `p` to the triangle (v0, v1, v2) with plane (n, d).
// Behind the plane: flag 0x63 and the (negative) plane distance. Otherwise the closest point,
// with `flag` describing which edges the projection lies outside (0 = inside the face).
struct Dist2Tri {
    unsigned flag = 0;
    Vec3 closest{};
    float dist = 0;
};

Vec3 closest_on_segment(const Vec3& p, const Vec3& a, const Vec3& b) {
    const Vec3 ab = b - a;
    const float len2 = dot(ab, ab);
    if (!(std::fabs(len2) > kTinyLengthSq)) return a;
    const float t = dot(p - a, ab) / len2;
    if (1.0f < t) return b;
    if (t < 0.0f) return a;
    return a + ab * t;
}

Dist2Tri dist_to_tri(const Vec3& p, const Vec3& v0, const Vec3& v1, const Vec3& v2, const Vec3& n, float d) {
    Dist2Tri r;
    const float plane = n[0] * p[0] + n[1] * p[1] + n[2] * p[2] + d;
    if (plane < 0.0f) {
        r.flag = 0x63;
        r.dist = plane;
        return r;
    }
    const Vec3* verts[3] = {&v0, &v1, &v2};
    unsigned flags = 0;
    // Edges (v2->v0), (v0->v1), (v1->v2); the third test is skipped once the flag reached 2.
    for (unsigned i = 0; i < 3 && flags < 2; ++i) {
        const Vec3& cur = *verts[i];
        const Vec3& prev = *verts[(i + 2) % 3];
        if (0.0f < dot(p - cur, cross(cur - prev, n))) flags |= 1u << i;
    }
    r.flag = flags;
    switch (flags) {
    case 1: r.closest = closest_on_segment(p, v0, v2); break;   // edge v2-v0, parameterised from v0
    case 2: r.closest = closest_on_segment(p, v1, v0); break;   // edge v0-v1, from v1
    case 4: r.closest = closest_on_segment(p, v2, v1); break;   // edge v1-v2, from v2
    case 3: r.closest = v0; break;
    case 6: r.closest = v1; break;
    case 5: r.closest = v2; break;
    default: r.closest = p + n * (-plane); break;
    }
    const Vec3 delta = r.closest - p;
    r.dist = std::sqrt(dot(delta, delta));
    return r;
}

// Intersect_CylGeom: sweeps every triangle whose box meets the (padded) capsule, pushing the
// capsule out of each one it penetrates (radius > distance from the axis point to the triangle,
// in front of its plane). The capsule end points and `delta` (DeltaP) accumulate every push, so
// later triangles see the moved capsule. Only the nearest surface is reported.
void cyl_geom(const Collision& c, Local& h, Found& d, Vec3& delta, std::size_t& tested) {
    delta = {0, 0, 0};
    d.dist = kNoHit;
    if (c.boxes.empty()) return;
    const float pad = h.radius + kBoxPad;
    Vec3 qmin = {std::min(h.a[0], h.b[0]) - pad, std::min(h.a[1], h.b[1]) - pad, std::min(h.a[2], h.b[2]) - pad};
    Vec3 qmax = {std::max(h.a[0], h.b[0]) + pad, std::max(h.a[1], h.b[1]) + pad, std::max(h.a[2], h.b[2]) + pad};
    std::vector<const CollisionBox*> leaves;
    gather_overlap(c, 0, qmin, qmax, leaves);
    if (leaves.empty()) return;

    const Vec3 axis = normalise(h.a - h.b);  // B -> A, fixed for the whole call
    const Vec3 seg0 = h.b - h.a;
    float best = h.radius;
    bool have = false;
    std::size_t best_tri = 0;
    bool done = false;

    for (const CollisionBox* leaf : leaves) {
        for (std::size_t t = leaf->first_tri; t < leaf->end_tri; ++t) {
            ++tested;
            const CollisionTri& tri = c.tris[t];
            const Vec3 &v0 = tri.v[0], &v1 = tri.v[1], &v2 = tri.v[2];
            // Triangle bounds vs the capsule bounds (which move with every push).
            if (qmax[0] - v0[0] < 0.0f) continue;
            const Vec3 tmin = {std::min({v0[0], v1[0], v2[0]}), std::min({v0[1], v1[1], v2[1]}),
                               std::min({v0[2], v1[2], v2[2]})};
            const Vec3 tmax = {std::max({v0[0], v1[0], v2[0]}), std::max({v0[1], v1[1], v2[1]}),
                               std::max({v0[2], v1[2], v2[2]})};
            if (qmax[0] - tmin[0] < 0.0f || qmax[1] - tmin[1] < 0.0f || qmax[2] - tmin[2] < 0.0f) continue;
            if (tmax[0] - qmin[0] < 0.0f || tmax[1] - qmin[1] < 0.0f || tmax[2] - qmin[2] < 0.0f) continue;

            const Vec3& n = tri.normal;
            const float pd = tri.plane_d;
            const float ns = n[0] * seg0[0] + n[1] * seg0[1] + n[2] * seg0[2];

            Vec3 p;
            Dist2Tri near_tri;
            if (std::fabs(ns) > kParallelEps) {
                // Where the axis crosses the triangle's plane, clamped to the segment.
                const float da = h.a[0] * n[0] + h.a[1] * n[1] + h.a[2] * n[2] + pd;
                const float raw = (-da) / ns;
                const float s = 0.0f <= raw ? (raw <= 1.0f ? raw : 1.0f) : 0.0f;
                if (kFacingCos < ns && 0.5f < s) continue;
                if (ns < -kFacingCos && s < 0.5f) continue;
                p = h.a + seg0 * s;
                near_tri = dist_to_tri(p, v0, v1, v2, n, pd);
                if (near_tri.flag != 0 && near_tri.flag != 0x63) {
                    // Outside the face: re-aim at the axis point closest to the triangle's nearest point.
                    const Vec3 seg = h.b - h.a;
                    const float len2 = dot(seg, seg);
                    if (std::fabs(len2) > kTinyLengthSq) {
                        const float u = dot(seg, near_tri.closest - h.a) / len2;
                        p = 1.0f < u ? h.b : (u < 0.0f ? h.a : h.a + seg * u);
                    } else {
                        p = h.a;
                    }
                }
            } else {
                // Axis parallel to the plane: use the point of the axis nearest the centroid.
                const Vec3 centroid = {((v0[0] + v1[0]) + v2[0]) * kThird, ((v0[1] + v1[1]) + v2[1]) * kThird,
                                       ((v0[2] + v1[2]) + v2[2]) * kThird};
                const Vec3 seg = h.b - h.a;
                const float len2 = dot(seg, seg);
                if (std::fabs(len2) > kTinyLengthSq) {
                    const float u = dot(seg, centroid - h.a) / len2;
                    p = 1.0f < u ? h.b : (u < 0.0f ? h.a : h.a + seg * u);
                } else {
                    p = h.a;
                }
                near_tri = dist_to_tri(p, v0, v1, v2, n, pd);
            }
            if (near_tri.dist < 0.0f || near_tri.flag == 0x63) continue;

            float dist = near_tri.dist;
            if (!(d.flags & 6)) {
                const Vec3 diff = p - near_tri.closest;
                if (dot(diff, n) <= 0.0f) continue;
                dist = std::sqrt(dot(diff, diff));
                const Vec3 dir = dist == 0.0f ? Vec3{0, 0, 0} : diff * (1.0f / dist);
                if (h.radius <= dist) continue;
                const Vec3 push = dir * (h.radius - dist);
                delta += push;
                qmin += push;
                qmax += push;
                h.a += push;
                h.b += push;
                if (h.contact == 0) {
                    // Floor test: face turned along the axis, nearest point below the lower end.
                    if (kContactCos < dot(n, axis)) {
                        const Vec3 to_hit = near_tri.closest - h.b;
                        if (dot(to_hit, axis) < 0.0f) {
                            const Vec3 unit = normalise(to_hit);
                            if (dot(unit, axis) < kContactCos) h.contact |= 1;
                        }
                    }
                }
            }
            if (!(dist < best)) continue;
            best = dist;
            best_tri = t;
            have = true;
            d.point = near_tri.closest;
            if (h.hit & hitflag::kFirstHit) {
                float scratch = 0.0f;
                const std::uint8_t mat = tri.material;
                const bool rejected = filter_rejects(mat, d.flags, h.pick, scratch);
                if (!rejected || (mat & kPassBits)) {
                    done = true;
                    break;
                }
            }
        }
        if (done) break;
    }
    if (!have) return;
    d.dist = best;
    d.material = c.tris[best_tri].material;
    d.normal = c.tris[best_tri].normal;
    d.tri = best_tri;
}

using Tri3 = std::array<Vec3, 3>;
constexpr std::size_t kMaxSoupTris = 166;  // TriHeap holds 501 vertices; PlaceObject reads at most 0xA6 triangles

// Intersect_CylTriGeom: appends (world space, via `model`) every triangle that comes within `radius`
// of the segment a-b, subject to the material mask, until the heap is full. Unlike CylGeom it
// never pushes anything.
void collect_triangles(const Collision& c, const Mat4& model, const Vec3& a, const Vec3& b, float radius,
                       unsigned pick, unsigned hit, std::vector<Tri3>& out) {
    if (c.boxes.empty()) return;
    constexpr float kPad = std::bit_cast<float>(0x3D4CCCCDu);        // 0.05
    constexpr float kNormalScale = std::bit_cast<float>(0x38800200u); // this variant scales normals by 2^-14 * (1 + 2^-14)
    constexpr float kBehind = std::bit_cast<float>(0xB951B717u);      // -2e-4
    const float pad = radius + kPad;
    const Vec3 qmin = {std::min(a[0], b[0]) - pad, std::min(a[1], b[1]) - pad, std::min(a[2], b[2]) - pad};
    const Vec3 qmax = {std::max(a[0], b[0]) + pad, std::max(a[1], b[1]) + pad, std::max(a[2], b[2]) + pad};
    std::vector<const CollisionBox*> leaves;
    gather_overlap(c, 0, qmin, qmax, leaves);
    const Vec3 seg = b - a;
    for (const CollisionBox* leaf : leaves) {
        for (std::size_t t = leaf->first_tri; t < leaf->end_tri; ++t) {
            const CollisionTri& tri = c.tris[t];
            const unsigned mat = tri.material;
            if ((pick & 1) && ((mat & 0x3F) - 0xD) < 2u) continue;
            if ((pick & 2) && (mat & 0x3F) == 0x10) continue;
            const Vec3 &v0 = tri.v[0], &v1 = tri.v[1], &v2 = tri.v[2];
            bool apart = false;
            for (int k = 0; k < 3; ++k) {
                if (std::max({v0[k], v1[k], v2[k]}) < qmin[k] || qmax[k] < std::min({v0[k], v1[k], v2[k]})) apart = true;
            }
            if (apart) continue;

            const Vec3 n = {std::round(tri.normal[0] * 16384.0f) * kNormalScale,
                            std::round(tri.normal[1] * 16384.0f) * kNormalScale,
                            std::round(tri.normal[2] * 16384.0f) * kNormalScale};
            const float ns = (n[0] * seg[0] + n[1] * seg[1]) + n[2] * seg[2];
            Vec3 p;
            if (kPad < std::fabs(ns)) {
                const float da = a[0] * n[0] + a[1] * n[1] + a[2] * n[2] + tri.plane_d;
                const float raw = (-da) / ns;
                p = a + seg * (0.0f <= raw ? (raw <= 1.0f ? raw : 1.0f) : 0.0f);
            } else {
                if (!(hit & 1)) continue;
                const Vec3 centroid = {((v0[0] + v1[0]) + v2[0]) * kThird, ((v0[1] + v1[1]) + v2[1]) * kThird,
                                       ((v0[2] + v1[2]) + v2[2]) * kThird};
                p = closest_on_segment(centroid, a, b);
            }
            const float dist = dist_to_tri(p, v0, v1, v2, n, tri.plane_d).dist;
            if (dist < kBehind || radius < dist) continue;
            if (out.size() >= kMaxSoupTris) continue;
            out.push_back({transform_point(model, v0), transform_point(model, v1), transform_point(model, v2)});
        }
    }
}

// QuickSort(list, n, 4, COMP_HITDATA): the original's unstable sort, ascending by dist. Ported
// as-is so that ties come out in the same order.
void quick_sort(std::vector<CollisionHit>& v) {
    const long n = long(v.size());
    if (n < 2) return;
    auto less = [&](long i, long j) { return v[std::size_t(i)].dist < v[std::size_t(j)].dist; };
    // comp(a, b): -1 if a < b, 1 if b < a, 0 otherwise.
    auto comp = [&](long i, long j) { return less(i, j) ? -1 : (less(j, i) ? 1 : 0); };
    std::vector<std::pair<long, long>> stack;
    long lo = 0, hi = n - 1;
    for (;;) {
        std::swap(v[std::size_t(lo)], v[std::size_t(lo + ((hi - lo + 1) >> 1))]);
        long i = lo + 1, j = hi + 1;
        for (;;) {
            while (i <= hi && comp(i, lo) < 1) ++i;
            --j;
            while (lo < j && -1 < comp(j, lo)) --j;
            if (j < i) break;
            std::swap(v[std::size_t(i)], v[std::size_t(j)]);
            ++i;
        }
        std::swap(v[std::size_t(lo)], v[std::size_t(j)]);
        bool pop = false;
        if ((j - lo) <= (hi - i)) {
            if (i < hi) stack.emplace_back(i, hi);
            hi = j - 1;
            if (j <= lo + 1) pop = true;
        } else {
            if (lo + 1 < j) stack.emplace_back(lo, j - 1);
            lo = i;
            if (!(i < hi)) pop = true;
        }
        if (pop) {
            if (stack.empty()) return;
            std::tie(lo, hi) = stack.back();
            stack.pop_back();
        }
    }
}

}  // namespace

struct CollisionWorld::Item {
    std::size_t placement = 0;
    std::shared_ptr<const Collision> coll;  // model space, shared by placements of one model
    Mat4 model{};                           // model -> world
    Mat4 inverse{};                         // world -> model
    Vec3 wmin{}, wmax{};                    // world bounds of the root box
};

CollisionWorld::CollisionWorld(Level& level) : CollisionWorld(level, std::nullopt) {}

CollisionWorld CollisionWorld::of_objects(Level& level, std::span<const std::uint32_t> classes) {
    return CollisionWorld(level, classes);
}

CollisionWorld::CollisionWorld(Level& level, std::optional<std::span<const std::uint32_t>> object_classes) {
    std::map<std::pair<std::size_t, std::size_t>, std::shared_ptr<const Collision>> cache;
    const auto& placements = level.placements();
    for (std::size_t i = 0; i < placements.size(); ++i) {
        const Placement& p = placements[i];
        // parsemap_block_map_data_static only turns statics flagged 0x8000 into cels (the world
        // geometry Collide_Pick walks); the rest become objects, which `of_objects` picks by class.
        const std::uint32_t flags = level.map()->chunk.statics.at(p.instance).flags;
        if (object_classes) {
            if (flags & 0xA000) continue;   // parsemap_block_map_data_dynamic skips these
            if (std::find(object_classes->begin(), object_classes->end(), flags & 0xFFFF) == object_classes->end()) continue;
        } else if (!(flags & kCelFlag)) {
            continue;
        }
        const Model& model = level.chunks().at(p.chunk).chunk.models.at(p.model);
        if (model.collision.empty()) continue;
        auto& slot = cache[{p.chunk, p.model}];
        if (!slot) slot = std::make_shared<const Collision>(parse_collision(model.collision));
        if (slot->boxes.empty() || slot->tris.empty()) continue;

        Item item;
        item.placement = i;
        item.coll = slot;
        item.model = p.transform;
        if (!invert_affine(item.model, item.inverse)) continue;
        const CollisionBox& root = slot->boxes.front();
        constexpr float inf = std::numeric_limits<float>::infinity();
        item.wmin = {inf, inf, inf};
        item.wmax = {-inf, -inf, -inf};
        for (int corner = 0; corner < 8; ++corner) {
            const Vec3 q = {corner & 1 ? root.max[0] : root.min[0], corner & 2 ? root.max[1] : root.min[1],
                            corner & 4 ? root.max[2] : root.min[2]};
            const Vec3 w = transform_point(item.model, q);
            for (int k = 0; k < 3; ++k) {
                item.wmin[k] = std::min(item.wmin[k], w[k]);
                item.wmax[k] = std::max(item.wmax[k], w[k]);
            }
        }
        items_.push_back(std::move(item));
    }
}

CollisionWorld::~CollisionWorld() = default;
CollisionWorld::CollisionWorld(CollisionWorld&&) noexcept = default;
CollisionWorld& CollisionWorld::operator=(CollisionWorld&&) noexcept = default;

std::size_t CollisionWorld::solid_count() const { return items_.size(); }

CollisionWorld::Solid CollisionWorld::solid(std::size_t i) const {
    const Item& item = items_.at(i);
    return {item.placement, *item.coll, item.model};
}

namespace {

// Collide_Intersect's shared tail: material filters, then bring the record back to world space.
// Returns false when the record is dropped (HITDATA+0xC == 1e8).
bool finish_hit(const Found& d, unsigned pick, std::uint8_t flags, float& dist) {
    dist = d.dist;
    const unsigned mat = d.material;
    if ((mat & 0x40) && (pick & 0x800)) dist = kNoHit;
    unsigned f = flags;
    if (mat & 0x40) f |= 2;
    if (mat & 0x80) f |= 2;
    if ((pick & 1) && ((mat & 0x3F) - 0xD) < 2u) dist = kNoHit;
    if ((pick & 2) && (mat & 0x3F) == 0x10) dist = kNoHit;
    if ((pick & 8) && (f & 2)) dist = kNoHit;
    return dist != kNoHit;
}

}  // namespace

std::vector<CollisionHit> CollisionWorld::ray_hits(const Vec3& from, const Vec3& to, unsigned pick,
                                                   unsigned hit) const {
    ++stats_.rays;
    std::vector<CollisionHit> hits;
    if (pick & pick::kSkipStatics) return hits;

    const Vec3 dir = to - from;
    const float length = std::sqrt(dot(dir, dir));
    const float radius = length > 1.0f ? length : 1.0f;
    const Vec3 lo = {std::min(from[0], to[0]), std::min(from[1], to[1]), std::min(from[2], to[2])};
    const Vec3 hi = {std::max(from[0], to[0]), std::max(from[1], to[1]), std::max(from[2], to[2])};

    // Candidates are visited last-registered first (the original pushes each node on the list head).
    for (std::size_t k = items_.size(); k-- > 0;) {
        const Item& item = items_[k];
        if (!box_box(lo, hi, item.wmin, item.wmax)) continue;
        float t;
        if (!ray_box(from, dir, radius, item.wmin, item.wmax, t)) continue;

        Local h;
        h.a = transform_point(item.inverse, from);
        h.b = transform_point(item.inverse, to);
        h.dir = h.b - h.a;
        h.radius = radius;
        h.pick = pick;
        h.hit = hit;
        Found d;
        d.flags = std::uint8_t(hit & 0xFF);
        ray_geom(*item.coll, h, d, stats_.triangles_tested);
        float dist;
        if (!finish_hit(d, pick, d.flags, dist)) continue;

        CollisionHit out;
        out.dist = dist;
        out.normal = rotate(item.model, d.normal);
        out.point = transform_point(item.model, d.point);
        out.material = d.material;
        out.flags = d.flags;
        out.placement = item.placement;
        out.triangle = d.tri;
        hits.push_back(out);
        if (hit & hitflag::kFirstHit) break;
    }
    quick_sort(hits);
    return hits;
}

std::optional<CollisionHit> CollisionWorld::ray(const Vec3& from, const Vec3& to, unsigned pick,
                                                unsigned hit) const {
    auto hits = ray_hits(from, to, pick, hit);
    if (hits.empty()) return std::nullopt;
    return hits.front();
}

bool CollisionWorld::line_of_sight(const Vec3& from, const Vec3& to, unsigned pick) const {
    return ray_hits(from, to, pick | 4, hitflag::kFirstHit).empty();
}

CylinderResult CollisionWorld::cylinder(const CylinderQuery& q) const {
    ++stats_.cylinders;
    CylinderResult res;
    res.a = q.a;
    res.b = q.b;
    if (q.pick & pick::kSkipStatics) return res;

    // Collide_Pick gathers candidates once, from the capsule as it starts (with slack for pushes).
    const float reach = 2.0f * q.radius + kBoxPad;
    const Vec3 lo = {std::min(q.a[0], q.b[0]) - reach, std::min(q.a[1], q.b[1]) - reach,
                     std::min(q.a[2], q.b[2]) - reach};
    const Vec3 hi = {std::max(q.a[0], q.b[0]) + reach, std::max(q.a[1], q.b[1]) + reach,
                     std::max(q.a[2], q.b[2]) + reach};

    for (std::size_t k = items_.size(); k-- > 0;) {
        const Item& item = items_[k];
        if (!box_box(lo, hi, item.wmin, item.wmax) || item.placement == q.ignore) continue;

        Local h;
        h.a = transform_point(item.inverse, res.a);
        h.b = transform_point(item.inverse, res.b);
        h.radius = q.radius;
        h.pick = q.pick;
        h.hit = q.hit;
        h.contact = res.contact;
        Found d;
        d.flags = std::uint8_t(q.hit & 0xFF);
        Vec3 delta{};
        cyl_geom(*item.coll, h, d, delta, stats_.triangles_tested);
        float dist;
        if (!finish_hit(d, q.pick, d.flags, dist)) continue;

        // Accepted: the push (rotated back to world space) moves the live capsule and the total.
        const Vec3 push = rotate(item.model, delta);
        res.push_out += push;
        res.a += push;
        res.b += push;
        res.contact |= h.contact;

        CollisionHit out;
        out.dist = dist;
        out.normal = rotate(item.model, d.normal);
        out.point = transform_point(item.model, d.point);
        out.material = d.material;
        out.flags = d.flags;
        out.placement = item.placement;
        out.triangle = d.tri;
        res.hits.push_back(out);
        if (q.hit & hitflag::kFirstHit) break;
    }
    quick_sort(res.hits);
    return res;
}

FeetResult CollisionWorld::feet_on_point(const Vec3& pos, const Vec3& top, const Vec3& up, float stand_height,
                                         unsigned contact) const {
    FeetResult r;
    const Vec3 to = top + up * -(stand_height + std::bit_cast<float>(0x3F19999Au));
    const auto hits = ray_hits(pos, to, 0xC, 8);
    if (!hits.empty()) {
        const CollisionHit& nearest = hits.front();
        r.nearest = nearest;
        if (nearest.material != 0) r.surface = nearest;
        if (nearest.dist - kBoxPad <= stand_height) {
            r.ground = nearest;
            r.ground_normal_y = nearest.normal[1];
        }
    }
    r.on_ground = contact != 0 || r.ground.has_value();
    return r;
}

PlaceResult CollisionWorld::place_object(const Vec3& pos_in, const PlaceFrame& frame, float height,
                                         float extent) const {
    PlaceResult r;
    r.frame = frame;
    r.pos = pos_in;
    r.pos[1] += std::bit_cast<float>(0x3D4CCCCDu);
    const Vec3 probe_end = r.pos + frame.up * -3.0f;
    const auto hit = ray(r.pos, probe_end, 0x1D, 0);
    if (!hit) return r;
    r.grounded = true;

    const Vec3 normal = normalise(hit->normal);
    const Vec3 rear = r.pos + normal * -0.1f;
    r.pos = hit->point + normal * height;

    // Every solid triangle within `extent` of the segment pos -> rear, in candidate order.
    std::vector<Tri3> soup;
    const Vec3 lo = {std::min(r.pos[0], rear[0]) - 2 * extent, std::min(r.pos[1], rear[1]) - 2 * extent,
                     std::min(r.pos[2], rear[2]) - 2 * extent};
    const Vec3 hi = {std::max(r.pos[0], rear[0]) + 2 * extent, std::max(r.pos[1], rear[1]) + 2 * extent,
                     std::max(r.pos[2], rear[2]) + 2 * extent};
    for (std::size_t k = items_.size(); k-- > 0;) {
        const Item& item = items_[k];
        if (!box_box(lo, hi, item.wmin, item.wmax)) continue;
        collect_triangles(*item.coll, item.model, transform_point(item.inverse, r.pos),
                          transform_point(item.inverse, rear), extent, 0xD, 1, soup);
    }

    // Grow the flat patch (min/max) over triangles whose plane lies within [-0.13, 0.2] of the
    // object's base and faces the same way (|n . n0| >= 0.96).
    Vec3 pmin = r.pos, pmax = r.pos;
    for (const Tri3& t : soup) {
        const Vec3 e1 = t[2] - t[1], e2 = t[1] - t[0];
        const Vec3 cr = cross(e2, e1);
        const float len = std::sqrt(dot(cr, cr));
        Vec3 pn{0, 0, 0};
        float pd = 0;
        if (len != 0.0f) {
            pn = cr * (1.0f / len);
            pd = -dot(pn, t[0]);
        }
        const float off = (((r.pos[0] * pn[0] + r.pos[1] * pn[1]) + r.pos[2] * pn[2]) + pd);
        const float gap = std::fabs(off) - height;
        if (gap < -0.13f) return r;
        if (gap <= 0.2f) {
            const float align = (pn[0] * normal[0] + pn[1] * normal[1]) + pn[2] * normal[2];
            if (0.96f <= std::fabs(align)) {
                for (const Vec3& v : t)
                    for (int k = 0; k < 3; ++k) {
                        pmin[k] = std::min(pmin[k], v[k]);
                        pmax[k] = std::max(pmax[k], v[k]);
                    }
            }
        }
    }

    // Mat_Align2Up(m, normal, dir): right = normalise(up x dir), dir = normalise(right x up).
    const Vec3 right = normalise(cross(normal, frame.dir));
    r.frame.right = right;
    r.frame.up = normal;
    r.frame.dir = normalise(cross(right, normal));

    const Vec3 dmin = pmin - r.pos, dmax = pmax - r.pos;
    auto reach = [&](const Vec3& d) {
        const float x = (d[0] * r.frame.right[0] + d[1] * r.frame.right[1]) + d[2] * r.frame.right[2];
        const float y = (d[0] * r.frame.dir[0] + d[1] * r.frame.dir[1]) + d[2] * r.frame.dir[2];
        return std::fabs(x) + std::fabs(y);
    };
    r.fits = extent <= reach(dmin) && extent <= reach(dmax);
    return r;
}

std::optional<Vec3> CollisionWorld::point_on_floor(const Vec3& pos, float range, const Vec3& dir) const {
    const Vec3 to = pos + dir * range;
    auto hit = ray(pos, to, 0x70C, 0);
    if (!hit) return std::nullopt;
    return hit->point;
}

}  // namespace nf

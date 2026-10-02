#include "driving/track_collision.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

#include "assets/reader.hpp"

namespace nf::driving {
namespace {

// ---------------------------------------------------------------------------------------------------
// CARP directory (original: sub_2D6940 / sub_2D51F0 / sub_2D5A10 / sub_2D5920 / sub_2D5700)
//
// The file header is a 16-byte TAR record {magic 'CARP', flags, plain_count, 1}. flags >> 5 is the number
// of sub-TAR heads ("Arti" x articles, "CDat", "Map ", "RNgp", "Shar"), plain_count the number of plain
// top-level records. The head records come first; each head's `offset` is counted in 16-byte records
// relative to the head record itself and `count` is its number of member records; members follow
// contiguously. (nf::CarpFile::entries() reads header word +4 as an entry count, which is really the flags
// word, and misses the tail of the member table.)

struct Record {
    std::string tag;
    std::int32_t index = -1;
    std::uint32_t flags = 0;
    std::uint32_t count = 0;
    std::uint32_t size = 0;
    std::size_t offset = 0;  // members: absolute payload offset
    std::size_t first = 0;   // heads: index of the first member record
};

class Directory {
public:
    explicit Directory(Bytes file) : file_(file) {
        const std::uint32_t flags = load<std::uint32_t>(file, 4);
        const std::size_t heads = flags >> 5, plain = load<std::uint32_t>(file, 8);
        std::size_t total = heads + plain;
        for (std::size_t i = 0; i < heads + plain; ++i) {
            records_.push_back(read(i));
            if (i < heads) {
                Record& h = records_.back();
                h.first = i + load<std::uint32_t>(file, position(i) + 12);
                total = std::max<std::size_t>(total, h.first + h.count);
            }
        }
        if (total > file.size() / 16) throw FormatError("CARP member table past end of file");
        for (std::size_t i = heads + plain; i < total; ++i) records_.push_back(read(i));
        heads_ = heads;
    }

    std::span<const Record> heads() const { return {records_.data(), heads_}; }
    const Record* head(std::string_view tag) const {
        for (const Record& h : heads())
            if (h.tag == tag) return &h;
        return nullptr;
    }
    std::span<const Record> members(const Record& head) const {
        if (head.first + head.count > records_.size()) throw FormatError("CARP group past end of directory");
        return {records_.data() + head.first, head.count};
    }
    Bytes payload(const Record& r) const { return slice(file_, r.offset, r.size); }

private:
    static std::size_t position(std::size_t i) { return 16 + i * 16; }

    Record read(std::size_t i) const {
        const std::size_t pos = position(i);
        Record r;
        r.flags = load<std::uint32_t>(file_, pos + 4);
        r.count = load<std::uint32_t>(file_, pos + 8);
        const std::uint32_t off = load<std::uint32_t>(file_, pos + 12);
        r.size = r.flags >> 8;
        if (r.flags & 1) {
            r.index = load<std::uint16_t>(file_, pos);
            r.tag = {char(file_[pos + 3]), char(file_[pos + 2])};
        } else {
            r.tag = {char(file_[pos + 3]), char(file_[pos + 2]), char(file_[pos + 1]), char(file_[pos])};
        }
        r.offset = (r.flags & 2) ? pos + off : off;
        return r;
    }

    Bytes file_;
    std::vector<Record> records_;
    std::size_t heads_ = 0;
};

const Record* find_member(std::span<const Record> members, std::string_view tag, std::int32_t index = -1) {
    for (const Record& r : members)
        if (r.tag == tag && r.index == index) return &r;
    return nullptr;
}

// ---------------------------------------------------------------------------------------------------
// Article collision geometry: the `ca` entry of an article, read by sub_213FF8 / sub_2138B0.
//
//   +0x00 u16 collider_count (+ 30 bytes of build-time data, unused at runtime)
//   +0x20 collider_count x 16: f32 centre.xyz, u16 radius (1/16 m), u16 group offset (from +0x20)
//   group (at +0x20 + offset): vertex_count x 16 bytes {f32 x, y, z; u32 w}: a triangle strip;
//     v0.w u32 = vertex_count, v1.w u32 = group flags (bit 0 first-triangle parity, bit 1 two-sided),
//     triangle k = (v[k], v[k+1], v[k+2]) with its attributes in v[k+2].w: u8 surface, u8 flags, u16 id.

struct LocalTri {
    std::array<Vec3, 3> v;
    std::uint8_t surface;
    std::uint8_t flags;
    bool two_sided;
};

constexpr std::uint8_t kFlagJoin = 0x04;  // strip-join triangle, skipped by sub_2138B0

using VertexBits = std::array<std::uint32_t, 3>;

VertexBits bits_of(const Vec3& v) {
    VertexBits b;
    for (int i = 0; i < 3; ++i) std::memcpy(&b[i], &v[i], 4);
    return b;
}

// Cyclically rotated so the smallest vertex comes first: equal triangles with equal winding compare equal.
std::array<std::uint32_t, 10> canonical_key(const LocalTri& t) {
    std::array<VertexBits, 3> b{bits_of(t.v[0]), bits_of(t.v[1]), bits_of(t.v[2])};
    std::size_t lo = 0;
    for (std::size_t i = 1; i < 3; ++i)
        if (b[i] < b[lo]) lo = i;
    std::array<std::uint32_t, 10> key{};
    for (std::size_t i = 0; i < 3; ++i)
        for (std::size_t k = 0; k < 3; ++k) key[i * 3 + k] = b[(lo + i) % 3][k];
    key[9] = std::uint32_t(t.surface) | std::uint32_t(t.flags) << 8 | std::uint32_t(t.two_sided) << 16;
    return key;
}

std::vector<LocalTri> parse_article(Bytes ca, std::size_t& strip_triangles) {
    const std::size_t groups = load<std::uint16_t>(ca, 0);
    std::vector<LocalTri> out;
    std::set<std::array<std::uint32_t, 10>> seen;  // colliders (spatial chunks) repeat straddling triangles
    for (std::size_t g = 0; g < groups; ++g) {
        const std::size_t base = 0x20 + 16 * g;
        const std::size_t start = 0x20 + load<std::uint16_t>(ca, base + 14);
        const std::size_t count = load<std::uint32_t>(ca, start + 12);
        if (count < 3) continue;
        slice(ca, start, 16 * count);
        const std::uint32_t group_flags = load<std::uint32_t>(ca, start + 28);
        const bool two_sided = (group_flags & 2) != 0;
        for (std::size_t k = 0; k + 2 < count; ++k) {
            const std::size_t attr = start + 16 * (k + 2) + 12;
            const std::uint8_t surface = ca[attr], flags = ca[attr + 1];
            if (flags & kFlagJoin) continue;
            if (surface >= kSurfaceCount) throw FormatError("collision triangle surface " + std::to_string(surface));
            ++strip_triangles;
            LocalTri t{};
            for (std::size_t i = 0; i < 3; ++i)
                for (std::size_t c = 0; c < 3; ++c)
                    t.v[i][c] = load<float>(ca, start + 16 * (k + i) + 4 * c);
            // Strips alternate winding; the group flag gives the parity of the first triangle.
            if (!two_sided && (((group_flags & 1) ^ (k & 1)) != 0)) std::swap(t.v[1], t.v[2]);
            t.surface = surface;
            t.flags = flags;
            t.two_sided = two_sided;
            if (seen.insert(canonical_key(t)).second) out.push_back(t);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------------------------------
// Collision instances (`ci`, 0x40 bytes, WCollisionInstance; sub_212ED0 builds its transform).
//
//   +0x00 f32 r0.xyz, f32 half extent x      world -> instance space: local = p * M + t, where the rows of M
//   +0x10 f32 0, f32 half extent y,           are r0, r1, r2 and t = row r3 (sub_25B890);
//         u8 flags, u8 (unknown, 0..7),       r1 = (0,1,0), or r2 x r0 when flags & 3 (sub_212ED0)
//         u16 instance-matrix index,
//         u16 article ref, u16 'sr' tag
//   +0x20 f32 r2.xyz, f32 half extent z
//   +0x30 f32 t.xyz, f32 bounding radius
// flags bit 0 also inverts the local "point above the triangle" test to "point below" (sub_213FF8).

constexpr std::size_t kInstanceBytes = 0x40;

struct Basis {
    // instance -> world: world = inv * (local - t)
    std::array<std::array<double, 3>, 3> inv;
    Vec3 t;
    bool mirrored;
};

Basis make_basis(Bytes rec) {
    const Vec3 r0{load<float>(rec, 0), load<float>(rec, 4), load<float>(rec, 8)};
    const Vec3 r2{load<float>(rec, 0x20), load<float>(rec, 0x24), load<float>(rec, 0x28)};
    const std::uint8_t flags = rec[0x18];
    const Vec3 r1 = (flags & 3) ? cross(r2, r0) : Vec3{0, 1, 0};
    // local - t = M^T p, so p = (M^T)^-1 (local - t); a = M^T has columns r0, r1, r2.
    const double a[3][3] = {{r0[0], r1[0], r2[0]}, {r0[1], r1[1], r2[1]}, {r0[2], r1[2], r2[2]}};
    const double c00 = a[1][1] * a[2][2] - a[1][2] * a[2][1], c01 = a[1][2] * a[2][0] - a[1][0] * a[2][2],
                 c02 = a[1][0] * a[2][1] - a[1][1] * a[2][0];
    const double det = a[0][0] * c00 + a[0][1] * c01 + a[0][2] * c02;
    if (std::abs(det) < 1e-3) throw FormatError("singular collision instance matrix");
    Basis b;
    b.inv = {{{c00 / det, (a[0][2] * a[2][1] - a[0][1] * a[2][2]) / det, (a[0][1] * a[1][2] - a[0][2] * a[1][1]) / det},
              {c01 / det, (a[0][0] * a[2][2] - a[0][2] * a[2][0]) / det, (a[0][2] * a[1][0] - a[0][0] * a[1][2]) / det},
              {c02 / det, (a[0][1] * a[2][0] - a[0][0] * a[2][1]) / det, (a[0][0] * a[1][1] - a[0][1] * a[1][0]) / det}}};
    b.t = {load<float>(rec, 0x30), load<float>(rec, 0x34), load<float>(rec, 0x38)};
    b.mirrored = det < 0;
    return b;
}

Vec3 to_world(const Basis& b, const Vec3& l) {
    const double d[3] = {double(l[0]) - b.t[0], double(l[1]) - b.t[1], double(l[2]) - b.t[2]};
    Vec3 w;
    for (int i = 0; i < 3; ++i)
        w[i] = float(b.inv[i][0] * d[0] + b.inv[i][1] * d[1] + b.inv[i][2] * d[2]);
    return w;
}

// ---------------------------------------------------------------------------------------------------
// Grid cells (`cn`, WGrid, sub_21C1F8 / sub_21CF80 / sub_217228): validated, see docs.

struct GridHeader {
    float x0, z0, cell;
    std::uint32_t cells_x, cells_z;
};

GridHeader read_grid(Bytes cgrd) {
    GridHeader g;
    g.x0 = load<float>(cgrd, 0);
    g.z0 = load<float>(cgrd, 8);
    g.cell = load<float>(cgrd, 16);
    g.cells_z = load<std::uint32_t>(cgrd, 24);  // WGrid + 0x18
    g.cells_x = load<std::uint32_t>(cgrd, 28);  // WGrid + 0x1C: cell id = z * cells_x + x
    if (!(g.cell > 0) || g.cells_x == 0 || g.cells_z == 0 || std::uint64_t(g.cells_x) * g.cells_z > (1u << 24))
        throw FormatError("bad collision grid header");
    return g;
}

std::size_t validate_cells(const Directory& dir, std::span<const Record> cdat, const GridHeader& grid,
                           std::size_t instances, std::size_t objects) {
    std::size_t cells = 0;
    for (const Record& r : cdat) {
        if (r.tag != "cn") continue;
        ++cells;
        if (r.count >= std::uint64_t(grid.cells_x) * grid.cells_z) throw FormatError("collision cell id out of range");
        const Bytes b = dir.payload(r);
        if (b.size() < 16) throw FormatError("collision cell too small");
        // Four sorted u16 lists: counts u8[4] at +4, byte offsets u16[4] at +8, from +0x10.
        for (std::size_t list = 0; list < 4; ++list) {
            const std::size_t n = b[4 + list], off = 16 + load<std::uint16_t>(b, 8 + 2 * list);
            const Bytes items = slice(b, off, 2 * n);
            if (list != 0 && list != 2) continue;  // lists 1 and 3 index tables not needed here
            for (std::size_t i = 0; i < n; ++i)
                if (load<std::uint16_t>(items, 2 * i) >= (list == 0 ? instances : objects))
                    throw FormatError("collision cell entry out of range");
        }
    }
    return cells;
}

Vec3 vec3_at(Bytes b, std::size_t off) { return {load<float>(b, off), load<float>(b, off + 4), load<float>(b, off + 8)}; }

}  // namespace

// ---------------------------------------------------------------------------------------------------

TrackCollision TrackCollision::load(const CarpFile& carp) {
    const Directory dir(carp.file());
    const Record* cdat_head = dir.head("CDat");
    if (!cdat_head) throw FormatError("track has no CDat group");
    const std::span<const Record> cdat = dir.members(*cdat_head);

    const Record* ci = find_member(cdat, "ci", 0);
    const Record* cgrd = nullptr;
    for (const Record& r : cdat)
        if (r.tag == "CGrd") cgrd = &r;
    if (!ci || !cgrd) throw FormatError("track lacks collision instances or grid");
    if (ci->size != std::uint64_t(ci->count) * kInstanceBytes || ci->count > 0xFFFF)
        throw FormatError("collision instance table size mismatch");
    const Bytes instances = dir.payload(*ci);
    const GridHeader grid = read_grid(dir.payload(*cgrd));

    // Article name -> its `ca` collision entry.
    std::unordered_map<std::string_view, const Record*> collision_of;
    for (const Record& h : dir.heads()) {
        if (h.tag != "Arti") continue;
        const std::span<const Record> m = dir.members(h);
        const Record* name = find_member(m, "Name");
        const Record* ca = find_member(m, "ca", 0);
        if (name && ca) collision_of.emplace(load_cstr(dir.payload(*name), 0), ca);
    }
    // `sr` references of the CDat group: index -> "CARP::<article name>".
    std::unordered_map<std::int32_t, std::string_view> reference;
    for (const Record& r : cdat)
        if (r.tag == "sr") reference.emplace(r.index, load_cstr(dir.payload(r), 0));

    TrackCollision tc;
    CollisionStats& stats = tc.stats_;
    stats.boxes = ci->count;

    struct Article {
        std::vector<LocalTri> tris;
        std::size_t strip_triangles = 0;
    };
    std::unordered_map<const Record*, Article> parsed;
    constexpr std::string_view kPrefix = "CARP::";
    for (std::size_t i = 0; i < ci->count; ++i) {
        const Bytes rec = slice(instances, i * kInstanceBytes, kInstanceBytes);
        if (nf::load<std::uint16_t>(rec, 0x1E) != 0x7372)  // bytes 'r','s': the `sr` tag stored reversed
            throw FormatError("collision instance without article reference");
        const auto ref = reference.find(nf::load<std::uint16_t>(rec, 0x1C));
        if (ref == reference.end() || !ref->second.starts_with(kPrefix))
            throw FormatError("collision instance " + std::to_string(i) + " has a dangling article reference");
        const auto art = collision_of.find(ref->second.substr(kPrefix.size()));
        if (art == collision_of.end())
            throw FormatError("collision article '" + std::string(ref->second) + "' not found");

        auto it = parsed.find(art->second);
        if (it == parsed.end()) {
            Article a;
            a.tris = parse_article(dir.payload(*art->second), a.strip_triangles);
            it = parsed.emplace(art->second, std::move(a)).first;
        }
        stats.strip_triangles += it->second.strip_triangles;
        const Basis basis = make_basis(rec);
        for (const LocalTri& lt : it->second.tris) {
            Tri t;
            for (std::size_t k = 0; k < 3; ++k) t.v[k] = to_world(basis, lt.v[k]);
            // A mirrored instance reverses the winding; swap to keep the front side.
            if (basis.mirrored) std::swap(t.v[1], t.v[2]);
            Vec3 n = cross(t.v[1] - t.v[0], t.v[2] - t.v[0]);
            const float len = length(n);
            if (!(len > 1e-6f)) continue;
            t.normal = n * (1.0f / len);
            t.surface = static_cast<Surface>(lt.surface);
            t.flags = lt.flags;
            t.instance = static_cast<std::uint16_t>(i);
            t.two_sided = lt.two_sided;
            tc.tris_.push_back(t);
        }
    }
    stats.articles = parsed.size();

    // `co`: 48-byte vertical cylinders {x, y, z, radius, -, half height, -, -, u8 dynamic, ...} (sub_218AF0).
    if (const Record* co = find_member(cdat, "co", 0)) {
        if (co->size != std::uint64_t(co->count) * 48) throw FormatError("collision object table size mismatch");
        const Bytes b = dir.payload(*co);
        for (std::size_t i = 0; i < co->count; ++i) {
            const Bytes rec = slice(b, i * 48, 48);
            const float radius = nf::load<float>(rec, 12), half = nf::load<float>(rec, 20);
            if (!(radius > 0) || !(half > 0)) throw FormatError("bad collision cylinder");
            tc.cylinders_.push_back({vec3_at(rec, 0), radius, 2 * half});
        }
    }
    stats.cylinders = tc.cylinders_.size();
    stats.cells = validate_cells(dir, cdat, grid, ci->count, tc.cylinders_.size());

    stats.triangles = tc.tris_.size();
    stats.min = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    stats.max = {std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                 std::numeric_limits<float>::lowest()};
    for (const Tri& t : tc.tris_) {
        ++stats.materials[static_cast<std::size_t>(t.surface)];
        for (const Vec3& v : t.v)
            for (int c = 0; c < 3; ++c) {
                stats.min[c] = std::min(stats.min[c], v[c]);
                stats.max[c] = std::max(stats.max[c], v[c]);
            }
    }
    if (tc.tris_.empty()) throw FormatError("track has no collision triangles");

    tc.grid_x0_ = grid.x0;
    tc.grid_z0_ = grid.z0;
    tc.cells_x_ = int(grid.cells_x);
    tc.cells_z_ = int(grid.cells_z);
    tc.cell_ = grid.cell;
    tc.build_grid();
    return tc;
}

// ---------------------------------------------------------------------------------------------------
// Grid: the original 24 m cells (`CGrd`) subdivided 4 x 4 (a plain per-cell instance list would make
// a vertical drop test scan whole instances of several thousand triangles), extended when triangles
// leave the original domain.

namespace {

constexpr int kSubdivision = 4;

// True when the XZ projection of `t` overlaps the rectangle (separating axis theorem).
bool overlaps_cell(const Tri& t, float x0, float z0, float x1, float z1) {
    for (int e = 0; e < 3; ++e) {
        const Vec3& p = t.v[e];
        const Vec3& q = t.v[(e + 1) % 3];
        const float nx = -(q[2] - p[2]), nz = q[0] - p[0];
        float tmin = std::numeric_limits<float>::max(), tmax = std::numeric_limits<float>::lowest();
        for (const Vec3& v : t.v) {
            const float d = nx * v[0] + nz * v[2];
            tmin = std::min(tmin, d);
            tmax = std::max(tmax, d);
        }
        float rmin = std::numeric_limits<float>::max(), rmax = std::numeric_limits<float>::lowest();
        for (const float x : {x0, x1})
            for (const float z : {z0, z1}) {
                const float d = nx * x + nz * z;
                rmin = std::min(rmin, d);
                rmax = std::max(rmax, d);
            }
        if (tmax < rmin || rmax < tmin) return false;
    }
    return true;
}

}  // namespace

void TrackCollision::build_grid() {
    cell_ /= kSubdivision;
    inv_cell_ = 1.0f / cell_;
    // Cover the original domain and every triangle.
    float min_x = grid_x0_, min_z = grid_z0_;
    float max_x = grid_x0_ + cells_x_ * (cell_ * kSubdivision), max_z = grid_z0_ + cells_z_ * (cell_ * kSubdivision);
    min_x = std::min(min_x, stats_.min[0]);
    min_z = std::min(min_z, stats_.min[2]);
    max_x = std::max(max_x, stats_.max[0]);
    max_z = std::max(max_z, stats_.max[2]);
    const float x0 = grid_x0_ + std::floor((min_x - grid_x0_) * inv_cell_) * cell_;
    const float z0 = grid_z0_ + std::floor((min_z - grid_z0_) * inv_cell_) * cell_;
    grid_x0_ = x0;
    grid_z0_ = z0;
    cells_x_ = int(std::ceil((max_x - x0) * inv_cell_)) + 1;
    cells_z_ = int(std::ceil((max_z - z0) * inv_cell_)) + 1;
    if (std::uint64_t(cells_x_) * cells_z_ > (1u << 26)) throw FormatError("collision grid too large");

    const std::size_t cells = std::size_t(cells_x_) * cells_z_;
    auto range = [&](const Tri& t, int& i0, int& i1, int& j0, int& j1) {
        const float lo_x = std::min({t.v[0][0], t.v[1][0], t.v[2][0]}), hi_x = std::max({t.v[0][0], t.v[1][0], t.v[2][0]});
        const float lo_z = std::min({t.v[0][2], t.v[1][2], t.v[2][2]}), hi_z = std::max({t.v[0][2], t.v[1][2], t.v[2][2]});
        i0 = std::clamp(int((lo_x - grid_x0_) * inv_cell_), 0, cells_x_ - 1);
        i1 = std::clamp(int((hi_x - grid_x0_) * inv_cell_), 0, cells_x_ - 1);
        j0 = std::clamp(int((lo_z - grid_z0_) * inv_cell_), 0, cells_z_ - 1);
        j1 = std::clamp(int((hi_z - grid_z0_) * inv_cell_), 0, cells_z_ - 1);
    };
    auto for_cells = [&](auto&& visit) {
        for (std::size_t n = 0; n < tris_.size(); ++n) {
            const Tri& t = tris_[n];
            int i0, i1, j0, j1;
            range(t, i0, i1, j0, j1);
            for (int j = j0; j <= j1; ++j)
                for (int i = i0; i <= i1; ++i) {
                    const float cx = grid_x0_ + i * cell_, cz = grid_z0_ + j * cell_;
                    if ((i0 == i1 && j0 == j1) || overlaps_cell(t, cx, cz, cx + cell_, cz + cell_))
                        visit(std::size_t(j) * cells_x_ + i, n);
                }
        }
    };
    cell_start_.assign(cells + 1, 0);
    for_cells([&](std::size_t cell, std::size_t) { ++cell_start_[cell + 1]; });
    for (std::size_t c = 0; c < cells; ++c) cell_start_[c + 1] += cell_start_[c];
    cell_tris_.resize(cell_start_[cells]);
    std::vector<std::uint32_t> fill(cell_start_.begin(), cell_start_.end() - 1);
    for_cells([&](std::size_t cell, std::size_t n) { cell_tris_[fill[cell]++] = std::uint32_t(n); });
}

// ---------------------------------------------------------------------------------------------------
// Queries

namespace {

constexpr float kMinGroundNormalY = 0.05f;

// Signed edge functions in XZ; inside (edges included) when all agree, whichever the winding (sub_2138B0).
bool contains_xz(const Tri& t, double x, double z) {
    double e[3];
    for (int i = 0; i < 3; ++i) {
        const Vec3& a = t.v[i];
        const Vec3& b = t.v[(i + 1) % 3];
        e[i] = (double(b[0]) - a[0]) * (z - a[2]) - (double(b[2]) - a[2]) * (x - a[0]);
    }
    return (e[0] >= 0 && e[1] >= 0 && e[2] >= 0) || (e[0] <= 0 && e[1] <= 0 && e[2] <= 0);
}

}  // namespace

bool TrackCollision::ground_below(const Vec3& p, GroundHit& out) const {
    const int i = int(std::floor((p[0] - grid_x0_) * inv_cell_)), j = int(std::floor((p[2] - grid_z0_) * inv_cell_));
    if (i < 0 || j < 0 || i >= cells_x_ || j >= cells_z_) return false;
    const std::size_t cell = std::size_t(j) * cells_x_ + i;
    const Tri* best = nullptr;
    double best_h = 0;
    for (std::uint32_t n = cell_start_[cell]; n < cell_start_[cell + 1]; ++n) {
        const Tri& t = tris_[cell_tris_[n]];
        if (!t.solid() || std::abs(t.normal[1]) < kMinGroundNormalY) continue;
        if (!t.two_sided && t.normal[1] < 0) continue;
        if (!contains_xz(t, p[0], p[2])) continue;
        const double h = t.v[0][1] - (double(t.normal[0]) * (p[0] - t.v[0][0]) + double(t.normal[2]) * (p[2] - t.v[0][2])) /
                                         t.normal[1];
        if (h > p[1] || (best && h <= best_h)) continue;
        best = &t;
        best_h = h;
    }
    if (!best) return false;
    out.point = {p[0], float(best_h), p[2]};
    out.normal = best->normal[1] < 0 ? best->normal * -1.0f : best->normal;
    out.surface = best->surface;
    return true;
}

namespace {

// Möller-Trumbore, both sides; returns the segment parameter in [0,1] or a negative number.
double segment_triangle(const Tri& tri, const Vec3& from, const Vec3& dir) {
    const double e1[3] = {double(tri.v[1][0]) - tri.v[0][0], double(tri.v[1][1]) - tri.v[0][1], double(tri.v[1][2]) - tri.v[0][2]};
    const double e2[3] = {double(tri.v[2][0]) - tri.v[0][0], double(tri.v[2][1]) - tri.v[0][1], double(tri.v[2][2]) - tri.v[0][2]};
    const double d[3] = {dir[0], dir[1], dir[2]};
    const double pv[3] = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0]};
    const double det = e1[0] * pv[0] + e1[1] * pv[1] + e1[2] * pv[2];
    if (std::abs(det) < 1e-12) return -1;
    const double tv[3] = {double(from[0]) - tri.v[0][0], double(from[1]) - tri.v[0][1], double(from[2]) - tri.v[0][2]};
    const double u = (tv[0] * pv[0] + tv[1] * pv[1] + tv[2] * pv[2]) / det;
    if (u < -1e-9 || u > 1 + 1e-9) return -1;
    const double qv[3] = {tv[1] * e1[2] - tv[2] * e1[1], tv[2] * e1[0] - tv[0] * e1[2], tv[0] * e1[1] - tv[1] * e1[0]};
    const double v = (d[0] * qv[0] + d[1] * qv[1] + d[2] * qv[2]) / det;
    if (v < -1e-9 || u + v > 1 + 1e-9) return -1;
    const double t = (e2[0] * qv[0] + e2[1] * qv[1] + e2[2] * qv[2]) / det;
    return (t < 0 || t > 1) ? -1 : t;
}

// Segment against a vertical cylinder (sub_231720 solves the same circle quadratic in XZ); entry point, or
// the start when it lies inside. Returns a negative number on a miss.
double segment_cylinder(const Cylinder& c, const Vec3& from, const Vec3& dir) {
    const double fx = double(from[0]) - c.base[0], fz = double(from[2]) - c.base[2];
    const double r2 = double(c.radius) * c.radius;
    const double a = double(dir[0]) * dir[0] + double(dir[2]) * dir[2];
    double t;
    if (fx * fx + fz * fz < r2) {
        t = 0;
    } else {
        if (a < 1e-12) return -1;
        const double b = fx * dir[0] + fz * dir[2];
        const double disc = b * b - a * (fx * fx + fz * fz - r2);
        if (disc < 0) return -1;
        t = (-b - std::sqrt(disc)) / a;
        if (t < 0 || t > 1) return -1;
    }
    const double y = double(from[1]) + t * dir[1];
    return (y > c.base[1] && y < double(c.base[1]) + c.height) ? t : -1;
}

}  // namespace

bool TrackCollision::segment_hit(const Vec3& from, const Vec3& to, SegmentHit& out) const {
    const Vec3 dir = to - from;
    double best_t = 2;
    const Tri* best_tri = nullptr;
    const Cylinder* best_cyl = nullptr;

    // Grid walk (Amanatides-Woo) over the part of the segment inside the grid.
    const double gx0 = grid_x0_, gz0 = grid_z0_, gx1 = gx0 + cells_x_ * double(cell_), gz1 = gz0 + cells_z_ * double(cell_);
    double t_in = 0, t_out = 1;
    const double lo[2] = {gx0, gz0}, hi[2] = {gx1, gz1}, o[2] = {from[0], from[2]}, dd[2] = {dir[0], dir[2]};
    bool inside = true;
    for (int axis = 0; axis < 2; ++axis) {
        if (std::abs(dd[axis]) < 1e-12) {
            if (o[axis] < lo[axis] || o[axis] >= hi[axis]) inside = false;
            continue;
        }
        double t0 = (lo[axis] - o[axis]) / dd[axis], t1 = (hi[axis] - o[axis]) / dd[axis];
        if (t0 > t1) std::swap(t0, t1);
        t_in = std::max(t_in, t0);
        t_out = std::min(t_out, t1);
    }
    if (inside && t_in <= t_out) {
        const double sx = o[0] + dd[0] * t_in - gx0, sz = o[1] + dd[1] * t_in - gz0;
        int i = std::clamp(int(sx * inv_cell_), 0, cells_x_ - 1), j = std::clamp(int(sz * inv_cell_), 0, cells_z_ - 1);
        const int step_i = dd[0] > 0 ? 1 : -1, step_j = dd[1] > 0 ? 1 : -1;
        const double inf = std::numeric_limits<double>::infinity();
        double next_i = std::abs(dd[0]) < 1e-12 ? inf : t_in + ((i + (step_i > 0)) * double(cell_) - sx) / dd[0];
        double next_j = std::abs(dd[1]) < 1e-12 ? inf : t_in + ((j + (step_j > 0)) * double(cell_) - sz) / dd[1];
        const double delta_i = std::abs(dd[0]) < 1e-12 ? inf : double(cell_) / std::abs(dd[0]);
        const double delta_j = std::abs(dd[1]) < 1e-12 ? inf : double(cell_) / std::abs(dd[1]);
        for (;;) {
            const std::size_t cell = std::size_t(j) * cells_x_ + i;
            for (std::uint32_t n = cell_start_[cell]; n < cell_start_[cell + 1]; ++n) {
                const Tri& t = tris_[cell_tris_[n]];
                if (!t.solid()) continue;
                const double h = segment_triangle(t, from, dir);
                if (h >= 0 && h < best_t) {
                    best_t = h;
                    best_tri = &t;
                }
            }
            const double exit_t = std::min(next_i, next_j);
            if (best_t <= exit_t || exit_t > t_out) break;
            if (next_i < next_j) {
                i += step_i;
                next_i += delta_i;
            } else {
                j += step_j;
                next_j += delta_j;
            }
            if (i < 0 || j < 0 || i >= cells_x_ || j >= cells_z_) break;
        }
    }
    for (const Cylinder& c : cylinders_) {
        const double h = segment_cylinder(c, from, dir);
        if (h >= 0 && h < best_t) {
            best_t = h;
            best_tri = nullptr;
            best_cyl = &c;
        }
    }
    if (best_t > 1) return false;

    out.t = float(best_t);
    out.point = from + dir * out.t;
    if (best_tri) {
        out.normal = dot(best_tri->normal, dir) > 0 ? best_tri->normal * -1.0f : best_tri->normal;
        out.surface = best_tri->surface;
    } else {
        Vec3 n{out.point[0] - best_cyl->base[0], 0, out.point[2] - best_cyl->base[2]};
        if (length(n) < 1e-6f) n = {from[0] - best_cyl->base[0], 0, from[2] - best_cyl->base[2]};
        const float len = length(n);
        out.normal = len > 1e-6f ? n * (1.0f / len) : Vec3{0, 1, 0};
        out.surface = Surface::NoDrive;
    }
    return true;
}

CollisionStats validate_collision(const CarpFile& carp) { return TrackCollision::load(carp).stats(); }

}  // namespace nf::driving

#include "assets/collision.hpp"

namespace nf {

namespace {

constexpr std::size_t kUnitSize = 0x40, kBoxSize = 0x30, kTriSize = 8;
constexpr float kNormalScale = 1.0f / 16384.0f;

Vec3 load_vec3(Bytes b, std::size_t off) {
    return {load<float>(b, off), load<float>(b, off + 4), load<float>(b, off + 8)};
}

}  // namespace

Collision parse_collision(Bytes block) {
    Collision c;
    c.version = load<std::uint32_t>(block, 4);
    if (c.version != 4 && c.version != 5) throw FormatError("coll_data_new: unknown version");
    const std::size_t box_count = load<std::uint16_t>(block, 8);
    const std::size_t tri_count = load<std::uint16_t>(block, 0xA);
    const std::size_t unit_count = load<std::uint16_t>(block, 0xC);

    const std::size_t pool = c.version == 4 ? 0x10 : 0x20;
    const std::size_t boxes = pool + unit_count * kUnitSize;
    const std::size_t tris = boxes + box_count * kBoxSize;
    const std::size_t materials = tris + tri_count * kTriSize;
    if (materials + tri_count > block.size()) throw FormatError("coll_data_new: arrays overrun block");

    c.boxes.reserve(box_count);
    c.tris.resize(tri_count);
    std::vector<bool> seen(tri_count, false);
    for (std::size_t i = 0; i < box_count; ++i) {
        const std::size_t o = boxes + i * kBoxSize;
        CollisionBox box{};
        box.min = load_vec3(block, o);
        box.max = load_vec3(block, o + 0x10);
        const auto a = load<std::int16_t>(block, o + 0xC);
        const auto b = load<std::int16_t>(block, o + 0xE);
        box.leaf = a < 0;
        if (!box.leaf) {
            if (std::size_t(a) >= box_count || b < 0 || std::size_t(b) >= box_count)
                throw FormatError("coll_data_new: child box out of range");
            box.child_a = a;
            box.child_b = b;
            c.boxes.push_back(box);
            continue;
        }
        box.first_tri = std::uint16_t(b);
        box.end_tri = load<std::uint16_t>(block, o + 0x1C);
        const std::size_t first_unit = load<std::uint16_t>(block, o + 0x1E);
        const std::size_t units = std::uint16_t(a) & 0x7FFF;
        if (box.first_tri > box.end_tri || box.end_tri > tri_count || first_unit + units > unit_count)
            throw FormatError("coll_data_new: leaf range out of bounds");
        const Vec3 origin = load_vec3(block, o + 0x20);
        const float scale = load<float>(block, o + 0x2C);
        // Indices are halfword offsets into this leaf's slice of the pool.
        const Bytes leaf_pool = slice(block, pool + first_unit * kUnitSize, units * kUnitSize);
        auto shorts = [&](std::uint16_t idx) {
            return std::array<float, 3>{float(load<std::int16_t>(leaf_pool, idx * 2u)),
                                        float(load<std::int16_t>(leaf_pool, idx * 2u + 2)),
                                        float(load<std::int16_t>(leaf_pool, idx * 2u + 4))};
        };
        for (std::size_t t = box.first_tri; t < box.end_tri; ++t) {
            if (seen[t]) throw FormatError("coll_data_new: triangle owned by two leaves");
            seen[t] = true;
            const std::size_t to = tris + t * kTriSize;
            CollisionTri& tri = c.tris[t];
            for (int k = 0; k < 3; ++k) {
                auto q = shorts(load<std::uint16_t>(block, to + k * 2));
                for (int j = 0; j < 3; ++j) tri.v[k][j] = q[j] * scale + origin[j];
            }
            auto n = shorts(load<std::uint16_t>(block, to + 6));
            for (int j = 0; j < 3; ++j) tri.normal[j] = n[j] * kNormalScale;
            tri.material = load<std::uint8_t>(block, materials + t);
        }
        c.boxes.push_back(box);
    }
    for (bool s : seen)
        if (!s) throw FormatError("coll_data_new: triangle not referenced by any leaf");
    return c;
}

}  // namespace nf

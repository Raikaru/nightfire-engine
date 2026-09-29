#include "assets/level.hpp"

namespace nf {

std::vector<std::uint8_t> read_level_bin(GameFiles& files, std::string& name) {
    for (const auto& f : files.files()) {
        if (!f.name.ends_with(".bin") || (!name.empty() && f.name != name)) continue;
        auto bin = files.read(f);
        if (parse_bin_archive(Bytes(bin)).empty()) continue;
        name = f.name;
        return bin;
    }
    return {};
}

std::array<float, 16> instance_transform(const StaticInstance& s) {
    auto [x, y, z, w] = s.quat;
    // Rotation from unit quaternion, then scale columns, then translate.
    std::array<float, 9> r = {
        1 - 2 * (y * y + z * z), 2 * (x * y + z * w),     2 * (x * z - y * w),
        2 * (x * y - z * w),     1 - 2 * (x * x + z * z), 2 * (y * z + x * w),
        2 * (x * z + y * w),     2 * (y * z - x * w),     1 - 2 * (x * x + y * y),
    };
    std::array<float, 16> m{};
    for (int c = 0; c < 3; ++c)
        for (int row = 0; row < 3; ++row) m[c * 4 + row] = r[c * 3 + row] * s.scale[c];
    m[12] = s.position[0];
    m[13] = s.position[1];
    m[14] = s.position[2];
    m[15] = 1;
    return m;
}

Level::Level(std::vector<std::uint8_t> bin) : bin_(std::move(bin)) {
    std::unordered_map<std::int32_t, std::pair<std::size_t, std::size_t>> by_hash;
    for (auto& entry : parse_bin_archive(Bytes(bin_))) {
        if (!is_map_chunk_file(entry.type)) continue;
        std::size_t index = chunks_.size();
        if (entry.type == EntryType::Map) {
            if (map_ != SIZE_MAX) throw FormatError("level .bin has more than one Map entry");
            map_ = index;
        }
        chunks_.push_back({entry, parse_map_chunk(entry.data)});
        const auto& models = chunks_.back().chunk.models;
        for (std::size_t m = 0; m < models.size(); ++m)
            if (models[m].hash != -1) by_hash.emplace(models[m].hash, std::pair{index, m});
    }
    if (map_ == SIZE_MAX) return;

    for (const auto& s : chunks_[map_].chunk.statics) {
        std::pair<std::size_t, std::size_t> target;
        if (s.hash == -1) {
            if (s.model_index >= chunks_[map_].chunk.models.size()) {
                ++unresolved_;
                continue;
            }
            target = {map_, s.model_index};
        } else if (auto it = by_hash.find(s.hash); it != by_hash.end()) {
            target = it->second;
        } else {
            ++unresolved_;
            continue;
        }
        placements_.push_back({target.first, target.second, instance_transform(s)});
    }
}

const GfxMesh& Level::mesh(std::size_t chunk, std::size_t model) {
    auto key = (std::uint64_t(chunk) << 32) | model;
    auto& slot = meshes_[key];
    if (!slot) slot = std::make_unique<GfxMesh>(decode_ps2_gfx(chunks_.at(chunk).chunk.models.at(model).gfx));
    return *slot;
}

}  // namespace nf

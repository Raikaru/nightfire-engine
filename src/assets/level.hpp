#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "assets/bin_archive.hpp"
#include "assets/map_file.hpp"
#include "assets/ps2_gfx.hpp"

namespace nf {

struct ChunkFile {
    BinEntry entry;
    MapChunk chunk;
};

struct Placement {
    std::size_t chunk;               // index into Level::chunks (owns the model + its textures)
    std::size_t model;
    std::array<float, 16> transform; // column-major, model -> world
};

// A level .bin with its map chunk files parsed and the Map entry's static instances resolved
// (local model index, or a hash registered by any chunk file in the same .bin).
class Level {
public:
    explicit Level(std::vector<std::uint8_t> bin);

    const std::vector<ChunkFile>& chunks() const { return chunks_; }
    const ChunkFile* map() const { return map_ < chunks_.size() ? &chunks_[map_] : nullptr; }
    const std::vector<Placement>& placements() const { return placements_; }
    std::size_t unresolved_instances() const { return unresolved_; }

    // Decoded on first use and cached.
    const GfxMesh& mesh(std::size_t chunk, std::size_t model);

private:
    std::vector<std::uint8_t> bin_;
    std::vector<ChunkFile> chunks_;
    std::size_t map_ = SIZE_MAX;
    std::vector<Placement> placements_;
    std::size_t unresolved_ = 0;
    std::unordered_map<std::uint64_t, std::unique_ptr<GfxMesh>> meshes_;
};

std::array<float, 16> instance_transform(const StaticInstance& s);

}  // namespace nf

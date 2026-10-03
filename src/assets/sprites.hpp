#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "assets/map_file.hpp"

namespace nf {

// 2D sprite textures. A map chunk file's palette_header block has one 8-byte entry per texture:
// `u32 ?; i32 hash`. A hash other than -1 registers the texture in the global hash table (type 3,
// `0x03xxxxxx`) where HUD, menu and font sprites find it (parsemap_block_palette_data_psx ->
// hashtable_additem; hashtable_getitem gives the `{data, u16 width, u16 height}` record).
class SpriteLibrary {
public:
    // Registers every hashed texture of `chunk`. A later registration of the same hash replaces the
    // earlier one (the level's own copy wins over the shared chunk, as with the runtime table).
    void add(const MapChunk& chunk);
    // Registers a texture the engine itself provides (the original art sheets, src/ui/art_sheet.hpp).
    void add(std::uint32_t hash, Texture texture) { textures_[hash] = std::move(texture); }

    const Texture* find(std::uint32_t hash) const;
    std::size_t size() const { return textures_.size(); }
    const std::unordered_map<std::uint32_t, Texture>& all() const { return textures_; }

private:
    std::unordered_map<std::uint32_t, Texture> textures_;
};

// Hash of every texture in `chunk` that has one, in texture order (index into chunk.textures).
std::vector<std::pair<std::size_t, std::uint32_t>> texture_hashes(const MapChunk& chunk);

}  // namespace nf

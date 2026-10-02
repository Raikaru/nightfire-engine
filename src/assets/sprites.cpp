#include "assets/sprites.hpp"

namespace nf {

std::vector<std::pair<std::size_t, std::uint32_t>> texture_hashes(const MapChunk& chunk) {
    std::vector<std::pair<std::size_t, std::uint32_t>> out;
    for (const auto& b : chunk.blocks) {
        if (b.id != std::uint8_t(BlockId::PaletteHeader)) continue;
        std::size_t count = (b.data.size() - 4) / 8;
        if (count != chunk.textures.size()) throw FormatError("palette header and texture counts disagree");
        for (std::size_t i = 0; i < count; ++i) {
            auto hash = load<std::uint32_t>(b.data, 4 + i * 8 + 4);
            if (hash != 0xFFFFFFFFu) out.emplace_back(i, hash);
        }
    }
    return out;
}

void SpriteLibrary::add(const MapChunk& chunk) {
    for (auto [index, hash] : texture_hashes(chunk)) textures_[hash] = chunk.textures[index];
}

const Texture* SpriteLibrary::find(std::uint32_t hash) const {
    auto it = textures_.find(hash);
    return it == textures_.end() ? nullptr : &it->second;
}

}  // namespace nf

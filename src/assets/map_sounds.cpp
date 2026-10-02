#include "assets/map_sounds.hpp"

namespace nf {

namespace {
constexpr std::size_t kRecordBytes = 32;
constexpr std::uint32_t kMaxMapSounds = 50;  // GMapSounds holds 50 records (1600 bytes)
}  // namespace

std::vector<MapSound> parse_map_sounds(Bytes block) {
    auto count = load<std::uint32_t>(block, 4);
    if (count > kMaxMapSounds || block.size() != 8 + std::size_t(count) * kRecordBytes)
        throw FormatError("bad map sound block");
    std::vector<MapSound> out;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::size_t at = 8 + i * kRecordBytes;
        out.push_back({load<std::uint32_t>(block, at), load<std::uint32_t>(block, at + 4),
                       {load<float>(block, at + 8), load<float>(block, at + 12), load<float>(block, at + 16)},
                       load<float>(block, at + 20), load<float>(block, at + 24), load<float>(block, at + 28)});
    }
    return out;
}

}  // namespace nf

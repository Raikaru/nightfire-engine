#include "assets/level_music.hpp"

#include <algorithm>

#include "assets/elf.hpp"
#include "assets/game_files.hpp"

namespace nf {

namespace {
constexpr std::size_t kMusicWords = 19;
constexpr std::size_t kMusicRowBytes = kMusicWords * 4;
constexpr std::size_t kDroneRowBytes = 16;
}  // namespace

LevelMusicTable::LevelMusicTable(const std::filesystem::path& gamedir) {
    Elf32 elf(read_file(gamedir / "ACTION.ELF"));
    auto music = elf.symbol("MapMusic");
    auto drone = elf.symbol("MapDroneData");
    if (!music || !drone || music->size % kMusicRowBytes || drone->size % kDroneRowBytes)
        throw FormatError("ACTION.ELF lacks MapMusic / MapDroneData");

    Bytes rows = elf.at(music->value, music->size);
    for (std::size_t off = 0; off < rows.size(); off += kMusicRowBytes) {
        auto word = [&](std::size_t i) { return load<std::int32_t>(rows, off + i * 4); };
        LevelMusic m;
        m.level = static_cast<std::uint32_t>(word(0));
        m.track_hash = static_cast<std::uint32_t>(word(1));
        m.start_section = word(2);
        m.on_death = word(3);
        m.on_success = word(4);
        m.finale_combat = word(5);
        m.finale_calm = word(6);
        m.calm_frames = word(7);
        m.attack_frames = word(8);
        for (std::size_t i = 0; i < LevelMusic::kListSize; ++i) {
            m.combat[i] = word(9 + i);
            m.stealth[i] = word(14 + i);
        }
        levels_.push_back(m);
    }

    Bytes drones = elf.at(drone->value, drone->size);
    for (std::size_t off = 0; off < drones.size(); off += kDroneRowBytes) {
        drone_.push_back({load<std::uint32_t>(drones, off), load<std::uint16_t>(drones, off + 4),
                          load<std::uint16_t>(drones, off + 6), load<std::int32_t>(drones, off + 8),
                          load<float>(drones, off + 12)});
    }
}

const LevelMusic* LevelMusicTable::find(std::uint32_t level) const {
    auto it = std::ranges::find(levels_, level, &LevelMusic::level);
    return it == levels_.end() ? nullptr : &*it;
}

MusicDroneParams LevelMusicTable::drone_params(std::uint32_t level) const {
    auto it = std::ranges::find(drone_, level, &MusicDroneParams::level);
    if (it == drone_.end()) return MusicDroneParams{level, 2, 0, 8, 25.0f};
    return *it;
}

}  // namespace nf

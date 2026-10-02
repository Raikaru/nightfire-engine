#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace nf {

// One row of ACTION.ELF's static `MapMusic` table (35 rows x 19 words). Field names describe how
// UpdateMusicalEvents / UpdateMFX_* use the words; a section number of -1 ends a section list.
struct LevelMusic {
    static constexpr std::size_t kListSize = 5;

    std::uint32_t level = 0;          // w0: level id (0x07000001 = Mayhew A)
    std::uint32_t track_hash = 0;     // w1: MFXINFO.MXI hash passed to SFXStartMusic; 0 = level has no music
    std::int32_t start_section = 0;   // w2: section SFXStartMusic begins at
    std::int32_t on_death = 0;        // w3: Music_Event(6): section jumped to (instant) and the music fades out
    std::int32_t on_success = 0;      // w4: Music_Event(7): same, for the mission-success event
    std::int32_t finale_combat = 0;   // w5: end-of-level (events 3/8): section to jump to when a combat section plays
    std::int32_t finale_calm = 0;     // w6: ... when any other section plays
    std::int32_t calm_frames = 0;     // w7: MusicUnderAttackCount2 reload (frames of alert before it counts as combat)
    std::int32_t attack_frames = 0;   // w8: MusicUnderAttackCount reload (frames combat music is held after the alert)
    std::array<std::int32_t, kListSize> combat{};   // w9..13: combat sections (PickRandomCombat), -1 terminated
    std::array<std::int32_t, kListSize> stealth{};  // w14..18: calm sections (PickRandomStealth), -1 terminated
};

// One row of `MapDroneData` (12 x 16 bytes): the drone AI's music parameters for a level. The row with
// level id 0xFFFFFF9D (-99) is the alarm profile UpdateMusicalEvents switches to on Music_Event(2, 2).
struct MusicDroneParams {
    static constexpr std::uint32_t kAlarmProfile = 0xFFFFFF9Du;

    std::uint32_t level = 0;
    std::uint16_t attackers_needed = 2;  // drones that must see the player before the fight starts (MusicVars+0)
    std::uint16_t attackers_reset = 0;   // MusicVars+2
    std::int32_t seconds = 8;            // MusicVars+4: how long a sighting counts (x frame rate)
    float distance = 25.0f;              // MusicVars+8: a drone this close always counts
};

// `MapMusic` and `MapDroneData` read from ACTION.ELF. Throws FormatError.
class LevelMusicTable {
public:
    // `gamedir` holds ACTION.ELF.
    explicit LevelMusicTable(const std::filesystem::path& gamedir);

    const std::vector<LevelMusic>& levels() const { return levels_; }
    const LevelMusic* find(std::uint32_t level) const;

    // MapDroneData lookup by level id, or the built-in default {2, 0, 8, 25.0} UpdateMusicalEvents falls
    // back to when the level has no row.
    MusicDroneParams drone_params(std::uint32_t level) const;

private:
    std::vector<LevelMusic> levels_;
    std::vector<MusicDroneParams> drone_;
};

}  // namespace nf

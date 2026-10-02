#pragma once

// Single-player mission data from ACTION.ELF (`Mission_Init`, `Mission_Update`,
// `Mission_MonitorObjectives`, `sp_level`): the level order, per-level objectives (switch
// channels), and the mission state machine. All addresses/offsets cite the decompilation;
// this is a clean-room reimplementation, not a transcription.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "assets/elf.hpp"

namespace nf {

// ---- objectives (`Mission_MonitorObjectives` 0x182f30) ------------------------------------------
// One 24-byte record at `MissionData[n][3]` (a .data pointer). Only the fields the monitor reads
// are named; the rest is preserved raw:
//
//   +0  u32 label        objective text (`Txt_BindLabel`, 0x04...)
//   +4  u32 fail_label   "Mission Failed: <label>" suffix (0xFFFFFFFF = none)
//   +8  u32 spare        (never read by the monitor)
//   +12 u8  channel      the switch channel this objective watches (0 = none)
//   +13 u8  init         `Mission_Init` presets `switch_channels[channel] = init`
//   +14 u8  spare2
//   +15 u8  channel2     the announce channel of state 1 objectives (0/255 = none)
//   +16 u8  flags        bit 1 (0x02): the watched channel reads inverted
//   +20 u32 state        runtime: 0 announce-when-reached, 1 wait for channel2, 2 monitored,
//                        3 done-shown, 4 done, 5 failed (never stored on disc, always 0)
struct MissionObjective {
    std::uint32_t label = 0;
    std::uint32_t fail_label = 0;
    std::uint32_t spare = 0;
    std::uint8_t channel = 0;
    std::uint8_t init = 0;
    std::uint8_t spare2 = 0;
    std::uint8_t channel2 = 0;
    std::uint8_t flags = 0;

    bool inverted() const { return (flags & 2) != 0; }
};

// Objective runtime states (`Mission_MonitorObjectives`).
enum class ObjectiveState : std::uint32_t {
    Announce = 0,   // shown once the current level reaches this mission's level, then -> Monitored/Done
    WaitSecond = 1, // shown once channel2 is set, then -> Monitored/Done
    Monitored = 2,  // watched: channel loss fails the mission, channel hold completes it
    DoneShown = 3,  // completed this frame, message shown, then -> Done
    Done = 4,
    Failed = 5,
};

// ---- missions (`MissionData` @0x2a4350, 24 x 10 words; `Mission_Init` 0x182270) ------------------
//   [0] level hash (0x070000xx)   [1] base map hash   [2] order in the base map
//   [3] objectives pointer        [4] objective count
//   [8] music/spawn profile (always 0x5604 on disc)   [9] unlock flags
struct MissionEntry {
    std::uint32_t level = 0;
    std::uint32_t base = 0;
    std::uint32_t order = 0;
    std::vector<MissionObjective> objectives;
    std::uint32_t profile = 0;
    std::uint32_t unlock = 0;
};

// Loads all 24 entries (objectives resolved through the .data pointers).
std::vector<MissionEntry> load_mission_data(const Elf32& elf);

// ---- level order (`sp_level` @0x2df2e0, 12 x 0x18 `M_ITEM`; `load_sp_menu`) ----------------------
// `value` is the level id: 0x070000xx ACTION story maps, 0x0900000x DRIVING.ELF missions
// (Driving slice: 1 Paris, 2 SnowMobile, 3 Alps, 5 Underwater, 6 JungleA).
struct SpLevelRow {
    std::uint32_t sprite = 0;
    std::uint32_t name = 0;         // Txt label of the mission name
    std::uint32_t description = 0;  // Txt label of the briefing
    std::uint32_t level = 0;        // level id
    bool enabled = false;
    std::uint32_t disabled_label = 0;
};

std::vector<SpLevelRow> load_sp_level_order(const Elf32& elf);

// First ACTION story level in `sp_level` order (the headless-verification target).
inline std::uint32_t first_action_level(const std::vector<SpLevelRow>& rows) {
    for (const SpLevelRow& r : rows)
        if ((r.level & 0x0F000000) == 0x07000000) return r.level;
    return 0;
}

// ---- mission end destinations (`Mission_Update` 0x182d70, InternalState 6) -----------------------
// Failing a mission shows "Mission Failed" (label 0x02000006) for 4*VIDEO_FRAME_RATE frames, then
// the game ends to `LevelToEndTo`: the level hash with bit 0x300000 set. Only these levels map;
// anything else ends to 0x07000008 (`LevelToEndTo = 117440584`).
inline std::uint32_t mission_fail_destination(std::uint32_t level) {
    switch (level) {
    case 0x07000004:
    case 0x07000008:
    case 0x0700000b:
    case 0x0700000d:
    case 0x0700000f:
    case 0x07000013:
    case 0x07000016:
    case 0x0700001b:
        return level | 0x00300000;
    default:
        return 0x07000008;
    }
}

// Level id from a level bin name ("07000005.bin" -> 0x07000005, 0 when not a hex name).
std::uint32_t level_id_from_bin_name(const std::string& bin_name);

}  // namespace nf

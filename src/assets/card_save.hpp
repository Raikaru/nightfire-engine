// Real PS2 memory-card codename blob (1026 bytes) and its import into our NFPR profile.
//
// Layout (LS_GetSaveStuff IFF chunks, each u32 tag + u32 total size incl. the 8-byte header,
// then a 4-byte file trailer; all six chunks are fixed-size):
//   PLRS(11) PlrSettings bits, MSSN(68) Mission bits, MPSG(13) MP-settings bits,
//   GSET(905) GlobalSettings bits, CHET(9) cheat bits, BNUS(16) u64 reward mask.
// Bit streams are LSB-first from the chunk byte 8 on (BIN_PushBits order, cursor starts 0x40).
// Field order/widths mirror LS_Make*/LS_Load* in ACTION.ELF.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "assets/profile.hpp"
#include "assets/reader.hpp"

namespace nf {

// One decoded card blob. Field comments name the RAM/global each bit came from.
struct CardSave {
    bool valid = false;
    std::string error;
    std::uint32_t status = 0;  // MSSN: Menu_GetNightfireStatus word (12 sp_level bits)
    struct LevelResult {
        std::uint32_t score = 0;  // PlrStats score-table row (0 = unplayed)
        int medal = 0;            // bonus_kind 4-bit row value (0 none .. 4 platinum on real saves)
    };
    std::array<LevelResult, 12> levels{};  // row i = sp_level[i] (fixed order, count is always 12)
    std::uint64_t bonus = 0;               // BNUS: Menu_GetBonus reward mask (hi word first)
    std::array<bool, 3> cheats{};         // CHET: CheatInfo words bit0
    // PLRS bits in file order (PlayerSetting offsets).
    bool invert = false;            // +0
    int style_a = 0, style_b = 0;   // +0xe, +0x10 (4 bits each; style_a matches controller style 0-7)
    bool auto_aim = false;          // +1
    bool mp_auto_aim = false;       // +2
    bool manual_aim = false;        // +3
    bool weapon_auto_switch = false;  // +10
    bool crouch_toggle = false;       // +4
    bool vibration = false;           // +9
    bool crosshairs = false;          // +8
    int flashing = 0;                 // +0xc (2 bits)
    bool hud_always_on = false;       // +0xb
    // MPSG: radar + handicap of the single saved MP slot.
    bool mp_radar = false;
    std::int32_t mp_handicap = 0;
    // GSET.
    int music_volume = 0, sfx_volume = 0, language = 0;  // 0-100, 0-100, Txt language
    bool subtitles = false;                              // DrawInfo+8
    int speaker = 0;           // SFX mode (0 mono, 1 stereo, 2 surround)
    bool widescreen = false;   // DrawInfo+4
    int split_screen = 0;      // DrawInfo+0 (0 horizontal, 1 vertical)
    int screen_x = 0, screen_y = 0;
};

// Decodes a raw 1026-byte blob (valid=false + error on any structural problem).
CardSave decode_card_save(Bytes blob);

// Builds an NFPR profile from a decoded save. `level_ids[i]` is the sp_level[i].value
// (map/bin id) for MSSN bit/row i; difficulty has no card source and stays 1.
Profile card_save_to_profile(const CardSave& save, std::string name, const std::vector<std::uint32_t>& level_ids);

}  // namespace nf

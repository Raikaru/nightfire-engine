// Player profiles: the memory-card codename save as files under the user config dir.
// Field coverage matches the card sections (LS_Make*/LS_Load* in ACTION.ELF): Mission (nightfire
// status + per-level scores), Bonus (reward mask), GlobalSettings (volumes, DrawInfo, screen pos),
// MPSettings (per-slot radar/health), PlrSettings (PlayerSetting bytes + style), Cheats, plus the
// codename and difficulty. Format is our own versioned binary (magic "NFPR", version 1); corrupt
// files fail load with nullopt and the menus fall back to a fresh profile.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace nf {

// One per-level result (LS_MakeMission score table + medal kind from Menu_SetLevelBonus).
struct ProfileLevel {
    std::uint32_t level_id = 0;  // GameState level id (bin number)
    std::int32_t score = 0;      // PlrStat best score
    int medal = 0;               // 0 none, 1 bronze .. 4 platinum (bonus_kind)
};

// MP slot tail (LS_MakeMPSettings per-slot radar/health).
struct ProfileMpSlot {
    bool radar = false;          // MPSettings slot+0x28
    std::int32_t handicap = 0;   // MPSettings slot+0x2c
};

struct Profile {
    std::string name = "Bond";   // codename (8 chars max, P_CNNAME)
    std::uint32_t difficulty = 1;  // GameState difficulty 1..3
    std::uint32_t status = 0;      // Menu_GetNightfireStatus word (unlocked missions)
    std::vector<ProfileLevel> levels;
    std::uint64_t bonus = 0;       // Menu_GetBonus reward mask (Menu_SetBonus, unlocks)
    // GlobalSettings section.
    int sfx_volume = 100, music_volume = 70;
    bool subtitles = true;
    int split_screen = 0, speaker = 1;
    bool widescreen = false;
    int screen_x = 0, screen_y = 0;
    // PlrSettings section: PlayerSetting bytes 0..12 + controller style.
    std::array<std::uint8_t, 13> player_setting{};
    int controller_style = 7;
    bool invert_y = false;
    // MPSettings section: 8 slots.
    std::array<ProfileMpSlot, 8> mp_slots{};
    // Cheats section (CheatInfo words) + session tweak levels by cheat control id.
    std::array<std::uint32_t, 3> cheats{};
    std::vector<std::pair<std::uint32_t, int>> tweak_levels;

    bool completed(std::uint32_t level_id) const;
    int best_score(std::uint32_t level_id) const;
};

// Profile directory: $XDG_CONFIG_HOME/nightfire or ~/.config/nightfire.
std::filesystem::path profile_dir();
// All saved codenames (fresh profile when the dir is empty/missing).
std::vector<std::string> list_profiles();
// Load (nullopt when missing/corrupt) / store (creates the dir).
std::optional<Profile> load_profile(const std::string& codename);
bool save_profile(const Profile& profile);
bool delete_profile(const std::string& codename);
// Fresh-save defaults (boot PlayerSetting: crouch toggle on, style 7).
Profile fresh_profile(std::string name);

}  // namespace nf

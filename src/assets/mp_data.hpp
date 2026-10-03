#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "assets/elf.hpp"
#include "assets/game_files.hpp"
#include "assets/strings.hpp"

namespace nf {

// Multiplayer ("arena") setup tables of ACTION.ELF, see docs/formats.md "Multiplayer data". Every
// text is a label hash (Txt_BindLabel: `StringTable::label(hash)`), never English; sprites are the
// 0x03xxxxxx texture hashes of the front-end bin (`SpriteLibrary`).

// Scenario mode words (the `value` of the mp_scenario items, MPSettings+0x1a4). Bit 29 marks a
// team game (MPSettings+0x18c), bit 30 a scenario scored by a float objective counter
// (MPSettings+0x190, hill/uplink).
namespace mp_mode {
constexpr std::uint32_t kQuickGame = 0;  // menu entry 0 only: presets a 3-bot arena match, never a mode
constexpr std::uint32_t kArena = 0x1;
constexpr std::uint32_t kTopAgent = 0x10;
constexpr std::uint32_t kAssassination = 0x400;
constexpr std::uint32_t kTeamFlag = 1u << 29;
constexpr std::uint32_t kObjectiveFlag = 1u << 30;
constexpr std::uint32_t kTeamArena = kTeamFlag | 0x2;
constexpr std::uint32_t kCaptureTheFlag = kTeamFlag | 0x4;
constexpr std::uint32_t kDemolition = kTeamFlag | 0x40;
constexpr std::uint32_t kProtection = kTeamFlag | 0x80;
constexpr std::uint32_t kIndustrialEspionage = kTeamFlag | 0x100;
constexpr std::uint32_t kGoldenEyeStrike = kTeamFlag | 0x200;
constexpr std::uint32_t kKingOfTheHill = kObjectiveFlag | 0x800;
constexpr std::uint32_t kUplink = kTeamFlag | kObjectiveFlag | 0x8;
constexpr std::uint32_t kTeamKingOfTheHill = kTeamFlag | kObjectiveFlag | 0x1000;
}  // namespace mp_mode

// Sides. Teams are 0 = Phoenix, 1 = MI6 (labels 0x1c7 / 0x1c8); 2 = "no team" (bots of a non-team game).
constexpr std::uint32_t kMpTeamPhoenix = 0, kMpTeamMi6 = 1, kMpTeamNone = 2;
constexpr std::uint32_t kMpTeamLabels[2] = {0x1c7, 0x1c8};

constexpr std::size_t kMpMaxHumans = 4;      // Local controller/connection slots.
constexpr std::size_t kMpMaxBots = 12;       // Extended set: 16 total slots minus 4 humans.
constexpr std::size_t kMpPs2Slots = 8;
constexpr std::size_t kMpGcXboxSlots = 10;
constexpr std::size_t kMpSlots = 16;         // Storage capacity; ArenaSettings::slot_count is the active match limit.
constexpr std::size_t kMpBotTable = 10;      // Original `mpbots` table rows; extensions synthesize extra rows.
enum class MpRuleSet : std::uint8_t { Ps2, GcXbox, Extended };
constexpr std::size_t mp_rule_slot_limit(MpRuleSet rules) {
    switch (rules) {
    case MpRuleSet::Ps2: return kMpPs2Slots;
    case MpRuleSet::GcXbox: return kMpGcXboxSlots;
    case MpRuleSet::Extended: return kMpSlots;
    }
    return kMpPs2Slots;
}
constexpr std::size_t kMpCharacters = 29;    // mp_characters / mp_characters_small / MP_skins / default_bot_stats rows

// An M_ITEM of a menu wheel/list (0x18 bytes in ACTION.ELF): the shared row layout of mp_level,
// mp_scenario, mp_options, mp_bots, mp_characters and mp_characters_small.
struct MpMenuItem {
    std::uint32_t sprite;          // +0x00 thumbnail/portrait sprite hash
    std::uint32_t name;            // +0x04 label hash
    std::uint32_t description;     // +0x08 label hash (0 none)
    std::uint32_t value;           // +0x0c the payload the handlers read (level id, scenario mode, character index)
    bool enabled;                  // +0x10 selectable at boot (the unlock code clears/sets it later)
    std::uint32_t disabled_label;  // +0x14 label shown when a disabled item is chosen (0 none)
};

// Level (`mp_level`): `value` is the level id, its .bin is "<id as 8 hex digits>.bin".
struct MpMap {
    MpMenuItem item;
    std::string bin_name;
};

// BOT_stats_t (MPBOTS bytes 0..13 == default_bot_stats row, copied to BOT_vars+0xa0 by BOT_init and
// applied by BOT_setDroneStats). Field names come from the P_MPBOTSETUP controls; unnamed bytes are
// kept raw.
struct BotStats {
    std::uint8_t accuracy;       // +0 "Accuracy Rating": 8 poor, 5 average, 3 good, 1 very good (aim error, lower is better)
    std::uint16_t aggression;    // +2 "Aggression": 2 normal, 3 high, 4 very high
    std::uint16_t health;        // +4 "Health": 50..300
    std::uint8_t move_speed;     // +6 "Move Speed": 0 slow, 1 normal, 2 fast
    std::uint8_t reaction_time;  // +7 "Reaction Time" percent 50..200
    std::uint8_t recovery_rate;  // +8 "Recovery Rate" percent 50..200
    std::uint8_t evil;           // +9 0 = MI6 character (Menu_IsBotGood), 1 = Phoenix side
    std::uint8_t raw_a;          // +10 (boss characters only)
    std::uint8_t personality;    // +11 "Personality": 0 none; good 1 Collector 2 Guardian 3 Team Player 4 Judge; evil 5 Berserker 6 Greedy 7 Vengeful 8 Assassin
    std::uint8_t ability_flags;  // +12 boss abilities (bits tested by the drone hit code: 1, 2, 4, 8, 0x10)
    std::uint8_t raw_b;          // +13 1 for the 15 regular characters, 0 for the fixed boss/extra characters
};
constexpr std::size_t kBotStatsSize = 14;

// One row of MP_skins (16 bytes): which model/skin and chunk file a character needs on load
// (MP_NeedSkin / MP_NeedFile / MP_setLoadingSkins).
struct MpSkin {
    std::uint32_t skin_hash;  // +0 model/skin hash (0x05xxxxxx)
    std::uint32_t file_hash;  // +4 chunk file hash (0x01xxxxxx) holding the model
    std::uint32_t kind;       // +8 (4, 5 or 6)
};

// A playable character (human skin and bot). Indices are the `value` field: the character index used
// in MPSettings slots, mpjoin, mpbots and every table below.
struct MpCharacter {
    std::uint32_t index;
    MpMenuItem large;          // mp_characters (portrait used in the skin wheel / bot list)
    std::uint32_t small_sprite;  // mp_characters_small[index].sprite (confirm/debrief portrait)
    std::uint32_t short_name;  // Menu_GetBotShortName label
    BotStats stats;            // default_bot_stats
    MpSkin skin;               // MP_skins
    bool good() const { return stats.evil == 0; }                                   // Menu_IsBotGood
    bool bond() const { return index == 0 || index == 12 || index == 14; }          // the three Bond outfits
};

// Scenario (`mp_scenario`): `item.value` is the mode word (mp_mode::*), item 0 is the Quick Game entry.
struct MpScenario {
    MpMenuItem item;
};

// One option of a rule control: `label` is a label hash, or 0 when the control shows the number itself.
struct MpChoice {
    std::int32_t value;
    std::uint32_t label;
};

// A rule (a radio control of the P_MPRULES / P_MPPLAYERMODS / P_MPENVIROMODS pages): the control id
// the handlers address, its caption label, the choices in list order and the default the boot code
// (bootup_bootup) stores in MPSettings.
enum class MpRule : std::uint8_t {
    Duration,          // P_MPRULES 0x10000008, MPSettings+0x1a0
    ScoreLimit,        // P_MPRULES 0x100000f6, +0x19c ("Points", "Lives" for Top Agent)
    FriendlyFire,      // P_MPPLAYERMODS 0x10000081, +0x198
    WeaponSet,         // 0x10000084, +0x1b4
    ProfessionalMode,  // 0x10000080, +0x1bc
    LocationDamage,    // 0x1000008c, +0x1c8
    TeamId,            // 0x1000008d, +0x1c4
    Respawn,           // P_MPENVIROMODS 0x100000a2, +0x1c0
    FixedGuns,         // 0x1000009f, +0x1b8
    ExplosiveScenery,  // 0x1000019b, +0x1d4 (a "Locked" placeholder until the reward unlocks it)
    Grapple,           // 0x10000227, +0x1d0
    MiniVehicles,      // 0x10000228, +0x1cc
    Count
};
constexpr std::size_t kMpRuleCount = std::size_t(MpRule::Count);

struct MpRuleInfo {
    MpRule rule;
    std::uint32_t control_id;
    std::uint32_t caption;       // label hash of the row caption
    std::uint32_t page;          // page id the control lives on (0x40000014 rules, 0x40000017 player mods, 0x40000028 enviro-mods)
    std::vector<MpChoice> choices;
    std::int32_t default_value;  // bootup_bootup
};

// One BOT_stats_t control of P_MPBOTSETUP (0x1000018x): choices in list order.
enum class BotStat : std::uint8_t { Enabled, Accuracy, Aggression, Health, MoveSpeed, Personality, ReactionTime, RecoveryRate, Count };
struct BotStatInfo {
    BotStat stat;
    std::uint32_t control_id;
    std::uint32_t caption;  // label hash
    std::vector<MpChoice> choices;  // Personality's list is per side, see MpData::personality_choices
};

// A default codename (`def_codename`, 0x38 bytes/entry, Menu_MapDefaultCodename copies it into the live
// settings). Only the multiplayer relevant fields are decoded; audio and single-player state are kept out.
struct MpCodename {
    std::uint32_t name;          // +0x00 label hash
    std::uint64_t bonus;         // +0x08 reward bit mask (Menu_SetBonus)
    std::int16_t handicap;       // +0x1c health handicap percent (MPSettings slot +0x2c)
    std::uint8_t hud;            // +0x1a MPSettings slot +0x28
    std::uint8_t professional;   // +0x2e MPSettings+0x1bc
    std::uint8_t respawn;        // +0x2f MPSettings+0x1c0
    std::uint8_t team_id;        // +0x30 MPSettings+0x1c4
};

// RewardsTable (12 rows x 0x40 bytes, ACTION.ELF): per single-player level (`level_id`) the reward of each
// of the four counters c = 1..4: {type, id}. Types: 1 = MP character, 2 = MP scenario, 3 = enviro mod,
// 4 = counted group, 0 = other. A reward is earned when bit `id` of the codename's bonus mask is set.
struct MpReward {
    std::uint16_t type;
    std::uint16_t id;
};
struct MpRewardRow {
    std::uint32_t level_id;
    std::array<MpReward, 4> counters;  // counters 1..4
};

struct MpData {
    std::vector<MpMap> maps;            // 8 levels (mp_level)
    std::vector<MpScenario> scenarios;  // 13 entries (mp_scenario), [0] = Quick Game
    std::vector<MpMenuItem> options;    // 5 entries (mp_options): Continue, AI Bots, Game Rules, Player Mods, Enviro-Mods
    std::vector<MpMenuItem> bot_menu;   // 17 entries (mp_bots): Continue, Setup Bot 1..16 (only 1..4 are used)
    std::vector<MpCharacter> characters;  // 29
    std::vector<MpRewardRow> rewards;   // RewardsTable
    std::vector<MpCodename> codenames;  // def_codename (2)
    std::array<MpRuleInfo, kMpRuleCount> rules;
    std::vector<BotStatInfo> bot_stats;  // P_MPBOTSETUP controls
    std::vector<MpChoice> personality_good, personality_evil;
    std::vector<MpChoice> handicap;      // C_RBMPSETUP health handicap wheel (-75 .. +100)

    const MpRuleInfo& rule(MpRule r) const { return rules[std::size_t(r)]; }
    const MpMap* find_map(std::uint32_t level_id) const;
    const MpScenario* find_scenario(std::uint32_t mode) const;
    const MpCharacter* find_character(std::uint32_t index) const;
};
// Menu_UnlockMPSkins / Menu_UnlockMPSettings: what a type 1 / 2 / 3 reward id unlocks. Rewards 0x26..0x37
// unlock a character (0x2c unlocks none), 0x38..0x3d a scenario (row of mp_scenario) and 0x3e the
// "Explosive Scenery" enviro-mod.
std::optional<std::uint32_t> mp_reward_character(std::uint16_t id);
std::optional<std::size_t> mp_reward_scenario(std::uint16_t id);
constexpr std::uint16_t kMpRewardExplosiveScenery = 0x3e;

// Reads the tables from `<gamedir>/ACTION.ELF`. Throws FormatError when a table is missing/mis-sized, a
// level bin named by mp_level is not in FILES.BIN, or a label does not resolve in `strings`.
MpData load_mp_data(GameFiles& files, const std::filesystem::path& gamedir, const StringTable& strings);

// The deeper consistency checks validate_mp runs (reads the level bins): every map's bin has a Map entry,
// every character's skin chunk file is in every map bin, tables agree with each other. One line per problem.
std::vector<std::string> check_mp_data(const MpData& data, GameFiles& files);

}  // namespace nf

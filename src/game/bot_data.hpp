#pragma once

// Multiplayer bot data (docs/spec-arena-ai.md Part 2A §1): BOT_stats_t helpers, personalities, flags,
// genders/voices, the MPBOTS entry a match is started from, BOT_getDefaultStats. The 29-row stat table itself
// (`default_bot_stats` @0x26d2f0) and MP_skins (@0x26d488) are decoded from ACTION.ELF by `load_mp_data`
// (assets/mp_data.hpp: `MpCharacter::stats` / `::skin`); nfdump validate cross-checks them against the spec dump.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "assets/mp_data.hpp"

namespace nf::bots {

// BOT_stats_t+0x0b personality (Menu personality wheel values; BOT_vars+0xab).
enum class Personality : std::uint8_t {
    None = 0, Collector, Guardian, TeamPlayer, Judge, Berserker, Greedy, Vengeful, Assassin,
};
inline Personality personality_of(const BotStats& s) { return Personality(s.personality); }
std::string_view personality_name(Personality p);

// BOT_stats_t+0x0c built-in flag bits.
namespace botflag {
constexpr std::uint8_t kFastAimRefresh = 0x01;   // NDrone2_SetOpponentAimPos refresh 1/4 (Scaramanga)
constexpr std::uint8_t kRangedBoost = 0x02;      // NDrone2_DoHitEffects: ranged damage x1.25 (Wai Lin, Xenia)
constexpr std::uint8_t kMelee = 0x04;            // fists at <= 1.5, melee damage x1.5 (Jaws, Oddjob)
constexpr std::uint8_t kAware = 0x08;            // omniscient sight radius 300, alerted when seen (Samedi)
constexpr std::uint8_t kRegen = 0x10;            // +5 health / second up to max health (Samedi)
}  // namespace botflag

constexpr int kCharacterCount = 29;
constexpr int kOddjob = 26;      // char 26 owns the hat weapon 0x45
constexpr int kSamedi = 25;
constexpr int kJaws = 24;

// English short names of the character indexes (Menu_GetBotShortName), used for logs and `--bot-char`.
std::string_view character_name(int character);
// Index by exact index string ("3") or by case-insensitive name/prefix ("kiko", "black ops", "bond"); -1 unknown.
int parse_character(std::string_view text);

// Quick-Game defaults (C_SBMPSCEN 0x51/..): bots 1..3 are Drake, Kiko, Rook.
constexpr int kQuickGameCharacters[3] = {1, 3, 2};

// BOT_setGender @0x126718: Drone+0x15 = 1 (female voice) for Kiko, Alura, Dominique, Galore .. Elektra.
bool bot_is_female(int character);

// BOT_getMovementSpeedMul @0x126568.
float movement_speed_mul(std::uint8_t speed_stat);
// BOT_getAggressionMul @0x126468 without the close-range bonus (that one needs the opponent distance).
float aggression_mul_base(std::uint8_t aggression_stat);
// Multiplier applied when an opponent is closer than 2.5 units in MP: min(3, (2.5 - d) + 1).
float aggression_mul(std::uint8_t aggression_stat, bool has_opponent, float opponent_dist);

// DroneWeap_BurstDelay's aggression scale {0:1.66, 1:1.33, 2:1.0, 3:0.66, 4:0.33}.
float burst_delay_aggression_scale(std::uint8_t aggression_stat);

// BOT_init create-info +0x64 skill class (semantics unresolved in the original, kept for completeness).
int skill_class(std::uint8_t accuracy);

// BOTSTATE_startRecovery / BotImpactStunGrenade times, in ticks of the original 30 Hz-normalised clock
// (`Drone+0xb9 * 0.6666667` and `Drone+0xb9 * 4`).
std::uint32_t recovery_ticks(std::uint8_t recovery_stat);
std::uint32_t stun_ticks(std::uint8_t recovery_stat);

// BOT_soundEffect @0x126618 (sfx ids are decimal in the pseudocode): pain 333 + Rand(5) (male) / 338 + Rand(2)
// (female); death 340 + Rand(3) (male) / 343 + Rand(3) (female).
struct BotVoice {
    int first;
    int count;
};
BotVoice pain_voice(bool female);
BotVoice death_voice(bool female);

// One entry of MPBOTS (`mpbots` +2 + i*0x12) in the shape a match is started from.
struct BotSpec {
    int slot = 4;                    // MPSettings/MPGame participant slot; legacy bots start at 4.
    int character = 1;               // 0..28
    BotStats stats{};                // BOT_stats_t (default_bot_stats[character] unless customised)
    int team = 2;                    // MPSettings[slot]+0x20 after BOT_init: 0 Phoenix, 1 MI6, 2 no team
    std::string name;                // slot name text
};

// BOT_init default entry (no MPBOTS): stats = default_bot_stats[character]; team = IsBotGood(char) ? 1 : 0.
BotSpec default_bot_spec(const MpData& data, int bot_index, int character, bool team_game);
// The Quick-Game roster: 3 bots (Drake, Kiko, Rook).
std::vector<BotSpec> quick_game_roster(const MpData& data, bool team_game);

}  // namespace nf::bots

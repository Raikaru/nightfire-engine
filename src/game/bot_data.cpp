#include "game/bot_data.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace nf::bots {

namespace {

constexpr std::string_view kNames[kCharacterCount] = {
    "Bond",        "Drake",     "Rook",       "Kiko",       "Alura",     "Dominique",  "Snow Guard",
    "Black Ops",   "Yakuza",    "Phoenix Commando", "Phoenix Soldier", "Ninja", "Bond Tux", "Drake Suit",
    "Bond (3rd)",  "Goldfinger", "Renard",    "Scaramanga", "Galore",    "Xmas Jones", "Wai Lin",
    "Xenia",       "May Day",   "Elektra",    "Jaws",       "Samedi",    "Oddjob",     "Nik Nack",
    "Zorin",
};

std::string lower(std::string_view s) {
    std::string r(s);
    std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return r;
}

}  // namespace

std::string_view personality_name(Personality p) {
    switch (p) {
    case Personality::None: return "none";
    case Personality::Collector: return "Collector";
    case Personality::Guardian: return "Guardian";
    case Personality::TeamPlayer: return "TeamPlayer";
    case Personality::Judge: return "Judge";
    case Personality::Berserker: return "Berserker";
    case Personality::Greedy: return "Greedy";
    case Personality::Vengeful: return "Vengeful";
    case Personality::Assassin: return "Assassin";
    }
    return "?";
}

std::string_view character_name(int character) {
    return character >= 0 && character < kCharacterCount ? kNames[character] : std::string_view("?");
}

int parse_character(std::string_view text) {
    if (text.empty()) return -1;
    if (std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c); })) {
        int v = std::atoi(std::string(text).c_str());
        return v < kCharacterCount ? v : -1;
    }
    const std::string want = lower(text);
    for (int i = 0; i < kCharacterCount; ++i)
        if (lower(kNames[i]) == want) return i;
    for (int i = 0; i < kCharacterCount; ++i)
        if (lower(kNames[i]).rfind(want, 0) == 0) return i;
    return -1;
}

bool bot_is_female(int c) { return (c >= 3 && c < 6) || (c >= 18 && c < 24); }

float movement_speed_mul(std::uint8_t s) {
    switch (s) {
    case 0: return 0.7f;   // 0x3f333333
    case 2: return 1.3f;   // 0x3fa66666
    default: return 1.0f;
    }
}

float aggression_mul_base(std::uint8_t a) {
    switch (a) {
    case 0: return 0.3f;
    case 1: return 0.5f;
    case 2: return 0.7f;
    case 3: return 0.85f;
    default: return 1.0f;
    }
}

float aggression_mul(std::uint8_t a, bool has_opponent, float dist) {
    const float base = aggression_mul_base(a);
    if (has_opponent && dist < 2.5f) return base * std::min(3.0f, (2.5f - dist) + 1.0f);
    return base;
}

float burst_delay_aggression_scale(std::uint8_t a) {
    switch (a) {
    case 0: return 1.66f;
    case 1: return 1.33f;
    case 2: return 1.0f;
    case 3: return 0.66f;
    case 4: return 0.33f;
    default: return 1.0f;
    }
}

int skill_class(std::uint8_t v) {
    if (v < 3) return 3;
    if (v < 6) return 0;
    if (v < 9) return 2;
    return 1;
}

std::uint32_t recovery_ticks(std::uint8_t v) { return std::uint32_t(float(v) * 0.6666667f); }
std::uint32_t stun_ticks(std::uint8_t v) { return std::uint32_t(float(v) * 4.0f); }

BotVoice pain_voice(bool female) { return female ? BotVoice{338, 2} : BotVoice{333, 5}; }
BotVoice death_voice(bool female) { return female ? BotVoice{343, 3} : BotVoice{340, 3}; }

BotSpec default_bot_spec(const MpData& data, int bot_index, int character, bool team_game) {
    const MpCharacter* c = data.find_character(std::uint32_t(character));
    BotSpec s;
    s.slot = 4 + bot_index;
    s.character = character;
    if (c) s.stats = c->stats;
    // C_SBMPBTCHOOSE: team = IsBotGood(char) (good -> 1 MI6, evil -> 0 Phoenix); BOT_init forces 2 (none)
    // when teams are off.
    s.team = team_game ? (s.stats.evil == 0 ? 1 : 0) : 2;
    s.name = std::string(character_name(character));
    return s;
}

std::vector<BotSpec> quick_game_roster(const MpData& data, bool team_game) {
    std::vector<BotSpec> r;
    for (int i = 0; i < 3; ++i) r.push_back(default_bot_spec(data, i, kQuickGameCharacters[i], team_game));
    return r;
}

}  // namespace nf::bots

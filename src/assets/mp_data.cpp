#include "assets/mp_data.hpp"

#include <algorithm>
#include <cstdio>

#include "assets/bin_archive.hpp"

namespace nf {

namespace {

constexpr std::size_t kItemSize = 0x18;
constexpr std::size_t kSkinSize = 0x10;
constexpr std::size_t kCodenameSize = 0x38;
constexpr std::size_t kRewardRowSize = 0x40;

Bytes table(const Elf32& elf, std::string_view name, std::size_t entry_size, std::size_t count) {
    auto sym = elf.symbol(name);
    if (!sym) throw FormatError("ACTION.ELF has no " + std::string(name) + " symbol");
    if (sym->size != entry_size * count)
        throw FormatError(std::string(name) + " is " + std::to_string(sym->size) + " bytes, expected " +
                          std::to_string(entry_size * count));
    return elf.at(sym->value, sym->size);
}

MpSkin load_skin(Bytes skins, std::size_t index) {
    const std::size_t offset = index * kSkinSize;
    return {load<std::uint32_t>(skins, offset), load<std::uint32_t>(skins, offset + 4),
            load<std::uint32_t>(skins, offset + 8)};
}

MpMenuItem load_item(Bytes t, std::size_t index) {
    std::size_t o = index * kItemSize;
    MpMenuItem item;
    item.sprite = load<std::uint32_t>(t, o);
    item.name = load<std::uint32_t>(t, o + 4);
    item.description = load<std::uint32_t>(t, o + 8);
    item.value = load<std::uint32_t>(t, o + 12);
    std::uint32_t enabled = load<std::uint32_t>(t, o + 16);
    if (enabled > 1) throw FormatError("menu item enable flag is not a bool");
    item.enabled = enabled != 0;
    item.disabled_label = load<std::uint32_t>(t, o + 20);
    return item;
}

std::vector<MpMenuItem> load_items(const Elf32& elf, std::string_view name, std::size_t count) {
    Bytes t = table(elf, name, kItemSize, count);
    std::vector<MpMenuItem> items;
    for (std::size_t i = 0; i < count; ++i) items.push_back(load_item(t, i));
    return items;
}

BotStats load_stats(Bytes t, std::size_t o) {
    BotStats s;
    s.accuracy = load<std::uint8_t>(t, o);
    s.aggression = load<std::uint16_t>(t, o + 2);
    s.health = load<std::uint16_t>(t, o + 4);
    s.move_speed = load<std::uint8_t>(t, o + 6);
    s.reaction_time = load<std::uint8_t>(t, o + 7);
    s.recovery_rate = load<std::uint8_t>(t, o + 8);
    s.evil = load<std::uint8_t>(t, o + 9);
    s.raw_a = load<std::uint8_t>(t, o + 10);
    s.personality = load<std::uint8_t>(t, o + 11);
    s.ability_flags = load<std::uint8_t>(t, o + 12);
    s.raw_b = load<std::uint8_t>(t, o + 13);
    return s;
}

// Menu_GetBotShortName: label 0x10002e2 + character index.
constexpr std::uint32_t kShortNameBase = 0x10002e2;

// The P_MPRULES / P_MPPLAYERMODS / P_MPENVIROMODS control lists are immediates of the page handlers
// (Menu_Send message 0x10 = add item: label/text, value); the numeric rows show the value as text.
constexpr std::uint32_t kOn = 0x1b7, kOff = 0x1b6;

std::vector<MpChoice> on_off() { return {{1, kOn}, {0, kOff}}; }

std::vector<MpChoice> steps(std::int32_t first, std::int32_t last, std::int32_t step) {
    std::vector<MpChoice> v;
    for (std::int32_t x = first; x <= last; x += step) v.push_back({x, 0});
    return v;
}

std::vector<MpChoice> concat(std::vector<MpChoice> a, const std::vector<MpChoice>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

std::array<MpRuleInfo, kMpRuleCount> make_rules() {
    constexpr std::uint32_t kRules = 0x40000014, kPlayer = 0x40000017, kEnviro = 0x40000028;
    // Durations: 1..10, 15..60 minutes, Unlimited (label 0x3ca, value -1).
    auto duration = concat(concat(steps(1, 10, 1), steps(15, 60, 5)), {{-1, 0x3ca}});
    // Score limit: 1..10, 15..120.
    auto score = concat(steps(1, 10, 1), steps(15, 120, 5));
    std::vector<MpChoice> weapon_sets;
    {
        const std::uint32_t labels[11] = {0x1a0, 0x1a2, 0x1a4, 0x1a6, 0x1a8, 0x24f, 0x3db, 0x3dc, 0x3dd, 0x3de, 0x1b2};
        for (std::int32_t i = 0; i < 11; ++i) weapon_sets.push_back({i, labels[i]});
    }
    std::array<MpRuleInfo, kMpRuleCount> r;
    auto put = [&](MpRule id, std::uint32_t control, std::uint32_t caption, std::uint32_t page,
                   std::vector<MpChoice> choices, std::int32_t def) {
        r[std::size_t(id)] = {id, control, caption, page, std::move(choices), def};
    };
    // Defaults are bootup_bootup's stores into MPSettings.
    put(MpRule::Duration, 0x10000008, 0x156, kRules, duration, 10);
    put(MpRule::ScoreLimit, 0x100000f6, 0x24c, kRules, score, 10);
    put(MpRule::FriendlyFire, 0x10000081, 0x1ae, kPlayer, on_off(), 0);
    put(MpRule::WeaponSet, 0x10000084, 0x24d, kPlayer, weapon_sets, 0);
    put(MpRule::ProfessionalMode, 0x10000080, 0x1ac, kPlayer, on_off(), 0);
    put(MpRule::LocationDamage, 0x1000008c, 0x24e, kPlayer, on_off(), 1);
    put(MpRule::TeamId, 0x1000008d, 0x1e5, kPlayer, on_off(), 1);
    put(MpRule::Respawn, 0x100000a2, 0x1af, kEnviro, {{0, 0x1b0}, {1, 0x1b1}, {2, 0x1b2}}, 2);
    put(MpRule::FixedGuns, 0x1000009f, 0x1ab, kEnviro, on_off(), 0);
    put(MpRule::ExplosiveScenery, 0x1000019b, 0x1aa, kEnviro, on_off(), 0);
    put(MpRule::Grapple, 0x10000227, 0x328, kEnviro, on_off(), 0);
    put(MpRule::MiniVehicles, 0x10000228, 0x5b9, kEnviro, {{0, 0x1b6}, {1, 0x10002d8}, {2, 0x10002d9}, {3, 0x1b2}}, 0);
    return r;
}

std::vector<BotStatInfo> make_bot_stats() {
    // P_MPBOTSETUP 0x4c fills these controls in this order.
    auto percent = steps(50, 200, 25);
    return {
        {BotStat::Enabled, 0x10000116, 0x295, {{1, 0x181}, {0, 0x182}}},
        {BotStat::Accuracy, 0x10000186, 0x15e, {{8, 0x241}, {5, 0x242}, {3, 0x243}, {1, 0x244}}},
        {BotStat::Aggression, 0x10000185, 0x246, {{2, 0x3bd}, {3, 0x31b}, {4, 0x31c}}},
        {BotStat::Health, 0x10000187, 0x1c6,
         {{50, 0}, {75, 0}, {100, 0}, {125, 0}, {150, 0}, {175, 0}, {200, 0}, {250, 0}, {300, 0}}},
        {BotStat::MoveSpeed, 0x1000018c, 0x247, {{0, 0x23c}, {1, 0x23d}, {2, 0x23e}}},
        {BotStat::Personality, 0x10000195, 0x232, {}},
        {BotStat::ReactionTime, 0x10000188, 0x23a, percent},
        {BotStat::RecoveryRate, 0x1000018a, 0x249, percent},
    };
}

std::string bin_name_for(std::uint32_t level_id) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%08x.bin", level_id);
    return buf;
}

// Menu_UnlockMPSkins' switch: reward id -> character index (offsets into mp_characters / 0x18).
constexpr std::pair<std::uint16_t, std::uint32_t> kRewardCharacters[] = {
    {0x26, 26}, {0x27, 12}, {0x28, 20}, {0x29, 27}, {0x2a, 17}, {0x2b, 24}, {0x2d, 25}, {0x2e, 21}, {0x2f, 15},
    {0x30, 22}, {0x31, 23}, {0x32, 19}, {0x33, 18}, {0x34, 16}, {0x35, 28}, {0x36, 13}, {0x37, 14},
};
// Menu_UnlockMPSettings' switch: reward id -> mp_scenario row.
constexpr std::pair<std::uint16_t, std::size_t> kRewardScenarios[] = {
    {0x38, 10}, {0x39, 4}, {0x3a, 12}, {0x3b, 6}, {0x3c, 7}, {0x3d, 9},
};

}  // namespace

MpSkin mp_skin_for_character(const Elf32& action_elf, std::uint32_t character_index) {
    if (character_index >= kMpCharacters) throw FormatError("MP character index is out of range");
    const Bytes skins = table(action_elf, "MP_skins", kSkinSize, kMpCharacters);
    return load_skin(skins, character_index);
}

std::optional<std::uint32_t> mp_reward_character(std::uint16_t id) {
    for (const auto& [reward, character] : kRewardCharacters)
        if (reward == id) return character;
    return std::nullopt;
}

std::optional<std::size_t> mp_reward_scenario(std::uint16_t id) {
    for (const auto& [reward, scenario] : kRewardScenarios)
        if (reward == id) return scenario;
    return std::nullopt;
}

const MpMap* MpData::find_map(std::uint32_t level_id) const {
    auto it = std::ranges::find_if(maps, [&](const MpMap& m) { return m.item.value == level_id; });
    return it == maps.end() ? nullptr : &*it;
}

const MpScenario* MpData::find_scenario(std::uint32_t mode) const {
    auto it = std::ranges::find_if(scenarios, [&](const MpScenario& s) { return s.item.value == mode; });
    return it == scenarios.end() ? nullptr : &*it;
}

const MpCharacter* MpData::find_character(std::uint32_t index) const {
    return index < characters.size() ? &characters[index] : nullptr;
}

MpData load_mp_data(GameFiles& files, const std::filesystem::path& gamedir, const StringTable& strings) {
    Elf32 elf(read_file(gamedir / "ACTION.ELF"));
    MpData d;

    for (auto& item : load_items(elf, "mp_level", 8)) {
        MpMap m{item, bin_name_for(item.value)};
        if (!files.find(m.bin_name)) throw FormatError("mp_level names " + m.bin_name + ", which is not in FILES.BIN");
        d.maps.push_back(std::move(m));
    }
    for (auto& item : load_items(elf, "mp_scenario", 13)) d.scenarios.push_back({item});
    d.options = load_items(elf, "mp_options", 5);
    d.bot_menu = load_items(elf, "mp_bots", 17);

    auto large = load_items(elf, "mp_characters", kMpCharacters);
    auto small = load_items(elf, "mp_characters_small", kMpCharacters);
    Bytes stats = table(elf, "default_bot_stats", kBotStatsSize, kMpCharacters);
    Bytes skins = table(elf, "MP_skins", kSkinSize, kMpCharacters);
    for (std::uint32_t i = 0; i < kMpCharacters; ++i) {
        if (large[i].value != i || small[i].value != i)
            throw FormatError("mp_characters row " + std::to_string(i) + " has value " + std::to_string(large[i].value));
        MpCharacter c;
        c.index = i;
        c.large = large[i];
        c.small_sprite = small[i].sprite;
        c.short_name = kShortNameBase + i;
        c.stats = load_stats(stats, i * kBotStatsSize);
        c.skin = load_skin(skins, i);
        d.characters.push_back(c);
    }

    Bytes rewards = table(elf, "RewardsTable", kRewardRowSize, 12);
    for (std::size_t row = 0; row < 12; ++row) {
        MpRewardRow r;
        r.level_id = load<std::uint32_t>(rewards, row * kRewardRowSize);
        // ProcessRewardCounter(counter c): {id u16 at +4+4c, type u16 at +6+4c}.
        for (std::size_t c = 1; c <= 4; ++c) {
            r.counters[c - 1].id = load<std::uint16_t>(rewards, row * kRewardRowSize + 4 + 4 * c);
            r.counters[c - 1].type = load<std::uint16_t>(rewards, row * kRewardRowSize + 6 + 4 * c);
            if (r.counters[c - 1].type > 4) throw FormatError("reward type out of range");
        }
        d.rewards.push_back(r);
    }

    Bytes codenames = table(elf, "def_codename", kCodenameSize, 2);
    for (std::size_t i = 0; i < 2; ++i) {
        std::size_t o = i * kCodenameSize;
        MpCodename c;
        c.name = load<std::uint32_t>(codenames, o);
        c.bonus = load<std::uint64_t>(codenames, o + 8);
        c.hud = load<std::uint8_t>(codenames, o + 0x1a);
        c.handicap = load<std::int16_t>(codenames, o + 0x1c);
        c.professional = load<std::uint8_t>(codenames, o + 0x2e);
        c.respawn = load<std::uint8_t>(codenames, o + 0x2f);
        c.team_id = load<std::uint8_t>(codenames, o + 0x30);
        d.codenames.push_back(c);
    }

    d.rules = make_rules();
    d.bot_stats = make_bot_stats();
    // P_MPBOTSETUP's Personality list depends on the character's side.
    d.personality_good = {{0, 0x10002d0}, {4, 0x24a}, {1, 0x233}, {2, 0x234}, {3, 0x235}};
    d.personality_evil = {{0, 0x10002d0}, {5, 0x237}, {6, 0x238}, {7, 0x239}, {8, 0x24b}};
    // C_RBMPSETUP's handicap wheel: -75..+100 percent in steps of 25.
    d.handicap = steps(-75, 100, 25);

    // Every label the tables name must resolve.
    auto require = [&](std::uint32_t hash, const char* what) {
        if (hash && strings.label(hash).empty())
            throw FormatError(std::string(what) + " label " + std::to_string(hash) + " does not resolve");
    };
    for (const auto& m : d.maps) {
        require(m.item.name, "map name");
        require(m.item.description, "map description");
    }
    for (const auto& s : d.scenarios) {
        require(s.item.name, "scenario name");
        require(s.item.description, "scenario description");
        require(s.item.disabled_label, "scenario locked");
    }
    for (const auto& o : d.options) require(o.name, "option name"), require(o.description, "option description");
    for (const auto& o : d.bot_menu) require(o.name, "bot menu name");
    for (const auto& c : d.characters) {
        require(c.large.name, "character name");
        require(c.short_name, "character short name");
        require(c.large.disabled_label, "character locked");
    }
    for (const auto& r : d.rules) {
        require(r.caption, "rule caption");
        for (const auto& ch : r.choices) require(ch.label, "rule choice");
    }
    require(0x1000076, "locked placeholder");
    for (const auto& b : d.bot_stats) {
        require(b.caption, "bot stat caption");
        for (const auto& ch : b.choices) require(ch.label, "bot stat choice");
    }
    for (const auto& ch : d.personality_good) require(ch.label, "personality");
    for (const auto& ch : d.personality_evil) require(ch.label, "personality");
    for (const auto& c : d.codenames) require(c.name, "codename");
    return d;
}

std::vector<std::string> check_mp_data(const MpData& data, GameFiles& files) {
    std::vector<std::string> problems;
    std::vector<std::vector<std::uint8_t>> bins;
    for (const auto& m : data.maps) {
        const GameFile* f = files.find(m.bin_name);
        if (!f) {
            problems.push_back(m.bin_name + ": not in FILES.BIN");
            continue;
        }
        bins.push_back(files.read(*f));
        bool has_map = false;
        std::vector<std::uint32_t> hashes;
        try {
            for (const auto& e : parse_bin_archive(Bytes(bins.back()))) {
                has_map |= e.type == EntryType::Map;
                hashes.push_back(e.hash);
            }
        } catch (const std::exception& e) {
            problems.push_back(m.bin_name + ": " + e.what());
            continue;
        }
        if (!has_map) problems.push_back(m.bin_name + ": no Map entry");
        for (const auto& c : data.characters)
            if (std::ranges::find(hashes, c.skin.file_hash) == hashes.end())
                problems.push_back(m.bin_name + ": no chunk file " + std::to_string(c.skin.file_hash) + " for character " +
                                   std::to_string(c.index));
    }

    // Reward ids must all map to something, and every mapped id must appear in the table.
    std::vector<std::uint16_t> seen;
    for (const auto& row : data.rewards)
        for (const auto& r : row.counters) {
            if (r.type == 1 && r.id != 0x2c && !mp_reward_character(r.id))
                problems.push_back("character reward " + std::to_string(r.id) + " unlocks nothing");
            if (r.type == 2 && !mp_reward_scenario(r.id))
                problems.push_back("scenario reward " + std::to_string(r.id) + " unlocks nothing");
            if (r.type == 3 && r.id != kMpRewardExplosiveScenery)
                problems.push_back("enviro reward " + std::to_string(r.id) + " unlocks nothing");
            if (r.type >= 1 && r.type <= 3) seen.push_back(r.id);
        }
    for (const auto& [id, ch] : kRewardCharacters)
        if (std::ranges::find(seen, id) == seen.end()) problems.push_back("no level awards character reward " + std::to_string(id));
    for (const auto& [id, s] : kRewardScenarios)
        if (std::ranges::find(seen, id) == seen.end()) problems.push_back("no level awards scenario reward " + std::to_string(id));

    for (const auto& c : data.characters)
        if (c.index < 12 && !c.large.enabled)
            problems.push_back("character " + std::to_string(c.index) + " is a stock agent but starts locked");
    if (data.scenarios.empty() || data.scenarios[0].item.value != mp_mode::kQuickGame)
        problems.push_back("scenario 0 is not Quick Game");
    for (const auto& r : data.rules) {
        auto it = std::ranges::find_if(r.choices, [&](const MpChoice& c) { return c.value == r.default_value; });
        if (it == r.choices.end()) problems.push_back("rule " + std::to_string(int(r.rule)) + " default is not a choice");
    }
    return problems;
}

}  // namespace nf

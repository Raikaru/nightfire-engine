#include "ui/mp_setup.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace nf {

namespace {

constexpr std::uint32_t kRavine = 0x0700004b;   // C_SBMPOPTIONS/Menu_PrepareBots: no bots on this level
constexpr std::uint32_t kPlayerLabel = 0x1c3;   // "Player" (bootup_bootup, C_RBMPCNAME)
constexpr const char* kBotName = "Bot";         // ELF string at 0x30ce20, the "%s %d" name of bot slots at boot
constexpr std::size_t kFixedStatsFrom = 15;     // P_MPBOTSETUP: characters >= 0xf have fixed statistics
constexpr std::size_t kAll = 0xff;              // Menu_UnlockMPSkins(0xff): the union of the four controllers' rewards

std::string numbered(std::string_view label, std::size_t n) { return std::string(label) + " " + std::to_string(n); }

std::uint64_t all_bonuses(const std::array<std::uint64_t, kMpMaxHumans>& b) { return b[0] | b[1] | b[2] | b[3]; }

// Menu_UnlockMPSettings / Menu_UnlockMPSkins walk every level's four rewards (PlrStarts_ProcessRewardCounter)
// and act on the earned ones; bit `id` of the bonus mask is the earned flag of reward `id` (type != 4).
template <typename F>
void for_earned_rewards(const MpData& data, std::uint64_t bonus, F&& f) {
    for (const auto& row : data.rewards)
        for (const auto& r : row.counters)
            if (r.type != 4 && r.id < 64 && (bonus >> r.id & 1)) f(r);
}

}  // namespace

std::string mp_handicap_text(std::int32_t handicap) {
    // P_MPCONFIRM "%s%d": a '+' prefix for non-negative values.
    return (handicap < 0 ? "" : "+") + std::to_string(handicap);
}

// ---- construction -------------------------------------------------------------------------------

MpSetup::MpSetup(const MpData& data, const StringTable& strings) : data_(data), strings_(strings) {
    // bootup_bootup: memset(MPSettings, 0) then these stores.
    for (std::size_t r = 0; r < kMpRuleCount; ++r) *rule_field(MpRule(r)) = data_.rules[r].default_value;
    settings_.mode = mp_mode::kArena;
    settings_.human_count = 1;
    for (std::size_t i = 0; i < kMpSlots; ++i) {
        MpPlayerSlot& s = settings_.slots[i];
        s.hud = true;
        s.team = i & 1;
        s.handicap = 0;
        s.character = 0;
        s.name = i < kMpMaxHumans ? numbered(strings_.label(kPlayerLabel), i + 1) : numbered(kBotName, i - 3);
    }
    begin_join();
    stored_settings_ = settings_;
}

// ---- unlocks -------------------------------------------------------------------------------------

void MpSetup::set_bonus(std::size_t slot, std::uint64_t mask) { bonus_.at(slot) = mask; }

bool MpSetup::scenario_available(std::size_t index) const {
    if (index >= data_.scenarios.size()) return false;
    if (unlock_everything_) return true;
    // Menu_UnlockMPSettings first clears the six reward-controlled rows, then re-enables the earned ones.
    bool controlled = false, earned = false;
    for (std::uint16_t id = 0x38; id <= 0x3d; ++id) controlled |= mp_reward_scenario(id) == index;
    if (!controlled) return data_.scenarios[index].item.enabled;
    for_earned_rewards(data_, all_bonuses(bonus_), [&](const MpReward& r) {
        earned |= r.type == 2 && mp_reward_scenario(r.id) == index;
    });
    return earned;
}

bool MpSetup::map_available(std::size_t index) const {
    return index < data_.maps.size() && (unlock_everything_ || data_.maps[index].item.enabled);
}

bool MpSetup::character_available(std::size_t slot, std::uint32_t character) const {
    if (character >= data_.characters.size()) return false;
    if (unlock_everything_) return true;
    if (character < 12) return data_.characters[character].large.enabled;
    // Menu_UnlockMPSkins clears rows 12..28 and re-enables the earned ones.
    std::uint64_t bonus = slot < kMpMaxHumans ? bonus_[slot] : all_bonuses(bonus_);
    bool earned = false;
    for_earned_rewards(data_, bonus, [&](const MpReward& r) { earned |= r.type == 1 && mp_reward_character(r.id) == character; });
    return earned;
}

bool MpSetup::explosive_scenery_unlocked() const {
    bool earned = false;
    for_earned_rewards(data_, all_bonuses(bonus_), [&](const MpReward& r) {
        earned |= r.type == 3 && r.id == kMpRewardExplosiveScenery;
    });
    return earned;
}

bool MpSetup::option_available(std::size_t index) const {
    if (index >= data_.options.size()) return false;
    // P_MPOPTIONS 0x4c: mp_options[1] ("AI Bots") is disabled on Ravine.
    if (index == 1) return settings_.level_id != kRavine;
    return data_.options[index].enabled;
}

// ---- P_MPJOIN --------------------------------------------------------------------------------------

void MpSetup::begin_join() {
    // P_MPJOIN 0x4c: bonuses cleared, mpjoin cleared with the per-controller defaults, states reset.
    bonus_ = {};
    join_ = {};
    for (std::size_t i = 0; i < kMpMaxHumans; ++i) {
        join_[i].character = std::uint32_t(i);
        join_[i].controller = std::uint8_t(i);
        join_[i].side = i % 2 == 0 ? kMpTeamMi6 : kMpTeamPhoenix;
    }
    controller_absent_ = {};
}

void MpSetup::set_controller_present(std::size_t slot, bool present, bool reset) {
    MpJoinSlot& j = join_.at(slot);
    j.controller_present = present;
    if (!present) {
        // "Insert analog controller ..." prompt; with `reset` the controller drops out of the join.
        if (reset) j.joined = j.ready = false;
        controller_absent_[slot] = true;
        return;
    }
    if (controller_absent_[slot] && reset) j.state = MpJoinState::Open;
    controller_absent_[slot] = false;
}

bool MpSetup::join(std::size_t slot) {
    MpJoinSlot& j = join_.at(slot);
    if (j.state != MpJoinState::Open || !j.controller_present) return false;
    j.state = MpJoinState::Codename;
    j.joined = true;
    j.ready = false;
    return true;
}

bool MpSetup::choose_codename(std::size_t slot, const std::string* saved_name, const MpCodename& profile) {
    MpJoinSlot& j = join_.at(slot);
    if (j.state != MpJoinState::Codename) return false;
    // C_RBMPCNAME: mask 0x2d when this is the only joined controller, else 0x25 (Menu_MapDefaultCodename).
    unsigned mask = joined_count() == 1 ? 0x2d : 0x25;
    apply_codename(slot, profile, mask);
    settings_.slots[slot].name = saved_name ? *saved_name : numbered(strings_.label(kPlayerLabel), slot + 1);
    j.state = MpJoinState::Ready;
    return true;
}

bool MpSetup::join_ready(std::size_t slot) {
    MpJoinSlot& j = join_.at(slot);
    if (j.state != MpJoinState::Ready) return false;
    j.ready = true;  // "Player Ready"
    return true;
}

void MpSetup::join_back(std::size_t slot) {
    MpJoinSlot& j = join_.at(slot);
    if (j.state == MpJoinState::Codename) {
        j.state = MpJoinState::Open;
        j.joined = false;
    } else if (j.state == MpJoinState::Ready) {
        j.state = MpJoinState::Codename;
        j.ready = false;
    }
}

std::size_t MpSetup::joined_count() const {
    return std::size_t(std::ranges::count_if(join_, [](const MpJoinSlot& j) { return j.joined; }));
}

bool MpSetup::are_we_ready() {
    // Menu_MPAreWeReady: `gap` = a joined controller follows an empty one, `unready` = a joined controller
    // is not ready; ready needs at least one joined, all of them ready, and no gap.
    bool gap = false, seen_empty = false, unready = false, any_joined = false;
    for (std::size_t i = 0; i < kMpMaxHumans; ++i) {
        join_[i].controller = std::uint8_t(i);
        if (!join_[i].joined) {
            seen_empty = true;
        } else {
            any_joined = true;
            if (seen_empty) gap = true;
            if (!join_[i].ready) unready = true;
        }
    }
    return !gap && any_joined && !unready;
}

// ---- P_MPSCENARIO / P_MPMAP -------------------------------------------------------------------------

bool MpSetup::select_scenario(std::size_t index, std::uint32_t random) {
    refusal_ = {};
    quick_game_ = false;
    if (index >= data_.scenarios.size()) return false;
    const MpMenuItem& item = data_.scenarios[index].item;
    if (!scenario_available(index)) {
        refusal_.message = item.disabled_label;
        return false;
    }
    scenario_selection_ = item.value;
    if (index != 0) {
        settings_.mode = item.value;  // then P_MPMAP
        return true;
    }

    // Quick Game (C_SBMPSCEN item 0): a 3-bot arena match on a random level, straight to P_MPCONFIRM.
    prepare_bots();
    settings_.duration = 10;
    settings_.mini_vehicles = 2;
    settings_.score_limit = 10;
    settings_.active = true;
    settings_.mode = mp_mode::kArena;
    settings_.explosive_scenery = 0;
    settings_.team_id = 1;
    settings_.friendly_fire = 0;
    settings_.grapple = 0;
    settings_.weapon_set = 0;
    // Rand_Random() % 7: the seven levels before Ravine.
    settings_.level_id = data_.maps[random % 7].item.value;
    std::uint32_t humans = 0;
    for (std::size_t i = 0; i < kMpMaxHumans; ++i) {
        if (!join_[i].joined) continue;
        ++humans;
        if (i == 0) {
            join_[0].side = kMpTeamMi6;
            join_[0].character = 0;  // Bond
        } else {
            join_[i].side = kMpTeamPhoenix;
            join_[i].character = 5 + std::uint32_t(i);  // Snow Guard, Black Ops, Yakuza
        }
    }
    static constexpr std::uint8_t kQuickBots[3] = {1, 3, 2};  // Drake, Kiko, Rook
    settings_.prepared_bot_count = settings_.bot_count = 3;
    for (std::size_t i = 0; i < 3; ++i) {
        MpBot& b = settings_.bots[i];
        b.character = kQuickBots[i];
        b.team = 0;
        b.enabled = true;
        b.edited = false;
        b.stats = data_.characters[b.character].stats;
        set_slot_name_from_character(kMpMaxHumans + i, b.character);
    }
    settings_.human_count = humans;
    quick_game_ = true;
    return true;
}

bool MpSetup::select_map(std::size_t index) {
    refusal_ = {};
    if (!map_available(index)) return false;
    settings_.level_id = data_.maps[index].item.value;  // MPSettings+0x1a8 and GameState level
    return true;
}

// ---- P_MPSETUP --------------------------------------------------------------------------------------

void MpSetup::begin_setup() {
    // Reservations taken by controllers are dropped, those held by bots (>= 10) stay.
    if (good_owner_ < 10) good_owner_ = 0;
    if (bond_owner_ < 10) bond_owner_ = 0;
    for (std::size_t i = 0; i < kMpMaxHumans; ++i) {
        MpJoinSlot& j = join_[i];
        j.controller = std::uint8_t(i);
        j.ready = false;
        if (team_game()) {
            j.state = MpJoinState::Team;
        } else {
            j.state = MpJoinState::Character;
            j.side = kMpTeamMi6;
        }
    }
}

std::vector<std::uint32_t> MpSetup::selectable_characters(std::size_t slot, bool hide_good) const {
    // Menu_GetMPSkins: all characters the controller's rewards enable, minus the side/reservation filters.
    std::vector<std::uint32_t> out;
    const MpJoinSlot& j = join_.at(slot);
    for (const MpCharacter& c : data_.characters) {
        if (!character_available(slot, c.index)) continue;
        if (!team_game()) {
            if (c.good() && (hide_good || (good_owner_ != 0 && good_owner_ != slot + 1))) continue;
        } else {
            if (c.good() ? j.side != kMpTeamMi6 : j.side != kMpTeamPhoenix) continue;
            if (c.bond() && bond_owner_ != 0 && bond_owner_ != slot + 1) continue;
        }
        out.push_back(c.index);
    }
    return out;
}

std::uint32_t MpSetup::initial_character(std::size_t slot) const {
    auto list = selectable_characters(slot);
    if (list.empty()) return join_.at(slot).character;
    // Selecting the remembered value in the wheel; an absent value leaves the first row.
    return std::ranges::find(list, join_[slot].character) != list.end() ? join_[slot].character : list.front();
}

bool MpSetup::choose_team(std::size_t slot, std::uint32_t team) {
    MpJoinSlot& j = join_.at(slot);
    if (!j.joined || j.state != MpJoinState::Team || team > kMpTeamMi6) return false;
    j.side = team;
    j.state = MpJoinState::Character;
    return true;
}

bool MpSetup::choose_character(std::size_t slot, std::uint32_t character) {
    MpJoinSlot& j = join_.at(slot);
    if (!j.joined || j.state != MpJoinState::Character) return false;
    auto list = selectable_characters(slot);
    if (std::ranges::find(list, character) == list.end()) return false;
    const MpCharacter& c = data_.characters[character];
    j.state = MpJoinState::Handicap;
    j.character = character;
    j.side = c.good() ? kMpTeamMi6 : kMpTeamPhoenix;
    if (!team_game() && c.good()) good_owner_ = std::uint8_t(slot + 1);
    if (c.bond()) bond_owner_ = std::uint8_t(slot + 1);
    return true;
}

bool MpSetup::choose_handicap(std::size_t slot, std::int32_t handicap) {
    MpJoinSlot& j = join_.at(slot);
    if (!j.joined || j.state != MpJoinState::Handicap) return false;
    if (std::ranges::none_of(data_.handicap, [&](const MpChoice& c) { return c.value == handicap; })) return false;
    settings_.slots[slot].handicap = handicap;
    j.state = MpJoinState::Ready;
    j.ready = true;
    return are_we_ready();
}

void MpSetup::setup_back(std::size_t slot) {
    MpJoinSlot& j = join_.at(slot);
    switch (j.state) {
    case MpJoinState::Character:
        if (team_game()) j.state = MpJoinState::Team;  // non-team games have nothing to go back to
        break;
    case MpJoinState::Handicap:
        j.state = MpJoinState::Character;
        // The reservation is released for the mode that took it (C_RBMPSETUP 0x6b).
        if (!team_game()) {
            if (good_owner_ == slot + 1) good_owner_ = 0;
        } else if (bond_owner_ == slot + 1) {
            bond_owner_ = 0;
        }
        break;
    case MpJoinState::Ready:  // C_RBMPFINISH 0x6b
        j.state = MpJoinState::Handicap;
        j.ready = false;
        break;
    default:
        break;
    }
}

// ---- rules -----------------------------------------------------------------------------------------

std::int32_t* MpSetup::rule_field(MpRule r) {
    switch (r) {
    case MpRule::Duration: return &settings_.duration;
    case MpRule::ScoreLimit: return &settings_.score_limit;
    case MpRule::FriendlyFire: return &settings_.friendly_fire;
    case MpRule::WeaponSet: return &settings_.weapon_set;
    case MpRule::ProfessionalMode: return &settings_.professional;
    case MpRule::LocationDamage: return &settings_.location_damage;
    case MpRule::TeamId: return &settings_.team_id;
    case MpRule::Respawn: return &settings_.respawn;
    case MpRule::FixedGuns: return &settings_.fixed_guns;
    case MpRule::ExplosiveScenery: return &settings_.explosive_scenery;
    case MpRule::Grapple: return &settings_.grapple;
    case MpRule::MiniVehicles: return &settings_.mini_vehicles;
    case MpRule::Count: break;
    }
    throw std::out_of_range("bad MpRule");
}

const std::int32_t* MpSetup::rule_field(MpRule r) const { return const_cast<MpSetup*>(this)->rule_field(r); }

std::int32_t MpSetup::rule(MpRule r) const { return *rule_field(r); }

std::vector<MpChoice> MpSetup::rule_choices(MpRule r) const {
    // P_MPENVIROMODS: without the reward the Explosive Scenery radio only holds the "Locked" entry (value 0x10).
    if (r == MpRule::ExplosiveScenery && !explosive_scenery_unlocked()) return {{0x10, 0x1000076}};
    return data_.rule(r).choices;
}

bool MpSetup::set_rule(MpRule r, std::int32_t value) {
    auto choices = rule_choices(r);
    if (std::ranges::none_of(choices, [&](const MpChoice& c) { return c.value == value; })) return false;
    *rule_field(r) = value;
    return true;
}

void MpSetup::cycle_rule(MpRule r, int direction) {
    auto choices = rule_choices(r);
    std::int32_t current = rule(r);
    auto it = std::ranges::find_if(choices, [&](const MpChoice& c) { return c.value == current; });
    std::ptrdiff_t n = std::ptrdiff_t(choices.size());
    std::ptrdiff_t at = it == choices.end() ? 0 : it - choices.begin();
    if (it != choices.end()) at = ((at + direction) % n + n) % n;
    *rule_field(r) = choices[std::size_t(at)].value;
}
void MpSetup::set_ruleset(MpRuleSet rules) {
    settings_.rules = rules;
    settings_.slot_count = std::uint32_t(mp_rule_slot_limit(rules));
    settings_.bot_count = std::min(settings_.bot_count, settings_.bot_limit());
    settings_.prepared_bot_count = std::min(settings_.prepared_bot_count, settings_.bot_limit());
}

std::uint32_t MpSetup::score_caption(std::uint32_t mode) const {
    // P_MPRULES 0x4c: "Lives" for Top Agent, the rule's own "Points" caption otherwise.
    return mode == mp_mode::kTopAgent ? 0x3d3 : data_.rule(MpRule::ScoreLimit).caption;
}

std::uint32_t MpSetup::score_unit_label(std::uint32_t mode, std::int32_t limit) const {
    // P_MPCONFIRM: the summary row's unit; an unlimited hill game reads "Minutes : Unlimited".
    if (limit == -1 && (mode == mp_mode::kKingOfTheHill || mode == mp_mode::kTeamKingOfTheHill)) return 0x10001de;
    return mode == mp_mode::kTopAgent ? 0x3d7 : 0x3d8;
}

// ---- options / bots ------------------------------------------------------------------------------------

void MpSetup::begin_options() {
    if (bots_defaulted_) return;
    // The original presets the six GC/Xbox roster rows; the Extended-only rows start from the same safe character.
    for (std::size_t i = 0; i < kMpMaxBots; ++i) settings_.bots[i].character = std::uint8_t(6 + i);
    bots_defaulted_ = true;
}

void MpSetup::prepare_bots() {
    // Enabled bots move to the front; rows outside the chosen ruleset are not match participants.
    std::size_t out = 0;
    for (std::size_t i = 0; i < settings_.bot_limit(); ++i) {
        if (!settings_.bots[i].enabled) continue;
        if (i != out) {
            std::string name = settings_.slots[kMpMaxHumans + i].name;
            std::swap(settings_.bots[i], settings_.bots[out]);
            settings_.slots[kMpMaxHumans + out].name = name;
        }
        ++out;
    }
    settings_.bots_prepared = true;
    settings_.prepared_bot_count = std::uint32_t(out);
    settings_.bot_count = std::uint32_t(out);
    if (settings_.level_id == kRavine) settings_.prepared_bot_count = settings_.bot_count = 0;
}

MpContinueResult MpSetup::continue_to_confirm() {
    prepare_bots();
    unsigned phoenix = 0, mi6 = 0;
    for (const MpJoinSlot& j : join_) {
        if (!j.joined) continue;
        (j.side == 0 ? phoenix : mi6)++;
    }
    settings_.human_count = phoenix + mi6;
    for (std::uint32_t i = 0; i < settings_.prepared_bot_count; ++i) (settings_.bots[i].team == 0 ? phoenix : mi6)++;

    MpContinueResult r;
    if (!team_game()) {
        if (mi6 > 1) {
            r.refusal = {0x1000311, 0, 0};  // good agents against each other in a free-for-all
            return r;
        }
    } else if (phoenix == 0 || mi6 == 0) {
        r.refusal = {0x389, kMpTeamLabels[phoenix == 0 ? 0 : 1], 8};  // "no players or bots on the %s team"
        return r;
    }
    if (settings_.human_count + settings_.bot_count > 1) {
        r.ok = true;
        return r;
    }
    r.refusal = {0x39e, 0, 7};  // at least two participants
    return r;
}

void MpSetup::begin_bot_choose(std::size_t bot) {
    editing_bot_ = std::min(bot, std::size_t(settings_.bot_limit() - 1));
}

bool MpSetup::bot_character_available(std::size_t bot, std::uint32_t character) const {
    if (!character_available(kAll, character)) return false;
    const MpCharacter& c = data_.characters[character];
    std::uint8_t me = std::uint8_t(bot + 10);
    // P_MPBOTCHOOSE 0x4c: a character another participant holds is disabled in the bot's list.
    if (!team_game() && c.good() && good_owner_ != 0 && good_owner_ != me) return false;
    if (c.bond() && bond_owner_ != 0 && bond_owner_ != me) return false;
    return true;
}

void MpSetup::browse_bot_character(std::size_t bot, std::uint32_t character, bool changed) {
    MpBot& b = settings_.bots.at(bot);
    if (changed) b.edited = false;  // C_SBMPBTCHOOSE 0x49: moving the wheel resets the statistics
    if (!b.edited) b.stats = data_.characters.at(character).stats;
}

bool MpSetup::choose_bot_character(std::size_t bot, std::uint32_t character) {
    if (character >= data_.characters.size()) return false;
    if (!unlock_everything_ && !bot_character_available(bot, character)) return false;
    const MpCharacter& c = data_.characters[character];
    MpBot& b = settings_.bots.at(bot);
    b.character = std::uint8_t(character);
    b.team = c.good() ? 1 : 0;
    std::uint8_t me = std::uint8_t(bot + 10);
    if (!team_game() && c.good()) good_owner_ = me;
    if (c.bond()) bond_owner_ = me;
    set_slot_name_from_character(kMpMaxHumans + bot, character);
    b.enabled = true;
    return true;
}

std::vector<MpChoice> MpSetup::bot_stat_choices(std::size_t bot, BotStat stat) const {
    if (stat == BotStat::Personality)
        return data_.characters.at(settings_.bots.at(bot).character).good() ? data_.personality_good : data_.personality_evil;
    return data_.bot_stats.at(std::size_t(stat)).choices;
}

bool MpSetup::bot_stats_editable(std::size_t bot) const { return settings_.bots.at(bot).character < kFixedStatsFrom; }

bool MpSetup::set_bot_stat(std::size_t bot, BotStat stat, std::int32_t value) {
    MpBot& b = settings_.bots.at(bot);
    auto choices = bot_stat_choices(bot, stat);
    if (std::ranges::none_of(choices, [&](const MpChoice& c) { return c.value == value; })) return false;
    if (stat == BotStat::Enabled) {
        b.enabled = value != 0;
        return true;
    }
    if (!bot_stats_editable(bot)) return false;  // the controls are greyed out
    switch (stat) {
    case BotStat::Accuracy: b.stats.accuracy = std::uint8_t(value); break;
    case BotStat::Aggression: b.stats.aggression = std::uint16_t(value); break;
    case BotStat::Health: b.stats.health = std::uint16_t(value); break;
    case BotStat::MoveSpeed: b.stats.move_speed = std::uint8_t(value); break;
    case BotStat::Personality: b.stats.personality = std::uint8_t(value); break;
    case BotStat::ReactionTime: b.stats.reaction_time = std::uint8_t(value); break;
    case BotStat::RecoveryRate: b.stats.recovery_rate = std::uint8_t(value); break;
    default: return false;
    }
    return true;
}

void MpSetup::commit_bot(std::size_t bot) {
    MpBot& b = settings_.bots.at(bot);
    b.edited = true;
    std::uint8_t me = std::uint8_t(bot + 10);
    const MpCharacter& c = data_.characters.at(b.character);
    if (!team_game() && good_owner_ == me && (!c.good() || !b.enabled)) good_owner_ = 0;
    if (bond_owner_ == me && (!c.bond() || !b.enabled)) bond_owner_ = 0;
}

// ---- P_MPCONFIRM ---------------------------------------------------------------------------------------

std::vector<MpParticipant> MpSetup::participants() const {
    std::vector<MpParticipant> out;
    for (std::size_t i = 0; i < kMpMaxHumans; ++i) {
        const MpJoinSlot& j = join_[i];
        if (!j.joined) continue;
        MpParticipant p;
        p.slot = std::uint32_t(i);
        p.controller = j.controller;
        p.name = settings_.slots[i].name;
        p.character = j.character;
        p.team = j.side;
        p.handicap = settings_.slots[i].handicap;
        p.hud = settings_.slots[i].hud;
        out.push_back(std::move(p));
    }
    for (std::uint32_t i = 0; i < settings_.prepared_bot_count; ++i) {
        const MpBot& b = settings_.bots[i];
        MpParticipant p;
        p.bot = true;
        p.slot = std::uint32_t(kMpMaxHumans + i);
        p.name = settings_.slots[kMpMaxHumans + i].name;
        p.character = b.character;
        p.team = b.team;
        p.stats = b.stats;
        out.push_back(std::move(p));
    }
    return out;
}

void MpSetup::store() {
    // Menu_StoreMPSettings copies MPSettings; `mpbots` is a separate global and is not part of it.
    stored_settings_ = settings_;
}

void MpSetup::restore() {
    auto bots = settings_.bots;
    bool prepared = settings_.bots_prepared;
    std::uint32_t count = settings_.prepared_bot_count;
    settings_ = stored_settings_;
    settings_.bots = bots;
    settings_.bots_prepared = prepared;
    settings_.prepared_bot_count = count;
}

MpLaunch MpSetup::start() {
    // P_MPCONFIRM 0x4b.
    store();
    struct Joined {
        MpPlayerSlot slot;
        MpJoinSlot join;
    };
    std::vector<Joined> joined;
    for (std::size_t i = 0; i < kMpMaxHumans; ++i)
        if (join_[i].joined) joined.push_back({settings_.slots[i], join_[i]});
    for (std::size_t i = 0; i < kMpMaxHumans; ++i) {
        if (i < joined.size()) {
            settings_.slots[i] = joined[i].slot;
            settings_.slots[i].team = joined[i].join.side;
            settings_.slots[i].character = joined[i].join.character;
        } else {
            settings_.slots[i].team = 0;
            settings_.slots[i].character = 0;
        }
    }
    if (settings_.duration != -1) settings_.duration *= 60;
    settings_.explosive_scenery &= 1;
    settings_.active = true;
    settings_.human_count = std::uint32_t(joined.size());

    // PS2 keeps four bot slots; GC/Xbox adds two and Extended uses the full participant capacity.
    MpLaunch launch;
    if (settings_.bots_prepared) settings_.bot_count = settings_.prepared_bot_count;
    settings_.bot_count = std::min(settings_.bot_count, settings_.bot_limit());
    for (std::uint32_t i = 0; i < settings_.human_count; ++i) {
        MpParticipant p;
        p.slot = i;
        p.controller = joined[i].join.controller;
        p.name = settings_.slots[i].name;
        p.character = settings_.slots[i].character;
        p.team = settings_.slots[i].team;
        p.handicap = settings_.slots[i].handicap;
        p.hud = settings_.slots[i].hud;
        launch.participants.push_back(std::move(p));
    }
    for (std::uint32_t i = 0; i < settings_.bot_count; ++i) {
        std::size_t slot = kMpMaxHumans + i;
        MpParticipant p;
        p.bot = true;
        p.slot = std::uint32_t(slot);
        if (settings_.bots_prepared) {
            settings_.slots[slot].team = settings_.bots[i].team;
            p.stats = settings_.bots[i].stats;
        } else {
            // BOT_init without MPBOTS: default bot 1 (Drake), team taken from the slot.
            p.stats = data_.characters[1].stats;
            p.default_stats = true;
        }
        std::uint32_t character = settings_.bots_prepared ? settings_.bots[i].character : 1;
        // BOT_init: bots of a non-team game have no team, Assassination puts them on team 0.
        if (!team_game()) settings_.slots[slot].team = kMpTeamNone;
        if (settings_.mode == mp_mode::kAssassination) settings_.slots[slot].team = 0;
        settings_.slots[slot].character = character;
        p.name = settings_.slots[slot].name;
        p.character = character;
        p.team = settings_.slots[slot].team;
        launch.participants.push_back(std::move(p));
    }

    launch.settings = settings_;
    const MpMap* map = data_.find_map(settings_.level_id);
    if (!map) throw FormatError("no map chosen for the multiplayer match");
    launch.level_bin = map->bin_name;
    launch.participant_count = settings_.human_count + settings_.bot_count;
    // MP_Init: Demolition/Protection rounds without a limit run 60 seconds.
    launch.time_limit_seconds = settings_.duration;
    if ((settings_.mode == mp_mode::kDemolition || settings_.mode == mp_mode::kProtection) && settings_.duration < 0)
        launch.time_limit_seconds = 60;
    // MP_setLoadingSkins: the skins of every participant are loaded, the rest are skipped (MP_NeedSkin/File).
    for (const MpParticipant& p : launch.participants)
        if (std::ranges::find(launch.needed_characters, p.character) == launch.needed_characters.end())
            launch.needed_characters.push_back(p.character);
    std::ranges::sort(launch.needed_characters);
    return launch;
}

// ---- codenames -------------------------------------------------------------------------------------------

MpCodename MpSetup::capture_codename(std::size_t slot) const {
    MpCodename c{};
    c.bonus = bonus_.at(slot);
    c.handicap = std::int16_t(settings_.slots.at(slot).handicap);
    c.hud = settings_.slots[slot].hud;
    c.professional = std::uint8_t(settings_.professional);
    c.respawn = std::uint8_t(settings_.respawn);
    c.team_id = std::uint8_t(settings_.team_id);
    return c;
}

void MpSetup::apply_codename(std::size_t slot, const MpCodename& c, unsigned mask) {
    if (mask & 1) {
        settings_.professional = c.professional;
        settings_.respawn = c.respawn;
        settings_.slots.at(slot).hud = c.hud;
    }
    if (mask & 4) settings_.slots.at(slot).handicap = c.handicap;
    if (mask) bonus_.at(slot) = c.bonus;
}

void MpSetup::set_slot_name_from_character(std::size_t slot, std::uint32_t character) {
    settings_.slots.at(slot).name = strings_.label(data_.characters.at(character).large.name);
}

// ---- MpMatch ---------------------------------------------------------------------------------------------

MpMatch::MpMatch(const MpLaunch& launch) : time_limit(float(launch.time_limit_seconds)), launch_(launch) {
    // MP_Init: every slot gets "no spawn / no target" markers (not modelled) and, in Top Agent, the life limit as its score.
    for (const MpParticipant& p : launch.participants) {
        players[p.slot].present = true;
        players[p.slot].bot = p.bot;
    }
    if (launch.settings.mode == mp_mode::kTopAgent)
        for (auto& p : players) p.score = float(launch.settings.score_limit);
}

std::int32_t MpMatch::score(std::size_t slot) const {
    const MpPlayerRuntime& p = players.at(slot);
    switch (launch_.settings.mode) {
    case mp_mode::kArena:
    case mp_mode::kTeamArena: return p.kills;
    case mp_mode::kTopAgent:
    case mp_mode::kAssassination:
    case mp_mode::kCaptureTheFlag:
    case mp_mode::kDemolition:
    case mp_mode::kProtection:
    case mp_mode::kIndustrialEspionage:
    case mp_mode::kGoldenEyeStrike:
    case mp_mode::kKingOfTheHill:
    case mp_mode::kUplink:
    case mp_mode::kTeamKingOfTheHill: return std::int32_t(p.score);
    default: return 0;
    }
}

int MpMatch::check_end_condition(float dt, bool paused) {
    const MpSettings& s = launch_.settings;
    if (!paused) elapsed += dt;
    int forced_out = -1;
    auto fold_players = [&] {
        for (const auto& p : players) best_score = std::max(best_score, std::int32_t(p.score));
    };
    // The team scores start the maximum: (int) of the larger of team 0 and (float)(int)team 1.
    best_score = std::int32_t(std::max(team_score[0], float(std::int32_t(team_score[1]))));

    if (s.mode == mp_mode::kDemolition || s.mode == mp_mode::kProtection) {
        if (time_limit > 0 && !time_reached && time_limit <= elapsed) state = MpEnd::RoundOver;
        fold_players();
    } else {
        if (s.mode == mp_mode::kTopAgent) {
            // Lives: a participant is out when eliminated (or, for a bot, dead) with all its lives used.
            std::uint32_t humans_out = 0, bots_out = 0;
            for (const auto& p : players) {
                if (!p.present || p.deaths < s.score_limit) continue;
                if (!p.bot && p.status == MpPlayerStatus::Out) ++humans_out;
                if (p.bot && p.status != MpPlayerStatus::Alive) ++bots_out;
            }
            std::uint32_t total = launch_.participant_count;
            if ((total > 0 && total - 1 <= humans_out + bots_out) || humans_out == s.human_count) state = MpEnd::ScoreLimit;
            if (s.human_count <= humans_out && s.bot_count != 0 && bots_out + 1 < s.bot_count) {
                // Everyone human is out: eliminate the first bot still fighting so the game winds down.
                for (std::size_t i = 0; i < players.size(); ++i)
                    if (players[i].present && players[i].bot && players[i].status == MpPlayerStatus::Alive) {
                        players[i].status = MpPlayerStatus::Out;
                        forced_out = int(i);
                        break;
                    }
            }
        }
        if (time_limit > 0.0f && !time_reached) time_reached = time_limit <= elapsed;
        if (!s.team_game()) fold_players();
        if (s.mode == mp_mode::kTopAgent) {
            // Top Agent skips the score limit test; only the timer can override the state set above.
            if (time_reached) state = MpEnd::TimeLimit;
            return forced_out;
        }
    }
    if (s.score_limit != -1) score_reached = s.score_limit <= best_score;
    if (score_reached) state = MpEnd::ScoreLimit;
    else if (time_reached) state = MpEnd::TimeLimit;
    return forced_out;
}

MpMatch::Result MpMatch::sort_out_who_won() {
    const MpSettings& s = launch_.settings;
    state = MpEnd::Finished;
    Result r;

    // MP_SortOutWhoWon's "Game Over" line.
    if (s.mode == mp_mode::kTopAgent) {
        std::int32_t fewest = 30000;
        int survivors = 0;
        std::optional<std::size_t> best;
        for (std::size_t i = 0; i < players.size(); ++i) {
            if (!players[i].present || players[i].deaths >= s.score_limit) continue;
            ++survivors;
            if (players[i].deaths < fewest) {
                best = i;
                fewest = players[i].deaths;
            } else if (players[i].deaths == fewest) {
                survivors = 0;
            }
        }
        if (survivors != 0) r.overlay_winner = best;
    } else if (!s.team_game()) {
        int reached = 0;
        std::optional<std::size_t> who;
        for (std::size_t i = 0; i < players.size(); ++i)
            if (float(s.score_limit) <= players[i].score) ++reached, who = i;
        if (reached == 1) r.overlay_winner = who;
    }

    // P_MPDEBRIEFING: humans then bots, stable descending sort by Menu_GetMPScore, ties share a place.
    for (const MpParticipant& p : launch_.participants) r.ranking.push_back({p.slot, score(p.slot), 0});
    std::ranges::stable_sort(r.ranking, [](const Rank& a, const Rank& b) { return a.score > b.score; });
    std::size_t place = 0;
    for (std::size_t i = 0; i < r.ranking.size(); ++i) {
        if (i > 0 && r.ranking[i].score != r.ranking[i - 1].score) place = i;
        r.ranking[i].place = place;
    }
    if (!s.team_game()) {
        r.draw = r.ranking.size() > 1 && r.ranking[0].score == r.ranking[1].score;
        if (!r.draw && !r.ranking.empty()) r.debrief_winner = r.ranking[0].slot;
    } else {
        r.draw = team_score[0] == team_score[1];
        if (!r.draw) r.winning_team = team_score[0] < team_score[1] ? kMpTeamMi6 : kMpTeamPhoenix;
    }
    return r;
}

}  // namespace nf

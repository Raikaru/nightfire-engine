#include "game/bot_match.hpp"

#include <algorithm>
#include <cstdio>
#include <stdexcept>

namespace nf::bots {

BotMatch::BotMatch(GameFiles& files, const std::filesystem::path& gamedir, const std::string& level_bin, Level& level, World& world,
                   const Elf32& action_elf, const std::string& tuning_text, const StringTable* strings,
                   const BotMatchOptions& options)
    : level_(level), world_(world), elf_(action_elf), tuning_text_(tuning_text),
      level_id_(level_id_from_name(level_bin)), options_(options) {
    if (!strings) throw std::runtime_error("bots need USATxt.dat (multiplayer tables)");
    mp_ = load_mp_data(files, gamedir, *strings);
    bank_ = open_character_bank(files, level_bin);
    nav_ = std::make_unique<NavNetwork>(level, world.collision(), NavLimits::for_level(level_id_));
    const int count = std::clamp(options_.count, 0, int(kMpMaxBots));
    const std::vector<int> chars = parse_bot_characters(options_.characters, count);
    for (int i = 0; i < count; ++i) roster_.push_back(default_bot_spec(mp_, i, chars[std::size_t(i)], true));
}

BotMatch::~BotMatch() = default;

void BotMatch::install(ArenaSession& session) {
    session_ = &session;
    ArenaSystem& arena = session.arena();
    ArenaSettings& settings = arena.mutable_settings();
    const std::size_t first_bot = settings.first_bot_slot();
    const std::size_t available = settings.slot_count > first_bot ? settings.slot_count - first_bot : 0;
    if (roster_.size() > available) roster_.resize(available);
    for (std::size_t i = 0; i < roster_.size(); ++i) roster_[i].slot = int(first_bot + i);
    if (session.options().roster_override)
        for (BotSpec& spec : roster_) spec.team = settings.slots[std::size_t(spec.slot)].team;
    // Bots are drones: multiplayer perception / firing branches, difficulty forced to 1 by P_MPCONFIRM_Handler.
    drone::DroneConfig dc;
    dc.elf = &elf_;
    dc.level_id = level_id_;
    dc.difficulty = 1;
    dc.multiplayer = true;
    dc.initial_timing = FrameTiming{float(options_.logic_hz)};
    dc.tuning = drone::DroneTuning::load(tuning_text_, level_id_);
    auto drones = std::make_unique<drone::DroneSystem>(world_, *bank_, dc);
    drones_ = drones.get();
    drones_->set_nav(nav_.get());
    drones_->set_weapons(&session.weapons());

    BotSystem::Config bc;
    bc.world = &world_;
    bc.drones = drones_;
    bc.arena = &arena;
    bc.weapons = &session.weapons();
    bc.nav = nav_.get();
    bc.mp = &mp_;
    bc.bank = bank_.get();
    auto bots = std::make_unique<BotSystem>(bc);
    bots_ = bots.get();
    drones_->callbacks().on_drop_weapon = [bots = bots_](drone::Drone& d) {
        bots->drop_weapon(d);
    };
    bots_->log_states = options_.log_states;
    // MP_PlayerKilled Vengeful (+2) bonus: only a participant currently marked as a bot qualifies.
    arena.set_vengeful_bonus_fn([bots = bots_, arena_ptr = &arena](int killer, int victim) {
        if (killer < 0 || killer >= int(arena_ptr->settings().slot_count) ||
            !arena_ptr->settings().slots[std::size_t(killer)].bot || victim < 0) return false;
        const BotSystem::Bot* b = bots->bot_at_slot(killer);
        if (!b || !b->brain) return false;
        if (b->brain->v.stats.personality != std::uint8_t(Personality::Vengeful)) return false;
        return b->brain->v.trait_opponent == victim;
    });
    if (options_.log) bots_->log = [](const std::string& line) { std::printf("%s\n", line.c_str()); };
    for (BotSpec& spec : roster_) {
        spec.team = settings.slots[std::size_t(spec.slot)].team;
        bots_->add_bot(spec);
    }
    // Original MP_Update runs MP_Pickup_Process before BOT_update (MP_Update__Fv.s: 0x184CD4-0x184CE8).
    // Game_Run then calls control_movement_object_handler (Game_Run__Fv.s: 0x1C97F0-0x1C9880), so a
    // DroneWeap_DropWeapon created during BOT_update reaches Pickup_Update in the same frame's object pass.
    world_.add_system(std::move(bots));
    world_.add_system(std::move(drones));
}

void BotMatch::start() {
    if (bots_) bots_->start();
}
void BotMatch::add_bot(BotSpec spec) {
    if (!session_ || !bots_) throw std::logic_error("BotMatch must be installed before changing its roster");
    ArenaSystem& arena = session_->arena();
    ArenaSettings& settings = arena.mutable_settings();
    if (spec.slot < 0 || spec.slot >= int(settings.slot_count) || spec.slot >= int(settings.slots.size()))
        throw std::out_of_range("bot participant slot is outside the match capacity");
    ArenaSettings::Slot& participant = settings.slots[std::size_t(spec.slot)];
    if (participant.present) throw std::logic_error("bot participant slot is already occupied");
    if (settings.bot_count() >= int(kMpMaxBots)) throw std::out_of_range("bot capacity exceeded");
    if (settings.mode == mp_mode::kAssassination) {
        spec.team = kTeamPhoenix;
    } else if (settings.team_game() && spec.team != kTeamPhoenix && spec.team != kTeamMi6) {
        int phoenix = 0, mi6 = 0;
        for (const ArenaSettings::Slot& other : settings.slots) {
            if (!other.present) continue;
            phoenix += other.team == kTeamPhoenix;
            mi6 += other.team == kTeamMi6;
        }
        spec.team = phoenix <= mi6 ? kTeamPhoenix : kTeamMi6;
    } else if (!settings.team_game()) {
        spec.team = kTeamNone;
    }
    participant.present = true;
    participant.bot = true;
    participant.name = spec.name;
    participant.team = spec.team;
    participant.character = spec.character;
    participant.health_bonus = 0;
    try {
        bots_->add_bot(spec);
    } catch (...) {
        participant = {};
        throw;
    }
    roster_.push_back(std::move(spec));
}

bool BotMatch::remove_bot(int slot) {
    if (!bots_ || !bots_->remove_bot(slot)) return false;
    std::erase_if(roster_, [slot](const BotSpec& spec) { return spec.slot == slot; });
    return true;
}

bool BotMatch::replace_bot_with_human(ArenaSession& session, int slot, std::string_view name) {
    if (session_ != &session || !remove_bot(slot)) return false;
    session.activate_human(slot, name);
    return true;
}


std::string BotMatch::summary() const {
    std::string out;
    char line[256];
    for (const auto& b : bots_->bots()) {
        const BotStatsCounters& c = b->c;
        std::snprintf(line, sizeof line, "  bot %d %-14s deaths %2d shots %4d pickups %2d respawns %2d walked %6.1f\n", b->spec.slot,
                      std::string(character_name(b->spec.character)).c_str(), c.deaths, c.shots, c.pickups, c.respawns,
                      double(c.distance));
        out += line;
    }
    return out;
}

}  // namespace nf::bots

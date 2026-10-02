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
    const int count = std::clamp(options_.count, 0, 4);
    const std::vector<int> chars = parse_bot_characters(options_.characters, count);
    for (int i = 0; i < count; ++i) roster_.push_back(default_bot_spec(mp_, i, chars[std::size_t(i)], true));
}

BotMatch::~BotMatch() = default;

void BotMatch::install(ArenaSession& session) {
    ArenaSystem& arena = session.arena();
    ArenaSettings& settings = arena.mutable_settings();
    fill_bot_slots(settings, roster_);
    // Bots are drones: multiplayer perception / firing branches, difficulty forced to 1 by P_MPCONFIRM_Handler.
    drone::DroneConfig dc;
    dc.elf = &elf_;
    dc.level_id = level_id_;
    dc.difficulty = 1;
    dc.multiplayer = true;
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
    bots_->log_states = options_.log_states;
    if (options_.log) bots_->log = [](const std::string& line) { std::printf("%s\n", line.c_str()); };
    for (BotSpec& spec : roster_) {
        spec.team = settings.slots[std::size_t(spec.slot)].team;
        bots_->add_bot(spec);
    }
    world_.add_system(std::move(drones));
    world_.add_system(std::move(bots));
}

void BotMatch::start() {
    if (bots_) bots_->start();
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

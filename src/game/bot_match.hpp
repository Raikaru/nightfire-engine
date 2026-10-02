#pragma once

// BotMatch: everything nfgame needs to put the multiplayer bots into an arena match (`--bots N`, `--bot-char a,b,c`):
// the level's character bank, the nav network, the DroneSystem (multiplayer perception / firing branches) and the
// BotSystem, wired in the order the original runs them. Usage (see nfgame_mp.cpp):
//
//     bots::BotMatch bots(gamefiles, dir, bin_name, level, world, action_elf, tuning_text, strings, {count, "kiko,3"});
//     ArenaSession session(world, table, options, strings, tuning_text, [&](ArenaSession& s) { bots.install(s); });
//     bots.start();                 // after ArenaSession (ArenaSystem::start ran): nav emitters for pickups / objectives

#include <memory>
#include <string>
#include <vector>

#include "assets/elf.hpp"
#include "assets/game_files.hpp"
#include "assets/level.hpp"
#include "assets/mp_data.hpp"
#include "assets/strings.hpp"
#include "game/arena_session.hpp"
#include "game/bot_system.hpp"
#include "game/drone_system.hpp"
#include "game/nav.hpp"

namespace nf::bots {

struct BotMatchOptions {
    int count = 0;                 // --bots N (1..4)
    std::string characters;        // --bot-char list (indices or names); defaults: Drake, Kiko, Rook, then 6..
    bool log = false;              // --bot-log
    bool log_states = false;       // --bot-log-states
};

class BotMatch {
public:
    BotMatch(GameFiles& files, const std::filesystem::path& gamedir, const std::string& level_bin, Level& level, World& world,
             const Elf32& action_elf, const std::string& tuning_text, const StringTable* strings, const BotMatchOptions& options);
    ~BotMatch();

    // ArenaSession's before_start: creates DroneSystem + BotSystem (added to the world after the arena's systems), fills the
    // bot slots of the settings, spawns the bots at MP_GetSpawnPoint and registers their bodies.
    void install(ArenaSession& session);
    // After the ArenaSession exists.
    void start();

    const std::vector<BotSpec>& roster() const { return roster_; }
    drone::DroneSystem& drones() { return *drones_; }
    BotSystem& bots() { return *bots_; }
    CharacterBank& bank() { return *bank_; }
    NavNetwork& nav() { return *nav_; }
    // One line per bot: character, kills, deaths, shots, pickups, respawns, path walked.
    std::string summary() const;

private:
    Level& level_;
    World& world_;
    const Elf32& elf_;
    std::string tuning_text_;
    std::uint32_t level_id_;
    BotMatchOptions options_;
    MpData mp_;
    std::vector<BotSpec> roster_;
    std::unique_ptr<CharacterBank> bank_;
    std::unique_ptr<NavNetwork> nav_;
    drone::DroneSystem* drones_ = nullptr;   // owned by the world
    BotSystem* bots_ = nullptr;              // owned by the world
};

}  // namespace nf::bots

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "assets/strings.hpp"
#include "assets/weapon_data.hpp"
#include "game/arena.hpp"
#include "game/weapons.hpp"

namespace nf {

// A multiplayer match the way nfgame launches one: command line options (the stand-in for the front end's
// P_MPCONFIRM record) -> ArenaSettings.
struct MatchOptions {
    bool enabled = false;                       // --mp
    std::uint32_t mode = mp_mode::kArena;       // --mode
    int humans = 1;                             // --players 1..4
    int bots = 0;                               // --bots 0..4
    std::int32_t score_limit = 10;              // --frag-limit N (-1 unlimited)
    float time_limit = 600.0f;                  // --time-limit MINUTES (< 0 unlimited)
    bool friendly_fire = false;                 // --friendly-fire
    int weapon_set = 0;                         // --weapons 0..10
    SpawnSelection spawn = SpawnSelection::Random;   // --spawn near|far|random
    int handicap = 0;                           // --handicap N: health bonus of every human (100 + N)
    bool side_by_side = false;                  // --split-vertical: two players side by side (DrawInfo == 1)
    std::uint32_t seed = 0x4E46;                // --seed
    bool log = false;                           // --mp-log: print the match events to stdout

    // Parses `args[i]` (and its value) when it is a match option; advances `i` past the value. Throws on a bad value.
    bool parse(const std::vector<std::string>& args, std::size_t& i);
    static std::vector<std::pair<const char*, std::uint32_t>> mode_names();
    ArenaSettings settings() const;
};

// The body of a local human: the movement half (Player) and the combat half (WeaponSystem).
class HumanBody : public ArenaBody {
public:
    HumanBody(World& world, WeaponSystem& weapons, int slot) : world_(world), weapons_(weapons), slot_(slot) {}
    Vec3 position() const override;
    bool alive() const override;
    bool give_weapon(int weapon_id, int rounds) override;
    int give_ammo(int weapon_id, int rounds) override;
    bool give_armour(float amount) override;
    void kill() override;
    void respawn(const Vec3& pos, float yaw, const MpLoadout& loadout) override;
    void died() override;

private:
    World& world_;
    WeaponSystem& weapons_;
    int slot_;
};

// World + WeaponSystem + ArenaSystem for one match, with the humans spawned through MP_GetSpawnPoint (Player_Init in
// MP_Start). The systems are owned by the World; the session keeps the handles and the bodies.
class ArenaSession {
public:
    // `before_start` runs after the humans exist and before ArenaSystem::start(): the place to fill the bot slots
    // (arena().mutable_settings()) and register their bodies (arena().register_body(4 + k, ...)).
    ArenaSession(World& world, WeaponTable table, const MatchOptions& options, const StringTable* strings,
                 std::string_view tuning_vars_txt, const std::function<void(ArenaSession&)>& before_start = {});

    ArenaSystem& arena() { return *arena_; }
    const ArenaSystem& arena() const { return *arena_; }
    WeaponSystem& weapons() { return *weapons_; }
    const MatchOptions& options() const { return options_; }
    int humans() const { return options_.humans; }

    // One logic frame: World::tick (players, weapons, arena), then the events of the frame are routed to `log`.
    void tick(const PadInputs& pads, FrameTiming timing = FrameTiming{World::kTickHz});

    // HUD data of viewer `slot` from its player's eye.
    ArenaHud hud(int slot) const;

    // Messages / sounds of the last tick, as the front end would show or play them.
    std::vector<MatchMessage>& messages() { return messages_; }
    std::vector<MatchSound>& sounds() { return sounds_; }

    std::function<void(const std::string&)> log;

private:
    World& world_;
    MatchOptions options_;
    WeaponSystem* weapons_ = nullptr;
    ArenaSystem* arena_ = nullptr;
    std::vector<std::unique_ptr<HumanBody>> bodies_;
    std::vector<MatchMessage> messages_;
    std::vector<MatchSound> sounds_;
    MatchPhase last_phase_ = MatchPhase::Running;
    std::vector<int> last_dead_;
    std::vector<int> last_score_;
};

}  // namespace nf

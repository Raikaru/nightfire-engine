#pragma once

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
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
    MpRuleSet rules = MpRuleSet::Ps2;           // --ruleset ps2|gc-xbox|extended
    std::uint32_t mode = mp_mode::kArena;       // --mode
    int humans = 1;                             // --players 1..4 local humans
    int bots = 0;                               // --bots 0..15 Extended; legacy rules reserve four human slots
    std::int32_t score_limit = 10;              // --frag-limit N (-1 unlimited)
    float time_limit = 600.0f;                  // --time-limit MINUTES (<= 0 disables the match timer)
    bool friendly_fire = false;                 // --friendly-fire
    int weapon_set = 0;                         // --weapons 0..10
    SpawnSelection spawn = SpawnSelection::Random;   // --spawn near|far|random
    int handicap = 0;                           // --handicap N: health bonus of every human (100 + N)
    bool side_by_side = false;                  // --split-vertical: two players side by side (DrawInfo == 1)
    std::uint32_t seed = 0x4E46;                // --seed
    bool rng_override = false;                  // --mp-rng X Y: seed the Rand stream from oracle frame-0 words
    std::uint32_t rng_x = 0, rng_y = 0;         // (lockstep/diff runs; skips the --seed fold)
    bool log = false;                           // --mp-log: print the match events to stdout
    bool roster_override = false;               // --mp-seed: exact MPSettings participant slots
    std::array<ArenaSettings::Slot, kMpSlots> roster{};
    bool grapple = false, radar_names = true;  // MPSettings+0x1D0 / +0x1C4

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
    // `before_start` runs after the humans exist and before ArenaSystem::start(): the place to fill bot slots and
    // register their bodies.
    ArenaSession(World& world, WeaponTable table, const MatchOptions& options, const StringTable* strings,
                 std::string_view tuning_vars_txt, const std::function<void(ArenaSession&)>& before_start = {},
                 const MpData* mp_data = nullptr);

    ArenaSystem& arena() { return *arena_; }
    const ArenaSystem& arena() const { return *arena_; }
    WeaponSystem& weapons() { return *weapons_; }
    const MatchOptions& options() const { return options_; }
    int humans() const { return options_.humans; }
    // Activates a server human in an empty/released slot; existing human actors remain unchanged.
    HumanBody& activate_human(int slot, std::string_view name);
    // Idempotently provisions a client-side player/body for snapshots; by default preserves bot metadata.
    HumanBody& ensure_human_actor(int slot, std::string_view name, bool preserve_bot = true);

    // One logic frame: World::tick, then MP weapon-view RNG after all bot systems, then event routing.
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
    const MpData* mp_data_ = nullptr;
    unsigned sleeve_for_character(int character) const;
    WeaponSystem* weapons_ = nullptr;
    ArenaSystem* arena_ = nullptr;
    std::array<std::unique_ptr<HumanBody>, kMpSlots> bodies_{};
    std::vector<MatchMessage> messages_;
    std::vector<MatchSound> sounds_;
    MatchPhase last_phase_ = MatchPhase::Running;
    std::vector<int> last_dead_;
    std::vector<int> last_score_;
};

}  // namespace nf

#pragma once

// What the bot brain reads from the match around it: MPSettings/MPGame participant records, the pickup registry
// (`MPpickups`, spec Part 2A §6) and the scenario objectives (`MPOBJECT`s, Part 1B). The brain never touches Arena /
// World / Nav directly, so its decision code (goal scoring, personalities, opponent choice) is exercised against
// a scripted environment as well as against the live one (bot_arena.cpp adapts ArenaSystem, World, NavNetwork).

#include <array>
#include <cstdint>
#include <source_location>
#include <vector>

#include "assets/mp_data.hpp"
#include "core/math.hpp"

namespace nf::bots {

// MPSettings+0x1a4 scenario masks the bot code tests (assets/mp_data.hpp mp_mode has the same numbers).
namespace scenario {
constexpr std::uint32_t kTopAgent = 0x10;
constexpr std::uint32_t kAssassination = 0x400;
constexpr std::uint32_t kCaptureTheFlag = 0x20000004;
constexpr std::uint32_t kDemolition = 0x20000040;
constexpr std::uint32_t kProtection = 0x20000080;
constexpr std::uint32_t kEspionage = 0x20000100;
constexpr std::uint32_t kGoldenEye = 0x20000200;
constexpr std::uint32_t kKingOfTheHill = 0x40000800;
constexpr std::uint32_t kTeamKingOfTheHill = 0x60001000;
constexpr std::uint32_t kUplink = 0x60000008;
}  // namespace scenario

// MPGame[slot]+0x26 per-player objective flags.
namespace objflag {
constexpr std::uint16_t kCarryingFlag = 1, kCarryingBlueprint = 2, kInHill = 0x10, kCarryingAny = 0xf;
}

// One participant slot (0..3 humans, 4..7 bots): MPSettings[slot] + MPGame[slot] + the body.
struct Participant {
    bool valid = false;            // a registered participant
    bool alive = false;            // alive test of BotGlobal (health > 0, not eliminated)
    bool is_bot = false;
    std::uint8_t object_type = 0;   // obj+0xff (Control_Plr2Ind / MP_ReSpawn; RAM confirms live 2/3): bot 2/out 0x11, human 3/out 0x12
    Vec3 pos{};                    // obj+0x30
    float yaw = 0;                 // obj+0x54
    int team = 2;                  // MPSettings[slot]+0x20 (0 Phoenix, 1 MI6, 2 none)
    float score = 0;               // MPGame[slot]+0x18
    float health = 100;            // Drone+0xac / player +0x894
    std::uint16_t obj_flags = 0;   // MPGame[slot]+0x26
    int last_killer = -1;          // MPGame[slot]+0x28 (slot of the last player who killed it)
    bool concealed = false;        // "revealed shooter": bots Drone+0x3c == 0 (burst not done), humans +0x95b == 1 (BOT_setOtherPlayerInfo bit 1)
    bool crouching = false;
    bool moving = false;
    bool controllable = false;     // Control_Plr2Ind >= 0: a human-controlled player
};

// Registered pickup (MPpickups[i] + PICKUPINFO).
struct PickupView {
    Vec3 pos{};
    int category = 0;              // PICKUPINFO+0x22: 0 weapon, 1 ammo, 3 armour/health
    int item = 0;                  // PICKUPINFO+0x24 weapon-def id
    bool respawning = false;       // PICKUPINFO+0x20 == 2
    float visit_until[kMpMaxBots] = {};   // MPpickups[i]+0x80 per-bot lock (game-clock seconds, 0 = free)
};

// Scenario objective object (MPOBJECT) reduced to what the bot goal code needs. `kind` is the goal kind of
// spec Part 2A §1.6 (1 CTF flag, 2 CTF base, 3 GoldenEye, 4 blueprint, 5 espionage base, 6 uplink, 7 hill,
// 8 demolition/protection target).
struct ObjectiveView {
    int kind = 0;
    Vec3 pos{};
    int team = 2;                  // owning side (0/1) or 2
    bool taken = false;            // team flag bits (`MPGame+team*2+0x1a8`) say it is already done / stolen
    int id = -1;                   // stable handle (index in the environment's objective list)
};

class BotEnv {
public:
    virtual ~BotEnv() = default;

    virtual std::uint32_t scenario() const = 0;          // MPSettings+0x1a4
    virtual bool teams_on() const = 0;                   // MPSettings+0x18c
    virtual float clock_seconds() const = 0;             // MPGame+0x19c
    virtual std::uint32_t tick() const = 0;              // GameState+0x34
    virtual int weapon_set_start() const = 0;            // startweap (base id of PickupMatrix[set].slot0)

    virtual Participant participant(int slot) const = 0;
    virtual int participant_count() const { return int(kMpSlots); }
    virtual bool match_playing() const { return true; }       // GameFlow_GetState() == 2
    virtual bool location_damage() const { return true; }     // MPSettings+0x1c8 (P_MPPLAYERMODS "location damage")
    virtual bool professional_mode() const { return false; }  // MPSettings+0x1bc (x3 damage taken)

    virtual int pickup_count() const = 0;
    virtual PickupView pickup(int index) const = 0;
    virtual void set_pickup_visit(int index, int bot_index, float until) = 0;   // BOTSTATE_setPickupVisitTime
    virtual void reset_pickup_visits(int bot_index) = 0;                        // MP_resetPickupBotVisitTimes
    // Path distance from the bot (its feet + nearest-node cache) to a pickup / objective through the nav
    // emitters (NDrone2_DistanceToEmitter). False = unreachable.
    virtual bool distance_to_pickup(int bot_slot, int index, float* out) = 0;

    // Opponent participant slot a bot currently targets (Drone+0x170 of slot's drone), -1 none / not a bot.
    virtual int bot_opponent(int slot) const { (void)slot; return -1; }
    // Personality of a bot slot (0 for humans / unknown), for Guardian's "non-Guardian bot" rule.
    virtual int bot_personality(int slot) const { (void)slot; return 0; }
    // Designated victim (BOT_vars+0x76b) of a bot slot, -1 none: the Vengeful +2 kill bonus reads it.
    virtual int bot_trait_opponent(int slot) const { (void)slot; return -1; }
    // Whether bot `target` is itself targeted by a bot (BOT_vars+0x770): Berserker/Guardian pile-on reads
    // the *candidate's* flag and only for bot candidates (slot >= 4).
    virtual bool bot_targeted(int slot) const { (void)slot; return false; }
    // Another bot's perception cache (BOT_vars+0x0b0 other[]) for the mirror in BOT_setOtherPlayerInfo:
    // when bot `slot` sees `other` as valid, this bot copies its sq_dist and visible bit instead of
    // recomputing. Defaults: not mirrored (fresh compute, as for humans).
    virtual bool bot_mirror(int slot, int other, float* sq_dist, bool* visible) const {
        (void)slot; (void)other; (void)sq_dist; (void)visible;
        return false;
    }
    // Assassination (0x400): the slot `slot` must hunt if it is the assassin, else -1 (MP_IsAssasin/MP_getAssassinTarget).
    virtual int assassin_target_for(int slot) const { (void)slot; return -1; }
    // MP_isPosOnHill: point-in-hill-volume test (hill-local AABB, no rotation) for the KOTH combat-move veto.
    virtual bool hill_contains(const Vec3& p) const { (void)p; return false; }
    // BOTSTATE_isObjAlreadyAnotherTeamObjective: a teammate bot (other than `slot`) already has this objective.
    virtual bool objective_claimed_by_teammate(int objective_id, int slot) const {
        (void)objective_id; (void)slot;
        return false;
    }

    // Objectives as slot `for_slot` sees them (`taken` depends on the asker's team).
    virtual std::vector<ObjectiveView> objectives(int for_slot) const { (void)for_slot; return {}; }
    virtual bool distance_to_objective(int bot_slot, const ObjectiveView& o, float* out) {
        (void)bot_slot; (void)o; (void)out;
        return false;
    }
    // The slot stands inside its team's protection / demolition zone (sphere r=3 against the objective box).
    virtual bool in_defended_zone(int slot) const { (void)slot; return false; }
    // MP_BluePrintReachedBase: a bot carrying the blueprint stands in its team's espionage base.
    virtual void objective_reached(int bot_slot, int objective_id) { (void)bot_slot; (void)objective_id; }
    // Rand_Rand(n) in [0, n).
    virtual std::uint32_t rand(std::uint32_t n,
                               const std::source_location& loc = std::source_location::current()) = 0;
};

}  // namespace nf::bots

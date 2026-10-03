#pragma once

// The MP bot brain: `BOT_vars_t` (spec Part 2A §1.5) plus the BOT_* / BOTSTATE_* logic that is not a state handler
// (goals, distraction/commitment, recovery, perception cache, opponent choice, combat move selection, weapon
// switching, pain). Attached to a drone as its `DroneExt` (Drone+0xd1c -> BOT_vars_t). The state handlers
// 0xc3..0xf9 live in bot_states.cpp and reach the brain through `Drone::ext_as<BotBrain>()`.
//
// Everything that is not brain logic goes through two seams:
//   * BotEnv  (bot_env.hpp)  - the match: participants, pickups, objectives, clock;
//   * BotBody (below)        - the drone's body services (NDrone2_Move*, CanStrafe*, aiming, firing, anim
//                              helpers); bot_drone.cpp implements it on DroneCore's Drone.

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "game/bot_data.hpp"
#include "game/bot_env.hpp"
#include "game/bot_weapons.hpp"
#include "game/drone.hpp"

namespace nf::bots {

// State ids (docs/spec-arena-ai.md 2A §2.1).
namespace st {
constexpr int kInit = 0xc3, kRespawn = 0xc4, kGlobal = 0xc5;
constexpr int kCollector = 0xc6, kAssassin = 0xce;             // 0xc6..0xce: never entered (stubs)
constexpr int kAttack = 0xcf, kAttackRun = 0xd0, kAttackNoRoute = 0xd1, kAttackFire = 0xd2, kAttackNoOpponent = 0xd3;
constexpr int kStrafeAimLeft = 0xd4, kStrafeAimRight = 0xd5, kRunChangePosition = 0xd6, kBackoff = 0xd7;
constexpr int kCrouch = 0xd8, kRollLeftCrouch = 0xd9, kRollRightCrouch = 0xda, kStepAimLeft = 0xdb,
              kStepAimRight = 0xdc, kReload = 0xdd, kChangeWeapon = 0xde, kUnarmed = 0xdf;
constexpr int kCoverRunTo = 0xe0, kCoverLeaveNow = 0xe8;       // cover states: rejected by validateStateChange
constexpr int kStuck = 0xe9, kAlertToPosition = 0xea, kGotoGoal = 0xeb;
constexpr int kSeenOpponent = 0xec, kSeenDroneShot = 0xed, kHeardNoise = 0xee;
constexpr int kImpactBullet = 0xef, kImpactExplosive = 0xf0, kImpactPunch = 0xf1;
constexpr int kDeathAnim = 0xf2, kDeathByExplosion = 0xf3, kDead = 0xf4, kImpactStunGrenade = 0xf5;
constexpr int kDoorOpen = 0xf6, kGuardFriendIdle = 0xf7, kGuardFriendFollow = 0xf8, kIdle = 0xf9;
}  // namespace st

// BOTSTATE_getStateType (`bot_state_types` @0x26f460, 55 bytes for ids 0xc3..0xf9).
int state_type(int state);

// Message ids of the MP bot messages (MP_sendBotMessage), spec §2.2.
namespace botmsg {
constexpr int kObjectiveChanged = 0x3a, kGoalComplete = 0x3b, kUninitGoal = 0x3c, kCancelGoalToObj = 0x3d,
              kDemolitionTarget = 0x40, kProtectionTarget = 0x41, kWeaponChange = 0x42, kPlayerDied = 0x43,
              kTeammateWeapon = 0x44, kOpponentSet = 0x45;
}

// NDrone2_Move* result codes (route status as returned to BOTSTATE_validateRoute).
namespace moveres {
constexpr int kArrived = 3, kBlocked = 5, kDoor = 10;
}

// The body of a bot: everything NDrone2_* the states call. Opponent / participants are addressed by MP slot.
class BotBody {
public:
    virtual ~BotBody() = default;

    // ---- navigation (AINetwork_SetupGoalPosition*, NDrone2_MoveTo*) ----
    virtual void setup_goal_position(const Vec3& pos, float speed_mul) = 0;
    virtual void setup_goal_participant(int slot, float speed_mul) = 0;          // ...ToObj (moving target)
    virtual int move_to_goal_position(float speed) = 0;                          // result codes (moveres)
    virtual int move_to_participant(int slot, float speed, bool run) = 0;        // NDrone2_MoveToObject
    virtual int move_to_alert_position(float speed) = 0;                         // NDrone2_MoveToAlertPosition
    virtual void invalidate_attack_route() = 0;                                  // NDrone2_InvalidateAttackRoute
    virtual bool at_dest() = 0;                                                  // NDrone2_AtDest
    virtual bool near_drone(float radius) = 0;                                   // NDrone2_NearDrone
    virtual void set_angle_to_dest() = 0;                                        // NDrone2_SetAngleToDest
    virtual void set_angle_to_participant(int slot, float offset) = 0;           // NDrone2_SetAngleToObj
    virtual void anim_for_route_distance() = 0;                                  // NDrone2_AnimForDist(route remaining distance)
    virtual void set_opponent(int slot) = 0;                                     // core side of NDrone2_SetOpponent (-1 = none)
    virtual bool can_see_participant(int slot) = 0;                              // NDrone2_CanSeeObject(drone, obj, 5, 0)

    // ---- move feasibility (MoveTest based) ----
    virtual bool can_backoff() = 0;
    virtual bool can_strafe_left() = 0;
    virtual bool can_strafe_right() = 0;
    virtual bool can_roll_left() = 0;
    virtual bool can_roll_right() = 0;
    virtual bool can_step_left() = 0;
    virtual bool can_step_right() = 0;
    virtual int evasive_move() = 0;             // NDrone2_EvasiveMove: 0x75 strafe L, 0x76 strafe R, 0x79 roll L, 0x7a roll R, 0

    // ---- firing / aiming ----
    virtual void fire(int mode) = 0;            // DroneWeap_Fire(1)
    virtual void aim_at_opponent() = 0;         // DroneWeap_AimTarget_IsOpponent(0, 1)
    virtual void reset_firing() = 0;            // Drone+0xbc0 = 0 (burst), Drone+0x3b = 0 (request)
    virtual bool fire_requested() = 0;          // Drone+0x3b: a firing state asked for DoFiring every tick
    virtual bool burst_done() = 0;              // Drone+0x3c: the requested burst finished
    virtual bool can_alt_attack(int index) = 0; // NDrone2_CanAltAttack
    virtual bool can_do_anim_state(int anim) = 0;   // DroneAnim_CanDoAnimState
    virtual bool door_is_open() = 0;                                             // NDrone2_DoorIsOpen
    virtual void open_door() = 0;                                                // NDrone2_OpenDoor
    virtual void impact_reaction(int msg_id) = 0;                                // NDrone2_Punch/Bullet/ExplosiveImpact (6/8/9)
    virtual void location_death_anim(int end_state) = 0;                         // DroneAnim_LocationDeathAnim
    virtual void explosion_death_anim(int end_state, std::intptr_t hit) = 0;     // NDrone2_ExplosionDir + anim 0x44
    virtual void drop_weapon() = 0;                                              // DroneWeap_DropWeapon
    virtual void invalidate_nearest_node() = 0;                                  // BOTSTATE_setNearestNavNode
    virtual void play_sfx(int sfx_id, bool replace) = 0;                         // NDrone2_PlaySFX (voice channel)
    virtual bool sfx_playing() = 0;
    virtual void stop_voice() = 0;
};

// One goal slot (BOT_vars+slot*0x50, spec §1.6).
struct BotGoal {
    Vec3 pos{};
    bool has_pos = false;          // CelPos cel pointer != 0 (a position target)
    float distraction_limit = 0;   // +0x20
    float set_time = 0;            // +0x24 game-clock seconds
    float timeout = 300.0f;        // +0x28
    float w_armour = 0, w_ammo = 0, w_weapon = 0, w_objective = 0;   // +0x2c..+0x38
    int target = -1;               // +0x3c: pickup index / objective id / participant slot
    int return_state = st::kIdle;  // +0x40
    bool complete = false;         // +0x44 bit0
    std::uint8_t type = 0;         // +0x45: 0 none, 1 pickup, 2 objective, 3 chase player
    std::uint8_t flags = 0;        // +0x46
    std::uint8_t max_range = 0xfe; // +0x47
    int last_result = 0;           // +0x48
    int slot = 0;                  // +0x49
    int kind = 0;                  // +0x4a: 0 pickup, 1 flag, 2 base, 3 goldeneye, 4 blueprint, 5 esp. base, 6 uplink, 7 hill, 8 demolition/protection, 9 chase
};
namespace goaltype { constexpr int kNone = 0, kPickup = 1, kObjective = 2, kPlayer = 3; }
namespace goalflag { constexpr std::uint8_t kFirstPassOnly = 2, kIgnoreRespawning = 4, kValidateRoute = 8, kNoInterrupt = 0x10, kIgnoreVisitLocks = 0x20; }

// BOT_vars other[8]: per participant perception cache.
struct OtherInfo {
    float stamp = 0;               // +0 concealment timestamp
    float sq_dist = 0;             // +4
    float facing = 0;              // +8 (deg)
    std::uint32_t flags = 0;       // +0xc
};
namespace otherflag { constexpr std::uint32_t kConcealed = 1, kValid = 2, kVisible = 4, kSameTeam = 8; }

// BOT_vars +0x734 bits.
namespace bitflag { constexpr std::uint32_t kRecovering = 2, kCommitted = 4; }

struct BotVars {
    std::array<BotGoal, 2> goal;
    BotStats stats{};
    std::uint16_t max_health = 100;               // +0xa4
    std::array<OtherInfo, 8> other;
    Vec3 prev_opponent_pos{};                     // +0x130
    std::array<int, 16> history{};                // +0x6dc opponent history ring (participant slots, -1 empty)
    int history_head = 0;                         // +0x76c
    float combat_range[3] = {4.0f, 12.0f, 13.0f}; // +0x71c/0x720/0x724: Drone +0xec / +0xf0 / +0xfc
    float distraction = 0;                        // +0x728
    int pending_state = 0;                        // +0x72c
    std::uint32_t goto_stamp = 0;                 // +0x730
    std::uint32_t bits = 0;                       // +0x734
    std::uint32_t next_regen = 0;                 // +0x738
    std::uint32_t last_hit_tick = 0;              // +0x740
    std::uint32_t last_route_fail_tick = 0;       // +0x744
    std::uint32_t recovery_end = 0;               // +0x748
    std::uint32_t hat_tick = 0;                   // +0x74c
    int friend_slot = -1;                         // +0x758 guard target
    int slot = 4;                                 // +0x75c participant slot 4..7
    int bot_index = 0;                            // +0x75e
    int state_override = 0;                       // +0x762
    int character = 1;                            // +0x764
    int active_goal = -1;                         // +0x765
    int state_type = 0;                           // +0x766
    int rr_index = 0;                             // +0x767 round-robin LOS
    int desired_weapon = 0;                       // +0x76a
    int trait_opponent = -1;                      // +0x76b
    int route_fail_count = 0;                     // +0x76d
    int last_pickup = -1;                         // +0x76e
    bool alerted = false;                         // +0x76f
    bool targeted_by_bot = false;                 // +0x770
    bool in_zone = false;                         // +0x771
    int armour = 0;                               // +0x769 armour points (cap 50)
    std::array<OtherInfo, kMpSlots - 8> extended_other{};
    OtherInfo& other_info(int participant_slot) {
        return participant_slot < 8 ? other[std::size_t(participant_slot)]
                                    : extended_other[std::size_t(participant_slot - 8)];
    }
    const OtherInfo& other_info(int participant_slot) const {
        return participant_slot < 8 ? other[std::size_t(participant_slot)]
                                    : extended_other[std::size_t(participant_slot - 8)];
    }
    Personality personality() const { return Personality(stats.personality); }
    bool has_flag(std::uint8_t f) const { return (stats.ability_flags & f) != 0; }
};

class BotBrain : public drone::DroneExt {
public:
    BotBrain(const BotSpec& spec, BotEnv& env, BotBody& body, const WeaponTable& weapons,
             BotArmoury::LoadedFn loaded = {});

    BotVars v;
    BotArmoury arm;
    BotEnv* env;
    BotBody* body;
    drone::Drone* self = nullptr;      // set by BotSystem right after the drone is spawned (Drone back-pointer)
    BotSpec spec;

    // ---- BOT_init / BOT_respawn (per-body part) ----
    void init_stats(drone::Drone& d);                   // BOT_setDroneStats + BOT_postLoadInit
    void reset_for_respawn();                           // memset BOT_vars, re-init weapons and goals
    // ---- perception ----
    void set_other_player_info();                       // BOT_setOtherPlayerInfo (once per tick)
    bool find_opponent();                               // NDrone2_FindOpponent, MP branch (+ history)
    void handle_opponent_history(int candidate);        // BOT_handleOpponentHistory
    void set_opponent(int slot);                        // NDrone2_SetOpponent (slot -1 = none)
    int opponent() const { return opponent_slot_; }
    void opponent_targetting();                         // BOT_opponentTargetting (aim wobble / motion tracking)
    // ---- stats ----
    float aggression_mul() const;                       // BOT_getAggressionMul
    float speed_mul() const { return movement_speed_mul(v.stats.move_speed); }
    bool move_possibility(int n);                       // BOT_getMovePossibility
    // ---- pain / death ----
    float handle_pain(float damage, int damage_type, int location);   // BOT_handlePain
    void set_health(float h);                           // BOT_SetHealth
    void sound_effect(int which);                       // BOT_soundEffect (30/31 pain, 1 death)
    // ---- distraction / recovery / state change ----
    bool increase_distraction(float d);                 // BOTSTATE_increaseDistraction
    bool is_distracted() const;                         // BOTSTATE_isDistracted
    void start_recovery();                              // BOTSTATE_startRecovery
    void set_state_change(int state);                   // BOTSTATE_setStateChange (SetState + remember for BotGlobal)
    int validate_state_change(int requested);           // BOT_validateStateChange: state to enter or -1
    // ---- goals ----
    void set_goal_pick_prefs(float w_armour, float w_ammo, float w_weapon, float w_obj, int slot, int max_range,
                             int flags);                // BOTSTATE_setGoalPickPrefs
    bool pick_goal(int slot);                           // BOTSTATE_pickGoal
    bool goto_goal(int slot, int return_state);         // BOTSTATE_gotoGoal
    void init_goal(int slot, int type, const Vec3* pos, int target, int kind);   // BOTSTATE_initGoal
    void uninit_goal(int slot);                         // BOTSTATE_uninitGoal
    void set_goal_complete(int slot) { v.goal[std::size_t(slot)].complete = true; }
    int active_goal() const { return v.active_goal; }   // BOTSTATE_getActiveGoal
    void cancel_goal_to_obj(int participant_or_objective, bool is_participant, int state);   // BOTSTATE_cancelGoalToObj
    void process_goals();                               // BOTSTATE_processGoals
    bool validate_route(int result);                    // BOTSTATE_validateRoute
    void set_pickup_visit_time(int pickup);             // BOTSTATE_setPickupVisitTime
    int preferred_trait_opponent();                     // BOTSTATE_getPreferredTraitOpponentObjIndex
    // ---- combat moves ----
    int choose_combat_move();                           // BOTSTATE_chooseCombatMove
    int evasive_move_state();                           // BOTSTATE_EvasiveMove
    int really_want_combat_move(int mask);              // BOTSTATE_reallyWantACombatMove
    int check_attack_move(int state);                   // BOTSTATE_checkAttackMove
    int choose_unarmed_attack_anim();                   // BOTSTATE_chooseUnarmedAttackAnim
    void default_combat_range();                        // BOTSTATE_defaultCombatRange
    void really_close_combat_range();                   // BOTSTATE_reallyCloseCombatRange
    bool opponent_is_missile() const;                   // BOTSTATE_opponentIsMissile
    // ---- weapons ----
    int combat_weapon_change_choice(bool check_same, bool silent);   // BOTSTATE_combatWeaponChangeChoice
    int change_weapon(int id, bool check_same, bool silent);          // BOTSTATE_changeWeapon
    bool switch_weapon(int id);                         // BOTWEAP_changeWeapon + drone side (anim set, mirrors)
    // ---- helpers ----
    std::uint32_t now() const { return self->now(); }
    bool in_hill() const;                               // MPGame[me]+0x26 & 0x10
    bool is_protection_or_demolition_defender() const;
    bool has_opponent() const { return opponent_slot_ >= 0; }
    int team() const { return team_; }
    void set_team(int t) { team_ = t; }
    const OtherInfo* opponent_info() const { return opponent_slot_ >= 0 ? &v.other[std::size_t(opponent_slot_)] : nullptr; }
    bool opponent_seen() const;                         // other[opp].flags & 6 == 6 or Drone+0x228 & 4
    bool alive_participant(int slot) const;

    std::uint32_t handled_msgs = 0;
    int opponent_slot_ = -1;                            // participant slot resolved from Drone+0x170 obj_tag*
    int team_ = 2;
    int door_return_state_ = st::kIdle;                 // Drone+0x5a6
    int react_to_opponent_sighted(int state);           // NDrone2_ReactToOpponentSighted (MP bots: the requested state)
    void update_opponent_tracking();                    // per-tick +0x1a0/+0x1d4/+0x270/+0x274/+0x238 upkeep
    // Stuck watchdog (ours; the original relies on route failures): position sampled while walking to a goal.
    Vec3 stuck_ref_{};
    std::uint32_t stuck_ref_tick_ = 0;
    std::uint32_t respawn_tick_ = 0;

    // Event log hook: (event name, detail) - used by nfgame --bot-log and nfdump.
    std::function<void(BotBrain&, const char* event, const std::string& detail)> on_event;
    void log_event(const char* event, const std::string& detail = {}) {
        if (on_event) on_event(*this, event, detail);
    }
    // Kill hook (BotDeathAnim: MP_PlayerKilled).
    std::function<void(BotBrain&)> on_died;
    std::function<void(BotBrain&)> on_respawn_request;   // BotRespawn enter -> BOT_respawn
};

// Registers states 0xc3..0xf9 with the drone core (call before spawning bots). Idempotent.
void register_bot_states();

}  // namespace nf::bots

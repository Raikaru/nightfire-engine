#pragma once

// The shared NDrone2 core: `Drone` mirrors the parts of `Drone_tag` (0xd20 B, obj+0xe0) that the AI reads.
// Field comments carry the original offsets (docs/spec-arena-ai.md Part 3). Used by both content layers:
// single-player enemies (DTYPE/DMODE tables, states 0..194) and multiplayer bots (BOT_*, DTYPE 0x1e,
// states 0xc3..0xf9). A bot IS a drone: `dtype == kDtypeBot`.
//
// Time: everything is counted in `World::timer_frame()` ticks (GameState+0x34) at the current session rate.
// `Drone::seconds(x)` converts seconds to ticks using FRAME_RATE; literal frame counts in the spec (e.g.
// "30-frame shout delay") stay literal.

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "assets/character.hpp"
#include "core/math.hpp"
#include "game/drone_sm.hpp"
#include "game/nav.hpp"

namespace nf {
class World;
class CollisionWorld;
struct FrameTiming;
}  // namespace nf

namespace nf::drone {

class DroneSystem;
struct DroneHit;

// ---- behaviour bit-set (`_BehaviourStruct`, 3 words, Drone+0x4dc / +0x4e8; ids 0..90 via bitDescs) ------
// ids 0-31 word0 bit id; 32 word1[0..1]; 33-47 word1 bits 2-16; 48 word1[17..19]; 49-60 word1 bits 20-31;
// 61-67 word2 bits 0-6; 68 word2[7..8]; 69-90 word2 bits 9-30. (behaviour_util_getProperty 0x1c7d98)
struct Behaviour {
    static constexpr int kCount = 91;
    std::array<std::uint32_t, 3> word{};
    unsigned get(int id) const;              // field value (0/1 for the single-bit ids)
    void set(int id, unsigned value = 1);
    bool has(int id) const { return get(id) != 0; }
};

// Named behaviour ids (inferred meanings, spec §2.3). Names ours; ids exact.
namespace beh {
enum : int {
    kAimCrouch = 0x11, kStrafeDodge = 0x13, kStrafe = 0x41, kStep = 0x42, kRoll = 0x2e,
    kPatrolStart = 0x24, kPatrolAlert = 0x25,
    kHearsNoise = 0x1f,
    kShoutReactFirstSight = 0x28, kShoutReactHurt = 0x27, kShoutReactType3 = 0x4f, kShoutReactType5 = 0x29,
    kReactsToAlertedDrone = 0x26,
    kShoutsOnFirstSight = 0x33, kShoutsWhenHurt = 0x32,
    kMaySurrender = 0x43,
    kNoticeBelowAware = 0x51,
    kSeesOpponents = 0x3c,
    kFireAtLastKnown = 0x17,      // keeps firing at last-known pos (no sight timeout)
    kNeverMovesInCombat = 0x40,
    kHeadTracks = 0x4c,
    kIdleFidget = 0x46,
    kAimStandPreferred = 0x18,
    kCombatMoveAlt = 0x19, kChallengeNear = 0x1a, kChallengeFar = 0x1b,
    kGrenadeTosser = 0x4a,
    kFailsMissionOnFirstSight = 0x52,
    kCaptainEasy = 9, kCaptainNormal = 5, kCaptainHard = 8,
    kCoverLimit = 0x44,
    kIgnoresExplosives = 0x34, kIgnoresGas = 0x36,
};
}

// ---- Drone+0x4f8 flag bits (partially named in the spec) -----------------------------------------------
namespace flag {
constexpr std::uint32_t kStationary = 0x10;
constexpr std::uint32_t kAlertedShout = 0x08;       // hurt shout already sent (BulletImpact)
constexpr std::uint32_t kActive = 0x100;
constexpr std::uint32_t kDisabled = 0x200;
constexpr std::uint32_t kDeathProcessed = 0x400;
constexpr std::uint32_t kDeadMask = 0x600;
constexpr std::uint32_t kCoverClaimed = 0x800, kCoverClaimed2 = 0x1000;
constexpr std::uint32_t kAlertableByDroneSight = 0x2000;
constexpr std::uint32_t kAware = 0x10000;
constexpr std::uint32_t kAlertedByDrone = 0x20000;
constexpr std::uint32_t kFirstSightShoutSent = 0x200000;
constexpr std::uint32_t kDeaf = 0x400000;
constexpr std::uint32_t kIgnoresShouts = 0x800000;
constexpr std::uint32_t kFirstAttackDone = 0x2000000;
constexpr std::uint32_t kAlertedByNoise = 0x4000000;
constexpr std::uint32_t kSawPlayer = 0x8000000;
constexpr std::uint32_t kShoutSource = 0x10000000, kNoiseSource = 0x20000000, kDroneAlertSource = 0x40000000;
}  // namespace flag

// Drone+0x228 sight flags: 4 = seen (reaction time elapsed), 8 = first sighting done.
namespace sight {
constexpr std::uint32_t kSeen = 4;
constexpr std::uint32_t kFirstSighted = 8;
}

// DALS_* (Drone_AlertStatusSet 0x13a8d8)
enum class AlertStatus : std::uint8_t { Relaxed = 0, Alert = 1, Scared = 2, Dead = 3 };

// DTYPE values used by the core (Drone+0xc5; DroneTypeSettings @0x29aee8)
constexpr int kDtypeSniper = 2, kDtypeHostageKiller = 4, kDtypeHostage = 5, kDtypeCivilian = 9,
              kDtypeNinja = 0xd, kDtypeSniperAlert = 0x19, kDtypeBot = 0x1e;
// Drone+0x44 side
enum Side : std::uint8_t { kSideEnemy = 1, kSideFriend = 2, kSideNeutral = 3 };

// Opponent reference: a player slot (World::player) or a drone (DroneSystem id). Original: 8 target slots
// NPCGlobals+0x300 (+4 obj*) allocated by DroneFunc_AllocateTargetID / NDrone2_SetOpponent 0x1422d0.
struct TargetRef {
    enum class Kind : std::uint8_t { None, Player, Drone } kind = Kind::None;
    int index = -1;   // player slot or drone id
    bool valid() const { return kind != Kind::None; }
    bool operator==(const TargetRef&) const = default;
    static TargetRef player(int slot) { return {Kind::Player, slot}; }
    static TargetRef drone(int id) { return {Kind::Drone, id}; }
};

// Content-layer data hung on a drone (SP: DIVars/spawner link/patrol; MP: BOT_vars_t at Drone+0xd1c).
struct DroneExt {
    virtual ~DroneExt() = default;
};

// Alert record (`DroneAlert_tag`, NPCGlobals+0x290): payload of shout / alert messages (Msg::ptr).
struct AlertRecord {
    int alerted_to = -1;     // +0  obj that the alert is about (TargetRef id, -1 none)
    int source = -1;         // +4  drone id that raised it
    std::uint32_t time = 0;  // +8
    int msg = 0;             // +0xc
    Vec3 position{};         // +0x10
    Vec3 direction{};        // +0x30
    float radius_a = 20.0f;  // +0x50
    float radius_b = 20.0f;  // +0x54
    float factor = 1.0f;     // +0x58
    TargetRef target;
};

// Character data referenced by a drone: skin + weapon-in-hand model (the renderer resolves the datum).
struct DroneLook {
    std::uint32_t skin_hash = 0;         // 0x5000004.. (DIVars+0x34)
    std::uint32_t weapon_model_hash = 0; // model attached at the right-hand datum, 0 = none
    int hand_datum = -1;                 // datum index for CharacterInstance::datum_world (-1 = none)
    const AnimSet* anim_set = nullptr;   // locomotion set (AnimSet_*), see drone_anim.hpp
};

// Everything needed to create a drone (DIVars: pos/rot + level_tag params, spec §1.2).
struct SpawnInfo {
    Vec3 feet{};                     // spawn position on the floor
    float yaw = 0;
    std::uint32_t skin_hash = 0;     // DIVars+0x34
    std::uint32_t script_id = 0x6000000;   // DIVars+0x38 (0x6000000 = none)
    int min_difficulty = 0;          // DIVars+0x3c
    int dmode = 0;                   // DIVars+0x48
    int alt_dmode = 0x65;            // DIVars+0x4c
    int voice_set = 0;               // DIVars+0x58
    int sight_profile = 0;           // DIVars+0x6c (0..5)
    int accuracy_class = 5;          // Drone+0xb4
    int aggression = 2;              // Drone+0xb5 (0..4; 2 = neutral)
    float health = 10.0f;            // Drone+0xac
    int weapon = 0;                  // weapon_data index
    int side = 0;                    // 0 = from dmode table
    int dtype = -1;                  // -1 = from dmode table
    int char_class = 0;              // Drone+0xd8
    int sub_class = 0;               // Drone+0xda
    int initial_state = -1;          // -1: Global ENTER picks dtype/dmode initial state
    Behaviour behaviour{};
    int player_slot = -1;            // bots: MP participant slot
    std::unique_ptr<DroneExt> ext;
};

// Hit delivered to a drone (adapts Weapons' HitInfo when damage.hpp lands; see DroneSystem::hurt_target).
struct DroneHit {
    float damage = 0;
    int type = 0;                 // weapon damage type
    int weapon = 0;               // weapon_data id
    int attacker = -1;            // participant slot; -1 env
    Vec3 point{};
    Vec3 direction{};
    int part = -1;                // hit bone id (5 head, arms {0x14,0x15,0x17,0x20,0x23,0x27}, legs 0x31..0x38)
    bool blast = false;           // explosion (no location, no direction) -> kMsgExplosive
};

class Drone {
public:
    // ---- identity / SM ------------------------------------------------------------------------------
    int id = 0;                         // StateMachineInfo+0 (unique, 1-based)
    std::uint8_t obj_type = 2;          // obj+0xff: 2 drone, 0x11 MP bot body
    int player_slot = -1;               // bots: MP participant slot; SP drones -1
    DroneSystem* sys = nullptr;

    struct StateMachine {               // Drone+0x108 StateMachineInfo_tag
        int cur = 0;                    // +4  (obj+0xf4 mirrors it)
        int prev = 0;                   // +8
        int next = 0;                   // +0xc
        int saved = 0;                  // +0x10 "return to" state for scripts/impacts
        std::uint32_t entry_time = 0;   // +0x14
        bool pending = false;           // +0x18
        int result = 0;                 // +0x1c
        std::intptr_t arg = 0;          // +0x24
    } smi;

    // ---- health / combat stats ------------------------------------------------------------------------
    float health = 10.0f;               // +0xac
    float max_health = 10.0f;           // +0xb0
    float last_damage = 0;              // +0x150
    float bullet_damage_mod = 1.0f;     // +0x100
    std::uint8_t accuracy_class = 5;    // +0xb4 (lower = better; p = 100 - 5*acc)
    std::uint8_t aggression = 2;        // +0xb5
    std::uint8_t armour = 0;            // +0xbb: 1 vest, 2 jacket, 4 helmet?, 8 combat (BOTWEAP_setDroneArmour)
    int hit_count = 0;                  // +0x1d8 (bullet hits taken)
    int last_shooter = -1;              // +0x2b4 (TargetRef index of the shooter, player slot or ~drone id)
    std::uint8_t start_channel = 0;     // +0x134 (WaitSwitch gate; 0 = none)
    bool start_channel_snapshot = false;      // +0x136 snapshot of the channel at creation
    std::uint8_t alt_channel = 0;       // +0x135 (alt mode switch)
    bool alt_channel_snapshot = false;        // +0x137
    bool hidden = false;                // obj+0xf0 & 0x10: not drawn, not controlled (NDrone2_Enable(false))
    bool ghost = false;                 // obj+0xf0 & 0x20: no longer hit by bullets
    std::uint32_t carried_item = 0;     // +0xbd4 (key card etc.)
    bool death_pending_channel = false; // +0xcd8
    bool weapon_dropped = false;
    float fade = 1.0f;                  // obj+0x106 / +0xcfc alpha while fading
    bool pending_delete = false;        // obj+0xfe & 1
    bool damageable = true;             // +0x14 (false: hits are recorded but do no damage)
    bool death_reported = false;        // DroneFunc_OnInitDeath ran
    int taser_hits = 0;                 // +0xbc
    std::uint32_t impact_anim_time = 0; // +0x540 last flinch anim
    bool has_last_hit = false;
    DroneHit last_hit;                  // +0x470 the last hit (kMsgBullet / kMsgExplosive payload)
    bool head_shot = false;             // +0x3a
    bool heavy_hit = false;             // +0x1c
    bool invulnerable_while_anim = false;  // +0x1b
    bool captain = false;               // +0x19

    // ---- type / mode ----------------------------------------------------------------------------------
    std::uint8_t dtype_base = 0;        // +0xc4
    std::uint8_t dtype = 0;             // +0xc5
    std::uint8_t dtype_alt = 0;         // +0xc6
    int dmode = 0;                      // +0x138
    int alt_dmode = 0x65;               // +0x13a
    std::uint8_t side = kSideEnemy;     // +0x44
    std::uint32_t skin_hash = 0;
    std::uint16_t char_class = 0;       // +0xd8
    std::uint16_t sub_class = 0;        // +0xda
    int voice_set = 0;
    int initial_state = 4;              // +0x5a2
    int pre_state = 0;                  // +0x5a0: previous return state (BOTSTATE_gotoGoal writes both)
    int alt_state = 0;                  // +0x5a4
    std::uint32_t script_id = 0x6000000;  // +0x554
    Behaviour behaviour[2];             // +0x4dc / +0x4e8
    int active_behaviour = 0;           // +0x4d8 points at [0] or [1]
    const Behaviour& beh() const { return behaviour[active_behaviour]; }
    bool has_beh(int id) const { return behaviour[active_behaviour].has(id); }
    bool is_bot() const { return dtype == kDtypeBot || obj_type == 0x11; }

    // ---- ranges (sight profile, spec §5.1) --------------------------------------------------------------
    float sight_cone = 1.5707964f;      // +0xe4 vision half-cone (rad), 0 = all round
    float sight_range = 24.0f;          // +0xe8
    float range_ec = 4.0f;              // +0xec
    float engage_dist = 12.0f;          // +0xf0 engagement/aim distance
    float range_f4 = 4.0f;              // +0xf4
    float min_cover_dist = 2.0f;        // +0xf8
    float max_combat_dist = 14.0f;      // +0xfc
    int   d0 = 15;                      // +0xd0

    // ---- flags -----------------------------------------------------------------------------------------
    std::uint32_t flags = flag::kActive;   // +0x4f8
    std::uint32_t alert_flags = 0;         // +0x4fc alert-source bits (1 first sight, 2 hurt, 4, 8, 0x10 attack ...)
    std::uint32_t sight_flags = 0;         // +0x228 (sight::k*)

    // ---- alertness ---------------------------------------------------------------------------------------
    float alertness = 0;                // +0x50c (0..1)
    float alertness_floor = 0;          // +0x500
    float alertness_floor2 = 0;         // +0x504
    float noise_loudness = 0;           // +0x510
    float noise_delta = 0;              // +0x514
    std::uint32_t noise_time = 0;       // +0xb38 last noise accepted (rate limit 1 per second unless louder)
    float noise_max = 0;                // +0xb3c loudness of that noise
    Vec3 alert_pos{};                   // +0xaf0 position of the last accepted sound / shout / drone alert
    AlertStatus alert_status = AlertStatus::Relaxed;
    std::uint32_t first_attack_time = 0;   // +0x524

    // ---- body (obj+0x30/+0x54/+0xe0, collision) -------------------------------------------------------------
    Vec3 pos{};                         // obj+0x30 (above the feet by stand_height)
    float yaw = 0;                      // obj+0x54, forward = (sin yaw, 0, cos yaw)
    Vec3 velocity{};                    // world-space walk velocity (units/s) set by locomotion
    Vec3 fall_velocity{};               // gravity part (units/s)
    Vec3 fly_velocity{};                // +0x480 flight / knock velocity (units/s) while mv.disabled (+0x23)
    bool gun_through_wall = false;      // +0x1e DroneVision_GunThroughWall result (not computed by the core yet)
    float stand_height = 1.0327658653f; // collbody+0xCC
    float radius = 0.55f;               // collision capsule radius
    bool on_ground = false;
    float ground_normal_y = 1.0f;
    float turn_rate = 0;                // current yaw rate (rad/tick)
    float speed_scale = 1.0f;

    Vec3 feet() const { return {pos[0], pos[1] - stand_height, pos[2]}; }
    // NDrone2_FeetPos 0x149b88: nav-node height = obj pos.y - (stand_height - 0.4)
    Vec3 nav_pos() const { return {pos[0], pos[1] - (stand_height - 0.4f), pos[2]}; }
    Vec3 forward() const;               // (sin yaw, 0, cos yaw)
    Vec3 eye() const;                   // head bone / eye position for LOS rays

    // ---- perception ---------------------------------------------------------------------------------------
    TargetRef opponent;                 // Drone+0x170 obj_tag* target; Drone+0x174 target-slot ID
    Vec3 opp_pos{};                     // +0x1f0 aim position (target pos + per-type offset)
    Vec3 opp_last_known{};              // +0x250
    float opp_last_known_yaw = 0;       // +0x260
    Vec3 aim_offset{};                  // +0x200 (miss wobble)
    Vec3 opp_vec{};                     // +0x1b0 vector to target
    float opp_dist = 1e9f;              // +0x1a0
    float opp_bearing = 0;              // +0x1c4 (ry lane of the aim euler below)
    Vec3 aim_euler{};                   // +0x1c0 euler angles (rx, ry, rz) feeding RotMatrix in
                                        //   DroneWeap_DoBulletAccuracy; GetOpponentInfo writes (0, bearing, 0)
    float opp_facing_a = 0;             // +0x1d0
    float opp_facing_b = 0;             // +0x1d4
    float opp_motion_mag = 0;         // +0x1e4: 40.0 while the opponent is moving, else 0 (BOT_opponentTargetting)
    float visibility = 0;               // +0x230
    float env_visibility = 1.0f;        // +0x22c (AI volume multiplier)
    std::uint32_t last_seen_time = 0;   // +0x238
    std::uint32_t seen_frames = 0;      // +0x270 continuous visible ticks
    std::uint32_t lost_frames = 0;      // +0x274 ticks since last LOS
    std::uint32_t reaction_wait = 0;    // ticks LOS has held while reaction time counts down
    bool opp_visible = false;           // LOS result this tick
    int los_bone = 0;                   // +0x1da rotating bone index into Drone_View_Bones
    bool opp_moving = false;            // +0x1dc
    bool opp_first_moved = false;       // +0x1dd
    bool opp_first_stopped = false;     // +0x1df
    std::uint32_t opp_motion_time = 0;  // +0x224
    bool opp_moved_started = false;     // +0x1de
    bool opp_stopped_started = false;   // +0x1e0
    int shooter_id = -1;                // WeaponSystem shooter id (register_target), -1 = not registered
    float ai_volume_mult = 1.0f;
    float pos_visibility = 1.0f;        // +0x52c: AI volume multiplier at this drone's own position
    std::uint8_t reaction_stat = 0;     // +0xb8 (DroneFunc_ReactionTime, Tower/Castle levels)
    bool first_seen_logged = false;     // +0x42 (DroneVision_LogSeenOpponent)
    std::uint32_t first_sight_time = 0; // +0x234
    std::uint32_t last_los_time = 0;    // +0xd4
    bool lost_since_shot = true;        // +0x41: sight lost since the last shot (first shot after regaining sight misses)
    std::uint32_t ally_cursor = 0;      // +0x2c0 round-robin cursor of the ally branch of FindOpponent
    int alerting_drone = -1;            // +0x528 drone whose alert brought this one to Attack

    // ---- firing (DroneWeap_*) ------------------------------------------------------------------------------
    int weapon = 0;                     // current weapon id; Drone+0xc58 mirrors BOT_vars+0x768 for MP bots
    int burst_left = 0;                 // +0xbc0
    std::uint32_t next_bullet_time = 0; // +0xbc4
    std::uint32_t last_shot_time = 0;   // +0xbc8
    bool shot_at = false;               // +0x2a a bullet came close / hit recently (30 frames of 60 Hz)
    std::uint32_t shot_at_time = 0;     // +0x2c4
    std::uint32_t shot_at_ticks = 0;    // +0x2c8 consecutive PreDroneControl ticks under fire
    bool fire_requested = false;        // +0x3b a firing state wants DoFiring every tick (weap::do_firing)
    bool burst_done = false;            // +0x3c the requested burst has finished
    bool one_shot = false;              // +0x3d one burst per request (states 0x44/0x46/0x47/0x55)
    bool firing_now = false;            // +0x3f Ready2Fire passed this tick
    bool fired_flag = false;            // +0x40 a shot was fired
    bool fire_lock = false;             // +0x3e animation-locked
    bool weapon_ready = true;           // +0x20 anim events 15/16/18
    bool fire_window = false;           // +0x21 anim events 16/18 (PlayFiringAnim gate)
    std::uint32_t shots_fired = 0, shots_hit_roll = 0;   // accuracy statistics (hit/miss decisions)

    // ---- timers ---------------------------------------------------------------------------------------------
    std::uint32_t idle_timeout = 0;     // +0x104 (0 = none): world tick at which msg 4 fires
    std::intptr_t idle_timeout_arg = 0; // +0x10c payload of that msg (NDrone2_SetIdleTimeOut)
    struct Timer { std::uint32_t at = 0; std::intptr_t payload = 0; };
    Timer timer1, timer2;               // +0xcc0/+0xccc -> msg 0xc / 0xd
    std::uint32_t rand_phase = 0;       // obj+0xec (Rand_Rand(10000)) accuracy wobble phase

    // ---- animation (DroneAnim_*) -----------------------------------------------------------------------------
    float anim_rate = 1.0f;             // +0x14c
    struct Anim {                       // DroneAnim call state (Drone+0x558.. / +0x568..)
        int req_state = 0;              // +0x558 requested DASC_* state (0 = none)
        int req_variant = 0;            // +0x560
        int req_blend = 8;              // blend ticks (CallAnim `param`)
        int req_end_state = 0;          // +0x596 AI state to enter when the anim ends
        int req_end_msg = 0;            // +0x598 self message to send when the anim ends
        float req_speed = 1.0f;         // +0x588
        int cur_state = 0;              // +0x56a current DASC_* state
        int prev_state = 0;             // +0x55c
        int cur_anim = 0;               // +0x56c current Drone_AnimInfo id
        int cur_type = 0;               // +0x568 Drone_AnimInfo +0xc
        std::uint32_t cur_flags = 0;    // +0x570 Drone_AnimInfo +0
        std::uint32_t script = 0;       // 0x6000000 + Drone_AnimTables[anim][sub_class]
        int next_anim = 0;              // +0x594 Drone_AnimInfo +0xa chained anim after the clip ends
        int end_state = 0, end_msg = 0; // applied end handler (+0x596/+0x598)
        bool applied = false;           // +0x57b the request has been turned into a clip
        bool clip_running = false;      // +0x579 non-looping clip not yet finished
        bool loop = true;
        bool source_gate_valid = false; // one-tick v5 AnimObjectUpdate eligibility from source Drone_Control
        bool source_update_due = true;  // NDrone2_DoAnimation or frame-stamp equality
        float source_root_yaw = 0;
        float source_root_height_offset = 0; // v5 recorded sAnimObject+0x60 offset over the sampled root pose
        bool source_collision_due = true; // v5 NDrone2_DoCollision result for this restored bot snapshot
        bool source_collision_valid = false; // v5 eligibility snapshot, consumed by collision_step once
        bool source_gravity_due = true; // v5 NDrone2_DoGravity result for this restored bot snapshot
        float step = 0;                 // +0x58c scalar fallback when AnimObjectUpdate is skipped
    } anim;
    // NDrone2 locomotion state (steering target, route, per-tick displacement).
    struct Move {
        bool disabled = false;          // +0x23 movement control off: no gravity, 3-D root motion, fly_velocity moves the body
        bool fly = false;               // +0x24 steer toward `dest` with fly_velocity while disabled
        float fly_speed = 0;            // +0x50 acceleration factor of that steering
        bool have_dest = false;
        Vec3 dest{};                    // +0x670 move target position (feet space)
        int dest_cel = kNoCel;          // +0x680
        float dest_dist = 0;            // +0x660 2-D distance feet -> dest
        float arrive_radius = 0.3f;     // +0x664
        float dest_angle = 0;           // +0x694 heading to steer to
        float turn_rate = 0.1f;         // +0x4a0 fraction of the heading error turned per tick
        float speed = 0;                // +0x474 actual 2-D displacement of the last tick
        Vec3 root_motion{};             // this tick's anim root motion in world space
        RouteStatus route_status = RouteStatus::Reset;
        float route_distance = 0;       // +0x8a0 remaining route length
        bool blocked = false;           // +0x37 obstructed by the player
        std::uint32_t obstructed_time = 0;
        int move_quad = 0;              // +0x3c6 DroneAnim_GetMoveQuad hysteresis
        std::uint32_t boundary_flags = 0;   // +0x31c DroneMove_SetBoundryFlags: 2 ahead / 4 behind / 8 left / 0x10 right blocked (0.5 m)
        bool bunched = false;           // +0x17 DroneMove_NoBunching: a moving drone is in the way
        bool goal_is_object = false;    // route goal is a moving target (move_to_object)
        TargetRef goal_target;
        float applied_height = 1.0327658653f;   // stand_height already folded into pos.y (feet stay planted)
        Vec3 reach_check_pos{};       // DroneReachCheckPos: last Can* probe destination (feet space), for the KOTH hill veto
        bool seeded_capsule_valid = false;
        Vec3 seeded_capsule_a_offset{}, seeded_capsule_b_offset{}; // oracle-restored body-local collision endpoints
        float seeded_capsule_radius = 0.4f;
    } mv;
    DroneLook look;
    std::unique_ptr<CharacterInstance> character;   // owned by the drone; created by DroneSystem::spawn
    std::unique_ptr<NavAgent> nav;                  // routes / goals (null when the level has no nav network)
    bool fresh = true;                              // Global ENTER not delivered yet (runs on the first tick)

    // ---- content layer data -------------------------------------------------------------------------------------
    std::unique_ptr<DroneExt> ext;
    template <class T> T* ext_as() { return static_cast<T*>(ext.get()); }

    // Content-layer hooks (null = generic core behaviour). Added for the MP bot layer:
    //  find_opponent        replaces NDrone2_FindOpponent inside ControlSTANDARD (original order kept: msg 3 first,
    //                       then FindOpponent); the hook sets opponent/opp_dist/opp_bearing/seen_frames/lost_frames.
    //  opponent_targetting  replaces DroneWeap_DoOpponentTargetting (called from DoFiring): aim wobble, opp_pos.
    //  has_ammo/on_round_fired  wrap DroneWeap_FireWeapon (bot clip lives in the content layer).
    //  bot_pain             replaces the SP NDrone2_HitDamage path in Drone::hurt (returns damage applied); the
    //                       bullet/explosive/punch message is still sent to the current state afterwards.
    //  damage_mul           extra multiplier on outgoing damage of this drone (BOT DoHitEffects).
    struct Hooks {
        std::function<void(Drone&)> find_opponent;
        std::function<void(Drone&)> opponent_targetting;
        std::function<bool(Drone&)> has_ammo;
        std::function<void(Drone&)> on_round_fired;
        std::function<float(Drone&, const DroneHit&)> bot_pain;
        std::function<float(const Drone&)> damage_mul;
    } hooks;

    // Optional hook (BOT_validateStateChange 0x124fb0): called before every state change with the requested
    // state; return the state to enter (may be the replacement), or -1 to reject the change.
    std::function<int(Drone&, int requested)> validate_state;

    // ---- API -------------------------------------------------------------------------------------------------
    bool alive() const { return health > 0 && (flags & flag::kDeadMask) == 0; }
    int state() const { return smi.cur; }
    std::uint32_t now() const;                       // world tick counter (GameState+0x34)
    std::uint32_t seconds(float s) const;            // s * FRAME_RATE_INT ticks at the current rate
    float rate() const;

    // Drone_SM_SetState: request a transition. Inside a handler the transition is applied after the handler
    // returns (pending flag); outside, immediately. Runs validate_state, then LEAVE(2)/ENTER(1).
    void set_state(int state, std::intptr_t arg = 0);
    // Drone_SM_SendMsgSelf / BroadcastMsg / Drone_Message. delay in ticks (0 = synchronous).
    void send_self(int msg_id, std::intptr_t arg = 0, int delay = 0, const void* ptr = nullptr, int state_filter = 0);
    void send_to(int drone_id, int msg_id, std::intptr_t arg = 0, int delay = 0, const void* ptr = nullptr);
    void broadcast(int msg_id, std::intptr_t arg = 0, int delay = 0, const void* ptr = nullptr);

    // Damage entry (NDrone2_HitDamage + Drone_BulletHit): applies region/armour/difficulty multipliers, then
    // sends kMsgBullet to the current state. Returns damage applied.
    float hurt(const DroneHit& hit);

    // Anim request (DroneAnim_CallAnim 0x13ab58): blend_ticks, DASC_* state, variant, state to enter when the
    // clip ends (-1 none). Returns true if a clip was started.
    bool call_anim(int blend_ticks, int anim_state, int variant = 0, int end_state = -1, std::intptr_t end_arg = 0);

    bool have_dest_or_target() const { return mv.have_dest; }

    // Handlers may re-set alert status (Drone_AlertStatusSet).
    void set_alert_status(AlertStatus s) { alert_status = s; }

    // Perception helpers (drone_vision.cpp)
    const Vec3* opponent_position() const;
};

}  // namespace nf::drone

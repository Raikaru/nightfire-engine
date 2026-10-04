#pragma once

// DroneSystem: owns every drone (SP enemy or MP bot), ticks them in the original's order and provides the
// shared services (world access, character bank, tuning, target queries, noise, delayed messages).
//
// Tick order (docs/spec-arena-ai.md Part 3 §12.2; `Drone_Control`/`NDrone2_ControlSTANDARD`):
//   Drone_SM_SendDelayedMsgs -> Drone_ProcessOpponents (visibility multipliers, opponent info)
//   -> DroneVision_FindAlertedDrones (<= 5 checks) -> per drone: PreDroneControl (timers -> msgs 4/0xc/0xd)
//   -> ControlSTANDARD (alertness decay, TICK msg 3, FindOpponent, movement/collision, firing).
//
// Usage:
//     auto sys = std::make_unique<drone::DroneSystem>(world, bank, cfg);
//     drone::DroneSystem& drones = *sys;  world.add_system(std::move(sys));
//     drone::SpawnInfo s; s.feet = ...; s.skin_hash = 0x5000004; s.dmode = 5;   // or set dtype/initial_state
//     drone::Drone& d = drones.spawn(std::move(s));

#include <functional>
#include <memory>
#include <optional>
#include <source_location>
#include <string_view>
#include <vector>

#include "assets/character.hpp"
#include "core/rng.hpp"
#include "game/damage.hpp"
#include "game/drone.hpp"
#include "game/world.hpp"

namespace nf {
class WeaponSystem;
}
namespace nf::audio { class AudioSystem; }

namespace nf::drone {

class DroneAnimData;

// TuningVars.txt [SECTION] values the drones read (ReadTuningVars 0x1d3a80; spec §6.2). Loaded with our own
// parser from FILES.BIN's TuningVars.txt: `DroneTuning::load(text, level_id)` picks the section by level id
// (ESTATE 1-4, CASTLE 5-8, TOWER1 9-b, POWERSTATION c-d, TOWER2 11-13/4a, EVILBASE 14-16, SPACESTATION 1b,
// MULTIPLAYER 0x21-0x29/4b/4c) and falls back to the .sdata defaults.
struct DroneTuning {
    float plr_dmod[3] = {0.5f, 0.7f, 1.2f};        // Plr_DMod_{Easy,Normal,Hard}
    float damage_diff[3] = {2.0f, 1.5f, 0.5f};     // DroneDamage_{Easy,Normal,Hard}
    float damage_head = 100.0f, damage_legs = 0.75f, damage_arms = 1.0f, damage_torso = 1.0f;
    float armour_helmet = 1.0f, armour_combat = 0.25f, armour_jacket = 0.5f, armour_vest = 0.75f;
    float burst_delay_min = 15, burst_delay_normal = 45, burst_delay_max = 60;
    float burst_min_dist = 4, burst_max_dist = 30;
    float accuracy[3] = {0.5f, 1.0f, 2.0f};        // DroneFiring_Accuracy_{Easy,Normal,Hard}
    float new_sighting_time = 2.0f;                 // DroneFiring_NewSighting_TimeToHit (s)
    float first_moved_time = 2.0f, first_moved_accuracy = 0.25f;
    float moving_accuracy = 0.75f;
    float first_stopped_time = 2.0f, first_stopped_accuracy = 1.0f;
    float too_close_dist = 4.0f, too_close_accuracy = 2.0f, too_close_damage = 2.0f;
    float back_shot_damage = 2.0f;
    float captain_bullet_damage = 2.0f, captain_bullet_accuracy = 2.0f, captain_health = 2.0f;
    float plr_dmod_multi = 4, plr_dmod_head = 4, plr_dmod_lower = 0.8f, plr_dmod_upper = 0.8f;   // MULTIPLAYER only

    static DroneTuning load(std::string_view tuning_vars_txt, std::uint32_t level_id);
};

struct DroneConfig {
    const Elf32* elf = nullptr;           // ACTION.ELF: DroneAnimStates / Drone_AnimInfo / Drone_AnimTables / Drone_View_Bones
    std::uint32_t level_id = 0x7000024;   // GameState+0xc (multiplayer maps 0x7000021..29)
    int difficulty = 2;                   // GameState+0x28 (1 easy, 2 normal, 3 hard; MP forces 1)
    float fade_seconds = 1.0f;            // Fade state length (NDrone2_FadeOut)
    bool blind_drones = false;            // switch_BLIND_DRONES: perception returns "nothing"
    bool multiplayer = false;             // MPSettings+0x184: bots' perception/firing branches
    DroneTuning tuning;
};

// Per-tick callbacks for the content layers / arena.
struct DroneCallbacks {
    std::function<void(Drone&, const DroneHit&)> on_hurt;      // after damage was applied
    std::function<void(Drone&, int killer)> on_death;          // health reached 0 (killer: DroneHit::attacker)
    // Bullets: build the shot (muzzle origin + direction incl. miss wobble) and hand it to the weapons
    // system. Return false if nothing was fired. Installed by main / the Weapons integration.
    std::function<bool(Drone&, const Vec3& origin, const Vec3& dir, int weapon)> fire;
    // Anim script events (DroneAnim_EventFunc): footsteps / sounds, and the callback codes the core does not
    // handle itself (1 throw grenade, 2 change weapon, 5/10/11 close-combat impact, 9 place tertiary, 14 hostage).
    std::function<void(Drone&)> on_footstep;
    std::function<void(Drone&, int sound_id)> on_anim_sound;
    std::function<void(Drone&, int code)> on_anim_event;
    // First time a drone notices the player (DroneVision_LogSeenOpponent / PlrStat_LogEnemyDetectedPlayer).
    std::function<void(Drone&)> on_first_seen;
    // Drone_VisibilityForPosition override (AI volumes owned by the SP placement layer). Default: DroneSystem::ai_volumes.
    std::function<float(const Vec3&)> visibility_at;
    // switch_channels[ch] != 0 (start channel / alt-mode channel checks of the Global state and PreDroneControl).
    std::function<bool(int channel)> switch_channel;
    // Player state the drones cannot read from Player: health > 0 and PlrStat_OkToUpdate (default: alive).
    std::function<bool(int slot)> player_alive;
    // Content hooks of the shared death / impact states.
    std::function<void(Drone&)> seen_and_attacking;   // NDrone2_SeenAndAttacking (combat music counter)
    std::function<void(Drone&)> on_drop_weapon;       // DroneWeap_DropWeapon
    std::function<void(Drone&)> on_death_channel;     // DroneFunc_SetDeathChannel
    std::function<void(Drone&)> dead_tick;            // DroneFunc_DeadDrone
    // NDrone2_ControlDTYPE_* (Zoe / Ninja / Astronaut): runs before ControlSTANDARD for non-bot drones.
    std::function<void(Drone&)> control_dtype;
    // DroneFunc_HandleExplosives 0x1435f0: end of ControlSTANDARD (skipped in multiplayer).
    std::function<void(Drone&)> handle_explosives;
    // BOT_reactToDroneAlertMsg: bots receive shouts / noises through this (returns the state to enter or 0).
    // `msg` is 0x11..0x16, 0x1e or 0x14 (sound), `alert` may be null.
    std::function<int(Drone&, int msg, const AlertRecord* alert)> bot_react_to_alert;
};

class DroneSystem : public System {
public:
    DroneSystem(World& world, CharacterBank& bank, DroneConfig config);
    ~DroneSystem() override;

    // ---- creation / queries -----------------------------------------------------------------------------
    Drone& spawn(SpawnInfo info);                       // NDrone2_CreateObj + DefaultInit + mode/type settings
    void remove(int id);                                // Drone_Delete
    Drone* find(int id);
    const std::vector<std::unique_ptr<Drone>>& drones() const { return drones_; }
    // AI volumes (Drone_AIVolume_Create, kind 0): oriented boxes whose `mult` scales how visible a target inside
    // them is (Drone_ProcessOpponents / Drone_VisibilityForPosition). Fed by the SP placement code.
    struct AiVolume {
        Vec3 center{};
        Vec3 half{1, 1, 1};     // half extents in box space
        float yaw = 0;          // rotation of the box about Y
        float mult = 1.0f;      // param * 0.01
    };
    void add_ai_volume(const AiVolume& v) { ai_volumes_.push_back(v); }
    float visibility_for_position(const Vec3& pos) const;   // product of mult over the boxes containing `pos`

    // Alive opponents of `d` (side-aware) nearest to `d`, no LOS test; nullptr if none.
    Drone* nearest_opponent_drone(const Drone& d, float max_dist = 1e9f);
    // Position/aliveness of a target (players read World::player).
    std::optional<Vec3> target_pos(const TargetRef& t) const;
    bool target_alive(const TargetRef& t) const;
    // Visible threats for the weapon idle/fidget gate (ELF 0x298E14 walk): any drone with health(d+172) > 0,
    // not waiting for its switch channel (state 1 = kStWaitSwitch), and alertness(d+1292) > 0.5. Note the
    // third term is `alertness` (+0x50c = 1292), not `visibility` (+0x230): the walk reads the 0..1
    // alert level, i.e. threats aware of the player. Read-only per frame, no tick-order dependency.
    bool any_visible_threat() const {
        for (const auto& o : drones_) {
            if (o->health > 0 && o->smi.cur != 1 && o->alertness > 0.5f) return true;
        }
        return false;
    }
    // ---- services ---------------------------------------------------------------------------------------
    World& world() { return world_; }
    const CollisionWorld& collision() const { return world_.collision(); }
    CharacterBank& bank() { return bank_; }
    const DroneAnimData* anim_data() const { return anim_data_.get(); }
    const DroneConfig& config() const { return config_; }
    DroneConfig& config() { return config_; }
    DroneCallbacks& callbacks() { return callbacks_; }
    // Navigation network of the level (owned by the caller); drones get a NavAgent when it is set before spawn().
    void set_nav(NavNetwork* nav) { nav_ = nav; }
    NavNetwork* nav() const { return nav_; }
    // Weapons: drones are registered as damage targets (Drone::shooter_id) and fire through WeaponSystem::fire unless
    // DroneCallbacks::fire is set. Call before spawn(); drones spawned earlier are registered too.
    void set_weapons(WeaponSystem* ws);
    WeaponSystem* weapons() const { return weapons_; }
    void set_audio(audio::AudioSystem* a) { audio_ = a; }
    audio::AudioSystem* audio() const { return audio_; }
    std::uint32_t now() const { return std::uint32_t(world_.timer_frame()); }
    // Shared per-tick state of NPCGlobals: LOS rays spent this tick (+0x282), the alert record (+0x290), the
    // round-robin cursors (+0x284/+0x288/+0x28c).
    std::uint32_t los_rays = 0;
    int count_enemies = 0, count_friends = 0, count_neutral = 0;   // NPCGlobals+0x17c/0x180/0x184 per tick
    std::uint32_t last_impact_anim_time = 0;   // NPCGlobals+0x1b0: flinch anims are rate limited to one per 2 s globally
    AlertRecord& alert_record() { return alert_record_; }
    std::uint32_t alerted_cursor_a() const { return alerted_cursor_a_; }
    std::uint32_t alerted_cursor_b() const { return alerted_cursor_b_; }
    void set_alerted_cursors(std::uint32_t a, std::uint32_t b) { alerted_cursor_a_ = a, alerted_cursor_b_ = b; }
    std::uint32_t sight_cursor() const { return sight_cursor_; }
    void set_sight_cursor(std::uint32_t c) { sight_cursor_ = c; }
    FrameTiming timing() const { return timing_; }
    std::uint32_t rand_int(std::uint32_t n,
                           const std::source_location& loc = std::source_location::current());  // Rand_Rand(n) in [0, n)
    float frand(float range,
                const std::source_location& loc = std::source_location::current());             // Rand_FRand

    // World-level noise (Sound_Alertness): weapons/footsteps call this; drones within 50 m that hear (behaviour
    // 0x1f) get a kMsgSoundAlert with loudness-scaled alertness. `loudness` is the original 0..100 value.
    void emit_noise(const Vec3& pos, float loudness, int source = -1);

    // Messages (Drone_SM_*): delayed queue is drained at the start of each tick.
    void post(Msg msg);                                  // routes / queues by deliver_at
    // Drone_MessageObjVicinity: message every drone within `radius` of `pos`.
    void message_vicinity(const Vec3& pos, float radius, int msg_id, std::intptr_t arg = 0);

    // Weapons integration: a DamageTarget view of a drone (register with WeaponSystem::register_target). hurt()
    // maps HitInfo -> DroneHit and calls Drone::hurt (NDrone2_HitDamage). Stable for the drone's lifetime.
    DamageTarget* damage_target(int id);

    // Damage entry for the Weapons integration (DamageTarget::hurt adapter): returns damage applied.
    float hurt_drone(int id, const DroneHit& hit);

    void tick(World& world, FrameTiming timing) override;
    void before_object_update(World& world, FrameTiming timing) override;
    void after_tick(World& world, FrameTiming timing) override;

    // Internal: transition loop (called by Drone::set_state).
    void run_transition(Drone& d);
    void deliver(Drone& d, const Msg& m);                // Drone_SM_RouteMsgDCV
    int delivery_depth() const { return delivery_depth_; }

private:
    World& world_;
    CharacterBank& bank_;
    DroneConfig config_;
    DroneCallbacks callbacks_;
    audio::AudioSystem* audio_ = nullptr;
    NavNetwork* nav_ = nullptr;
    WeaponSystem* weapons_ = nullptr;
    std::unique_ptr<DroneAnimData> anim_data_;
    FrameTiming timing_{};
    struct Target;
    std::vector<std::unique_ptr<Drone>> drones_;
    std::vector<std::unique_ptr<Target>> targets_;   // parallel to drones_
    std::vector<Msg> delayed_;
    std::vector<AiVolume> ai_volumes_;
    std::uint32_t alerted_cursor_a_ = 0, alerted_cursor_b_ = 0;   // NPCGlobals+0x288/+0x28c
    std::uint32_t sight_cursor_ = 0;
    AlertRecord alert_record_;
    int next_id_ = 1;
    int delivery_depth_ = 0;
    bool object_prelude_done_ = false;
};

}  // namespace nf::drone

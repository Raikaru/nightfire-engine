#pragma once

// Shared foundation of the single-player enemy layer (docs/ai-sp.md): per-drone extension (`SpExt`), the SP system
// that owns the level data / switch channels / spawners (`SpSystem`), and the helpers every state handler uses
// (dispatch skeleton pieces, timers, anim shortcuts). Conventions:
//   * one state = `int state_xxx(Drone&, const Msg&)` registered with drone::register_state (see sp_states.hpp);
//     return non-zero when the message was consumed, 0 = unhandled (falls through to the core's Global state);
//   * ticks: the original counts `N * FRAME_RATE_INT` seconds; use `d.seconds(N)`; literal frame counts stay
//     literal (`d.now() + 30`);
//   * every original function/address that a piece of code mirrors is named in a comment.

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "game/drone.hpp"
#include "game/drone_anim.hpp"
#include "game/drone_impact.hpp"
#include "game/drone_move.hpp"
#include "game/drone_system.hpp"
#include "game/drone_vision.hpp"
#include "game/drone_weap.hpp"
#include "game/sp_placement.hpp"
#include "game/sp_state_ids.hpp"
#include "game/sp_tables.hpp"

namespace nf::sp {

using drone::Drone;
using drone::DroneSystem;
using drone::Msg;
using drone::TargetRef;
using namespace nf::sp::st;

class SpSystem;

// Voice line kinds (NDrone2_PatrolTalk / AttackTalk / PainTalk / DeathTalk ...).
enum class Speech : int { Patrol, Attack, Pain, Death, Surrender, CivilianScared, Other };

// ---- switch channels (`switch_channels` @0x26fc90, 256 bytes; scripts/levels set them) --------------------------
// No scripting engine exists yet: every channel starts OFF; `WaitSwitch` waits for its channel, spawners and cover
// nodes read theirs. Channel 0 means "no channel" everywhere.
class SwitchChannels {
public:
    bool on(int ch) const { return ch > 0 && ch < 256 && v_[std::size_t(ch)] != 0; }
    void set(int ch, bool value) {
        if (ch > 0 && ch < 256) v_[std::size_t(ch)] = value ? 1 : 0;
    }
    void toggle(int ch) { set(ch, !on(ch)); }

private:
    std::array<std::uint8_t, 256> v_{};
};

// ---- per-drone extension ----------------------------------------------------------------------------------------
// Content areas (cover, civilians, ninja, ...) hang their own state on a drone with `slot<T>()`: `T` derives from
// `SlotBase`; the slot is default-constructed on first use and lives as long as the drone.
struct SlotBase {
    virtual ~SlotBase() = default;
};

struct SpExt : drone::DroneExt {
    SpSystem* sp = nullptr;
    SpNpcSpec spec;                  // the DIVars this drone was created from
    NpcResolved cfg;                 // what DefaultInit / DoModeSettings decided (Drone+... in comments)
    std::uint32_t static_index = 0;  // map static it came from
    int spawner = -1;                // index into SpSystem spawners if a spawner made it, else -1
    int template_index = -1;         // spawner template it was cloned from
    bool deleted_by_spawner = false;
    std::uint16_t return_state = 0;        // Drone+0x5a0: state to return to (EnemyAlerts/AlertToPosition/Obstructed/RunToAlarm...)
    std::uint16_t return_state2 = 0;       // Drone+0x5a6: second return state (ActionAnim/StandFiddle/ReachedDestNode)
    std::uint16_t ammo = 0, ammo_max = 0;   // Drone+0xbbc / +0xbbe: clip (reload states refill); 10000 = effectively unlimited
    bool heard_noise = false;              // Drone+0x33: heard something (EnemyAlerts keeps the return state while set)
    bool alert_turned = false;             // Drone+0x34: already turned towards the alert position
    std::uint8_t noise_level = 0;          // Drone+0x43: HeardNoise escalation 2 aware / 3 suspect / 4 alert
    std::uint32_t last_combat_move = 0;    // Drone+0x11c: last NDrone2_ChooseCombatMove time (AimCrouchFire cooldown)
    Vec3 home_pos{};                       // Drone+0x5b0: feet position when the alert arrived (SearchArea walks back to it)
    std::uint8_t death_channel = 0;        // Drone+0x13c (DIVars key12): switch channel set on death (DroneFunc_SetDeathChannel)
    // Civilian / hostage / alarm layer (sp_civilian_util.hpp, sp_states_civilian.cpp); other areas read them.
    bool running_to_alarm = false;         // Drone+0x16: RunToAlarm sets, PressAlarm clears (GoToGoalPosition / alerts read it)
    bool scared_hiding = false;            // Drone+0x2b: Run-away / HideFromScaryObject; ConsiderExplosive ignores while set
    int partner = -1;                      // Drone+0x2b0: paired drone id (hostage killer <-> hostage), -1 none
    int door_partner = -1;                 // Drone+0x2bc: door-guard partner drone id, -1 none
    int near_drone = -1;                   // Drone+0x2b8: armed enemy drone found by NDrone2_NearArmedDrone
    int cover_node = -1;                   // claimed cover node index into SpLevel::cover_nodes (-1 none)
    struct AiGoal {                        // AIPoint_tag (Drone+0x6f0): the fixed position goal a state walks to
        bool set = false;                  //   +0x04 valid
        Vec3 pos{};                        //   +0x20 goal position (feet space)
        float radius = 2.0f;               //   +0x14 arrival radius (0 -> 2.0)
    } ai_goal;
    struct Scary {                         // scary object (Drone+0x324/+0x330/+0x340/+0x350/+0x354): ConsiderExplosive fills it
        std::uint32_t until = 0;           //   +0x324 keep running away until this tick
        Vec3 pos{};                        //   +0x330
        int cel = -1;                      //   +0x340
        float radius = 0;                  //   +0x350 safety distance
        int return_state = 0;              //   +0x354 state to return to afterwards (0 = none)
    } scary;

    template <class T>
    T& slot() {
        const void* key = key_of<T>();
        for (auto& [k, p] : slots_)
            if (k == key) return static_cast<T&>(*p);
        slots_.emplace_back(key, std::make_unique<T>());
        return static_cast<T&>(*slots_.back().second);
    }
    template <class T>
    T* find_slot() {
        const void* key = key_of<T>();
        for (auto& [k, p] : slots_)
            if (k == key) return static_cast<T*>(p.get());
        return nullptr;
    }

private:
    template <class T>
    static const void* key_of() {
        static const char tag = 0;
        return &tag;
    }
    std::vector<std::pair<const void*, std::unique_ptr<SlotBase>>> slots_;
};

// The SP extension of a drone (never null for drones created by SpSystem; bots have none).
inline SpExt& sx(Drone& d) { return *static_cast<SpExt*>(d.ext.get()); }
inline SpExt* sx_or_null(Drone& d) { return dynamic_cast<SpExt*>(d.ext.get()); }
inline SpSystem& sp_of(Drone& d) { return *sx(d).sp; }

// ---- the SP system ---------------------------------------------------------------------------------------------
// Owns the level's placement data, switch channels and the spawners; registered with the World before the
// DroneSystem (its tick runs Drone_ProcessCoverNodes / DroneSpawner_Control before the drones run).
// Drone_ProcessCoverNodes (0x138e78) and the cover-node registry live behind this interface (sp_cover.hpp).
class CoverSystem {
public:
    virtual ~CoverSystem() = default;
    virtual void process(SpSystem& sp) = 0;   // once per tick, before the drones run
};

struct SpConfig {
    std::uint32_t level_id = 0;
    int difficulty = 2;
};

class SpSystem : public nf::System {
public:
    SpSystem(DroneSystem& drones, SpLevel level, SpTables tables, SpConfig config);
    ~SpSystem() override;

    // ---- data ----
    DroneSystem& drones() { return drones_; }
    const SpLevel& level() const { return level_; }
    const SpTables& tables() const { return tables_; }
    const SpConfig& config() const { return config_; }
    SwitchChannels channels;
    CoverSystem* cover() { return cover_.get(); }
    void set_cover(std::unique_ptr<CoverSystem> c);

    // ---- creation ----
    // Drone_Create + NDrone2_CreateObj + NDrone2_DefaultInit for every placed NPC (difficulty gate applied, spawner
    // group templates skipped when a spawner exists for the group). Returns the number created.
    std::size_t spawn_placed();
    // One NPC (used by spawn_placed and by the spawners). `feet` overrides the position when non-null.
    Drone* spawn_npc(const SpNpcSpec& spec, std::uint32_t static_index, const Vec3* feet = nullptr, int spawner = -1,
                     int template_index = -1);
    // Cutscene event-8 coder spawn (Drone_CoderCreate): `feet` is the event object position, `args` the 4
    // event dwords (engage_dist bits, range_f4 bits, unused, d0). The drone is otherwise default-configured
    // (zeroed DIVars: default type/mode/skin) with initial state Idle, like the original. Consumed from
    // MissionSystem::take_spawns() after World::tick by the session (see DroneCli --sp wiring for the call).
    Drone* spawn_scripted(const Vec3& feet, const std::uint32_t args[4]);
    // NDrone2_DefaultInit results onto a spawned drone (config, behaviour, ranges, flags, skin swap for captains).
    void apply_config(Drone& d, const NpcResolved& r);

    // ---- queries ----
    std::vector<Drone*> sp_drones();      // every live drone that has an SpExt
    std::size_t spawned() const { return spawned_; }
    // NDrone2_SeenAndAttacking (0x145150) bookkeeping: every attacking drone is counted once per frame
    // (NPCGlobals+0x18c, dedup pNPCPassed); Drone_InitComms copies the total to +0x170 for the next frame.
    void note_attacker(Drone& d);                       // the drone callbacks().seen_and_attacking hook
    int attackers_prev_frame() const { return attackers_prev_; }   // NPCGlobals+0x170
    int attackers_this_frame() const { return attackers_now_; }
    bool combat_music_active() const { return music_active_; }
    std::function<void(Drone&, Speech)> speech;         // voice line hook (audio); null = silent
    std::function<void()> on_combat_music;              // Music_Event(2, 5): the arena/audio hook
    bool alarm_raised = false;            // NPCGlobals+0x1c76 (level 0x7000007 alarm: drones widen their sight)
    // Hooks of the civilian / mission layer (sp_civilian_util.hpp); all optional, null = the effect is not presented.
    //   on_mission_fail  DroneFunc_SetMissionFailReason (0x13e560): (drone, reason 1..0x10, fail label 0x4000033/34 or 0)
    //   on_sfx           NDrone2_PlaySFX / SpeechSFX: (drone, Drone_SfxInfo id) -> sound handle (0 = nothing played)
    //   sfx_finished     Sound_IsFinnished(handle)
    //   on_music_event   Music_Event(event, arg)     on_text  Text_AddMsg(label)
    std::function<void(Drone&, int reason, std::uint32_t label)> on_mission_fail;
    std::function<int(Drone&, int sfx_id)> on_sfx;
    std::function<bool(int handle)> sfx_finished;
    std::function<void(int event, int arg)> on_music_event;
    std::function<void(std::uint32_t label)> on_text;
    int hostages_saved = 0;               // NPCGlobals+0x19c
    int mission_fail_reason = 0;          // last reason passed to DroneFunc_SetMissionFailReason (0 = none)

    // Generic per-drone "scratch" statistics collected for the verification logs.
    struct Stats {
        std::size_t created = 0, captains = 0, deaths = 0, shots = 0, spawner_spawns = 0;
    } stats;

    void tick(nf::World& world, nf::FrameTiming timing) override;

private:
    struct SpawnerRuntime;
    friend struct SpawnerRuntime;
    DroneSystem& drones_;
    SpLevel level_;
    SpTables tables_;
    SpConfig config_;
    DroneStats drone_stats_;   // drone_stats rows: persist over the level in placement order
    std::unique_ptr<CoverSystem> cover_;
    std::vector<std::unique_ptr<SpawnerRuntime>> spawners_;
    std::size_t spawned_ = 0;
    int attackers_now_ = 0, attackers_prev_ = 0;
    std::vector<int> passed_;                // pNPCPassed: drone ids already counted this combat
    bool music_active_ = false;              // MusicVars+0xc
    int music_count_ = 0;                    // MusicVars+0xe
    std::uint32_t music_time_ = 0;           // MusicVars+0x10
    float music_need_ = 2, music_seconds_ = 8, music_radius_ = 25;   // MapDroneData row of the level
    std::uint32_t max_last_seen_ = 0;
    float min_dist_ = 1e9f, min_seen_dist_ = 1e9f;
};

// ---- helpers shared by the state handlers -------------------------------------------------------------------------
// NDrone2_SetIdleTimeOut(dcv, min, rand): Drone+0x104 = now + (rand(r) + min) * FRAME_RATE_INT; msg 4 fires then.
void set_idle_timeout(Drone& d, int min_seconds, int rand_seconds);

// The three dispatch groups of the skeleton every state uses (spec §3.2). They call set_state themselves.
//   impact:  msgs 6..9 and 0x17..0x19 -> DroneFunc_HandleImpact(msg, default_state, non_punch)
//   alerts:  msgs 0x11,0x12,0x14,0x15,0x16,0x1e -> DroneVision_EnemyAlerts (next state or 0)
//   forced:  msg 0x1f -> DroneFunc_DoForcedAttack
//   explosive: msg 0x21 -> DroneFunc_ConsiderExplosive
// Each returns true if `m` belonged to that group (so the handler returns 1).
bool skel_impact(Drone& d, const Msg& m, int default_state = kStAttack);
bool skel_alerts(Drone& d, const Msg& m);
bool skel_forced_attack(Drone& d, const Msg& m);
bool skel_explosive(Drone& d, const Msg& m, int default_state = kStAttack);
// DroneFunc_ConsiderExplosive (0x1431c0), called for msg 0x21 (m.arg = explosive object id/handle, meaning defined by
// the Combat area, which implements it in sp_states_combat.cpp together with DroneFunc_HandleExplosives 0x1435f0).
void consider_explosive(Drone& d, const Msg& m, int default_state);
// All four in the order the Idle/Alert/Patrol states use them; true when consumed.
bool skel_common(Drone& d, const Msg& m, int impact_default_state = kStAttack);   // impact, alerts, forced attack, explosive

// Voice lines (NDrone2_PatrolTalk / AttackTalk / PainTalk / DeathTalk / ...): the audio hook installed by main
// (`SpSystem::speech`) picks the sfx; states call `talk` where the original calls the *Talk function.
void talk(Drone& d, Speech what);

// NDrone2_Enable(flag, drone) 0x14a?: false = hide + no collision + clear the active flags (&= 0xffe7fe5f), true =
// (unless flagged disabled 0x200) make active again (|= 0x801a0, un-hide).
void enable_drone(Drone& d, bool enable);

// DroneFunc_SetAsAttacking (0x147d70): alertness 1, first-attack flags, alt mode cleared, widened sight.
void set_as_attacking(Drone& d);
// DroneFunc_DoForcedAttack (0x148ff0).
bool do_forced_attack(Drone& d);

// NDrone2_SetOpponent-less convenience: the player target of a drone (`NDrone2_Player`): the SP player slot 0.
inline TargetRef player_target() { return TargetRef::player(0); }

}  // namespace nf::sp

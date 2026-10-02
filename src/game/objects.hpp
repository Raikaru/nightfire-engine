#pragma once

// Dynamic objects from map statics: `parsemap_create_dynamic_objects` (0x1d04f0) builds one
// control object per non-world static; this system is its functional reimplementation. Every
// entry below names the original `*_Create`/`*_Update` pair and the static params it reads
// (`StaticInstance::param(key)` = `level_tag+0x2c+4*key`). Only gameplay-relevant behaviour is
// modelled; purely visual/ambience objects are recorded for validate + the renderer and otherwise
// static. Approximations are marked [INFERENCE].
//
// Owned by `game/mission.hpp`'s mission system, which supplies channels, players and sounds:
//   tick() updates everything, publishes solid movers for `ObjectWorld::set_movers` (doors,
//   lifts, parked cars: closed poses block like the original's object collision; open poses move
//   the volume with the panel), draw overrides for `LevelRenderer::draw_objects` (open doors,
//   broken hidings, spinning rotors) and 3D/2D sounds for the audio hook.

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>
#include "assets/nav_data.hpp"
#include "core/rng.hpp"
#include "game/object_world.hpp"

namespace nf {

class Player;
class WeaponSystem;
enum class DamageType : std::uint8_t;
struct WeaponEvents;
struct SoundEvent;
struct ObjectDraw;

namespace sp {
class SwitchChannels;
}

// Entity types of `parsemap_create_dynamic_objects` handled here (the rest: Bots' NPC/spawner/
// cover/AI classes, Movement's ladder/wire/grapple/creep, world spawns, ObjectWorld icons,
// MP-only registrations, renderer sky/env). Unknown classes are counted for validate.
enum class ObjectKind : std::uint32_t {
    Breakable = 32,      // `Break_Create` / `Break_Update`: glass/crates, HP from param 48
    Sensor = 40,         // `Sensor_Create` / `Sensor_Update`: laser trip -> alarm channel
    Switch = 41,         // `Switch_Create` / `Switch_Update` + `Switch_Activate`: use toggles channel
    Lock = 46,           // `Lock_Create` / `Lock_Update`: keypad, gate channel -> unlock channel
    Copter = 47,         // `Copter_Create` / `Copter_Update`: attack helicopter
    Monitor = 48,        // `Monitor_Create` / `Monitor_Update`: use -> channel (security screens)
    FuseBox = 49,        // `FuseBox_Create` / `FuseBox_Update`: use -> channel (needs fuse [INFERENCE])
    Hint = 51,           // `Hint_Create` / `Hint_Update`: channel -> hint text + sound
    GunTurret = 52,      // `GunImp_Create` / `GunImp_Update`: aimed turret
    GrapplePoint = 58,   //-physics anchor only (Movement); recorded for validate
    DynamicProp = 59,    // `DynamicObject_Create` / `DynamicObject_Update`: path-animated prop
    WireAnchor = 62,     // physics anchor only (Movement); recorded for validate
    CreepAnchor = 63,    // physics anchor only (Movement); recorded for validate
    Light = 78,          // `Light_Create`: level light (Characters' bank); recorded
    Rotor = 113,         // `rotor_init`: spinning rotor (angle animated for the renderer)
    Shooter = 210,       // `Shooter_Create`: fixed lane shooter
    Car = 218,           // `Car_Create` / `Car_Update`: parked car (solid; scripted motion [INFERENCE: static])
    Door = 219,          // `Door_Create` / `Door_Update` / `Door_Activate` / `Door_Interp`
    Trigger = 220,       // `Trigger_Create` / `Trigger_Update` (+ Touch/TouchOnce/Multiplex/LoadLevel/MoviePlayer)
    Searchlight = 222,   // `Searchlight_Create`: sweeping cone -> alarm channel
    Creature = 224,      // `Creature_Create` / `Creature_Update`: underwater attacker
    Destroyable = 225,   // `Destroy_Create` / `Destroy_Update` / `Destroy_Smash`: channel-gated smash
    SimpleScript = 226,  // `SS_Create` + `SS_Operate`: touch + gate channel -> output channel
    HurtVolume = 228,    // `Hurt_Create` / `Hurt_Update`: channel-gated damage volume
    LoadLevel = 232,     // `Trigger_LoadLevelCreate`: touch -> level transition
    TouchOnce = 234,     // `Trigger_TouchOnce`: fires once
    Touch = 235,         // `Trigger_Touch`: latches while touched
    MultiplexIn = 236,   // `Trigger_MultiplexIn`: AND of up to 8 input channels
    MultiplexSeq = 237,  // `Trigger_MultiplexSIn`: inputs in sequence (4-tick window)
    MultiplexOut = 238,  // `Trigger_MultiplexOut`: fans the output out to inputs
    MultiplexOr = 239,   // `Trigger_MultiplexOrIn`: OR of up to 8 input channels
    Pickup = 240,        // `Pickup_Create`: SP walk-over grants (weapon/ammo/armour/health)
    MoviePlayer = 244,   // `Trigger_MoviePlayer`: touch -> play cutscene
    SoundTrigger = 249,  // `SoundTrigger_Create` / `SoundTrigger_Update`: channel -> sound
    MusicTrigger = 251,  // `MusicTrigger_Create` / `MusicTrigger_Update`: channel -> Music_Event
    CamSubject = 253,    // `CamSubject_Create`: script-camera look-at anchor
    Mine = 254,          // `Mine_Create` / `Mine_Update`: proximity explosive
};

// One trigger volume (`Trigger_Create`): type = param 44 (Touch/TouchOnce/Multiplex*/LoadLevel/
// MoviePlayer preset it to 2/3/4/5/12/13/10 first, like the originals do).
struct TriggerObject {
    std::size_t placement = 0;
    std::uint16_t type = 2;        // +56: 1 gate, 2 touch latch, 3 AND, 4 sequence, 5 fan-out, 10 activate, 12 OR, 13 load
    std::uint32_t out_channel = 0;  // +32 = param 48
    std::uint16_t gate_channel = 0;  // +36 = param 52 (0 = always armed)
    std::uint16_t inputs[8]{};      // +38.. = params 52/56/... (multiplex inputs)
    std::uint16_t cooldown = 0;      // +60 countdown
    std::uint16_t extra = 0;        // +58 (movie id / level id low word for LoadLevel/MoviePlayer)
    float rate = 0;                 // +8: spline anim rate for roving volumes
    std::uint32_t level_id = 0;     // LoadLevel destination (params 48/52 combined [INFERENCE])
    std::uint32_t script_hash = 0;  // MoviePlayer script to play
    bool once_fired = false;        // TouchOnce latch
    bool touched = false;           // touch state last tick (rising edges)
    int seq_pos = 0;                // MultiplexSeq progress
    float seq_tick = 0;             // MultiplexSeq last-advance tick
    std::uint32_t value = 0;        // SS value channel (param 3): out |= channels[value]
};

// One door (`Door_Create`): params 44 flags, 48 unlock channel, 52 group, 56 lock channel,
// 60 swing/spline selector, 64/68/72 sounds (open/close/locked), 76/80 mode bits, 84/88 bytes.
struct DoorObject {
    std::size_t placement = 0;
    std::uint16_t unlock_channel = 0;
    std::uint16_t lock_channel = 0;
    std::uint16_t group = 0;
    std::uint16_t open_sound = 0, close_sound = 0, locked_sound = 0;
    float progress = 0;      // +80: 0 closed .. 1 open (`Door_Interp`)
    float rate = 0;          // +84: 1 / param 4 (or 1 / FRAME_RATE)
    float direction = 1;     // +88: +1 opening, -1 closing
    bool opening = false;    // target state (`Door_Activate` toggles)
    bool locked_shown = false;  // +112 & 0x10: the "locked" message already fired
    std::uint16_t auto_close = 0;  // +114: frames without anyone near before auto-close
    bool swing = true;       // yaw fallback: no framelist path (`Door_SetupSwing` doors)
    bool spline = false;     // framelist eval mode (`+112 & 1` from param 4: spline vs keyframe)
    std::uint16_t path_index = 0;  // spline path when !swing
    bool has_path = false;
    bool auto_door = false;  // opens on proximity (`obj+244 & 2`, data: p0 = 2)
    std::uint16_t flags = 0;        // param 0 (door mode bits)
    float idle_frames = 0;          // open frames with nobody near (auto-close)
    std::array<float, 3> last_max{};  // mover displacement tracking
    bool has_last = false;
    std::vector<PathKey> path;    // spline track (`static_path_refs` match, else swing)
    std::array<float, 3> local_mn{}, local_mx{};  // model-space bounds (rotated each tick)
    bool has_local = false;
};

struct SoundRequest {
    std::uint32_t id = 0;
    std::array<float, 3> pos{};
    bool positional = false;
};


class SpObjects {
public:
    SpObjects(Level& level, std::uint32_t level_id, sp::SwitchChannels& channels);

    // Build from the map statics (classes above; everything else is ignored here).
    void build();

    // One 30 Hz tick. Touchers come from the mission system (slot 0 is Bond, -1 a drone);
    // weapon impacts/explosions break breakables; `weapons` grants pickups (null in tools).
    struct Toucher {
        std::array<float, 3> pos{};
        float radius = 0.55f;
        float height = 1.0f;
        bool is_player = true;
        int slot = -1;  // player slot for use-presses and hurt, -1 = drone
    };
    // `hurt(slot, damage, type, from)`: damage sink (players), null = ignore.
    using HurtSink = std::function<void(int, float, DamageType, const std::array<float, 3>&)>;
    void tick(const std::vector<Toucher>& touchers, const std::vector<bool>& use_pressed,
              const WeaponEvents& weapon_events, float dt_frames, WeaponSystem* weapons,
              const HurtSink& hurt);

    // `ObjectWorld::set_movers` input (doors/panels/cars this frame).
    const std::vector<Mover>& movers() const { return movers_; }
    // `LevelRenderer::draw_objects` overrides (open doors, hidden broken, rotor angles).
    struct DrawOverride {
        std::size_t placement = 0;
        std::array<float, 16> transform{};  // model -> world, column-major
        bool hide = false;                  // broken/removed: skip the static draw
    };
    struct ThirdCamAnchor {  // `ThirdCam_Create` (231): script-camera anchor by id
        std::size_t placement = 0;
        std::uint32_t id = 0;
    };
    struct ScriptPlayerAnchor {  // `SP_CreateScriptPlayer` (217): cutscene hook
        std::size_t placement = 0;
        std::uint32_t hash = 0;
        bool auto_play = false;  // param 2 == 1: play at level start
        std::uint16_t trigger_channel = 0;  // param 6: play on rising edge
        bool started = false;
    };
    // One live script entity (`Script_CreateEntity`): model hash + interpolated key pose.
    // Entity hashes name models (`SP_SetPosRot` drives the object); poses are key offsets
    // composed with the placement transform.
    struct ScriptEntity {
        std::uint32_t hash = 0;
        Vec3 pos{};
        std::array<float, 4> quat{0, 0, 0, 1};
    };
    // Publish movers (+ draw overrides / hides) for script entities whose model hash matches
    // a placement with collision. Called after tick(), before the movers are read.
    void update_script_entities(const std::vector<ScriptEntity>& ents);
    struct Rotor {  // `rotor_init` (113): spins for the renderer
        std::size_t placement = 0;
        float angle = 0;
    };
    const std::vector<DrawOverride>& draws() const { return draws_; }
    // Placements the static draw pass must skip this frame (taken over by draws_).
    const std::vector<std::size_t>& hides() const { return hides_; }
    const std::vector<ThirdCamAnchor>& thirdcams() const { return thirdcams_; }
    const std::vector<ScriptPlayerAnchor>& script_players() const { return script_players_; }
    std::vector<ScriptPlayerAnchor>& script_players() { return script_players_; }
    std::vector<SoundRequest> take_sounds();
    // Use-action entry for Movement's `Player_Activate` probe (Cross): toggles the nearest
    // door / switch / lock / monitor / fusebox in the sphere, true when something took it.
    bool activate_at(const Vec3& center, float radius);
    // Music events (`MusicTrigger_Update`: id with value 2) for the music hook.
    std::vector<std::uint32_t> take_music();
    // HUD text (`Hint_Update`, locked doors): (label, frames, type).
    struct Text {
        std::uint32_t label = 0;
        int frames = 180;
        int type = 1;
    };
    std::vector<Text> take_texts();

    // Cutscene/script hooks.
    void break_near(const std::array<float, 3>& pos);              // `Break_DoBreak` (event 10)
    void set_link_byte(const std::array<float, 3>& pos, std::uint8_t value);  // event 11 (door lock)
    std::size_t door_count() const;
    bool any_door_open() const;
    const std::vector<DoorObject>& doors() const { return doors_; }
    // Live panel transform for the renderer/movers (`Door_Interp` pose).
    void door_pose(DoorObject& d, std::array<float, 16>& out) const;
    // LoadLevel/MoviePlayer requests (level id / script hash, 0 = none).
    std::uint32_t take_load_level();
    std::uint32_t take_movie();

    // Validation: per-class placement counts + issues (unknown classes, dangling params).
    struct Census {
        std::array<std::size_t, 256> per_class{};
        std::vector<std::string> issues;
    };
    Census census() const;

private:
    struct Breakable {
        std::size_t placement = 0;
        float hp = 50;
        std::uint32_t sound = 0;
        std::uint16_t gate = 0;  // Destroy gate channel (param 1); 0 = damage-driven
        bool broken = false;
        std::array<float, 3> center{};
        float radius = 1;
    };
    struct Sensor {
        std::size_t placement = 0;
        std::uint16_t alarm_channel = 0, gate_channel = 0;
        float range = 20.0f;      // param 2 (`+64`)
        float half_sin = 0.10f;   // param 1 (`+60 = sin(p1 * 0.5 deg)` [INFERENCE])
        bool tripped = false;
        // Searchlight sweep (`Searchlight_Update` case 0): yaw/pitch pan around the base
        // angles with the param 4/5 degree range. Only class 222 sweeps (`sweep`).
        bool sweep = false;
        float phase = 0;      // `+4`: advances 1.0 per tick (init `Rand(480)` in the original)
        float amp = 0;        // `+56`: half-range in radians
        float base_yaw = 0;   // `+60`: range center in radians
        float base_pitch = 0;  // `+20`: pitch center (placement pitch; original source unknown)
    };
    struct Turret {
        std::size_t placement = 0;
        std::uint32_t kind = 0;  // Copter / GunTurret / Shooter / Creature
        float range = 30, damage = 10, period = 60, timer = 0;
        std::uint32_t sound = 0;
    };
    struct Volume {  // Hurt volumes (damage per tick) and mines (authored blast damage/radius)
        std::size_t placement = 0;
        std::uint32_t kind = 0;
        std::uint16_t gate_channel = 0;
        float damage = 10, radius = 2;
        std::uint32_t script = 0;  // mines: explosion script hash (p5); 0 = weapon default
        bool spent = false;  // mines: detonated once, then hidden
    };
    struct Switch {
        std::size_t placement = 0;
        std::uint16_t channel = 0, gate = 0;
        std::uint16_t out = 0;  // Lock/Monitor/FuseBox unlock channel (param 1)
        std::uint16_t label = 0, sound = 0;  // Hint text + sound (params 0/1)
        std::uint32_t event = 0;             // MusicTrigger event id (param 2)
        bool fired = false;                  // one-shot latches (Hint/SoundTrigger)
    };
    struct PickupObj {
        std::size_t placement = 0;
        std::uint16_t kind = 0;  // param 44: weapon/ammo/armour/health selector
        std::uint32_t arg0 = 0, arg1 = 0;
        std::uint32_t sound = 245;  // pickup jingle (param 4, 0xFFFF = default)
        std::uint32_t respawn = 0;  // frames until it comes back (param 6, 0 = never)
        float respawn_in = 0;
        bool taken = false;
    };
    struct StaticNote {  // ambience/visual/static: kept for census + renderer only
        std::size_t placement = 0;
        std::uint32_t cls = 0;
    };

    const StaticInstance* statics(std::size_t placement) const;
    std::array<float, 3> placement_pos(std::size_t placement) const;
    void object_bounds(std::size_t placement, std::array<float, 3>& mn, std::array<float, 3>& mx) const;
    bool touch_test(std::size_t placement, const Toucher& t) const;
    bool use_door(DoorObject& d);  // `Door_Activate` toggle/deny for one use edge
    bool use_switch(Switch& sw);   // switch toggle / lock-out set for one use edge
    void tick_switches(const std::vector<Toucher>& touchers, const std::vector<bool>& use_pressed);
    void tick_damage(const std::vector<Toucher>& touchers, const WeaponEvents& weapon_events, float dt_frames,
                     WeaponSystem* weapons, const HurtSink& hurt);

    Level& level_;
    std::uint32_t level_id_ = 0;
    sp::SwitchChannels& channels_;
    CollisionWorld trigger_volumes_;
    std::vector<TriggerObject> triggers_;
    std::vector<DoorObject> doors_;
    std::vector<Breakable> breakables_;
    std::vector<Sensor> sensors_;
    std::vector<Turret> turrets_;
    std::vector<Volume> volumes_;
    std::vector<Switch> switches_;
    std::vector<PickupObj> pickups_;
    std::vector<StaticNote> statics_;
    std::vector<TriggerObject> multiplex_;  // types 3/4/5/12 live here (same update, no volume)
    std::vector<ThirdCamAnchor> thirdcams_;
    std::vector<ScriptPlayerAnchor> script_players_;
    std::vector<Rotor> rotors_;
    std::vector<Mover> movers_;
    std::vector<DrawOverride> draws_;
    std::vector<std::size_t> hides_;  // placements the static pass must skip (doors/rotors/broken/taken)
    std::vector<SoundRequest> sounds_;
    std::vector<std::uint32_t> music_;
    std::vector<Text> texts_;
    // World-space bounds per placement with collision (touch tests + movers).
    std::unordered_map<std::size_t, std::pair<std::array<float, 3>, std::array<float, 3>>> bounds_;
    // Model-space solid leaves per script-driven placement (parsed once from the model's
    // collision BVH, pass-through tris skipped) and the last published world max per merged
    // group (for the mover displacement; regrouping resets it, which is benign).
    struct ScriptSolid {
        std::vector<std::pair<std::array<float, 3>, std::array<float, 3>>> leaves;
        std::vector<std::array<float, 3>> last_max;
    };
    std::unordered_map<std::size_t, ScriptSolid> script_solids_;
    std::uint32_t load_level_ = 0, movie_ = 0;
    std::array<std::uint8_t, 256> prev_{};  // channel snapshot for rising edges
    std::uint64_t tick_ = 0;

    static CollisionWorld make_volumes(Level& level);
};

}  // namespace nf

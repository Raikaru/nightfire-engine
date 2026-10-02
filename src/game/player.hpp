#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <functional>

#include "core/math.hpp"
#include "game/actions.hpp"
#include "game/player_aim.hpp"
#include "game/collision_world.hpp"
#include "game/damage.hpp"
#include "game/ladder.hpp"
#include "game/player_health.hpp"
#include "game/player_rope.hpp"
#include "game/player_vehicle.hpp"
#include "game/player_basis.hpp"
#include "game/player_scan.hpp"
#include "game/player_water.hpp"
#include "game/player_zerog.hpp"

namespace nf {

class ObjectWorld;

// Logic-frame duration as the original derives it: GS_SetRefreshRate(rate) sets FRAME_RATE = rate,
// FRAME_RATE_MUL = 60 / rate (scales per-frame quantities) and REC_FRAME_RATE = 1 / rate (scales
// per-second quantities: gravity, velocity integration). The original re-measures `rate` every main
// loop iteration (60 / vsyncs per frame); nfgame uses a constant 30.
struct FrameTiming {
    float rate = 30.0f;
    float mul() const { return 60.0f / rate; }
    float rec() const { return 1.0f / rate; }
};

// Tunables from TuningVars.txt [GLOBAL] that the movement code reads (Plr_NoAimTurnSpeed_*).
struct LookTuning {
    struct Axis {
        float speed, mul, steps;
    };
    Axis turn{0.04f, 2.0f, 120.0f};   // Plr_NoAimTurnSpeed_X{,_Mul,_Steps}: yaw stick
    Axis pitch{0.01f, 2.0f, 120.0f};  // Plr_NoAimTurnSpeed_Y{,_Mul,_Steps}: pitch stick
    // Weapon raised, aim box (Plr_AimSpeed_*, Plr_AimTurnSpeed_*: TuningVars.txt overrides the ELF's 0.15/0.16).
    float aim_speed_x = 0.1275f, aim_speed_y = 0.136f;
    float aim_turn_x = 0.05f, aim_turn_y = 0.06f;
    // Weapon raised with a scope (Plr_ScopeSpeed_*); the aim box also borrows their ramp multiplier and step count.
    Axis scope_x{0.015f, 3.0f, 120.0f}, scope_y{0.011f, 3.0f, 120.0f};
};

// Constants that live in the original's data or in the animation/skin the player wears.
struct PlayerParams {
    LookTuning look;
    // WldGravity (Player_Init): (0, -9.8, 0) units/s^2.
    Vec3 gravity{0.0f, -9.8f, 0.0f};
    HealthParams health;        // Plr_DMod_*, ContinueHealthBoost*
    bool debug_scanmode = false;   // `debug_scanmode`: Player_StandAtNewPosition starts every player in scan mode
};

// obj+0xF6. Dead / DeadInWater are the substates Player_CheckForDeath switches to (collbody+0x60 bit 0x100 = in water).
// SpawnWait is MP_ReSpawn's countdown into the first-person camera; Car / Gun / Driven are the
// vehicle seats (Car_Activate, GunImp_Activate, GT_TakeControl: input frozen, the vehicle drives).
enum class SubState : std::int16_t {
    Walk = 0, Climb = 1, Grapple = 2, Swim = 3, Crouch = 4, Scan = 5, Wire = 6, Creep = 7, ZeroG = 8, ZeroGWalk = 9,
    SpawnWait = 10, Car = 11, Gun = 12, Dead = 13, DeadInWater = 14, Zipline = 15, Driven = 16
};

// collbody+0x60 status bits the player code communicates through.
namespace body {
constexpr std::uint16_t kZoomed = 0x01;
constexpr std::uint16_t kOnGround = 0x08;   // set by Player_FeetOnPoint
constexpr std::uint16_t kTouching = 0x10;   // Player_CollisionHandler: non-ghost capsule hit
constexpr std::uint16_t kHitAbove = 0x20;   // ...whose contact point is above the capsule's top centre
constexpr std::uint16_t kInWater = 0x100;   // Player_InWater
}  // namespace body

// One player: the `obj_tag` (+0x30 position, +0x50 rotation, +0x90 matrix) plus the BLData fields
// the walking code uses. Names carry the original offsets in comments.
class Player {
public:
    Player(const Vec3& position, float yaw, const PlayerParams& params);

    // Player_Update for the alive/walk/crouch case, including Player_Collision (gravity, capsule).
    void update(const ActionInput& input, const PlayerSettings& settings, const CollisionWorld& world,
                FrameTiming timing);

    // Collide_Update + Player_CollisionHandler.
    void resolve_collisions(const CollisionWorld& world);

    // Player_PositionCamera + Camera_SetToPlayer (first person): eye and orientation.
    void update_camera(FrameTiming timing);
    Vec3 eye() const;
    // Camera pitch in radians (positive looks up); the heading is `yaw`.
    float view_pitch() const {
        if (const auto cam = rope_camera()) return cam->pitch;
        if (matrix_driven()) return matrix_view_pitch();
        return pitch * 1.5707964f;
    }
    // The camera's heading: the body's `yaw`, except on a wire or zip line (the third-person tracking camera).
    float view_yaw() const {
        if (const auto cam = rope_camera()) return cam->yaw;
        if (matrix_driven()) return matrix_view_yaw();
        return yaw;
    }

    // Player_StandAtNewPosition: teleport, reset motion state, drop onto the floor below.
    // `start` is the substate the marker asks for (SpawnPoint::start_type: swimming 3, zero-G 8, crouching 4).
    void stand_at(const Vec3& position, float yaw, const CollisionWorld& world, SubState start = SubState::Walk);
    // Resume from a recorded standing pose (oracle replays): same reset as stand_at without the drop,
    // with the ground contact the previous frame's collision pass would have left behind.
    void place_at_rest(const Vec3& position, float yaw, float pitch, float ground_normal_y);

    // --- swimming, zero-G, scan mode (player_water.cpp, player_zerog.cpp, player_scan.cpp; docs/gameplay.md "Water, zero-G and scan mode") ---
    // Substates 5, 8 and 9 turn the whole body matrix instead of a yaw: `yaw` then only accumulates the turn deltas
    // (obj+0x54), the camera reads view_axes() / view_yaw() / view_pitch() (roll is dropped by the last two).
    bool matrix_driven() const {
        return substate == SubState::Scan || substate == SubState::ZeroG || substate == SubState::ZeroGWalk;
    }
    // Camera_SetToPlayer: viewer+0x120, rows left, up, forward: the body matrix turned by -pitch about its left axis.
    Basis view_axes() const;
    // Player_Update's debug toggle (scan mode = the free-fly camera): `on` enters substate 5, off returns to walking
    // with the camera heading as the body yaw.
    void set_scan_mode(bool on);
    // cel+0x90 of the room the player is in (kNoWater when there is none or the level has no water).
    float water_level() const;
    // Player_MonitorAir's head position (the head bone 0x80000005): where the head-under-water test is made.
    Vec3 head_position() const;

    // --- health, damage, death (player_health.cpp; docs/gameplay.md "Health, damage and death") -----------------
    // Player_Hurt / Player_HandlePain: scales `amount` by game mode, difficulty and body part (Plr_DMod_*), lets
    // armour absorb it (bullets and explosions, type 0, only), takes the rest from health and raises the pain
    // feedback fields. `direction` (unit travel direction of a bullet, zero for explosions and scripts) picks the
    // pain indicator like Player_DealWithObjHit; `from` is remembered in `last_hit`. Death itself is decided by
    // the collision pass (Player_CheckForDeath). Returns the scaled damage, 0 when it was ignored.
    float hurt(float amount, const Vec3& from, const Vec3& direction, DamageType kind = DamageType::Bullet,
               int part = bodypart::kNone);
    float hurt(const HitInfo& hit);
    float health() const { return vitals.health; }
    float armor() const { return vitals.armour; }
    // Health above 0 and not in the death states (MP_playerIsDead).
    bool alive() const { return vitals.health > 0.0f && life != LifeState::Dead && life != LifeState::DeadHold; }
    void set_health(float health);   // Player_SetHealth: never below 0
    void set_armor(float armor);     // BLData+0x8B0
    void kill();                     // Player_Kill: health 0, the next collision pass runs the death
    // Player_HandleDeath (multiplayer): the delay before MP_ReSpawn has passed. Spawn point choice and loadout
    // are the caller's (MP_ReSpawn / MP_EquipPlayer).
    bool respawn_due() const;
    // The player half of MP_ReSpawn: armour 0, `health`, freeze cleared, pitch 0, then Player_StandAtNewPosition.
    void respawn(const Vec3& position, float yaw, const CollisionWorld& world, float health = kStartHealth);
    // Player_Disable / Player_Enable. disable(false): Player_Update and the collision handler stop entirely and
    // the capsule is removed; disable(true): only the movement handlers stop (BLData+0x94B). enable() re-arms
    // after two frames; enable_at teleports first (obj position and heading from a matrix).
    void disable(bool freeze_input_only = false);
    void enable();
    void enable_at(const Vec3& position, float yaw);
    bool enabled() const { return enabled_ != 0; }
    bool input_frozen() const { return input_frozen_; }
    // Player_RamSave / Player_RamLoad (health and armour part): `continuing` = going on to the next level, which
    // guarantees at least the ContinueHealthBoost of the difficulty.
    PlayerCarry carry_over() const { return {vitals.health, vitals.armour}; }
    void restore_carry(const PlayerCarry& carry, bool continuing);
    // Player_SetFlashBang: a white-out that holds for half of `duration` frames and fades in the rest.
    void set_flash_bang(float duration, std::uint8_t colour);
    // Camera_SetFade argument Player_Update computes this frame (-100000 = hold, -timer = fading, 0 = none).
    float screen_fade() const { return screen_fade_; }
    // Player_Activate's probe: the sphere the use action tests against the doors, triggers and locks around the
    // head (radius 1 at head + view direction); the object classes it dispatches to are listed in the docs.
    struct ActivationProbe {
        Vec3 center;
        float radius;
    };
    ActivationProbe activation_probe() const;
    // The camera's forward vector (row 2 of viewer+0x120): body yaw, then pitch.
    Vec3 view_direction() const;
    // Collected side effects (sounds, rumble, death); cleared by taking them.
    HealthEvents take_events();

    // --- rope-like movement (player_rope.cpp, grapple.cpp, wire.cpp; docs/gameplay.md "Grapple, wire and zip line") ---
    // Player_SetGrapplePoint: the grapple hook (weapons 0x50 / 0x51) caught at `point` (the bullet's position when
    // it ended on a 'Q' object, see RopeWorld::grapple_ray). Substate 2 then pulls the player to within 2 units of
    // the point and hangs there while the fire button (action 9) is held.
    void begin_grapple(const Vec3& point);
    int grapple_phase() const { return rope_.grapple.phase; }   // BL+0x15C (GrappleState::Phase); weapons 0x50/0x51 cannot fire in substate 2 while it is 1
    Vec3 grapple_point() const { return rope_.grapple.point; }  // BL+0x140
    bool grapple_rope_visible(int weapon_id) const;             // Player_GrappleSetRope's show test for the rope model
    // The wire / zip line objects and animation the substates need (World::spawn_player attaches the world's).
    void set_rope_world(const RopeWorld* world) { rope_world_ = world; }
    void set_rope_animator(std::shared_ptr<RopeAnimator> animator) { rope_animator_ = std::move(animator); }
    // Player_WeaponNone was applied on grabbing a wire; the weapon code puts the weapon back on release.
    bool weapon_stowed() const { return rope_.weapon_stowed; }
    // The objects the collision pass reports beyond the world cels (ladders, wires, grapple points, ThirdIcon zones),
    // the ladder / creep-wall registries and the switch channels (World::spawn_player attaches the world's).
    void set_object_world(ObjectWorld* world) { object_world_ = world; }
    // --- ladders and creep walls (player_climb.cpp; docs/gameplay.md "Ladders and creep walls") ---
    // BLData+0x95F: type of the ThirdIcon zone the capsule overlapped in the previous collision pass (0xFF = none).
    // Icon 6 = "hug the wall": the use action (Player_Activate, Cross) then grabs the nearest creep wall.
    std::uint8_t icon_context() const { return icon_context_; }
    // Player_Activate took the use action (Cross) this frame: a creep wall was in reach (icon 6). The weapon code
    // must not also reload on that press.
    bool use_action_consumed() const { return use_consumed_; }
    // Scripting's use-action half (SpObjects::activate_at): called with the activation probe when Cross
    // is pressed and no creep wall took it; its return value joins use_consumed_.
    void set_use_handler(std::function<bool(const ActivationProbe&)> handler) { use_handler_ = std::move(handler); }
    // --- vehicles (player_vehicle.cpp; docs/gameplay.md "Vehicles") ---
    // Car_Activate / GunImp_Activate / GT_TakeControl, player halves: links the vehicle object
    // (BLData+0x878), switches to substate 11 / 12 / 16 with camera mode 0xD / 0xE / 0xF, stows the
    // weapon (Player_WeaponNone) and freezes input (Player_Disable(obj, 1)). The vehicle-side state
    // (its own substate, sounds, gun flags) is the Driving slice's; `object` is its object handle.
    void board_vehicle(VehicleKind kind, std::uint32_t object);
    // Car_Deactivate / GunImp_Deactivate, player half: first-person camera, back on foot, input released.
    void leave_vehicle();
    bool in_vehicle() const { return vehicle_.kind != VehicleKind::None; }
    BoardedVehicle vehicle() const { return vehicle_; }
    // Player_SetCamMode (BLData+0x950). Entering a vehicle mode also resets the HUD (nf_ui's Hud::reset;
    // the caller applies it, see player_vehicle.hpp).
    void set_cam_mode(CamMode mode);
    CamMode cam_mode() const { return cam_mode_; }
    // MP_ReSpawn's countdown (substate 10): after `frames` logic frames the camera returns to first person.
    void enter_spawn_wait(int frames = kSpawnWaitFrames);

    Vec3 pos{};                 // obj+0x30
    float yaw = 0;              // obj+0x54, forward = (sin yaw, 0, cos yaw)
    float pitch = 0;            // BL+0x8A8, in units of pi/2 (positive looks up), clamped to [-1, 1]
    SubState substate = SubState::Walk;   // obj+0xF6

    Vec3 velocity{};            // BL+0x10: this frame's walk step in body space (x = left, z = forward)
    Vec3 fall_velocity{};       // BL+0x50: world-space velocity from gravity / jumping (units/s)
    std::uint16_t body_flags = 0;   // collbody+0x60
    float ground_normal_y = 1.0f;   // BL+0x110
    std::uint8_t jump_state = 0;    // BL+0x94C: 0 grounded, 1 rising, 2 falling
    std::uint16_t ground_history = 0xF;  // BL+0x940: one bit per recent frame with ground contact

    // Last capsule handed to the collision pass (BL+0x760/0x770, radius BL+0x7CC).
    Vec3 capsule_a{}, capsule_b{};
    float capsule_radius = 0.55f;
    Vec3 settled_pos{};         // obj+0xC0: position at the end of the last collision handler

    // collbody+0xCC: distance from obj+0x30 down to the skin's feet in the CURRENT animation pose.
    // Player_Collision builds the capsule from it and Player_FeetOnPoint probes to it, so the walk
    // cycle bobs the player and crouching lowers the capsule. The original refreshes it from the
    // animation system every frame; this port has no animation, so it stays at the standing-idle
    // value of the multiplayer skin unless the caller (an oracle replay) supplies the recorded one.
    // While the player stands on the ground every change of it moves obj+0x30 by the same amount
    // (measured: the frame's dy minus the collision push equals the frame's change of collbody+0xCC
    // exactly), i.e. the animation keeps the feet planted.
    float stand_height = 1.0327658653f;

    float eye_height = 0.7f;    // BL+0x908
    float crouch_dip = 0.0f;    // BL+0x90C

    Vitals vitals;              // BL+0x894 health, +0x8B0 armour, +0x8BC damage flash, +0x967 pain direction, +0x968 overlay
    LifeState life = LifeState::Alive;   // obj+0xF4
    LastHit last_hit;
    bool movement_frozen = false;   // BL+0x160: set while a guided projectile is flown, Player_Move ignores the sticks
    float model_alpha = 1.0f;       // BL+0x888: how solid the body is for other views, eases back to 1 (obj+0x106)
    float fade_total = 0.0f;        // BL+0x918 / +0x91C: flash-bang duration and frames left
    float fade_timer = 0.0f;
    std::uint8_t fade_colour = 0xFF;   // BL+0x963
    bool death_pane = false;        // Player_HandleDeath (single player): the "dead" HUD pane 0xC is up
    bool mission_failed = false;    // switch channel 0x62, set by Player_HandleDeath once the death pane is up

    // --- water and air (player_water.cpp) ---
    const RoomMap* rooms = nullptr;   // the level's rooms (World::spawn_player attaches them); without them there is no water
    WaterState water;                 // obj+0x20 room, BL+0x8C0 air, `surfaced`, HUD air meter
    // Aim cursor inside the aim box, each axis in [-1, 1] (HUD crosshair offset; 0 while scoped or lowered).
    std::array<float, 2> aim_cursor() const { return {aim_state_.cursor_x / kAimBoxX, aim_state_.cursor_y / kAimBoxY}; }
    bool scope_aiming = false;        // the raised weapon is scoped (weapon flags & 0x40): Player_Aiming's scope branch
    float zoom = 1.0f;                // BL+0x8D0: Player_Zoom's factor; the zero-G look speed divides by it while zoomed
    ZeroGState zerog;                 // BL+0xD0
    ScanState scan;                   // viewer+0x118

private:
    void move(const ActionInput& input, FrameTiming timing, float speed_scale);   // Player_Move
    void handle_jump(const ActionInput& input, const CollisionWorld& world, FrameTiming timing);                                          // Player_HandleJump
    void aim(const ActionInput& input, FrameTiming timing);                       // Player_Aiming
    void aim_raised(const ActionInput& input, FrameTiming timing);                // Player_Aiming, weapon raised
    void collision_setup(FrameTiming timing);                                     // Player_Collision
    void set_substate(SubState s, FrameTiming timing);                            // Player_ChangeSubState
    bool begin_update(const PlayerSettings& settings, FrameTiming timing);        // Player_Update before the state switch
    void update_dead(FrameTiming timing);                                         // Player_Update, states 0/2/3
    void step_dead_body(FrameTiming timing);                                      // Player_Collision, substates 13/14
    void check_for_death();                                                       // Player_CheckForDeath(obj, 3)
    void fall_damage(FrameTiming timing);                                         // Player_CollisionHandler fall timer
    void reset_motion(const Vec3& position, float yaw);
    std::array<Vec3, 3> orientation() const;
    void update_grapple(const ActionInput& input, const CollisionWorld& world, FrameTiming timing);   // Player_SSGrapple
    void update_wire(const ActionInput& input, FrameTiming timing);               // Player_Wire
    void update_zipline(FrameTiming timing);                                      // Player_Zipline
    void collide_wire(const WireObject& wire);                                    // Player_CollWire
    void deal_with_wire_hits(const CylinderResult& result);                       // Player_DealWithObjHit, class ','
    bool jump_onto_wire();                                                        // Player_HandleJump, BL+0x95F == 3
    void release_rope(std::uint8_t lockout);                                      // leaving substates 6 / 15
    void apply_rope_root_motion();                                                // AnimObjectUpdate on a wire
    void play_rope_script(std::uint32_t script, float speed);                     // AnimScriptAdd(Speed)
    bool rope_script_stopped() const;                                             // AnimScriptIsStopped
    void wire_channel(bool on);                                                   // switch_channels[wire channel]
    std::optional<RopeCamera> rope_camera() const;                                // camera mode 0xB
    bool in_water();                                                              // Player_InWater
    void monitor_air(FrameTiming timing);                                         // Player_MonitorAir
    void swim_direction();                                                        // Player_Move, substate 3
    void update_swim(const ActionInput& input, FrameTiming timing);               // Player_Update, case 3
    void track_room(const CollisionWorld& world);                                 // control_handle_cel_change
    void update_zerog(const ActionInput& input, FrameTiming timing);              // Player_ZeroG
    void update_scan(const ActionInput& input, FrameTiming timing);               // Player_SSScanMode
    float matrix_view_pitch() const;
    float matrix_view_yaw() const;
    void climb_update(const ActionInput& input, FrameTiming timing);              // Player_Climb
    void creep_update(const ActionInput& input);                                 // Player_Creep
    void touch_ladder(const LadderObject& ladder);                                // Player_CollLadder
    bool mount_creep_wall(const CreepWallObject& wall, const CollisionWorld& world);   // Player_CollCreepWall
    void resolve_climb(const CylinderResult& result);                             // Player_CollisionHandler, substate 1
    void deal_with_ladder_hits(const CylinderResult& result);                     // Player_DealWithObjHit, class 0x2D
    void clear_inertia();                                                         // Player_ClearInertia
    void apply_creep_root_motion();                                               // AnimObjectUpdate while creeping
    void leave_climb(std::uint8_t lockout);                                       // Player_ChangeSubState(0) after 1 / 7
    void activate_creep_wall(const ActionInput& input, const CollisionWorld& world);   // Player_Activate, creep walls
    void update_spawn_wait();                                                          // Player_Update, case 10

    PlayerParams params_;
    Vec3 prev_velocity_{};      // BL+0x20
    float yaw_step_ = 0;        // BL+0x34
    float turn_speed_ = 0;      // BL+0x8FC
    float pitch_speed_ = 0;     // BL+0x900
    AimState aim_state_;        // BL+0x118.. / +0x8EC..: raised-weapon aim cursor and ramps
    float aim_yaw_ = 0;         // yaw the raised-weapon look adds after this frame's movement (obj+0x50 tail)
    float pitch_target_ = 0;    // BL+0x8AC
    std::uint8_t look_state_ = 0;    // BL+0x130: 1 = looking, 2 = recentring, 3 = recentred
    std::uint8_t walk_class_ = 1;    // BL+0x131
    std::uint8_t jump_delay_ = 0;    // BL+0x952
    std::uint8_t crouch_timer_ = 0;  // BL+0x960
    float applied_height_ = 1.0327658653f;   // stand_height already folded into pos
    float last_height_delta_ = 0.0f;          // foot-height delta applied this frame (along last_height_vec_)
    Vec3 last_height_vec_{};              // pos shift from the foot height (reverted if frozen-airborne)
    Vec3 prev_pos_{};           // BL+0x00
    CylinderResult last_cylinder_;
    FrameTiming timing_;        // FRAME_RATE / _MUL / REC_FRAME_RATE as of the last update
    std::uint8_t enabled_ = 1;  // BL+0x94A: 0 disabled, 1 running, 2.. counting down after Player_Enable
    bool input_frozen_ = false; // BL+0x94B
    float fall_timer_ = 0;      // BL+0x928: frames of falling (FRAME_RATE_MUL each), > 60 hurts on landing
    int death_frames_ = 0;      // GameState frame - obj+0xEC
    float screen_fade_ = 0;
    std::uint32_t rand_state_ = 1;   // Rand(4) of the pain grunt
    HealthEvents events_;
    Basis body_ = yaw_basis(0.0f);   // obj+0x90 rows in the matrix-driven substates (5, 8, 9); otherwise built from `yaw`
    // Rope-like movement (player_rope.cpp): grapple, wire and zip line state.
    RopeState rope_;
    const RopeWorld* rope_world_ = nullptr;
    std::shared_ptr<RopeAnimator> rope_animator_;
    std::size_t capsule_ignore_ = SIZE_MAX;   // BL+0x7B8: object the capsule ignores (the wire hung on)
    unsigned capsule_pick_ = 0;               // BL+0x7D8: 0x1C0 on a zip line
    ObjectWorld* object_world_ = nullptr;
    // Ladders and creep walls (player_climb.cpp). BL+0x884 holds the object linked to; BL+0x951 (the re-grab
    // lockout) is RopeState::attach_lockout, shared with the wires.
    const LadderObject* ladder_ = nullptr;          // substate 1
    const CreepWallObject* creep_wall_ = nullptr;   // substate 7
    std::uint8_t creep_dir_ = 0;                    // BL+0x94D while creeping: 0 idle, 1 / 2 shuffling either way
    std::uint32_t creep_script_ = 0;                // the animation script Player_Creep started last (0 = none)
    std::uint8_t icon_context_ = 0xFF;              // BL+0x95F
    std::uint8_t icon_next_ = 0xFF;                 // what the last collision pass saw; latched by the next update
    bool use_consumed_ = false;
    std::function<bool(const ActivationProbe&)> use_handler_;   // Scripting::SpObjects::activate_at
    // Vehicles (player_vehicle.cpp). BL+0x878 holds the vehicle/gun object while boarded.
    BoardedVehicle vehicle_;
    CamMode cam_mode_ = CamMode::FirstPerson;   // BL+0x950
    std::int16_t spawn_timer_ = 0;              // BL+0x4A2: substate 10 countdown
    bool spawn_flag_ = false;                   // BL+0x21E: substate 10 camera still to restore
};

}  // namespace nf

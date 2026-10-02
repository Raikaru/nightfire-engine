#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include "core/rng.hpp"
#include <vector>

#include "assets/character.hpp"
#include "assets/weapon_data.hpp"
#include "game/damage.hpp"
#include "game/projectiles.hpp"
#include "game/world.hpp"
namespace nf { struct HudState; }   // ui/hud.hpp; defined there, filled by fill_hud for the UI slice.
namespace nf::drone { class DroneSystem; }   // game/drone_system.hpp; threat query for idle fidgets
namespace nf {

// Weapon animation object states (`obj+244` of BLData+2024, docs/spec-weapons.md 6.1).
enum class WeaponAnim : std::uint8_t {
    Idle = 0, Lower = 1, LowerWait = 2, RaiseStart = 3, RaiseWait = 4, ReloadStart = 5, Reload = 6, ReloadEnd = 7,
    ModeSwitch = 8, Firing = 9, RaiseReverse = 10, FireHold = 11, FireRepeat = 12, LowerAlt = 13, LowerWaitAlt = 14,
    AimIn = 15, AimOut = 16,
};

// What a player spawns with (MP_EquipPlayer): weapon 1 (fists) with 0 rounds, `start_weapon` with `start_rounds`
// (default 2 x clip), optionally the grapple (0x50).
struct SpawnLoadout {
    float health = 100.0f;
    int start_weapon = 6;
    int start_rounds = -1;     // -1: twice the clip size (BOTWEAP_InitWeapon / MP_EquipPlayer)
    bool grapple = false;
};

// Sounds and visual hooks of one tick for the frontend (audio, decals, sparks, explosion sprites).
struct SoundEvent {
    int id;                    // SFX id (Sound_Play / Sound_Play3D argument)
    Vec3 position;
    bool positional;           // Sound_Play3D at `position`; false = Sound_Play (2D, the player's own gun)
    int listener;              // player slot that hears a 2D sound; -1 = everybody
    int exclude = -1;          // player slot that must not hear it (its own gun plays the 2D version)
};
struct ImpactEvent {
    Vec3 point, normal;
    int surface;               // material & 0x3F (EffectInfo row)
    int weapon;
    bool on_body;              // hit a player / target (blood) instead of the world
    int shooter;
};
struct ExplosionEvent {
    Vec3 position;
    float radius;
    int weapon;
};
struct WeaponEvents {
    std::vector<SoundEvent> sounds;
    std::vector<ImpactEvent> impacts;
    std::vector<ExplosionEvent> explosions;
    void clear() { sounds.clear(), impacts.clear(), explosions.clear(); }
};

// Per-player weapon state (BLData weapon fields + obj220 weapon bytes + the weapon anim object).
struct PlayerWeapons {
    struct Slot {
        std::int16_t clip = 0;         // BLData+440 + 12 w
        bool owned = false;            // +442
        std::uint8_t mode_index = 0;   // +443 fire-mode cycle index
        std::int8_t upgrade_off = 0;   // +444
        float saved_zoom = 1.0f;       // +436
    };
    std::array<Slot, WeaponTable::kWeaponCount> weapon{};
    std::array<std::uint16_t, WeaponTable::kAmmoCount> pool{};   // BLData+368
    int current = 71, selected = 71, previous = 71;               // obj220+98 / +99 / +100
    int last_gun = 0, last_gadget = 0;   // BLData+2352 / +2354
    bool aim = false;                  // obj220+96 bit 0
    bool aim_latched = false;          // bit 1
    WeaponAnim anim_state = WeaponAnim::Idle;
    float cooldown = 0;                // BLData+2348 (60 Hz frames)
    int shots_left = 0;                // +2358
    int cycle_start = 0;               // burst size at trigger time (spread growth: def+40 - shots_left)
    std::uint8_t idle_phase = 0;       // +2393: 0 wait, 1 fidget playing, 3 fidget armed (spec 760)
    int idle_frames = 0;               // +2362: settled frames while idle
    int fidget_frames = 0;             // +2366: fidget countdown (20 s once started)
    bool bullet_spawned = false;       // +2396
    bool alt_reload = false;           // +2392
    bool alt_hand = false;
    bool fire_blocked = false;         // Input_ClearAction(pad, 9): fire ignored until released
    bool anim_reverse = false;         // aim-out plays the aim-in script backwards
    float reverse_frame = 0;
    float zoom = 1.0f, zoom_target = 1.0f;   // +2256 / +2260
    int muzzle_frames = 0;             // +2360: frames left of the muzzle flash quad
    float recoil_phase = 0;
    bool dead = false;                 // mirrors !Player::alive(): the gun is put away and stops firing
    std::uint32_t rumble = 0;
    // The weapon anim object: skin of def[current].model_gfx, its clip player.
    std::unique_ptr<CharacterInstance> anim;
    int anim_weapon = -1;              // weapon id `anim` was built for
    float anim_frame_prev = 0;
    std::size_t anim_cmd_next = 0;     // next script sound command to trigger
    std::uint32_t anim_script = 0;
    unsigned sleeve = 0;               // BLData+2405
    std::uint32_t datum0_entity = 0;   // Player_WeaponFiring datum-0 override: entity model hash, 0 = hidden
    std::uint32_t datum0_part = 0;     // skin part hidden while the override runs (def.datum0_gfx, e.g. suppressor)
};

// First-person weapon draw data (Player_SetWeaponAnim / Player_PositionGun).
struct ViewModel {
    bool visible = false;
    int weapon = 0;
    const SkinDef* skin = nullptr;
    const CharacterInstance* anim = nullptr;
    unsigned sleeve = 0;
    Vec3 offset{};              // gun position in view space (x right, y up, z forward), including recoil sway
    float muzzle_flash = 0;     // > 0 while the muzzle flash quad shows (frames left)
    Vec3 flash_color{};         // 0..1 (def+86/85/84)
    float zoom = 1.0f;
    bool aiming = false;
    std::uint32_t datum0_entity = 0;   // draw this model at datum 0 (0 = hide; Characters honours it)
    std::uint32_t datum0_part = 0;     // skip the skin part with this hash (parked suppressor placeholder)
};

// The weapon system: inventory, the weapon state machine, bullets/projectiles/explosions and damage. A
// `System` that runs after the players moved.
class WeaponSystem : public System {
public:
    WeaponSystem(WeaponTable table, DamageTuning tuning);
    ~WeaponSystem() override;

    const WeaponTable& table() const { return table_; }
    DamageTuning& tuning() { return tuning_; }

    // Weapon skins and animation scripts (a level's CharacterBank). Without it weapons have no animation
    // timing: every anim counts as finished at once and there is no view model.
    void set_bank(CharacterBank* bank) { bank_ = bank; }
    void set_drone_system(const drone::DroneSystem* drones) { drones_ = drones; }   // threat gate for idle fidgets; null = none
    void set_match_rules(MatchRules* rules) { rules_ = rules; }
    // The world the shooters live in; tick() attaches it too, call this earlier when fire()/spawn_player() run before the first tick.
    void attach(World& world) { world_ = &world; }

    // --- combatants -----------------------------------------------------------------------------
    // Bots and drones: returns the shooter id (>= 4) to pass to fire() and see in HitInfo::attacker.
    int register_target(DamageTarget* target);
    void unregister_target(DamageTarget* target);

    // Any shooter (Player_WeaponInitBullet's spawn loop -> Bullet_init): spawns weapon `weapon_id`'s bullets or
    // projectile from `origin` along `direction`. `owner_aiming` matters for F1 & 0x400000 weapons.
    struct Shooter {
        int id = -1;
        Vec3 origin{}, direction{0, 0, 1};
        bool owner_aiming = false;
        int shots_in_burst = 0;
        float damage_scale = 1.0f;   // multiplies the direct and blast damage of this shot (Drone_ModBulletDamage)
    };
    void fire(const Shooter& shooter, int weapon_id);
    // Explode_Create at `position` with the weapon's radius and damage.
    void explode(const Vec3& position, int weapon_id, int attacker);

    // --- players (slot 0..3) ----------------------------------------------------------------------
    void spawn_player(int slot, const SpawnLoadout& loadout = {});   // MP_EquipPlayer, done lazily by tick()
    void respawn(int slot, const Vec3& position, float yaw, const SpawnLoadout& loadout);
    bool has_player(int slot) const;
    bool alive(int slot) const;
    float health(int slot) const;
    float armour(int slot) const;
    void kill(int slot);
    bool give_weapon(int slot, int weapon_id, int rounds);   // Player_EquipWeapon; false if refused (model missing)
    bool give_ammo(int slot, int weapon_id, int rounds);     // Player_EquipAmmo; false when the pool is full
    bool give_armour(int slot, float amount);                // false when armour >= 50
    // Damage from scripts / the environment (Player_Hurt).
    void hurt_player(int slot, float damage, DamageType type, int attacker = -1);

    // Inventory and state queries.
    int current_weapon(int slot) const;
    int selected_weapon(int slot) const;
    WeaponAnim anim_state(int slot) const;
    bool owns(int slot, int weapon_id) const;
    int clip(int slot, int weapon_id) const;               // Player_AmmoInGun
    int ammo_pool(int slot, int ammo_type) const;
    bool aiming(int slot) const;
    float zoom(int slot) const;                            // current zoom factor (BLData+2256)
    // Fills the weapon/aiming/ammo fields of the HUD state for the UI slice (HUD_UpdateAmmoPane,
    // HUD_UpdateCrossHair): weapon in hand / being switched to, clip + reserve, aim/zoom/scope, crosshair
    // row, damage flash. Needs ui/hud.hpp where defined; no link dependency (plain data).
    void fill_hud(int slot, HudState& hud) const;
    ViewModel viewmodel(int slot) const;
    const PlayerWeapons* state(int slot) const;
    PlayerWeapons* state(int slot);
    void select_weapon(int slot, int weapon_id);           // scripted / debug select (Player_WeaponSelect path)
    Vec3 aim_direction(int slot, const World& world) const;

    // --- simulation ---------------------------------------------------------------------------------
    void tick(World& world, FrameTiming timing) override;
    WeaponEvents& events() { return events_; }               // cleared by the consumer
    const std::vector<Projectile>& projectiles() const { return projectiles_; }

    // Ammo pool / clip rules (docs/spec-weapons.md 3.2), public for the HUD and bots.
    int ammo_index(int weapon_id) const;                   // Player_AmmoIndex
    bool has_ammo(const PlayerWeapons& p, int weapon_id) const;   // Player_WeaponHasAmmo

    // Deterministic random source shared by spread, ricochets, sound picks: the process-global
    // game Rand stream (core/rng.hpp; Rand_Rand/Rand_FRand/Rand_FRand_MVar2 exact).
    float frand();                                          // [0, 1), Rand_FRand(1)
    GameRng& rng() { return game_rng(); }
    void seed(std::uint32_t x, std::uint32_t y) { game_rng().seed(x, y); }   // differential poking
    void seed_match(std::uint32_t s) {   // --seed / options.seed: fold one word into both lanes
        game_rng().seed(GameRng::kBootX ^ s, GameRng::kBootY ^ (s * 0x85EBCA6Bu));
    }

private:
    // One thing a bullet can hit: a player capsule or a registered target.
    struct Victim {
        int id;
        Vec3 a, b;
        float radius;
        DamageTarget* target;   // null for players
    };
    struct SegmentHit {
        bool world = false;     // false: a victim
        float t = 1.0f;
        Vec3 point{}, normal{};
        int surface = 0;
        int victim = -1;        // Victim::id
        int part = bodypart::kNone;
    };

    void ensure_player(int slot, World& world);
    void tick_player(int slot, World& world, FrameTiming timing);
    void anim_update(int slot, PlayerWeapons& p, World& world, FrameTiming timing);   // Player_SetWeaponAnimObj
    void weapon_input(int slot, PlayerWeapons& p, World& world, FrameTiming timing);  // Player_Weapon
    void weapon_firing(int slot, PlayerWeapons& p, World& world, FrameTiming timing); // Player_WeaponFiring
    void weapon_change(PlayerWeapons& p, int dir, int group);
    void weapon_select(PlayerWeapons& p);
    void weapon_none(PlayerWeapons& p);
    void handle_no_ammo(PlayerWeapons& p);
    void best_weapon(PlayerWeapons& p);
    bool round_to_fire(PlayerWeapons& p, int weapon_id, int need, int use);
    bool reload_ammo(PlayerWeapons& p, int weapon_id, bool dry);
    void equip_weapon(PlayerWeapons& p, int weapon_id, int rounds, bool auto_switch);
    bool better_weapon(const PlayerWeapons& p, int candidate) const;
    void init_bullet(int slot, PlayerWeapons& p, World& world);            // Player_WeaponInitBullet
    void set_firing_anim(PlayerWeapons& p, const WeaponDef& d);            // Player_SetFiringAnim
    void start_reload(PlayerWeapons& p, const WeaponDef& d);
    void set_weapon_anim(PlayerWeapons& p);                                // Player_SetWeaponAnim
    void update_datum0(PlayerWeapons& p);                                  // Player_WeaponFiring datum-0 override
    void play_script(PlayerWeapons& p, std::uint32_t script, bool loop, float speed = 1.0f);
    void play_script_reversed(PlayerWeapons& p, std::uint32_t script);
    bool script_stopped(const PlayerWeapons& p) const;
    float script_frame(const PlayerWeapons& p) const;
    void advance_anim(int slot, PlayerWeapons& p, const World& world);
    void update_zoom(PlayerWeapons& p, const ActionInput& in, FrameTiming timing);          // Player_Zoom
    void reset_zoom_for_weapon(PlayerWeapons& p);
    void clamp_zoom_target(PlayerWeapons& p);
    bool owned(const PlayerWeapons& p, int weapon_id) const;
    void sound(int id, const Vec3& pos, bool positional, int listener = -1, int exclude = -1);
    Vec3 head_pos(int slot, const World& world) const;

    // projectiles.cpp
    std::vector<Victim> collect_victims(const World& world) const;
    void spawn_projectile(const Shooter& shooter, const WeaponDef& def);
    void step_projectiles(World& world, FrameTiming timing);
    bool step_projectile(Projectile& b, World& world, FrameTiming timing, const std::vector<Victim>& victims);
    Projectile* find_guided(int owner);              // live guided (F2 & 0x4) projectile of `owner`, if any
    void detonate_owned(int owner, int weapon_id);  // explode every live projectile of the pair (detonators)
    void update_owner_locks(World& world);           // guided / weapon-lock projectiles freeze their owner
    SegmentHit trace_segment(const World& world, const Vec3& from, const Vec3& to, int owner,
                             const std::vector<Victim>& victims) const;
    void bullet_hit(Projectile& b, const SegmentHit& hit, const WeaponDef& def, const Vec3& dir);
    void hurt_victim(int victim_id, const HitInfo& hit);
    void damage_player(int slot, const HitInfo& hit);
    void explode_at(const Vec3& pos, const WeaponDef& def, int attacker, float scale = 1.0f);

    WeaponTable table_;
    DamageTuning tuning_;
    CharacterBank* bank_ = nullptr;
    const drone::DroneSystem* drones_ = nullptr;   // threat gate for idle fidgets (Bots-2 any_visible_threat)
    MatchRules* rules_ = nullptr;
    std::array<std::unique_ptr<PlayerWeapons>, World::kMaxPlayers> players_;
    std::array<SpawnLoadout, World::kMaxPlayers> loadouts_{};
    std::vector<DamageTarget*> targets_;
    std::vector<int> target_ids_;
    int next_target_id_ = World::kMaxPlayers;
    std::vector<Projectile> projectiles_;
    WeaponEvents events_;
    World* world_ = nullptr;   // valid during tick() and fire()
    FrameTiming timing_;
    // Weapon anim scripts advance FRAME_RATE_MUL script frames per logic frame (60 fps scripts), see docs/gameplay.md.
    static constexpr float kScriptRate = 2.0f;
};

}  // namespace nf

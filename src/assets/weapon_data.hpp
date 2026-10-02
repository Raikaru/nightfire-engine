#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "assets/elf.hpp"

namespace nf {

// weapon_definition_tag (ACTION.ELF symbol `weapon_data`, 115 rows x 268 bytes). The bytes in the ELF file are
// zero: the table is filled at boot by the C++ static initializer `_GLOBAL_$I$weapon_data` (calls
// `__static_initialization_and_destruction_0` at 0x1B3488). WeaponTable::from_elf runs that function on a small
// R5900 interpreter and decodes the resulting memory. Field offsets in comments; semantics in docs/spec-weapons.md.
struct WeaponDef {
    std::uint16_t id = 0;              // +0  == table index
    std::uint16_t base = 0;            // +2  clip / zoom owner variant
    std::uint8_t selectable = 0;       // +4  1 = appears in weapon cycling / may be owned
    std::int8_t alt = 0;               // +5  signed id delta to the alternate fire-mode variant
    std::uint8_t category = 0;         // +6
    float blast_radius = 0;            // +8  > 0: explodes on impact (Explode_Create radius)
    float damage = 0;                  // +12
    std::uint16_t class_flags = 0;     // +16 mask tested by Collide_FilterBullets; & 0xF8 = counts as shot
    float autoaim = 0;                 // +20 percent
    std::uint8_t pellets = 0;          // +24
    float range = 0;                   // +28
    float speed = 0;                   // +32 units per frame at 60 Hz
    float spread = 0;                  // +36 base spread, 0.0014 rad units
    std::array<std::uint16_t, 4> fire_count{};   // +40 rounds per trigger cycle by mode; [3] = number of modes
    std::uint32_t mode_label = 0;      // +48
    std::uint32_t name_label = 0;      // +56 (SP)
    std::uint32_t mp_name_label = 0;   // +60
    std::uint32_t fire_interval = 0;   // +64 frames
    std::uint16_t fire_delay = 0;      // +68 frames after the fire anim starts before the bullet spawns
    std::uint32_t muzzle_script = 0;   // +72
    std::uint8_t flash_b = 0, flash_g = 0, flash_r = 0;   // +84,+85,+86 muzzle light colour
    std::uint32_t projectile_gfx = 0;  // +92
    std::uint32_t fire_sound = 0;      // +96 SFX id of the shot as heard by other players / from drones (SFX_WEAPON_DRONE_*_SHOT)
    std::uint32_t flags1 = 0;          // +104 player-side behaviour
    std::uint32_t flags2 = 0;          // +108 projectile behaviour
    std::uint32_t flags3 = 0;          // +112 impact behaviour
    std::int16_t trail_length = 0;     // +118
    std::uint8_t casing_speed = 0;     // +120
    std::uint32_t datum0_gfx = 0;      // +124
    std::uint32_t pickup_celglist = 0; // +128
    float zoom_max = 1.0f;             // +136
    std::uint8_t ammo_type = 0;        // +144 index into ammo_data / the ammo pool (0 = infinite)
    std::uint8_t rounds_per_shot = 0;  // +145
    std::int16_t clip_size = 0;        // +146
    std::uint8_t rumble = 0;           // +148
    float spread_growth = 0;           // +152 extra spread per consecutive shot of a burst
    // Animation scripts (0x06xxxxxx), 0 = none.
    std::uint32_t anim_idle = 0;         // +160
    std::uint32_t anim_reload = 0;       // +164
    std::uint32_t anim_reload_start = 0; // +168
    std::uint32_t anim_reload_end = 0;   // +172
    std::uint32_t anim_fire = 0;         // +176
    std::uint32_t anim_fire_alt = 0;     // +180
    std::uint32_t anim_aim = 0;          // +184
    std::uint32_t anim_aim_alt = 0;      // +188
    std::uint32_t anim_draw = 0;         // +192
    std::uint32_t anim_holster = 0;      // +196
    std::uint32_t anim_misc = 0;         // +212 (idle fidget)
    std::uint32_t anim_holster_alt = 0;  // +216
    std::uint32_t model_gfx = 0;         // +220 first-person weapon skin hash
    std::array<float, 3> gun_offset{};      // +224 hip offset (SP)
    std::array<float, 3> gun_offset_aim{};  // +236 aiming offset (SP) / MP hip offset

    bool has(std::uint32_t flag1) const { return (flags1 & flag1) != 0; }
};

// F1 (WeaponDef::flags1) bits, docs/spec-weapons.md 4.1.
namespace wf1 {
constexpr std::uint32_t kFireUnderwater = 0x2, kLaserSight = 0x8, kClipVariant = 0x10, kHideAmmo = 0x20,
                        kScope = 0x40, kShellReload = 0x100, kAltReloadAnim = 0x200, kHoldFire = 0x400,
                        kSwapDatum = 0x800, kDryFireAnim = 0x1000, kNoSmoke = 0x2000, kDetonatable = 0x4000,
                        kNoRetrigger = 0x8000, kLauncherOrigin = 0x10000, kLockOn = 0x20000,
                        kAlternateHands = 0x40000, kRandomAltFire = 0x80000, kRepeatFire = 0x100000,
                        kAccurateAiming = 0x400000;
}
// F2 (flags2) bits, 4.2.
namespace wf2 {
constexpr std::uint32_t kGuided = 0x4, kGravity = 0x8, kTracer = 0x10, kEjectCasing = 0x40, kTaserBeam = 0x80,
                        kLaserBeam = 0x100, kQuarterSpeed = 0x200, kHoldStates = 0x400, kSolidWater = 0x800,
                        kTracerAll = 0x1000, kLight = 0x2000, kWeaponLock = 0x4000, kSwoosh = 0x10000;
// kSolidWater (was kIgnoreWorld): water surfaces stay solid for the sweep (ray mask 520, not 522);
// it never skips the world — grenades carrying it still explode on walls. Water itself is unmodelled.
}
// F3 (flags3) bits, 4.3.
namespace wf3 {
constexpr std::uint32_t kPlayerEffect = 0x2, kExplodes = 0x4, kRicochet = 0x10, kGrapple = 0x40, kBounce = 0x80,
                        kSticky = 0x100, kTripbomb = 0x80000;
}

struct AmmoDef {
    std::int16_t start = 0;           // +0 default pickup grant
    std::int16_t max = 0;             // +2 pool limit
    std::uint32_t casing_gfx = 0;     // +4
    std::uint32_t name_label = 0;     // +8
};

// One row of ACTION.ELF's `EffectInfo` (23 x 136 bytes, indexed by a surface's material & 0x3F; rows >= 23 clamp
// to 22): what a bullet impact spawns and how projectiles bounce off it (Effect_Bullet, Bullet_CollisionHandler).
struct SurfaceEffect {
    std::string name;                 // +0 name pointer ("CARPET", "METALTHIN", ...)
    std::uint32_t decal_gfx = 0;      // +4 bullet hole decal (0 = none)
    std::uint32_t puff_gfx[2] = {};   // +8, +12 smoke / dust sprites
    std::uint32_t spark_gfx = 0;      // +16 sparks
    std::uint16_t impact_sound = 0;   // +20 SFX id (SFX_IMPACT_*)
    std::uint16_t ricochet_sound = 0; // +24 SFX id
    std::uint8_t ricochet_prob = 0;   // +26 Effect_RicochetProb: Rand(prob) test
    float restitution = 0;            // +28 speed kept by a bouncing projectile
};

class WeaponTable {
public:
    static constexpr int kWeaponCount = 115;
    static constexpr int kAmmoCount = 34;

    // Runs `_GLOBAL_$I$weapon_data` on an R5900 interpreter over the ELF image and decodes weapon_data, reads
    // ammo_data, BestWeapon and the Upgraded* tables. Throws FormatError when the initializer uses an
    // instruction the interpreter does not know.
    static WeaponTable from_elf(const Elf32& action_elf);

    const WeaponDef& weapon(int id) const { return weapons_.at(std::size_t(id)); }
    const AmmoDef& ammo(int index) const { return ammo_.at(std::size_t(index)); }
    const std::vector<WeaponDef>& weapons() const { return weapons_; }
    // BestWeapon (0x2BEDE0): preference order for Player_GetBestWeapon / Player_IsBetterWeapon.
    const std::vector<std::uint16_t>& best_weapons() const { return best_; }
    const SurfaceEffect& surface(int material) const { return surfaces_.at(std::size_t(material > 22 ? 22 : material)); }

    // Upgrade_Weapon (0x1A8F30) with every upgrade byte 0 (a fresh game): base id -> variant.
    int upgrade(int base_id) const;
    // Upgrade_Weapon for an upgrade level 0..3 of the tables (handguns, snipers, silenced snipers, dart guns,
    // tasers, lasers, PDAs); other ids return `base_id`.
    int upgrade(int base_id, unsigned level) const;

private:
    std::vector<WeaponDef> weapons_;
    std::vector<AmmoDef> ammo_;
    std::vector<std::uint16_t> best_;
    std::vector<SurfaceEffect> surfaces_;
    struct Upgrade {
        int base;
        std::array<std::uint32_t, 4> to;
    };
    std::vector<Upgrade> upgrades_;
};

}  // namespace nf

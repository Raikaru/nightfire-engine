#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "core/math.hpp"
#include "driving/attributes.hpp"

namespace nf::driving {

// Vehicle weapons and damage for the driving missions, after the SWeaponManager family
// (FirePrimary/FireSecondary/FireSpecial/FireEMP/SelectSecondary/GetNextSecondaryType/
// FindSortedSecondary/CalcWeaponPriorities/HandleClipUpdate) and PBondCar_ApplyDamage/
// GetDamageZones/SetVisualDamage/IsTyreShredded, driven by the pvehicle attributes:
//
//   HAS_MACHINEGUNS/HAS_MISSLES(sic)/HAS_ROCKETS/HAS_TORPEDOS/HAS_CANNONS/HAS_TURRET,
//   MISSILES_FIRED_SIMULTANEOUSLY, SECONDARY_TYPE (the AI's preferred target car, e.g. the
//   Paris helicopter's "vanquish"), BULLET_STREAK_TYPE (tracer look), TYRE_DAMAGE_POINTS
//   (30 in default.atr: hits before a tyre blows, cf. the "BLOWOUT" mission event).
//
// Gadgets (drivecfg.def GAMEACTION_FIREGADGET = L1, FIRESECONDARY = R1, TOGGLESECONDARY =
// R2/D-pad) and pickups (EPowerUpAmmo/Health/Shield/Invulnerable + smackable PowerUp*
// articles): missiles, rockets, torpedoes (subs), cannon shells, mines, machine guns,
// smoke screen, oil slick, EMP burst, rocket boost, shield. Exact ammo counts, cooldowns and
// damage numbers below are tuned stand-ins [INFERENCE]; banks/SFX names are the originals
// (SFX_HMachGun1, SFX_RocketTrans, SFX_UMissileTrans, SFX_htorpedofire, SFX_BoosterFire...).
// Audio stays outside: update() emits SoundEvents the app turns into DrivingAudio voices.

enum class SecondaryKind : std::uint8_t { None, Missiles, Rockets, Torpedoes, Cannon, Mines };
enum class GadgetKind : std::uint8_t { None, Smoke, Oil, Emp, Boost, Shield, Mine };

// The weapon fit of one vehicle, from its (already overlaid) attributes.
struct WeaponSpec {
    bool machine_guns = false;
    bool missiles = false;
    bool rockets = false;
    bool torpedoes = false;
    bool cannons = false;
    bool turret = false;
    int simultaneous = 1;        // MISSILES_FIRED_SIMULTANEOUSLY
    int bullet_streak = 0;      // BULLET_STREAK_TYPE
    std::string target_car;      // SECONDARY_TYPE: preferred AI target

    static WeaponSpec load(const Attributes& a);
    bool has_secondary(SecondaryKind k) const;
};

struct SoundEvent {
    std::string name;  // bank sound, e.g. "SFX_RocketTrans" (role Generic)
    Vec3 pos{};
    float volume = 1.0f;
};

struct Projectile {
    Vec3 pos{}, vel{};
    float life = 0;        // seconds left
    float damage = 0;
    float blast = 0;       // splash radius (0 = direct hit only)
    bool from_player = false;
    bool homing = false;
    SecondaryKind kind = SecondaryKind::None;
};

// A lingering hazard: smoke cloud (blinds AI: accuracy/range collapse while inside,
// AIGroundVehicle_GetBeenInSmoke), oil slick (tyre grip collapse, GetBeenInOil) or an
// armed mine waiting for proximity.
struct HazardZone {
    Vec3 pos{};
    float radius = 0;
    float life = 0;        // seconds left (mines: armed lifetime)
    GadgetKind kind = GadgetKind::None;
    bool from_player = false;
    float strength = 1.0f;
};

struct DamageZone {
    float health = 100.0f;  // EAGL::DamageZones entries (body, leftfrontwheel, ...)
};

// One vehicle's weapons, ammo, gadgets and damage state.
class WeaponSet {
public:
    explicit WeaponSet(const WeaponSpec& spec, float max_health = 100.0f);

    const WeaponSpec& spec() const { return spec_; }
    bool alive() const { return body_.health > 0; }

    // Secondary selection (SelectSecondary/GetNext/GetPreviousSecondaryType).
    std::vector<SecondaryKind> secondaries() const;
    SecondaryKind selected() const { return selected_; }
    void select_next();
    void select_prev();
    int ammo(SecondaryKind k) const;
    int gadget_count(GadgetKind k) const;

    // Pickups (EPowerUpAmmo/Health/Shield...): rockets/missiles/torpedoes/cannon/mines ammo,
    // gadget charges, repairs, shield/invulnerability timers.
    void add_ammo(SecondaryKind k, int n);
    void add_gadget(GadgetKind k);
    void repair(float amount);
    void shield(float seconds);

    // Fire lines. primaria aims along `forward` with spread; AI callers pass their accuracy.
    // Returns false on dry fire (SFX_DryFire equivalent: caller plays the click).
    bool fire_primary(float now, const Vec3& muzzle, const Vec3& forward, float spread,
                      std::vector<SoundEvent>& sfx);
    bool fire_secondary(float now, const Vec3& muzzle, const Vec3& forward, bool from_player,
                        std::vector<Projectile>& out, std::vector<SoundEvent>& sfx);
    // L1 gadget line: mines drop behind, smoke/oil lay a zone at `at`, emp bursts around the
    // car, boost/shield are self effects reported through boost_now()/shielded().
    bool fire_gadget(float now, GadgetKind k, const Vec3& at, const Vec3& forward, bool from_player,
                     std::vector<HazardZone>& zones, std::vector<SoundEvent>& sfx);

    // Timed effects.
    bool boost_now() const { return boost_timer_ > 0; }  // PBondCar_EnableRocketBoost window
    bool shielded() const { return shield_timer_ > 0 || invuln_timer_ > 0; }
    void trigger_boost(float seconds) { boost_timer_ = seconds; }
    void emp_hit(float seconds);  // AIGroundVehicle_DisableSteering equivalent effect on this car
    bool emp_held() const { return emp_timer_ > 0; }

    // Damage (PBondCar_ApplyDamage): zone 0 = body, 1..4 = wheels FL/FR/RL/RR. Returns true when
    // this hit kills. Blown tyres (IsTyreShredded) drag and pull; the renderer reads visual().
    bool apply_damage(float amount, int zone);
    bool tyre_blown(int wheel) const;
    float visual() const { return visual_; }  // 0 = pristine .. 1 = wreck (SetVisualDamage)
    float health() const { return body_.health; }
    float max_health() const { return max_health_; }
    void update(float dt);

    // Hazard exposure, evaluated by the mission for cars inside zones.
    bool in_smoke = false;  // GetBeenInSmoke
    bool in_oil = false;    // GetBeenInOil

private:
    WeaponSpec spec_;
    float max_health_ = 100.0f;
    DamageZone body_;
    std::array<DamageZone, 4> wheels_{};
    std::array<int, 4> tyre_points_{};  // hits until blowout (TYRE_DAMAGE_POINTS)
    float visual_ = 0;
    SecondaryKind selected_ = SecondaryKind::None;
    std::array<int, 6> ammo_{};         // indexed by SecondaryKind
    std::array<int, 7> gadgets_{};      // indexed by GadgetKind
    float primary_cooldown_ = 0, secondary_cooldown_ = 0, gadget_cooldown_ = 0;
    float clock_ = 0;
    float boost_timer_ = 0, shield_timer_ = 0, invuln_timer_ = 0, emp_timer_ = 0;
};

}  // namespace nf::driving

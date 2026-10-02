#pragma once

#include <cstdint>
#include <string_view>

#include "core/math.hpp"

namespace nf {

// Player_HandlePain's damage `type` argument (0 bullet/explosion, 1-4 scripted hurt volumes, 5-7 environment).
enum class DamageType : std::uint8_t { Bullet = 0, HurtVolume = 1, Environment = 5, Fall = 6, Drown = 7 };

// Body part ids Player_HandlePain looks at (skeleton bones): -1 = no location (explosions), 5 = head.
namespace bodypart {
constexpr int kNone = -1, kHead = 5, kTorso = 2, kLowerLimb = 50;
}

// One damaging event as a victim sees it (HITDATA mirrored on the victim: +8 damage, +82 body part).
struct HitInfo {
    float damage = 0;
    DamageType type = DamageType::Bullet;
    int attacker = -1;       // shooter id: 0..3 player slot, >= 4 bots / drones, -1 environment
    int weapon = 0;          // weapon_data id
    Vec3 point{};            // where it hit
    Vec3 direction{};        // unit travel direction of the projectile (zero for explosions); the pain
                             // indicator reads this (HITDATA+48, proven by disassembly: normalized ray dir)
    int part = bodypart::kNone;
};

// A non-player combatant (bot, drone) that bullets and explosions can hit. The hit volume is a vertical
// capsule: the segment centre() +- (0, half_height(), 0) swept by radius().
class DamageTarget {
public:
    virtual ~DamageTarget() = default;
    virtual Vec3 center() const = 0;
    virtual float radius() const = 0;
    virtual float half_height() const = 0;
    virtual bool alive() const = 0;
    // The damage is already the raw weapon damage (bullet: def+12, explosion: distance scaled); modifiers that
    // depend on the victim (armour, head multipliers, difficulty) belong to the implementor.
    virtual void hurt(const HitInfo& hit) = 0;
    // Stun-grenade blast (Bullet_DoTrails message 24): bots enter ImpactStunGrenade; default ignores.
    virtual void stun() {}
};

// Game-mode rules that decide whether damage counts (team / friendly fire) and what a kill triggers.
class MatchRules {
public:
    virtual ~MatchRules() = default;
    // Called before damaging a player; false skips the damage (friendly fire off). attacker -1 = environment.
    virtual bool hit_applies(int attacker, int victim) = 0;
    // Side-effect-free team test for gates that must not record hits (auto-aim): MP_areObjectsOnSameTeam.
    virtual bool teammates(int, int) const { return false; }
    // Assassination: the assassin's hits on the target are lethal (Player_DealWithObjHit sets the bullet
    // damage to the victim's health). The caller applies it pre-armour, exactly like the original.
    virtual bool assassin_lethal(int, int, int) const { return false; }
    // Once, when the victim's health reached 0 (Player_CheckForDeath). attacker -1 = suicide / environment.
    virtual void player_killed(int victim, int attacker, int weapon_id) = 0;
    virtual void environment_kill(int victim) = 0;
};

enum class GameMode : std::uint8_t { SinglePlayer, Multiplayer };

// Tunables of Player_HandlePain: TuningVars.txt Plr_DMod_* (ELF .sdata defaults 0x30CC40..0x30CC58 apply
// when a key is missing) and the MP option flags (dword_2A4968 location damage, dword_2A495C rapid mode).
struct DamageTuning {
    GameMode mode = GameMode::Multiplayer;
    int difficulty = 2;              // dword_2A3790: 1 easy, 2 normal, 3 hard, 4 hardest
    float easy = 0.75f, normal = 1.0f, hard = 1.25f;                            // Plr_DMod_Easy / Normal / Hard
    float multi = 4.0f, head = 4.0f, lower_limb = 0.8f, upper_limb = 0.8f;      // Plr_DMod_Multi / Head / LowerLimb / UpperLimb
    bool location_damage = true;
    bool rapid = false;
    bool enabled = true;             // false: Player_HandlePain returns at once (cheat byte_26FCEF, or a team match that is not in play)

    // Applies [GLOBAL] then `section` (a level name such as "MULTIPLAYER") of a TuningVars.txt.
    void load(std::string_view tuning_vars_txt, std::string_view section);
};
// Player_AutoAim / Check_AutoAim tunables (ELF .sdata Autoaim_*; TuningVars.txt [GLOBAL] then the level
// section override them). Defaults are the MULTIPLAYER-tuned values (docs/spec-weapons.md 9).
struct AutoaimTuning {
    float range = 25.0f;    // Autoaim_Range: scan starts here, best distance only shrinks
    float angle_h = 0.12f;  // Autoaim_Angle_H: yaw half-window, scaled by the target weight
    float angle_v = 0.22f;  // Autoaim_Angle_V: pitch half-window, scaled by the target weight
    float lock_mul = 1.4f;  // Autoaim_LockOnMul: window of the already-locked target
    float easy = 1.9f;      // Autoaim_EasyMul: weight at difficulty 1 (forced in MP)
    float normal = 1.0f;    // Autoaim_NormalMul: difficulty 2 (and anything but 1/3/4)
    float hard = 0.0f;      // Autoaim_HardMul: difficulty 3-4 disables auto-aim
    bool enabled = true;    // PlayerSetting+1 (SP) / +2 (MP); the port has no options menu yet, default on

    // Applies [GLOBAL] then `section` of a TuningVars.txt (same section pass as DamageTuning::load).
    static AutoaimTuning load(std::string_view tuning_vars_txt, std::string_view section);
};

// Health and armour of a player (BLData+2196 / +2224) and the pain feedback fields.
struct Vitals {
    float health = 100.0f;
    float max_health = 100.0f;
    float armour = 0.0f;
    float flash = 0.0f;          // BLData+2236: damage flash, 1.0 on a hit
    std::uint8_t pain_dir = 0;   // BLData+2407: bit 1 hit from below, 2 above, 4 left, 8 right (12 = unspecific, 2 alone = fall)
    std::uint8_t pain_alpha = 0; // BLData+2408: red overlay alpha; += min(128, 21.33 * damage) per hit, the HUD fades it by FRAME_RATE_MUL per frame
    bool alive() const { return health > 0.0f; }
};

// Player_HandlePain: scales the incoming damage by mode / difficulty / body part, armour absorbs first (type 0
// only: hurt volumes, environment, falls and drowning types 1-7 ignore armour), the rest comes off health; a
// remainder under 1 point kills. Returns the scaled damage (0 when ignored); `health_damage`, when given, gets
// the part of it that armour did not absorb.
float apply_player_pain(Vitals& v, const DamageTuning& tuning, float damage, int part, DamageType type,
                        float* health_damage = nullptr);

}  // namespace nf

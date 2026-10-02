#pragma once

// BOTWEAP_* / BOTSTATE_*WeaponChangeChoice (docs/spec-arena-ai.md Part 2A §5): the inventory a bot carries
// (`BOT_vars+0x140` weapon[id] {rounds in clip, has-weapon} and `+0x698` ammo reserve per ammo type) and the
// weapon-selection rules. Weapon numbers come from the decoded `weapon_data`/`ammo_data` (WeaponTable):
//   +2 base, +5 alt (non-zero = shares the parent's clip when the ammo type matches), +6 category (4 = heavy /
//   explosive), +8 blast radius (the "minimum useful range" of the bot code), +0x68 flags (0x10 clip variant,
//   0x100 shell reload), +0x90 ammo type, +0x92 clip size, +0xdc model hash (resource that must be loaded).

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

#include "assets/weapon_data.hpp"
#include "game/bot_data.hpp"

namespace nf::bots {

namespace weap {
constexpr int kFists = 1;
constexpr int kAlwaysPreferred = 6;
constexpr int kDetonator = 0x3b;      // defenders in Protection/Demolition
constexpr int kOddjobHat = 0x45;
constexpr int kMaxId = 0x52;
constexpr int kSlots = 0x55;          // BOT_vars weapon[] records
constexpr int kAmmoTypes = 0x21;
}  // namespace weap

// One candidate of the held+loaded list: BOTWEAP_listHeldLoadedWeapons packs (id << 8) | score; score 0xff = not
// held / not loaded. Lower score = better (154 + rank in the ranking table, fists +1).
struct WeaponScore {
    int id;
    std::uint8_t score;
};

// Everything BOTSTATE_combatWeaponChangeChoice reads besides the inventory.
struct WeaponChoiceContext {
    bool opponent_alerted = false;   // has an opponent and Drone+0x228 & 4
    float opponent_distance = 0;     // Drone+0x1a0
    bool opponent_is_missile = false;// BOTSTATE_opponentIsMissile: attacking the scenario objective
    Personality personality = Personality::None;
    std::uint8_t weapon_preference = 0;   // stat +0x0a
    bool defender = false;           // protection team 0 / demolition team 1
    bool opponent_is_active_objective = false;  // the objective object the detonator may be used on
};

class BotArmoury {
public:
    // `resource_loaded(model_hash)` answers weapon_data+0xdc's hashtable lookup ("skin is in the loaded level").
    using LoadedFn = std::function<bool(std::uint32_t)>;
    BotArmoury(const WeaponTable& table, LoadedFn resource_loaded = {});

    // BOTWEAP_InitWeapon: clears everything, fists x999 (id 1), the start weapon with two clips, detonator for
    // defenders, Oddjob's hat, then CheckWeaponsLoaded. `start_weapon` is `startweap` = weapon_data[set slot 0].base.
    void init(int start_weapon, bool defender, int character);

    const WeaponTable& table() const { return *table_; }
    int current() const { return current_; }
    int start_weapon() const { return start_; }
    void set_current(int id) { current_ = id; }

    // ---- inventory ----
    bool has_weapon(int id) const;                                 // BOTWEAP_hasWeapon
    bool equip_weapon(int id, int amount);                         // BOTWEAP_EquipWeapon
    bool equip_ammo(int weapon_id, int amount);                    // BOTWEAP_EquipAmmo
    int rounds_in_clip(int id) const;                              // clip counter of the clip owner of `id`
    int reserve(int id) const;                                     // ammo pool of the weapon's ammo type
    int ammo_amount(int id) const;                                 // BOTWEAP_getWeaponAmmoAmount (clip + reserve)
    int clip_size(int id) const;                                   // BOTWEAP_getWeaponClipSize
    bool ammo_in_gun(int id) const;                                // BOTWEAP_AmmoInGun (start weapon: always)
    bool weapon_has_ammo(int id) const;                            // BOTWEAP_WeaponHasAmmo
    bool has_loaded_weapon(int id) const;                          // BOTWEAP_hasLoadedWeapon
    // BOTWEAP_ReloadAmmoType(dryRun): false = nothing to load. On success the current weapon's clip/reserve
    // change (unless dry_run); out params mirror Drone+0xbbc / +0xbbe.
    bool reload(bool dry_run);
    // BOTWEAP_decrRounds: current weapon's clip -= n (floor 0). Returns false when the clip was already empty
    // and the current weapon is not the (infinite) start weapon. `hat_thrown` set when weapon 0x45 was fired.
    bool decrement_rounds(int n, bool* hat_thrown = nullptr);
    // One more round in `id`'s clip up to the clip size (Oddjob's hat regrowing, BotGlobal).
    bool add_round(int id);
    // BOTWEAP_changeWeapon: succeeds only when held; sets the current weapon.
    bool change_weapon(int id);
    // Drone+0xbbc / +0xbbe: the rounds left in the gun / reserve as the firing code sees them. Set by
    // change_weapon and reload, decremented by decrement_rounds (the start weapon's clip refills to full size
    // without touching the reserve: it is infinite).
    int clip_mirror() const { return clip_mirror_; }
    int reserve_mirror() const { return reserve_mirror_; }

    // ---- selection ----
    std::vector<WeaponScore> list_held_loaded(std::uint8_t weapon_preference, bool preferred_only,
                                              int* held_count = nullptr) const;   // BOTWEAP_listHeldLoadedWeapons
    // BOTWEAP_hasLoadedExplosiveForRange: first held+loaded of {2d,2c,2a,33,2b,2e,2f} whose blast radius < dist.
    int loaded_explosive_for_range(float dist) const;
    // BOTWEAP_tooCloseForWeapon(id, dist): the opponent is inside weapon `id`'s blast radius while the bot holds
    // another loaded non-fist non-heavy weapon (Berserkers and objective attacks are never "too close").
    bool too_close_for_weapon(int id, float dist, Personality p, bool opponent_is_missile) const;
    // BOTSTATE_combatWeaponChangeChoice: weapon id to switch to, 0 = keep.
    int combat_choice(const WeaponChoiceContext& ctx) const;

    // ---- static rules ----
    static bool is_valid_weapon(int id);                           // BOTWEAP_isValidWeapon
    static int weapon_anim_set(int id);                            // BOTWEAP_getWeaponAnimSet
    static bool punch_is_better_if_close(int id);                  // BOTWEAP_punchIsBetterIfClose
    static bool reload_anim_for_weapon(int id);                    // BOTWEAP_reloadAnimForWeapon
    static bool is_preferred(std::uint8_t preference, const WeaponDef& def, int id);   // BOTSTATE_isPreferredWeapon
    // Position of `id` in the 60-entry ranking table @0x2f3e10 (60 = not listed).
    static int rank_of(int id);
    static const std::array<std::uint16_t, 60>& ranking_table();

private:
    int clip_owner(int id) const;
    bool loaded_flag(int id) const;

    const WeaponTable* table_;
    LoadedFn loaded_;
    int start_ = 6;
    int current_ = 1;
    int clip_mirror_ = 0, reserve_mirror_ = 0;
    std::array<std::uint16_t, weap::kAmmoTypes> ammo_{};
    struct Record {
        std::uint16_t rounds = 0;
        bool has = false;
    };
    std::array<Record, weap::kSlots> rec_{};
};

}  // namespace nf::bots

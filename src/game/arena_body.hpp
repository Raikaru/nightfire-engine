#pragma once

#include "core/math.hpp"

namespace nf {

// MP_EquipPlayer's inputs: what a participant is (re)spawned with.
struct MpLoadout {
    float health = 100.0f;    // Player_SetHealth(100 + handicap bonus), armour is zeroed
    int weapon_set = 0;       // MPSettings+0x1B4 (PickupMatrix row); slot 0 of the row is the starting weapon
    int start_weapon = 6;     // PickupMatrix[set][0] (before Upgrade_MPWeapon)
    bool grapple = false;     // MPSettings+0x1D0: weapon 0x50
    bool demolition_charge = false;   // weapon 0x3B for the attacking team of Demolition / Protection
};

// The body of a match participant: the object whose inventory, health and position the arena rules read and
// change. Humans are backed by WeaponSystem + Player, bots by the drone code (Pickup_Handler has separate
// bot branches, BOTWEAP_*). The arena never looks inside; it only asks these questions.
class ArenaBody {
public:
    virtual ~ArenaBody() = default;

    virtual Vec3 position() const = 0;   // obj+0x30
    virtual bool alive() const = 0;      // alive and in the play state (obj+0xF4 == 1 / bot not dying)

    // Pickup_Handler. give_weapon = Player_EquipWeapon / BOTWEAP_EquipWeapon (an owned weapon takes the rounds as
    // ammo); false when refused. give_ammo returns the rounds accepted (0 = pool full, pickup stays).
    // give_armour: Player: refused (false) when armour >= 50, else armour = 50; bot: +20 health, +amount armour.
    virtual bool give_weapon(int weapon_id, int rounds) = 0;
    virtual int give_ammo(int weapon_id, int rounds) = 0;
    virtual bool give_armour(float amount) = 0;

    // Player_Kill / bot health = -1 (GoldenEye strike, kill volumes).
    virtual void kill() = 0;
    // MP_ReSpawn: stand at `pos` facing `yaw` with the loadout of MP_EquipPlayer, full health, no armour.
    virtual void respawn(const Vec3& pos, float yaw, const MpLoadout& loadout) = 0;
    // The arena registered this participant's death (MP_PlayerKilled): a body that has a movement half puts it into the
    // death state here.
    virtual void died() {}
};

}  // namespace nf

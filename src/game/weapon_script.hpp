#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "game/weapons.hpp"

namespace nf {

// Scripted gameplay scenario for headless nfgame runs (`--script file`): pad presses and inventory / position
// commands keyed by logic frame, with `print` lines that report the weapon state. One command per line:
//
//   @<frame> hold <slot> <button> [frames]   button: fire reload aim mode gadget_next gadget_prev gun_next gun_prev
//                                            zoom_in zoom_out crouch jump; default 1 frame
//   @<frame> give <slot> <weapon> <rounds>   Player_EquipWeapon
//   @<frame> ammo <slot> <weapon> <rounds>   Player_EquipAmmo
//   @<frame> select <slot> <weapon>
//   @<frame> teleport <slot> x y z yaw_deg [pitch_deg]
//   @<frame> face <slot> yaw_deg [pitch_deg]
//   @<frame> health <slot> hp [armour]
//   @<frame> print [label]                   weapon / health line of every player, then pending impacts
//   @<frame> throw <slot> <weapon>           spawns the weapon's projectile at the eye along the view (WeaponSystem::fire)
//   '#' starts a comment.
class WeaponScript {
public:
    static WeaponScript parse(const std::string& path);

    // Prints the impacts / explosions / sounds of one tick (`--events`).
    static void dump_events(long frame, const WeaponEvents& events);

    long last_frame() const { return last_frame_; }
    // Runs at the start of logic frame `frame` (before World::tick): applies the commands due, ORs the held buttons
    // into `pads`.
    void apply(long frame, World& world, WeaponSystem& weapons, PadInputs& pads);

private:
    struct Command {
        long frame;
        std::string verb;
        std::vector<std::string> args;
    };
    struct Hold {
        long from, to;
        int slot;
        std::uint16_t buttons;
    };
    std::vector<Command> commands_;
    std::vector<Hold> holds_;
    long last_frame_ = 0;
};

}  // namespace nf

#pragma once

#include <array>
#include <cstdint>

#include "assets/elf.hpp"
#include "game/input.hpp"

namespace nf {

// psiInput_PollDevices leaves the four sticks in tSlot after a dead-zone remap; everything above the
// libpad layer (psiInput_MapInputs, Input_Update, the player code) only ever sees these values.
// `PadState` sticks in `World::tick` are expected in that form. Raw device samples (keyboard, SDL
// gamepad) go through compensate_sticks first; recorded oracle traces already contain them.
PadState compensate_sticks(PadState raw);

// The PS2 pad word the game keeps at tSlot+0x122 (Sony libpad layout, active-high: Up 0x1000,
// Cross 0x40, ...). PadState::buttons uses the other byte order (see input.hpp).
std::uint16_t sony_pad_word(std::uint16_t buttons);
std::uint16_t buttons_from_sony_pad_word(std::uint16_t word);

// Indices into PlayerSetting's per-player action arrays (GameActions_tag), as used by Input_Action /
// Input_Actionf. Only actions whose meaning the movement code pins down are named.
enum Action : int {
    kActTurn = 0,        // yaw stick (Player_Move)
    kActStrafe = 1,
    kActForward = 2,
    kActLookX = 3,       // aim/zoom look (Player_Aiming, zoomed branch)
    kActLookY = 4,
    kActLookPitch = 5,   // pitch stick (Player_Aiming, unzoomed branch)
    kActJump = 7,        // Player_HandleJump
    kActCrouch = 8,
    kActUse = 14,        // Cross: Player_Activate (doors, creep walls) / reload
    kActPause = 30,      // Player_Update opens the pause menu
    kActionCount = 40,
};

// Per-player options read from PlayerSetting (the defaults of a fresh save; also what the oracle
// savestate holds).
struct PlayerSettings {
    bool invert_look = false;    // PlayerSetting[0]: Input_Update negates the two aim axes
    bool crouch_toggle = true;   // PlayerSetting[4]: Player_SSCrouch / Player_HandleJump
    bool auto_center = true;     // PlayerSetting[7]: Player_SSWalk recentres pitch when walking
    bool health_fade = false;    // PlayerSetting[0xB]: Player_Update lets the health bar's damage flash fade (default 0: stays lit)
    // Controller style (PlayerSetting+0xE). Only style 7, the default, is mapped.
};

// gAnalogStickMappingFunction: 129-entry response curve used by MapAnalogStick.
struct InputTables {
    std::array<std::uint8_t, 129> stick_curve{};
    static InputTables from_elf(const Elf32& action_elf);
};

// One player's PlayerSetting action state: psiInput_MapInputs (style 7) + Input_Update.
class ActionInput {
public:
    // Input_Update for this player: maps the pad, shapes the sticks (StickCompensation2/3), clamps
    // to [-1, 1] and derives the flag bytes (held = 1, newly pressed = 4, auto-repeat = 8).
    void update(const PadState& pad, const InputTables& tables, const PlayerSettings& settings);

    float actionf(int action) const { return value_[action]; }            // Input_Actionf
    bool held(int action) const { return (flags_[action] & 1) != 0; }      // Input_Action(.., 1)
    bool pressed(int action) const { return (flags_[action] & 4) != 0; }   // Input_Action(.., 4)

    const std::array<float, kActionCount>& values() const { return value_; }
    const std::array<std::uint8_t, kActionCount>& flags() const { return flags_; }

private:
    std::array<float, kActionCount> value_{};
    std::array<std::uint8_t, kActionCount> flags_{};
    std::array<std::uint16_t, kActionCount> hold_{};
};

}  // namespace nf

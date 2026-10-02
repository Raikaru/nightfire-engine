#pragma once

namespace nf {

// Player_Aiming state while the weapon is raised (collbody+0x60 bit 1): the aim cursor inside the aim box
// (BLData+0x118 / +0x11C), the ramped turn speeds at its edge (+0x8F4 / +0x8F8) and the scope look ramps
// (+0x8EC / +0x8F0).
struct AimState {
    float cursor_x = 0, cursor_y = 0;
    float turn_x = 0, turn_y = 0;
    float scope_x = 0, scope_y = 0;
};

// Half extents of the aim box (Player_Aiming): the cursor never leaves +-0.296 horizontally, +-0.4 vertically.
constexpr float kAimBoxX = 0.296f;
constexpr float kAimBoxY = 0.4f;

}  // namespace nf

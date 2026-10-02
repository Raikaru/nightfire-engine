#pragma once

#include <cstdint>

namespace nf {

// Player_ZeroG state that is not shared with the walking code (BLData +0xD0).
struct ZeroGState {
    std::int16_t level_timer = 0;   // BL+0xD0: frames left of the roll correction Cross starts (1.5 s)
};

// Turn rate of the free-floating body: 0.4 * pi radians per second at full stick.
inline constexpr float kZeroGTurnRate = 1.2566371f;

}  // namespace nf

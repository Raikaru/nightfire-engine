#pragma once

namespace nf {

// The viewer's vertical field of view Camera_CalcViewAngles(0x3F860A92) restores when scan mode ends (60 degrees).
inline constexpr float kDefaultFov = 1.0471976f;

// Player_SSScanMode state that lives outside the shared player fields.
struct ScanState {
    float fov = kDefaultFov;   // viewer+0x118: the D-pad zooms it (0.2 .. 3.14) while the zoom bit is set
};

}  // namespace nf

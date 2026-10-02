#pragma once

#include <cstdint>

#include "core/math.hpp"

namespace nf::audio {

// Integer spatialisation of the IOP's SFX library (SFX.IRX: PS2_SFXCalculate3D, PS2_SFXSetup2D/3D,
// ES_GetDist). The EE sends every position as int(float * 1000) (SFXStart3D, SFXSetupListener), so
// the arithmetic, including its truncations, is reproduced on those integers.
struct SfxPoint {
    int x = 0, y = 0, z = 0;
};

SfxPoint to_sfx_point(const Vec3& v);  // int(v * 1000), truncating

// SFXSetupListener arguments. dir/up/norm are unit vectors scaled by 1000.
struct ListenerFrame {
    SfxPoint pos, vel, dir{0, 0, 1000}, up{0, 1000, 0}, norm{1000, 0, 0};
};

// PS2_SquareRoot: ten Newton steps from 1. Exact for small arguments, an over-estimate for large ones
// (arguments above roughly 90000), which is why far sounds fade a little early.
int ps2_sqrt(int v);

// Channel volumes 0..100 (front left, front right, rear).
struct ChannelVolumes {
    int left = 0, right = 0, rear = 0;
};

// Distance from the listener to `p` in game units (ES_GetDist).
int distance_units(const ListenerFrame& l, const SfxPoint& p);

// PS2_SFXCalculate3D: attenuation between inner and outer radius (game units), squared, then split
// between the ears by the direction to the source; sound behind the listener shifts to the rear channel.
// In mono mode the three channels are averaged.
ChannelVolumes calculate_3d(const ListenerFrame& listener, const SfxPoint& source, int inner_radius, int outer_radius,
                            bool stereo);

// PS2_SFXSetup3D for the "head locked" tracking type: constant level, no direction.
ChannelVolumes head_locked(int volume);

// PS2_SFXSetup3D: scale the pan result by the effect's combined volume (0..100).
ChannelVolumes apply_volume(const ChannelVolumes& pan, int volume);

// PS2_SFXSetup2D: pan -100..100 with volume 0..100.
ChannelVolumes pan_2d(int pan, int volume, bool stereo);

}  // namespace nf::audio

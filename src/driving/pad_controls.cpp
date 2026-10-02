#include "driving/pad_controls.hpp"

namespace nf::driving {

float pad_stick_axis(std::uint8_t raw) {
    // 0.0078431377 = 2/255 (sub_169218); chop bounds are the floats 0xBE99999A / 0x3E99999A.
    const float v = static_cast<float>(raw) * 0.0078431377f - 1.0f;
    return (v > -0.3f && v < 0.3f) ? 0.0f : v;
}

PadActions actions_from_pad(const PadHistory& pad) {
    PadActions a;
    a.drive.steer = pad_stick_axis(pad.now.lx);
    a.drive.gas = pad.now.held(kPadCross) ? 1.0f : 0.0f;
    a.drive.brake = pad.now.held(kPadSquare) ? 1.0f : 0.0f;
    a.drive.handbrake = pad.now.held(kPadCircle);
    a.change_camera = pad.pressed(kPadTriangle);
    a.look_back = pad.now.held(kPadL2);
    return a;
}

}  // namespace nf::driving

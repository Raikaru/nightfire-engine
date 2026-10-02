#pragma once

#include <cstdint>

#include "game/input.hpp"

namespace nf::driving {

// What the driving physics reads from the controller each tick (the values PBondCar::GetControllerInput,
// sub_18ADD8, receives through its action queue). All are the raw action values; the dead zone, the
// throttle ramp and the steering slew are applied by Vehicle::step.
struct DriveInput {
    float steer = 0;         // GAMEACTION_STEER: left stick X after the device chop, -1 left .. +1 right
    float gas = 0;           // GAMEACTION_GAS: cross pressure 0..1
    float brake = 0;         // GAMEACTION_BRAKE: square pressure 0..1
    bool handbrake = false;  // GAMEACTION_HANDBRAKE: circle held (pressure above 0.1)
};

// The non-physics driving actions of `control/drivecfg.def`.
struct PadActions {
    DriveInput drive;
    bool change_camera = false;  // GAMEACTION_CHANGECAMERA: triangle, on the press edge
    bool look_back = false;      // GAMEACTION_CAMLOOKBACK / ...RELEASE: L2 held
};

// Device layer of PS2PadDevice::PollDevice (sub_169288): a stick byte b becomes b * 2/255 - 1 (sub_169218)
// and values strictly inside (-0.3, 0.3) are zeroed (sub_167C48, bounds +-0.3 loaded in sub_169288).
float pad_stick_axis(std::uint8_t raw);

// The DualShock 2 reports button pressure as a byte per button and the original turns it into
// min(pressure / 192, 1) (sub_169248); GAS/BRAKE/HANDBRAKE are bound to those analogue face-button
// scalars (AButtonCross/Square/Circle). PadState only carries the digital bits, and the original maps a
// digital press to exactly 1.0, so a held button reads 1.0 here.
//
// Mapping of drivecfg.def: GAS = cross, BRAKE = square, HANDBRAKE = circle (press when the scalar
// crosses above 0.1), STEER = left stick X, CHANGECAMERA = triangle (digital press), CAMLOOKBACK = L2
// (digital press / release). GAMEACTION_STEERVERTICAL (left stick Y) is bound but feeds only the
// air-control input the ported physics never reads.
PadActions actions_from_pad(const PadHistory& pad);

}  // namespace nf::driving

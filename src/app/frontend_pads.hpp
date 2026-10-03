#pragma once

#include <array>
#include <cstdint>
#include <map>

#include "game/input.hpp"
#include "ui/frontend.hpp"
#include "ui/input_devices.hpp"

namespace nf::app {

// The front end's four controller slots fed from the PC devices (keyboard + mouse and every gamepad).
// Outside the multiplayer join flow every device drives slot 0. P_MPJOIN starts the flow: a device that is not
// yet in a slot claims the lowest free one with Cross (Enter), so keyboard and gamepads join in any order;
// the claims last until the main menu and are what the split-screen session plays with (slot_devices).
class FrontendPads {
public:
    // One 30 Hz sample: reads the devices (plus `injected` keyboard buttons, the --press replay), updates the
    // claims and the join page's controller presence, and returns the four slots' histories.
    const std::array<PadHistory, 4>& sample(Frontend& frontend, std::uint16_t injected_keyboard = 0);
    // Direct slot-0 input (plain --press tokens): bypasses the devices as the old single-pad replay did.
    const std::array<PadHistory, 4>& push_slot0(Frontend& frontend, std::uint16_t buttons);

    const std::array<SlotDevice, 4>& slot_devices() const { return claims_; }
    bool joining() const { return joining_; }

private:
    void set_joining(bool on);
    void publish(Frontend& frontend);

    std::array<SlotDevice, 4> claims_{};
    bool joining_ = false;
    std::array<PadHistory, 4> pads_{};
    std::uint16_t keyboard_prev_ = 0;
    std::map<SDL_JoystickID, std::uint16_t> pad_prev_;
};

}  // namespace nf::app

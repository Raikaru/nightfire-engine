#pragma once

#include <array>
#include <cstdint>

namespace nf {

// DualShock 2 buttons, bit order of the PS2 pad report (libpad), active-high here.
enum PadButton : std::uint16_t {
    kPadSelect = 1 << 0,
    kPadL3 = 1 << 1,
    kPadR3 = 1 << 2,
    kPadStart = 1 << 3,
    kPadUp = 1 << 4,
    kPadRight = 1 << 5,
    kPadDown = 1 << 6,
    kPadLeft = 1 << 7,
    kPadL2 = 1 << 8,
    kPadR2 = 1 << 9,
    kPadL1 = 1 << 10,
    kPadR1 = 1 << 11,
    kPadTriangle = 1 << 12,
    kPadCircle = 1 << 13,
    kPadCross = 1 << 14,
    kPadSquare = 1 << 15,
};

// One controller sample per 30 Hz logic frame, in the raw form the game reads, so recorded
// oracle inputs replay bit-exactly. Sticks: 0 = left/up, 0x80 = centre, 0xFF = right/down.
struct PadState {
    std::uint16_t buttons = 0;
    std::uint8_t rx = 0x80, ry = 0x80, lx = 0x80, ly = 0x80;

    bool held(PadButton b) const { return (buttons & b) != 0; }
};

// Edge detection across frames.
struct PadHistory {
    PadState now, prev;

    void push(const PadState& s) { prev = now, now = s; }
    bool pressed(PadButton b) const { return now.held(b) && !prev.held(b); }
    bool released(PadButton b) const { return !now.held(b) && prev.held(b); }
};

using PadInputs = std::array<PadState, 4>;

}  // namespace nf

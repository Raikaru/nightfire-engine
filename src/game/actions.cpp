#include "game/actions.hpp"

#include <algorithm>
#include <cmath>

namespace nf {

namespace {

// Sony libpad word bits (tSlot+0x122).
constexpr std::uint16_t kL2 = 0x1, kR2 = 0x2, kL1 = 0x4, kR1 = 0x8, kTriangle = 0x10, kCircle = 0x20,
                        kCross = 0x40, kSquare = 0x80, kSelect = 0x100, kStart = 0x800, kUp = 0x1000,
                        kRight = 0x2000, kDown = 0x4000, kLeft = 0x8000;

// (byte - 127) / 128, the stick scaling psiInput_MapInputs applies to every axis.
float axis(std::uint8_t b) { return (float(b) - 127.0f) * 0.0078125f; }

// MapAnalogStick: response curve lookup on a magnitude in [0, 1+].
float map_analog_stick(const InputTables& t, float magnitude) {
    int i = int(magnitude * 127.0f);
    const int sign = i < 0 ? -1 : 1;
    i = std::min(std::abs(i), 0x80);
    return float(sign * int(t.stick_curve[std::size_t(i)])) * 0.003921569f;
}

// StickCompensation2: radial remap of a stick pair through the response curve.
void stick_compensation_radial(const InputTables& t, float& x, float& y) {
    const float mag = std::sqrt(x * x + y * y);
    if (mag > 0.0002f || mag < -0.0002f) {
        const float mapped = map_analog_stick(t, mag);
        const float k = mapped / mag;
        x *= k;
        y *= k;
    }
}

// StickCompensation3: snap a stick pair to full deflection on an axis once it is near it.
void stick_compensation_snap(float& x, float& y) {
    constexpr float kSnap = 0.707107f;
    if (x >= -kSnap && x <= kSnap) {
        if (y <= -kSnap) y = -1.0f;
        else if (y >= kSnap) y = 1.0f;
    }
    if (y >= -kSnap && y <= kSnap) {
        if (x <= -kSnap) x = -1.0f;
        else if (x >= kSnap) x = 1.0f;
    }
}

std::uint8_t compensate_axis(std::uint8_t v) {
    float d = float(int(v) - 0x7F);
    if (d > 0.0f) d = d - 40.0f >= 0.0f ? (d - 40.0f) * 1.4597701f : 0.0f;
    else if (d < 0.0f) d = d + 40.0f > 0.0f ? 0.0f : (d + 40.0f) * 1.4597701f;
    return std::uint8_t(int(d) + 0x7F);
}

}  // namespace

PadState compensate_sticks(PadState raw) {
    raw.rx = compensate_axis(raw.rx);
    raw.ry = compensate_axis(raw.ry);
    raw.lx = compensate_axis(raw.lx);
    raw.ly = compensate_axis(raw.ly);
    return raw;
}

std::uint16_t sony_pad_word(std::uint16_t buttons) {
    return std::uint16_t((buttons << 8) | (buttons >> 8));
}

std::uint16_t buttons_from_sony_pad_word(std::uint16_t word) { return sony_pad_word(word); }

InputTables InputTables::from_elf(const Elf32& elf) {
    const auto sym = elf.symbol("gAnalogStickMappingFunction");
    if (!sym) throw FormatError("ACTION.ELF has no gAnalogStickMappingFunction");
    InputTables t;
    const Bytes b = elf.at(sym->value, std::uint32_t(t.stick_curve.size()));
    std::copy(b.begin(), b.end(), t.stick_curve.begin());
    return t;
}

void ActionInput::update(const PadState& pad, const InputTables& tables, const PlayerSettings& settings) {
    const std::uint16_t w = sony_pad_word(pad.buttons);
    const auto bits = [w](std::uint16_t mask) { return float(w & mask); };
    value_.fill(0.0f);

    // psiInput_MapInputs, controller style 7 (the default). Sticks: rx = tSlot+0x128, ry = +0x129,
    // lx = +0x12A, ly = +0x12B.
    value_[kActForward] = -axis(pad.ly);
    value_[kActStrafe] = -axis(pad.rx);
    value_[kActLookPitch] = -axis(pad.ry);
    value_[kActTurn] = axis(pad.lx);
    value_[19] = bits(kL1);
    value_[kActLookY] = -axis(pad.ly);
    value_[kActLookX] = axis(pad.lx);
    if (w & kUp) value_[6] = -1.0f;
    if (w & kDown) value_[6] = 1.0f;
    value_[kActJump] = bits(kTriangle);
    value_[kActCrouch] = bits(kL2);
    value_[9] = bits(kR1);
    value_[14] = bits(kCross);
    value_[12] = bits(kSquare);
    value_[16] = bits(kDown | kR2);
    value_[15] = bits(kUp);
    value_[11] = bits(kRight | kCircle);
    value_[10] = bits(kLeft);
    value_[13] = bits(kSelect);
    // Digital shadows of the buttons and sticks (common tail of psiInput_MapInputs).
    value_[26] = bits(kUp);
    value_[27] = bits(kDown);
    value_[28] = bits(kLeft);
    value_[29] = bits(kRight);
    if (axis(pad.lx) < -0.5f) value_[28] = 1.0f;
    if (axis(pad.lx) > 0.5f) value_[29] = 1.0f;
    if (axis(pad.ly) < -0.5f) value_[26] = 1.0f;
    if (axis(pad.ly) > 0.5f) value_[27] = 1.0f;
    value_[31] = bits(kCross);
    value_[32] = bits(kSquare);
    value_[33] = bits(kCircle);
    value_[30] = bits(kStart);
    value_[34] = bits(kTriangle);
    value_[25] = bits(kStart | kCross);
    if (axis(pad.lx) < -0.8f && axis(pad.rx) > 0.8f && (w & kUp)) value_[38] = 1.0f;

    // Input_Update: shape the sticks, honour the invert option.
    stick_compensation_radial(tables, value_[kActLookX], value_[kActLookY]);
    stick_compensation_radial(tables, value_[kActTurn], value_[kActLookPitch]);
    stick_compensation_snap(value_[kActStrafe], value_[kActForward]);
    if (settings.invert_look) {
        value_[kActLookY] = -value_[kActLookY];
        value_[kActLookPitch] = -value_[kActLookPitch];
    }

    for (int a = 0; a < kActionCount; ++a) {
        value_[a] = std::clamp(value_[a], -1.0f, 1.0f);
        std::uint8_t state = 0;
        if (value_[a] == 0.0f) {
            hold_[a] = 0;
        } else {
            state = flags_[a] == 0 ? 5 : 1;
            hold_[a] = std::uint16_t(std::min<int>(hold_[a] + 1, 0x7FFF));
            if (hold_[a] >= 0x2E) state = (hold_[a] & 7) != 0 ? state & ~8 : state | 8;
        }
        flags_[a] = state;
    }
}

}  // namespace nf

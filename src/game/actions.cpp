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

std::array<float, kActionCount> map_inputs(const PadState& p, int style, int player) {
    const std::uint16_t w = sony_pad_word(p.buttons);
    const auto bits = [w](std::uint16_t mask) { return float(w & mask); };
    std::array<float, kActionCount> v{};

    // psiInput_MapInputs (ACTION.ELF 0x10CD80): the per-style switch (jtbl_002E8350), translated instruction by
    // instruction from the decomp; `nfmips diff-input` checks every style against the original. Sticks: rx = tSlot+0x128,
    // ry = +0x129, lx = +0x12A, ly = +0x12B (the +0x148.. copies hold the same values after psiInput_PollDevices).
    switch (style) {
        case 0:   // NightFire
            v[2] = -axis(p.ly);
            v[1] = -axis(p.lx);
            v[5] = -axis(p.ry);
            v[0] = axis(p.rx);
            v[19] = bits(0x4);
            v[4] = -axis(p.ry);
            v[3] = axis(p.rx);
            if (w & 0x1000) v[6] = -1.0f;
            if (w & 0x4000) v[6] = 1.0f;
            v[7] = bits(0x10);
            v[8] = bits(0x40);
            v[9] = bits(0x8);
            v[14] = bits(0x82);
            v[12] = bits(0x20);
            v[16] = bits(0x2001);
            v[15] = bits(0x8000);
            v[11] = bits(0x1000);
            v[10] = bits(0x4000);
            v[13] = bits(0x100);
            break;
        case 1:   // Moonraker
            v[2] = -axis(p.ly);
            v[1] = -axis(p.lx);
            v[5] = -axis(p.ry);
            v[0] = axis(p.rx);
            v[19] = bits(0x4);
            v[4] = -axis(p.ry);
            v[3] = axis(p.rx);
            if (w & 0x1000) v[6] = -1.0f;
            if (w & 0x4000) v[6] = 1.0f;
            v[7] = bits(0x10);
            v[8] = bits(0x1);
            v[9] = bits(0x8);
            v[14] = bits(0x40);
            v[12] = bits(0x80);
            v[16] = bits(0x2002);
            v[15] = bits(0x8000);
            v[11] = bits(0x1000);
            v[10] = bits(0x4000);
            v[13] = bits(0x20);
            break;
        case 2:   // Octopussy: L1 makes the left stick strafe
            v[2] = -axis(p.ly);
            v[1] = -axis((w & 0x4) ? p.lx : p.rx);
            v[5] = -axis(p.ry);
            v[0] = axis(p.lx);
            v[19] = bits(0x4);
            v[4] = -axis(p.ry);
            v[3] = axis(p.rx);
            if (w & 0x1000) v[6] = -1.0f;
            if (w & 0x4000) v[6] = 1.0f;
            v[7] = bits(0x10);
            v[8] = bits(0x1);
            v[9] = bits(0x8);
            v[14] = bits(0x40);
            v[12] = bits(0x80);
            v[16] = bits(0x2002);
            v[15] = bits(0x8000);
            v[11] = bits(0x1000);
            v[10] = bits(0x4000);
            v[13] = bits(0x20);
            break;
        case 3:   // Goldfinger
            v[2] = -axis(p.ly);
            v[1] = -axis(p.lx);
            v[5] = -axis(p.ry);
            v[0] = axis(p.rx);
            v[19] = bits(0x4);
            v[4] = -axis(p.ry);
            v[3] = axis(p.rx);
            if (w & 0x1000) v[6] = -1.0f;
            if (w & 0x4000) v[6] = 1.0f;
            v[7] = bits(0x10);
            v[8] = bits(0x40);
            v[9] = bits(0x8);
            v[14] = bits(0x1);
            v[12] = bits(0x2);
            v[16] = bits(0x2000);
            v[15] = bits(0x8000);
            v[11] = bits(0x1000);
            v[10] = bits(0x4000);
            v[13] = bits(0xA0);
            break;
        case 4:   // Dr. No: D-pad up/down jump and crouch, triangle/cross zoom
            v[2] = -axis(p.ry);
            v[1] = -axis(p.rx);
            v[5] = -axis(p.ly);
            v[0] = axis(p.lx);
            v[19] = bits(0x4);
            v[4] = -axis(p.ly);
            v[3] = axis(p.lx);
            if (w & 0x10) v[6] = -1.0f;
            if (w & 0x40) v[6] = 1.0f;
            v[7] = bits(0x1000);
            v[8] = bits(0x4000);
            v[9] = bits(0x8);
            v[14] = bits(0x1);
            v[12] = bits(0x2);
            v[16] = bits(0x20);
            v[15] = bits(0x80);
            v[11] = bits(0x10);
            v[10] = bits(0x40);
            v[13] = bits(0xA000);
            break;
        case 5:   // Thunderball: triangle/cross zoom; jump and crouch only without L1
            v[2] = -axis(p.ly);
            v[1] = -axis(p.rx);
            v[5] = -axis(p.ry);
            v[0] = axis(p.lx);
            v[19] = bits(0x4);
            v[4] = -axis(p.ly);
            v[3] = axis(p.lx);
            if (w & 0x10) v[6] = -1.0f;
            if (w & 0x40) v[6] = 1.0f;
            if (!(w & 0x4)) {
                v[7] = bits(0x10);
                v[8] = bits(0x40);
            }
            v[9] = bits(0x8);
            v[14] = bits(0x1);
            v[12] = bits(0x2);
            v[16] = bits(0x2000);
            v[15] = bits(0x8000);
            v[11] = bits(0x1000);
            v[10] = bits(0x4000);
            v[13] = bits(0xA0);
            break;
        case 6:   // GoldenEye: L1/R1 strafe, R2 aims
            v[2] = -axis(p.ly);
            v[1] = -axis(p.rx);
            if (w & 0x4) v[1] = -axis(p.rx) + 1.0f;
            if (w & 0x8) v[1] = v[1] - 1.0f;
            v[5] = -axis(p.ry);
            v[0] = axis(p.lx);
            v[19] = bits(0x2);
            v[4] = -axis(p.ly);
            v[3] = axis(p.lx);
            if (w & 0x1000) v[6] = -1.0f;
            if (w & 0x4000) v[6] = 1.0f;
            v[7] = bits(0x10);
            v[8] = bits(0x1);
            v[9] = bits(0x40);
            v[14] = bits(0x80);
            v[12] = bits(0x20);
            v[16] = bits(0x4000);
            v[15] = bits(0x1000);
            v[11] = bits(0x2000);
            v[10] = bits(0x8000);
            v[13] = bits(0x100);
            break;
        case 7:   // Classic Bond (the default)
            v[2] = -axis(p.ly);
            v[1] = -axis(p.rx);
            v[5] = -axis(p.ry);
            v[0] = axis(p.lx);
            v[19] = bits(0x4);
            v[4] = -axis(p.ly);
            v[3] = axis(p.lx);
            if (w & 0x1000) v[6] = -1.0f;
            if (w & 0x4000) v[6] = 1.0f;
            v[7] = bits(0x10);
            v[8] = bits(0x1);
            v[9] = bits(0x8);
            v[14] = bits(0x40);
            v[12] = bits(0x80);
            v[16] = bits(0x4002);
            v[15] = bits(0x1000);
            v[11] = bits(0x2020);
            v[10] = bits(0x8000);
            v[13] = bits(0x100);
            break;
        default: break;   // out-of-range styles map no action
    }

    // Digital shadows of the buttons and sticks (common tail, L0010E070).
    v[30] = bits(kStart);
    v[26] = bits(kUp);
    v[27] = bits(kDown);
    v[28] = bits(kLeft);
    v[29] = bits(kRight);
    if (axis(p.lx) < -0.5f) v[28] = 1.0f;
    if (axis(p.lx) > 0.5f) v[29] = 1.0f;
    if (axis(p.ly) < -0.5f) v[26] = 1.0f;
    if (axis(p.ly) > 0.5f) v[27] = 1.0f;
    v[31] = bits(kCross);
    v[32] = bits(kSquare);
    v[33] = bits(kCircle);
    v[34] = bits(kTriangle);
    if (player == 0) v[25] = bits(kStart | kCross);   // written into player 0's record only
    if (axis(p.lx) < -0.8f && axis(p.rx) > 0.8f && (w & kUp)) v[38] = 1.0f;
    return v;
}

void ActionInput::update(const PadState& pad, const InputTables& tables, const PlayerSettings& settings, int player) {
    value_ = map_inputs(pad, settings.controller_style, player);

    // Input_Update: shape the sticks (Octopussy and Thunderball keep the turn and pitch axes raw), honour the invert
    // option.
    stick_compensation_radial(tables, value_[kActLookX], value_[kActLookY]);
    if (settings.controller_style == 2 || settings.controller_style == 5) {
        stick_compensation_snap(value_[kActTurn], value_[kActForward]);
    } else {
        stick_compensation_radial(tables, value_[kActTurn], value_[kActLookPitch]);
        stick_compensation_snap(value_[kActStrafe], value_[kActForward]);
    }
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

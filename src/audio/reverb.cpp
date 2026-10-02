#include "audio/reverb.hpp"

#include <algorithm>
#include <tuple>

namespace nf::audio {

namespace {
// LIBSD.IRX effect preset for sceSdSetEffectAttr mode 4 (STUDIO_C), 32 registers in the order dAPF1,
// dAPF2, vIIR, vCOMB1-4, vWALL, vAPF1, vAPF2, mLSAME, mRSAME, mLCOMB1, mRCOMB1, mLCOMB2, mRCOMB2, dLSAME,
// dRSAME, mLDIFF, mRDIFF, mLCOMB3, mRCOMB3, mLCOMB4, mRCOMB4, dLDIFF, dRDIFF, mLAPF1, mRAPF1, mLAPF2,
// mRAPF2, vLIN, vRIN. Offsets are in units of 8 bytes; volumes are signed 16 bit fractions of 0x8000.
constexpr std::uint16_t kStudioC[32] = {
    0x00E3, 0x00A9, 0x6F60, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0xA680, 0x5680, 0x52C0, 0x0DFB,
    0x0B58, 0x0D09, 0x0A3C, 0x0BD9, 0x0973, 0x0B59, 0x08DA, 0x08D9, 0x05E9, 0x07EC, 0x04B0,
    0x06EF, 0x03D2, 0x05EA, 0x031D, 0x031C, 0x0238, 0x0154, 0x00AA, 0x8000, 0x8000};

enum Reg {
    dAPF1, dAPF2, vIIR, vCOMB1, vCOMB2, vCOMB3, vCOMB4, vWALL, vAPF1, vAPF2, mLSAME, mRSAME, mLCOMB1, mRCOMB1,
    mLCOMB2, mRCOMB2, dLSAME, dRSAME, mLDIFF, mRDIFF, mLCOMB3, mRCOMB3, mLCOMB4, mRCOMB4, dLDIFF, dRDIFF,
    mLAPF1, mRAPF1, mLAPF2, mRAPF2, vLIN, vRIN
};

constexpr std::size_t kWorkAreaSamples = 28640 / 2;  // PS2_SetupReverb: 28640 bytes

float vol(Reg r) { return static_cast<float>(static_cast<std::int16_t>(kStudioC[r])) / 32768.0f; }
int off(Reg r) { return kStudioC[r] * 4; }  // 8 bytes = 4 samples per register unit
}  // namespace

Reverb::Reverb() : ring_(kWorkAreaSamples, 0.0f) {}

float& Reverb::at(int reg_offset) {
    const auto size = static_cast<std::int64_t>(ring_.size());
    std::int64_t i = (static_cast<std::int64_t>(pos_) + reg_offset) % size;
    return ring_[static_cast<std::size_t>(i < 0 ? i + size : i)];
}

void Reverb::cycle(float in_l, float in_r, float& out_l, float& out_r) {
    const float lin = in_l * vol(vLIN), rin = in_r * vol(vRIN);
    const float iir = vol(vIIR), wall = vol(vWALL);
    const int prev = static_cast<int>(ring_.size()) - 2;  // "address - 2": the previous cycle's slot

    auto reflect = [&](Reg dst, float input, Reg src) {
        float& d = at(off(dst));
        float last = at(off(dst) + prev);
        d = std::clamp((input + at(off(src)) * wall - last) * iir + last, -1.0f, 1.0f);
    };
    reflect(mLSAME, lin, dLSAME);
    reflect(mRSAME, rin, dRSAME);
    reflect(mLDIFF, lin, dRDIFF);
    reflect(mRDIFF, rin, dLDIFF);

    auto network = [&](Reg c1, Reg c2, Reg c3, Reg c4, Reg apf1, Reg apf2) {
        float v = vol(vCOMB1) * at(off(c1)) + vol(vCOMB2) * at(off(c2)) + vol(vCOMB3) * at(off(c3)) +
                  vol(vCOMB4) * at(off(c4));
        for (auto [reg, delay, gain] : {std::tuple{apf1, dAPF1, vAPF1}, std::tuple{apf2, dAPF2, vAPF2}}) {
            float& slot = at(off(reg));
            float delayed = at(off(reg) - off(delay));
            v -= vol(gain) * delayed;
            slot = v = std::clamp(v, -1.0f, 1.0f);
            v = v * vol(gain) + delayed;
        }
        return v;
    };
    out_l = network(mLCOMB1, mLCOMB2, mLCOMB3, mLCOMB4, mLAPF1, mLAPF2);
    out_r = network(mRCOMB1, mRCOMB2, mRCOMB3, mRCOMB4, mRAPF1, mRAPF2);

    pos_ = (pos_ + 2) % ring_.size();
}

void Reverb::process(const float* in, float* out, std::size_t frames, int volume) {
    const float gain = static_cast<float>(std::clamp(volume, 0, 0x7FFF)) / 32768.0f;
    for (std::size_t f = 0; f < frames; ++f) {
        float wet_l, wet_r;
        if (!have_pending_) {
            // Even frame: the network output of the previous cycle.
            pending_l_ = in[f * 2];
            pending_r_ = in[f * 2 + 1];
            have_pending_ = true;
            wet_l = next_l_;
            wet_r = next_r_;
        } else {
            // Odd frame: run one cycle on the average of the pair, output halfway to the new result.
            float l, r;
            cycle((pending_l_ + in[f * 2]) * 0.5f, (pending_r_ + in[f * 2 + 1]) * 0.5f, l, r);
            prev_l_ = next_l_;
            prev_r_ = next_r_;
            next_l_ = l;
            next_r_ = r;
            have_pending_ = false;
            wet_l = (prev_l_ + next_l_) * 0.5f;
            wet_r = (prev_r_ + next_r_) * 0.5f;
        }
        out[f * 2] += wet_l * gain;
        out[f * 2 + 1] += wet_r * gain;
    }
}

}  // namespace nf::audio

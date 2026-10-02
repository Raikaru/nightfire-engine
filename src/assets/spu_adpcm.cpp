#include "assets/spu_adpcm.hpp"

#include <algorithm>

namespace nf {

namespace {
// SPU filter coefficients, in 1/64.
constexpr int kF0[5] = {0, 60, 115, 98, 122};
constexpr int kF1[5] = {0, 0, -52, -55, -60};
}  // namespace

void adpcm_decode_frame(const std::uint8_t* frame, AdpcmState& st, std::int16_t* out) {
    int predictor = frame[0] >> 4;
    if (predictor > 4) throw FormatError("ADPCM frame with predictor " + std::to_string(predictor));
    int shift = frame[0] & 15;
    if (shift > 12) shift = 9;  // hardware behaviour for the unused shift values
    const int f0 = kF0[predictor], f1 = kF1[predictor];
    for (std::size_t i = 0; i < kAdpcmFrameSamples; ++i) {
        int nibble = (frame[2 + i / 2] >> (4 * (i & 1))) & 15;
        int s = static_cast<std::int16_t>(nibble << 12) >> shift;
        s += (st.h1 * f0 + st.h2 * f1 + 32) >> 6;
        s = std::clamp(s, -32768, 32767);
        st.h2 = st.h1;
        st.h1 = s;
        out[i] = static_cast<std::int16_t>(s);
    }
}

DecodedSample decode_spu_sample(Bytes data) {
    if (data.size() % kAdpcmFrameBytes != 0) throw FormatError("ADPCM data not a whole number of frames");
    DecodedSample out;
    AdpcmState st;
    std::size_t loop_frame = 0;
    std::size_t frames = data.size() / kAdpcmFrameBytes;
    out.pcm.resize(frames * kAdpcmFrameSamples);
    for (std::size_t f = 0; f < frames; ++f) {
        const std::uint8_t* frame = data.data() + f * kAdpcmFrameBytes;
        std::uint8_t flags = frame[1];
        if (flags & kAdpcmLoopStart) loop_frame = f;
        adpcm_decode_frame(frame, st, out.pcm.data() + f * kAdpcmFrameSamples);
        if (flags & kAdpcmEnd) {
            out.pcm.resize((f + 1) * kAdpcmFrameSamples);
            if (flags & kAdpcmRepeat) {
                out.loops = true;
                out.loop_start = loop_frame * kAdpcmFrameSamples;
            }
            break;
        }
    }
    return out;
}

void decode_mono_block(const std::uint8_t* block, AdpcmState& state, std::int16_t* out) {
    for (std::size_t f = 0; f < 16; ++f)
        adpcm_decode_frame(block + f * kAdpcmFrameBytes, state, out + f * kAdpcmFrameSamples);
}

void decode_stereo_block(const std::uint8_t* block, AdpcmState& left, AdpcmState& right, std::int16_t* out) {
    std::int16_t frame[kAdpcmFrameSamples];
    for (std::size_t f = 0; f < 8; ++f) {
        adpcm_decode_frame(block + f * kAdpcmFrameBytes, left, frame);
        for (std::size_t i = 0; i < kAdpcmFrameSamples; ++i) out[(f * kAdpcmFrameSamples + i) * 2] = frame[i];
        adpcm_decode_frame(block + 128 + f * kAdpcmFrameBytes, right, frame);
        for (std::size_t i = 0; i < kAdpcmFrameSamples; ++i) out[(f * kAdpcmFrameSamples + i) * 2 + 1] = frame[i];
    }
}

}  // namespace nf

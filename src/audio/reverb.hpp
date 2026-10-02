#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace nf::audio {

// The SPU2 hardware reverb in the "Studio C" mode the game sets up (PS2_SetupReverb calls
// sceSdSetEffectAttr with mode 4, work area 28640 bytes). The register values are LIBSD.IRX's preset
// table entry for that mode (the classic "Studio Large" network); the algorithm is the SPU reverb:
// per side an IIR-filtered same/different-side reflection stage, four comb taps and two all-pass
// stages, all in one shared ring buffer of 16-bit samples.
//
// The hardware runs the network at 24 kHz per channel; this class takes 48 kHz stereo wet input,
// averages pairs of frames into the network and interpolates its output back up.
class Reverb {
public:
    Reverb();

    // Adds the reverb of `in` (interleaved stereo, `frames` frames) to `out`, scaled by `volume`
    // (the SPU effect volume register, 0..0x7FFF).
    void process(const float* in, float* out, std::size_t frames, int volume);

private:
    void cycle(float in_l, float in_r, float& out_l, float& out_r);
    float& at(int reg_offset);

    std::vector<float> ring_;
    std::size_t pos_ = 0;          // buffer address, advances 2 samples per cycle
    float pending_l_ = 0, pending_r_ = 0;   // first frame of the pair being averaged
    bool have_pending_ = false;
    float prev_l_ = 0, prev_r_ = 0;          // the last two network outputs, for interpolation
    float next_l_ = 0, next_r_ = 0;
};

}  // namespace nf::audio

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "assets/reader.hpp"

namespace nf {

// PS2 SPU2 ADPCM ("VAG"): 16-byte frames, 28 samples each. Every sound file on the disc (sound banks,
// the music `.SSD` files, `STREAMS.BIN`) is stored in this format and uploaded to SPU RAM as is
// (`psiLoadSoundBank`, `psiStreamMemCpy` in SFX.IRX).
//
//   byte 0: low nibble = shift, high nibble = predictor (filter 0..4)
//   byte 1: flags
//   bytes 2..15: 28 signed 4-bit samples, low nibble first
constexpr std::size_t kAdpcmFrameBytes = 16;
constexpr std::size_t kAdpcmFrameSamples = 28;

enum AdpcmFlag : std::uint8_t {
    kAdpcmEnd = 1,        // last frame of the sample / loop body
    kAdpcmRepeat = 2,     // with kAdpcmEnd: jump back to the loop start, otherwise the voice stops
    kAdpcmLoopStart = 4,  // remember this frame as the loop target
};

struct AdpcmState {
    std::int32_t h1 = 0, h2 = 0;  // previous two output samples
};

// Decodes one frame to 28 samples. Throws FormatError for a predictor above 4 (not a valid frame).
void adpcm_decode_frame(const std::uint8_t* frame, AdpcmState& state, std::int16_t* out);

// A sound bank sample decoded up to its end flag.
struct DecodedSample {
    std::vector<std::int16_t> pcm;
    bool loops = false;             // end frame carried the repeat bit
    std::size_t loop_start = 0;     // sample index the voice jumps back to (only if loops)
};

// Decodes frames until the first frame with kAdpcmEnd (inclusive). The loop start is the last
// kAdpcmLoopStart frame before it (or the first frame when there is none). Data with no end flag is
// decoded whole.
DecodedSample decode_spu_sample(Bytes data);

// Music/stream data is stored as consecutive 256-byte blocks: mono = 16 frames (448 samples);
// stereo = 8 frames of left followed by 8 frames of right (224 samples per channel), see
// psiStreamMemCpy's de-interleave. `state`/`left`/`right` carry over between blocks.
constexpr std::size_t kStreamBlockBytes = 256;
constexpr std::size_t kMonoBlockSamples = 16 * kAdpcmFrameSamples;
constexpr std::size_t kStereoBlockFrames = 8 * kAdpcmFrameSamples;

void decode_mono_block(const std::uint8_t* block, AdpcmState& state, std::int16_t* out /* 448 */);
void decode_stereo_block(const std::uint8_t* block, AdpcmState& left, AdpcmState& right,
                         std::int16_t* out /* 224 interleaved L,R pairs */);

}  // namespace nf

#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "assets/reader.hpp"
#include "assets/spu_adpcm.hpp"

namespace nf {

// One SPU sample of a bank: SB_n.SHF entry (`SampleHeaderData`, 36 bytes).
struct SampleHeader {
    std::uint32_t flags;        // bit 0: sample loops (its ADPCM frames carry loop flags)
    std::uint32_t offset;       // byte offset in SB_n.SBF (samples are packed back to back)
    std::uint32_t size;         // bytes, multiple of 16
    std::uint32_t pitch;        // SPU pitch register value: 0x1000 = 48000 Hz
    std::uint32_t real_size;    // bytes up to and including the end frame (the rest is padding)
    std::uint32_t channels;     // always 1
    std::uint32_t bits;         // always 4 (ADPCM)
    std::uint32_t tool_offset;  // running index * 0x60 written by the sound tool; unused at runtime
    std::uint32_t loop_offset;  // sound tool value for looping samples; the SPU uses the frame flags

    bool loops() const { return flags & 1; }
    float sample_rate() const { return static_cast<float>(pitch) * 48000.0f / 4096.0f; }
};

// How the position of a sound is used (`SFXParameters::TrackingType`, PS2_SFXSetup2D/3D).
enum class Tracking : std::int32_t {
    Flat = 0,        // 2D: only the sample's pan applies
    Front = 1,       // distance attenuation, but always centred in front of the listener
    Positional = 2,  // full 3D pan; low priority sounds are culled beyond the outer radius
    HeadLocked = 3,  // fixed near the listener: constant level, not attenuated
};

// SFXSamplePoolFiles (28 bytes). Percent-style values are 0..100, pitch offsets are 1/12288 of the
// sample's frequency.
struct PoolSample {
    std::int32_t file_ref;  // >= 0: sample index in the bank; < 0: STREAMS.BIN entry -file_ref-1
    std::int32_t pitch_offset;
    std::int32_t random_pitch_offset;
    std::int32_t base_volume;
    std::int32_t random_volume_offset;
    std::int32_t pan;  // -100 (left) .. 100 (right), Tracking::Flat only
    std::int32_t random_pan;

    bool is_stream() const { return file_ref < 0; }
    std::uint32_t stream_index() const { return static_cast<std::uint32_t>(-static_cast<std::int64_t>(file_ref) - 1); }
};

// SFXParameters (68 bytes) followed by `sample_count` PoolSamples. Placeholder effects with no
// samples exist (the game asks for them, nothing plays).
struct SfxParams {
    std::int32_t reverb_send;
    Tracking tracking;
    std::int32_t inner_radius;   // game units: full volume inside
    std::int32_t outer_radius;   // silent beyond
    std::int32_t max_voices;     // concurrent instances of this id (0 = unlimited)
    std::int32_t priority;       // 0..100; lowest is stolen first
    std::int32_t group;
    std::int16_t max_reject;     // refuse to start (instead of stealing) when voices are short
    std::int16_t action2;
    std::int16_t ignore_age;
    std::int16_t ducker;         // percent the music is ducked to while this plays (0 = off)
    std::int32_t ducker_length;
    std::int32_t master_volume;  // 0..100
    std::int32_t min_delay;      // frames between samples / loop repeats
    std::int32_t max_delay;
    std::int16_t multi_sample;   // play the pool as a sequence (or polyphonically) instead of picking one
    std::int16_t random_pick;
    std::int16_t shuffled;
    std::int16_t loop;           // restart the whole effect when it ends
    std::int16_t polyphonic;     // multi_sample: start every pool sample at once, staggered by the delay
    std::uint8_t outdoors;
    std::uint8_t pause_in_nis;
    std::vector<PoolSample> samples;
};

struct SfxEntry {
    std::uint32_t id;  // `HashCode`: the game's SFX_* enum value (see HashCodeToString in ACTION.ELF)
    SfxParams params;
};

// One sound bank: SB_n.SFX (effect table), SB_n.SHF (sample headers), SB_n.SBF (ADPCM data).
class SoundBank {
public:
    SoundBank(int slot, const std::vector<std::uint8_t>& sfx, const std::vector<std::uint8_t>& shf,
              std::vector<std::uint8_t> sbf);

    int slot() const { return slot_; }
    const std::vector<SampleHeader>& samples() const { return samples_; }
    const std::vector<SfxEntry>& effects() const { return effects_; }
    const SfxEntry* find(std::uint32_t id) const;

    Bytes sample_data(std::size_t index) const;      // whole allocation (size bytes)
    DecodedSample decode(std::size_t index) const;   // up to the end frame

private:
    int slot_;
    std::vector<SampleHeader> samples_;
    std::vector<SfxEntry> effects_;
    std::vector<std::uint8_t> sbf_;
};

}  // namespace nf

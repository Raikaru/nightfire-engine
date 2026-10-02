#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "assets/driving_audio.hpp"
#include "audio/engine_sound.hpp"
#include "audio/pcm_sink.hpp"

namespace nf::audio {

// Mixer for the driving missions' audio (DRIVING.ELF's BondAudioPS2 + the EA SND library on the IOP), independent of
// AudioSystem. Pull model: `render()` produces 48 kHz interleaved stereo int16 (or let `open_device()` do it on an
// SDL device). All methods may be called from the game thread while the device thread renders.
//
//   DrivingMission mission(dir, "MIS01");
//   DrivingAudio audio;
//   audio.load_level(mission, "paris_mis01");                    // banks.ini section
//   audio.start_vehicle(cfg);                                    // the ten engine layers + road/skid/scrape voices
//   audio.play_music(stream, /*loop=*/true);                     // EaStream from MISxx.MUS
//   // every game frame:
//   audio.update_vehicle(state, path);                           // VehicleState (rpm, gas, speed, slip...) + SoundPath
//   audio.play_sample("Generic", "SFX_TrafficRN", {...});
//   audio.render(out, frames);
//
// Voice parameters are quantised like AVoice::View::Play does (volume/127, pitch/4096, 16-bit azimuth) and reach the mix
// with a 256-frame ramp. A voice whose volume drops to zero stops and restarts from the beginning when it comes back.
// Parameters of a sample voice / a stream (declared outside the class so they can be default arguments).
struct DrivingPlayParams {
    float volume = 1.0f;   // 0..1
    float pitch = 1.0f;    // multiples of the sample's own rate
    float azimuth = 0.0f;  // turns clockwise from ahead (0.25 = to the right)
    bool loop = true;      // honour the sample's loop points; false plays one pass and stops
};
struct DrivingStreamParams {
    float volume = 1.0f;
    float azimuth = 0.0f;  // mono streams only
    bool loop = false;     // repeat from the first block forever
    float fade_in_s = 0.0f;
};

class DrivingAudio {
public:
    static constexpr std::uint32_t kRate = 48000;

    DrivingAudio();
    ~DrivingAudio();
    DrivingAudio(const DrivingAudio&) = delete;
    DrivingAudio& operator=(const DrivingAudio&) = delete;

    // --- data ------------------------------------------------------------------------------------------------
    // Loads every bank listed for `level` in banks.ini (roles "Engine", "Generic", "Collisions", ...) and the mission's
    // g_common.bnk (role "Common"). Replaces the previously loaded banks; running voices are stopped.
    void load_level(DrivingMission& mission, std::string_view level);

    struct SampleInfo {
        std::string role;      // banks.ini role
        std::string bank;      // banks.ini path
        int slot = 0;
        std::uint32_t rate = 0;
        bool xa = false;
        bool loops = false;
        double first_pass_seconds = 0;
    };
    // Finds `name` ("SFX_horn") in the banks of `role` (case-insensitive, first bank that has it wins).
    std::optional<SampleInfo> find_sample(std::string_view role, std::string_view name) const;
    std::optional<SampleInfo> find_slot(std::string_view role, int slot) const;

    // --- sample voices -----------------------------------------------------------------------------------------
    using VoiceId = std::uint32_t;  // 0 = none
    using PlayParams = DrivingPlayParams;
    // Returns 0 if the sound does not exist in the loaded banks.
    VoiceId play_sample(std::string_view role, std::string_view name, const PlayParams& p = {});
    VoiceId play_slot(std::string_view role, int slot, const PlayParams& p = {});
    void set_voice(VoiceId id, const VoiceParams& p);
    void stop_voice(VoiceId id);
    bool voice_playing(VoiceId id) const;
    std::size_t active_voices() const;

    // --- vehicle ---------------------------------------------------------------------------------------------
    // Creates the voices of a VehicleSound for the loaded banks (voices whose sound is missing, e.g. "SFX_IN-midload" in
    // the vanquish bank, stay silent). update_vehicle() runs one VehicleSound::update and applies it.
    void start_vehicle(const VehicleConfig& cfg = {});
    void update_vehicle(const VehicleState& state, const SoundPath& path);
    void stop_vehicle();
    const VehicleSound* vehicle() const;

    // --- streams (MISxx.MUS / MISxx<lang>.SPE `*.asf`) -----------------------------------------------------------
    // Two AStream-like channels: music and speech. play() replaces what plays on the channel, queue() starts when the
    // current stream ends (AStream::Next); `loop` repeats a stream from its first block forever. Streams are 48 kHz.
    enum class Channel { Music, Speech };
    using StreamParams = DrivingStreamParams;
    void play_stream(Channel c, std::shared_ptr<const EaStream> stream, const StreamParams& p = {});
    void queue_stream(Channel c, std::shared_ptr<const EaStream> stream, const StreamParams& p = {});
    void stop_stream(Channel c, float fade_out_s = 0.0f);
    bool stream_playing(Channel c) const;
    double stream_position_seconds(Channel c) const;  // of the stream currently playing

    // --- output ----------------------------------------------------------------------------------------------
    void render(std::int16_t* out, std::size_t frames);  // interleaved L,R
    bool open_device();                                  // false (last_error()) without a device
    void close_device();
    const std::string& last_error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace nf::audio

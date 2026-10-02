#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace nf::audio {

// The driving executable's vehicle audio, as BondAudioPS2 (linked into DRIVING.ELF) computes it every frame.
// It decides *which* bank sound plays with *what* volume / pitch / pan; playing them is DrivingAudio's job.
//
// Recovered from DRIVING.ELF (addresses are the shipped executable's):
//   AEngine::Update      0x2FA8F0   rpm and throttle smoothing, layer weights
//   AEngine::Play        0x2FF320   the ten engine layers (idle/lowload/midload/hiload/cruz, IN and OUT)
//   APlayerVehicle::Play 0x2F0900   rpm floor, road noise, calls the two above
//   AVehicle::Play       0x2E1768   rpm wobble, scrapes, tyre skids, off-road noise
//   AVoice::View::Play   0x2DDD20   quantisation of the voice parameters (see quantise())
//   AVoice update        0x2DDA50   per-frame hand-over to the SND library (SNDsetpitch etc.)
// The names come from DRIVING.SYM (its addresses belong to another build; DRIVING.ELF has no symbols).

// Quantised voice parameters, exactly what AVoice::View::Play keeps for the SND library.
struct VoiceParams {
    float volume = 0.0f;   // 0..1, multiplied by the sample's own level
    float pitch = 1.0f;    // playback rate as a multiple of the sample's rate
    float azimuth = 0.0f;  // turns clockwise from straight ahead (APath[8])
    float wet = 0.0f;      // reverb send 0..1
};

struct QuantisedVoice {
    std::int8_t volume = 0;          // round(volume * 127); 0 stops the voice
    std::uint16_t pitch = 4096;      // round(min(|pitch|, 4) * 4096); 4096 = the sample's own rate
    std::int16_t azimuth = 0;        // round(azimuth * 65536), wraps like the 16-bit angle it is
    std::int8_t wet = 0;             // round(clamp(wet) * 127)
};
QuantisedVoice quantise(const VoiceParams& p);

// SND library channel pitch -> SPU2 pitch register (SNDPLATFORM channel start, 0x30E788): rate * 4096 / 48000,
// scaled by the channel pitch, clamped to 0x3FFF. 0x1000 plays at 48 kHz. Also the ratio the IOP mixer
// resamples EA-XA sounds by, expressed as input frames per 48 kHz output frame.
std::uint16_t spu_pitch(std::uint32_t sample_rate, std::uint16_t channel_pitch);
double frames_per_output(std::uint32_t sample_rate, std::uint16_t channel_pitch);

// Linear left/right speaker gain (0..1) for a stereo output. The SND library interpolates between the two speakers
// with f(x) = 2x - x^2/127 on 0..127 (SNDI precalc of azimuth -> speaker volume table, function at 0x30C640);
// the speaker angles of the stereo layout are set at run time and were not recovered: +-45 degrees is assumed.
struct SpeakerGain {
    float left = 0, right = 0;
};
SpeakerGain speaker_gain(std::int16_t azimuth);

// What APath carries into the engine (filled per listener by ASoundManager::BuildPaths).
struct SoundPath {
    float volume = 1.0f;    // APath[0]: distance attenuation
    float pitch = 1.0f;     // APath[4]: Doppler
    float azimuth = 0.0f;   // APath[8]: bearing of the source, turns
    float cabin = 3.0f;     // APath[0x14]: <= 1 only the IN (cabin) engine layers, 1..2 crossfade IN -> OUT, >= 2 only OUT
};

// The vehicle voices, in the order of VehicleSound::voices().
enum class VehicleVoice : std::uint8_t {
    IdleIn, IdleOut, LowloadIn, LowloadOut, MidloadIn, MidloadOut, HiloadIn, HiloadOut, CruzIn, CruzOut,
    RoadNoise,
    ScrapeRock, ScrapeMetal, ScrapeSand,
    SkidFront, SkidBack, GrassSkidFront, GrassSkidBack, OffRoadSkidFront, OffRoadSkidBack, IceSkidFront, IceSkidBack,
    OffRoadNoise, GravelNoise, CobbleNoise,
    Count
};
constexpr std::size_t kVehicleVoiceCount = static_cast<std::size_t>(VehicleVoice::Count);

// Which banks.ini role holds a voice's sound and what it is called in that bank's `.h` file.
struct VoiceSource {
    std::string_view bank_role;  // "Engine", "Generic" or "Collisions"
    std::string_view name;       // "SFX_IN-idle"; empty for RoadNoise (chosen by surface, see VehicleSound::road_noise_name)
};
VoiceSource voice_source(VehicleVoice v, bool player_skids = true);

// AVehicle surface types as the physics sets them per wheel (AVehicle::SetSurface); 0 = wheel off the ground.
// The numbers are the executable's; the names are inferred from the sounds each one is routed to.
namespace surface {
constexpr int kNone = 0, kPaved = 1, kGravel = 2, kGrass = 3, kCobble = 4, kDirt = 5, kRail = 7, kIce = 8, kSnow = 9,
              kOffRoad = 10, kWood = 12, kOffRoadAlt = 15;
}

// Category volumes of the mission's mix preset (AMix::GetVolume for "Player Car", "Asphalt", "OffRoad", "Skids",
// "Scrapes"; see data\audio\<level>.ini).
struct VehicleMix {
    float player_car = 1.0f, asphalt = 1.0f, off_road = 1.0f, skids = 1.0f, scrapes = 1.0f;
};

struct VehicleConfig {
    float engine_pitch_scale = 4500.0f;  // AEngine::Get third argument: APlayerVehicle passes 4500.0
    float sfx_volume = 1.0f;             // the vehicle's SFX_VOLUME attribute (ABaseSound volume)
    float skid_multiple = 1.0f;          // SFX_SKIDMULTIPLE attribute
    bool rpm_floor = true;               // APlayerVehicle::Play raises the rpm to 500
    bool player_skids = true;            // SFX_SkidFront1/Back1; other vehicles use SFX_SquealFront1/Back1
    VehicleMix mix;
};

// Per-frame physics state (AVehicle setters / getters).
struct VehicleState {
    std::uint32_t frame_ms = 16;             // ASoundManager frame time (dword 0x355E60), whole milliseconds
    float rpm = 800.0f;                      // AVehicle::SetRpm
    float gas = 0.0f;                        // AVehicle::SetGas, 0..1
    float speed = 0.0f;                      // |velocity| in m/s (AVehicle::CalculateSpeed)
    float front_slip = 0.0f;                 // AVehicle::SetFrontSlip (clamped to <= 1)
    float back_slip = 0.0f;                  // AVehicle::SetBackSlip
    std::array<int, 4> surface = {1, 1, 1, 1};   // wheel surfaces: front left/right, back left/right
    std::array<int, 3> scrape_ms = {0, 0, 0};    // rock, metal, sand: restarts that collision timer (PlayCollision) when > 0
};

struct VehicleVoiceState {
    VoiceParams params;
    bool has_sound = true;  // false: no sound assigned (road noise on a surface without one)
};

class VehicleSound {
public:
    explicit VehicleSound(const VehicleConfig& cfg = {});

    // One AVehicle::Play + AEngine::Play + APlayerVehicle::Play pass.
    void update(const VehicleState& state, const SoundPath& path);

    const VehicleConfig& config() const { return cfg_; }
    const std::array<VehicleVoiceState, kVehicleVoiceCount>& voices() const { return voices_; }
    const VehicleVoiceState& voice(VehicleVoice v) const { return voices_[static_cast<std::size_t>(v)]; }

    // Bank sound name of the road-noise voice ("" while it has none).
    std::string_view road_noise_name() const { return road_noise_; }

    // True on the frame the throttle passes 0.2 after being released: AEngine::Play starts the one-shot
    // "SFX_vaccuum" (Engine bank) at kVacuumVolume with the "Player Car" mix.
    bool vacuum_triggered() const { return vacuum_; }
    static constexpr float kVacuumVolume = 0.1f;

    // Smoothed engine state (AEngine fields), for inspection.
    struct Engine {
        float pitch = 0;        // [0]  (rpm + 2000) / engine_pitch_scale (+ start-up wobble)
        float load = 0;         // [4]  sqrt(gas)
        float coast = 0;        // [8]  sqrt(1 - gas)
        float timer_ms = 0;     // [12] throttle-on timer
        float rpm = 0;          // [16] smoothed rpm
        float gas = 0;          // [24] smoothed throttle
        float idle_weight = 0;  // [0x4C]
        float load_weight = 1;  // [0x50]
    };
    const Engine& engine() const { return engine_; }
    float rpm_wobble() const { return wobble_; }

private:
    VehicleConfig cfg_;
    Engine engine_;
    float wobble_ = 0;      // AVehicle +0x120, added to the rpm
    float wobble_dir_ = 1;  // AVehicle +0x124
    bool vacuum_ = false;
    std::array<VehicleVoiceState, kVehicleVoiceCount> voices_{};
    std::array<float, 3> scrape_timer_{};  // collision timers in ms (AVehicle +0x17C..)
    std::array<float, 3> scrape_level_{};
    float front_skid_ = 0, back_skid_ = 0;
    std::array<float, 3> noise_level_{};  // off-road, gravel, cobble
    std::array<int, 4> road_surface_seen_{};
    std::string_view road_noise_;
    float rpm_field_ = 0;  // the rpm after APlayerVehicle's floor
};

// SFX names the road-noise voice takes for a wheel surface (APlayerVehicle::Play); empty when the surface has none.
std::string_view road_noise_for_surface(int surface_type);

// The engine layer weights the original derives from a rpm (AEngine::Play): idle = sqrt(1 - rpm/3000),
// load = sqrt(rpm/3000) below 2000 rpm, 0 and 1 above (a discontinuity the executable really has).
void engine_layer_weights(float rpm, float& idle, float& load);

// Sine of a phase in turns as the executable computes it (0x2CBD50: range reduction + 9th-order polynomial).
float game_sin_turns(float turns);

}  // namespace nf::audio

#include "audio/engine_sound.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <initializer_list>

namespace nf::audio {

namespace {

// Float constants of DRIVING.ELF, by bit pattern where the literal is not exactly representable.
const float kInv3000 = std::bit_cast<float>(0x39AEC33Eu);   // AEngine::Play: rpm scale of the layer weights
const float kInv35 = std::bit_cast<float>(0x3CEA0EA1u);     // APlayerVehicle::Play: road noise ramp (speed - 15) / 35
const float kInv15 = std::bit_cast<float>(0x3D888889u);     // AVehicle::Play: scrape level = speed / 15
const float kFadeRate = std::bit_cast<float>(0x3D23D70Au);  // AVehicle::Play: level slew per ms (0.04)
const float kGasRate = std::bit_cast<float>(0x3D4CCCCDu);   // AEngine::Update: throttle slew per ms (0.05)
const float kRpmFollow = std::bit_cast<float>(0x3DCCCCCDu); // AEngine::Update: rpm follows its target by 10% per frame
const float kIdlePitch = std::bit_cast<float>(0x3F8CCCCDu); // AEngine::Play: idle layer pitch factor (1.1)
const float kWobbleSine = std::bit_cast<float>(0x3E99999Au);// AEngine::Play: start-up pitch wobble depth (0.3)
const float kFifth = std::bit_cast<float>(0x3E4CCCCDu);     // AVehicle::Play: off-road noise ramp below 5 m/s (0.2)

float sqrtf_(float x) { return std::sqrt(x); }

// The "approach" AVehicle::Play and AEngine::Update use everywhere: move `value` towards `target` by at most `step`.
float approach(float value, float target, float step) {
    const float d = target - value;
    if (d <= -step) return value - step;
    if (step <= d) return value + step;
    return target;
}

bool either_in(int a, int b, std::initializer_list<int> set) {
    for (int s : set)
        if (a == s || b == s) return true;
    return false;
}

}  // namespace

QuantisedVoice quantise(const VoiceParams& p) {
    auto round_half_away = [](float x) { return static_cast<int>(x + (x < 0.0f ? -0.5f : 0.5f)); };
    QuantisedVoice q;
    q.volume = static_cast<std::int8_t>(std::clamp(round_half_away(p.volume * 127.0f), -128, 127));
    const float pitch = std::min(std::fabs(p.pitch), 4.0f);
    q.pitch = static_cast<std::uint16_t>(round_half_away(pitch * 4096.0f));
    q.azimuth = static_cast<std::int16_t>(static_cast<std::uint16_t>(round_half_away(p.azimuth * 65536.0f)));
    q.wet = static_cast<std::int8_t>(round_half_away(std::clamp(p.wet, 0.0f, 1.0f) * 127.0f));
    return q;
}

std::uint16_t spu_pitch(std::uint32_t sample_rate, std::uint16_t channel_pitch) {
    const std::uint32_t base = (sample_rate << 12) / 48000;
    return static_cast<std::uint16_t>(std::min<std::uint32_t>((base * channel_pitch) >> 12, 0x3FFF));
}

double frames_per_output(std::uint32_t sample_rate, std::uint16_t channel_pitch) {
    return double(spu_pitch(sample_rate, channel_pitch)) / 4096.0;
}

SpeakerGain speaker_gain(std::int16_t azimuth) {
    constexpr int kLeft = 0xE000, kRight = 0x2000;
    auto f = [](int x) { return x + (x - x * x / 127); };  // 0x30C640
    const int pos = (static_cast<std::uint16_t>(azimuth) >> 8) << 8;  // the table has one row per 256 units
    int l, r;
    if (pos >= kLeft || pos <= kRight) {  // the arc through straight ahead
        const int span = 0x4000, into = (pos - kLeft) & 0xFFFF;
        l = f((span - into) * 127 / span);
        r = f(into * 127 / span);
    } else {                              // the arc behind the listener
        const int span = kLeft - kRight, into = pos - kRight;
        r = f((span - into) * 127 / span);
        l = f(into * 127 / span);
    }
    return {l / 127.0f, r / 127.0f};
}

float game_sin_turns(float t) {
    if (t > 1.0f) t -= static_cast<float>(static_cast<int>(t));
    else if (t < 0.0f) t -= static_cast<float>(static_cast<int>(t) - 1);
    if (t < -0.5f) t += 1.0f;
    else if (t > 0.5f) t -= 1.0f;
    const float x = t * std::bit_cast<float>(0x40C90FDBu);
    const float c3 = std::bit_cast<float>(0xBE2AAAA4u), c5 = std::bit_cast<float>(0x3C08873Eu),
                c7 = std::bit_cast<float>(0xB94FB21Fu), c9 = std::bit_cast<float>(0x362E9C14u);
    const float x2 = x * x;
    return x * (1.0f + x2 * (c3 + x2 * (c5 + x2 * (c7 + x2 * c9))));
}

void engine_layer_weights(float rpm, float& idle, float& load) {
    if (rpm < 2000.0f) {
        const float x = std::max(rpm * kInv3000, 0.0f);
        idle = sqrtf_(1.0f - x);
        load = sqrtf_(x);
    } else {
        idle = 0.0f;
        load = 1.0f;
    }
}

VoiceSource voice_source(VehicleVoice v, bool player_skids) {
    switch (v) {
        case VehicleVoice::IdleIn: return {"Engine", "SFX_IN-idle"};
        case VehicleVoice::IdleOut: return {"Engine", "SFX_OUT-idle"};
        case VehicleVoice::LowloadIn: return {"Engine", "SFX_IN-lowload"};
        case VehicleVoice::LowloadOut: return {"Engine", "SFX_OUT-lowload"};
        case VehicleVoice::MidloadIn: return {"Engine", "SFX_IN-midload"};
        case VehicleVoice::MidloadOut: return {"Engine", "SFX_OUT-midload"};
        case VehicleVoice::HiloadIn: return {"Engine", "SFX_IN-hiload"};
        case VehicleVoice::HiloadOut: return {"Engine", "SFX_OUT-hiload"};
        case VehicleVoice::CruzIn: return {"Engine", "SFX_IN-cruz"};
        case VehicleVoice::CruzOut: return {"Engine", "SFX_OUT-cruz"};
        case VehicleVoice::RoadNoise: return {"Generic", ""};
        case VehicleVoice::ScrapeRock: return {"Collisions", "SFX_ScrapeRock"};
        case VehicleVoice::ScrapeMetal: return {"Collisions", "SFX_ScrapeMetal"};
        case VehicleVoice::ScrapeSand: return {"Collisions", "SFX_scrapesand"};
        case VehicleVoice::SkidFront: return {"Generic", player_skids ? "SFX_SkidFront1" : "SFX_SquealFront1"};
        case VehicleVoice::SkidBack: return {"Generic", player_skids ? "SFX_SkidBack1" : "SFX_SquealBack1"};
        case VehicleVoice::GrassSkidFront:
        case VehicleVoice::GrassSkidBack: return {"Generic", "SFX_GrassSkid"};
        case VehicleVoice::OffRoadSkidFront:
        case VehicleVoice::OffRoadSkidBack: return {"Generic", "SFX_OffRoadSkid"};
        case VehicleVoice::IceSkidFront: return {"Generic", "SFX_iceskidfront"};
        case VehicleVoice::IceSkidBack: return {"Generic", "SFX_iceskidback"};
        case VehicleVoice::OffRoadNoise: return {"Generic", "SFX_OffRN"};
        case VehicleVoice::GravelNoise: return {"Generic", "SFX_GravelRN"};
        case VehicleVoice::CobbleNoise: return {"Generic", "SFX_CobbleRN"};
        case VehicleVoice::Count: break;
    }
    return {};
}

std::string_view road_noise_for_surface(int type) {
    switch (type) {
        case surface::kPaved: return "SFX_PavedRN";
        case surface::kGravel: return "SFX_GravelRN";
        case surface::kCobble: return "SFX_CobbleRN";
        case surface::kIce: return "SFX_IceRN";
        case surface::kSnow: return "SFX_SnowRN";
        case surface::kOffRoad:
        case surface::kOffRoadAlt: return "SFX_OffRN";
        default: return {};
    }
}

VehicleSound::VehicleSound(const VehicleConfig& cfg) : cfg_(cfg), road_surface_seen_{1, 1, 1, 1}, road_noise_("SFX_PavedRN") {}

void VehicleSound::update(const VehicleState& s, const SoundPath& path) {
    const float dt = static_cast<float>(s.frame_ms);
    const float rate = dt * kFadeRate;
    auto set = [&](VehicleVoice v, float volume, float pitch = 1.0f) {
        auto& out = voices_[static_cast<std::size_t>(v)];
        out.params = {volume * path.volume, pitch * path.pitch, path.azimuth, 0.0f};
    };

    // APlayerVehicle::Play: the rpm never drops below 500.
    rpm_field_ = s.rpm;
    if (cfg_.rpm_floor && rpm_field_ + wobble_ < 500.0f) rpm_field_ = 500.0f;

    // --- AVehicle::Play -----------------------------------------------------------------------------------
    // rpm wobble: a triangle wave of +-150 rpm at 5 rpm per ms.
    wobble_ += dt * 5.0f * wobble_dir_;
    if (wobble_ > 150.0f) wobble_dir_ = -1.0f;
    else if (wobble_ < -150.0f) wobble_dir_ = 1.0f;

    // Collision scrapes: a timer per material keeps the level up while it runs.
    static constexpr VehicleVoice kScrapeVoice[3] = {VehicleVoice::ScrapeRock, VehicleVoice::ScrapeMetal, VehicleVoice::ScrapeSand};
    for (int i = 0; i < 3; ++i) {
        if (s.scrape_ms[i] > 0) scrape_timer_[i] = static_cast<float>(s.scrape_ms[i]);
        float target = 0.0f;
        if (scrape_timer_[i] > 0.0f) {
            scrape_timer_[i] = std::max(scrape_timer_[i] - dt, 0.0f);
            if (scrape_timer_[i] >= 4.0f) target = s.speed < 15.0f ? s.speed * kInv15 : 1.0f;
        }
        scrape_level_[i] = approach(scrape_level_[i], target, rate);
        set(kScrapeVoice[i], scrape_level_[i] * cfg_.mix.scrapes);
    }

    // Tyre skids follow the slip with the same slew; the loudness is scaled by SFX_SKIDMULTIPLE.
    front_skid_ = approach(front_skid_, std::min(s.front_slip, 1.0f), rate);
    back_skid_ = approach(back_skid_, std::min(s.back_slip, 1.0f), rate);
    const float front = front_skid_ * cfg_.mix.skids * cfg_.skid_multiple;
    const float back = back_skid_ * cfg_.mix.skids * cfg_.skid_multiple;
    const auto& w = s.surface;
    struct SkidRoute {
        VehicleVoice front_voice, back_voice;
        std::initializer_list<int> types;
        float back_pitch;
    };
    static const SkidRoute kSkids[] = {
        {VehicleVoice::SkidFront, VehicleVoice::SkidBack, {surface::kPaved, surface::kCobble}, 1.0f},
        {VehicleVoice::GrassSkidFront, VehicleVoice::GrassSkidBack, {surface::kGrass, surface::kOffRoad}, 0.75f},
        {VehicleVoice::OffRoadSkidFront, VehicleVoice::OffRoadSkidBack, {surface::kDirt, surface::kGravel, surface::kWood}, 0.75f},
        {VehicleVoice::IceSkidFront, VehicleVoice::IceSkidBack, {surface::kIce, surface::kSnow}, 1.0f},
    };
    for (const SkidRoute& r : kSkids) {
        set(r.front_voice, either_in(w[0], w[1], r.types) ? front : 0.0f);
        set(r.back_voice, either_in(w[2], w[3], r.types) ? back : 0.0f, r.back_pitch);
    }

    // Off-road noise: each wheel adds to the target of the material under it.
    float offroad = 0.0f, gravel = 0.0f, cobble = 0.0f;
    for (int i = 0; i < 4; ++i) {
        switch (w[i]) {
            case surface::kWood: gravel += 0.15f; break;
            case surface::kDirt: gravel += 0.15f; break;
            case surface::kGravel: gravel += 0.25f; break;
            case surface::kCobble: cobble += 0.25f; break;
            case surface::kGrass: offroad += 0.15f; break;
            case surface::kOffRoad: offroad += 0.25f; break;
            default: break;
        }
    }
    const float targets[3] = {offroad, gravel, cobble};
    static constexpr VehicleVoice kNoiseVoice[3] = {VehicleVoice::OffRoadNoise, VehicleVoice::GravelNoise, VehicleVoice::CobbleNoise};
    const float speed_ramp = s.speed < 5.0f ? s.speed * kFifth : 1.0f;
    for (int i = 0; i < 3; ++i) {
        noise_level_[i] = approach(noise_level_[i], targets[i], rate);
        set(kNoiseVoice[i], noise_level_[i] * speed_ramp * cfg_.mix.off_road);
    }

    // --- AEngine::Update ----------------------------------------------------------------------------------
    const float max_drpm = dt * 150.0f, max_dgas = dt * kGasRate;
    const bool in_air = w[2] == surface::kNone || w[3] == surface::kNone;
    float rpm_target, gas_target = s.gas, gas_step = max_dgas;
    if (in_air) rpm_target = s.gas > 0.0f ? 10000.0f : 0.0f;
    else rpm_target = rpm_field_ + wobble_ - back_skid_ * 1000.0f;
    const float delta = rpm_target - engine_.rpm;
    if (delta <= -max_drpm) {  // revs falling fast: they drop at half the maximum rate and the throttle closes quickly
        engine_.rpm -= max_drpm * 0.5f;
        gas_step *= 3.0f;
        gas_target *= 0.125f;
    } else {
        engine_.rpm += delta * kRpmFollow;
    }
    engine_.gas = approach(engine_.gas, gas_target, gas_step);
    engine_.pitch = (engine_.rpm + 2000.0f) * (1.0f / cfg_.engine_pitch_scale);
    engine_.load = sqrtf_(engine_.gas);
    engine_.coast = sqrtf_(1.0f - engine_.gas);

    // --- AEngine::Play ------------------------------------------------------------------------------------
    vacuum_ = false;
    if (engine_.gas < 0.2f) {
        engine_.timer_ms = 0.0f;
    } else if (engine_.timer_ms < 60.0f) {
        if (engine_.timer_ms == 0.0f) vacuum_ = true;
        engine_.timer_ms += dt;
        if (engine_.gas > 0.5f) {  // a short pitch wobble while the throttle is first opened
            float phase = engine_.timer_ms / 10.0f;
            if (phase > 1.0f) {
                phase -= 1.0f;
                while (phase > 1.0f) phase -= 1.0f;
            }
            engine_.pitch += game_sin_turns(phase) * kWobbleSine * (2.0f / (engine_.timer_ms + 2.0f));
        }
    }
    engine_layer_weights(engine_.rpm, engine_.idle_weight, engine_.load_weight);

    const float level = cfg_.sfx_volume * cfg_.mix.player_car;  // vehicle volume * "Player Car" mix
    float in_gain, out_gain;                                    // IN/OUT equal-power crossfade on the cabin value
    if (path.cabin > 2.0f) {
        in_gain = 0.0f;
        out_gain = level;
    } else if (path.cabin > 1.0f) {
        in_gain = sqrtf_(2.0f - path.cabin) * level;
        out_gain = sqrtf_(path.cabin - 1.0f) * level;
    } else {
        in_gain = level;
        out_gain = 0.0f;
    }
    const float gains[2] = {in_gain, out_gain};
    static constexpr VehicleVoice kLayers[5][2] = {
        {VehicleVoice::IdleIn, VehicleVoice::IdleOut},     {VehicleVoice::LowloadIn, VehicleVoice::LowloadOut},
        {VehicleVoice::MidloadIn, VehicleVoice::MidloadOut}, {VehicleVoice::HiloadIn, VehicleVoice::HiloadOut},
        {VehicleVoice::CruzIn, VehicleVoice::CruzOut}};
    for (int view = 0; view < 2; ++view) {
        const float g = gains[view];
        set(kLayers[0][view], g * engine_.idle_weight * engine_.coast, engine_.pitch * kIdlePitch);
        set(kLayers[1][view], g * engine_.load, engine_.pitch);
        set(kLayers[2][view], g * engine_.load_weight * engine_.load, engine_.pitch);
        set(kLayers[3][view], g * engine_.load_weight * engine_.load, engine_.pitch);
        set(kLayers[4][view], g * engine_.load_weight * engine_.coast, engine_.pitch);
    }

    // --- APlayerVehicle::Play: road noise, re-selected whenever a wheel's surface changes -----------------------
    for (int i = 0; i < 4; ++i) {
        if (s.surface[i] == road_surface_seen_[i]) continue;
        road_surface_seen_[i] = s.surface[i];
        road_noise_ = road_noise_for_surface(s.surface[i]);
    }
    const float road = s.speed < 15.0f ? 0.0f : s.speed < 50.0f ? (s.speed - 15.0f) * kInv35 : 1.0f;
    set(VehicleVoice::RoadNoise, road * cfg_.mix.asphalt);
    voices_[static_cast<std::size_t>(VehicleVoice::RoadNoise)].has_sound = !road_noise_.empty();
}

}  // namespace nf::audio

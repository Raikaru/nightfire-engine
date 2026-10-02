#include "audio/driving_mixer.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <deque>
#include <map>
#include <mutex>
#include <utility>

namespace nf::audio {

namespace {

bool iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

constexpr float kRampPerFrame = 1.0f / 256.0f;  // gain slew: a full-scale change takes 256 frames

}  // namespace

struct DrivingAudio::Impl {
    struct LoadedBank {
        std::string role, path;
        std::shared_ptr<const EaBank> bank;
        std::shared_ptr<const EaIndex> index;
    };

    struct Voice {
        VoiceId id = 0;
        std::shared_ptr<const EaSample> sample;
        float level = 1.0f;  // the sample's own level (tag 0x06)
        bool once = false;
        bool persistent = false;  // survives reaching the end / silence (vehicle voices)
        bool playing = false, finished = false, in_body = false;
        double pos = 0;
        double step = 1;
        QuantisedVoice q;
        float gl = 0, gr = 0, tl = 0, tr = 0;

        void restart() {
            playing = true;
            in_body = false;
            pos = 0;
            settle();
        }
        // Moves `pos` into a valid segment position; ends one-shot voices.
        void settle() {
            const EaSample& s = *sample;
            for (;;) {
                const auto& seg = in_body ? s.body : s.head;
                if (pos < static_cast<double>(seg.size())) return;
                pos -= static_cast<double>(seg.size());
                if (!in_body) {
                    if (s.body.empty() || (s.xa && once)) return end();
                    in_body = true;
                } else {
                    if (once && !s.xa) return end();
                    if (s.body.empty()) return end();
                }
            }
        }
        void end() {
            playing = false;
            finished = true;
        }
        void apply(const VoiceParams& p) {
            if (p.volume <= 0.0f) {  // AVoice::View::Play: a non-positive volume stops the voice (and frees it to restart)
                playing = false;
                finished = false;
                tl = tr = 0.0f;
                return;
            }
            q = quantise(p);
            if (!playing && !finished) restart();
            step = frames_per_output(sample->sample_rate, q.pitch);
            const SpeakerGain sg = speaker_gain(q.azimuth);
            const float g = static_cast<float>(q.volume) / 127.0f * level;
            tl = g * sg.left;
            tr = g * sg.right;
        }
        float read() const {
            const EaSample& s = *sample;
            const auto& seg = in_body ? s.body : s.head;
            const std::size_t i = static_cast<std::size_t>(pos);
            const float a = seg[i];
            float b;
            if (i + 1 < seg.size()) {
                b = seg[i + 1];
            } else if (!in_body && !s.body.empty() && !(s.xa && once)) {
                b = s.body[0];
            } else if (in_body && !once) {
                b = s.body[0];
            } else {
                b = a;
            }
            return a + (b - a) * static_cast<float>(pos - static_cast<double>(i));
        }
    };

    struct StreamChannel {
        struct Item {
            std::shared_ptr<const EaStream> stream;
            StreamParams params;
        };
        std::optional<Item> cur;
        std::deque<Item> queue;
        std::size_t block = 0;
        std::vector<std::int16_t> buf;
        std::size_t buf_pos = 0;  // in frames
        std::uint64_t played = 0;
        float fade = 1.0f, fade_rate = 0.0f;  // per output frame

        void begin(Item item, float fade_in_s) {
            cur = std::move(item);
            block = 0;
            buf.clear();
            buf_pos = 0;
            played = 0;
            fade = fade_in_s > 0.0f ? 0.0f : 1.0f;
            fade_rate = fade_in_s > 0.0f ? 1.0f / (fade_in_s * kRate) : 0.0f;
        }
        // Ensures one frame is available in `buf`; false when the channel has nothing left to play.
        bool ensure_frame() {
            while (cur) {
                const std::size_t ch = cur->stream->info().channels;
                if (buf_pos * ch < buf.size()) return true;
                buf.clear();
                buf_pos = 0;
                if (block >= cur->stream->block_count()) {
                    if (cur->params.loop) {
                        block = 0;
                        played = 0;
                    } else if (!queue.empty()) {
                        Item next = std::move(queue.front());
                        queue.pop_front();
                        float keep_fade = fade;
                        begin(std::move(next), 0.0f);
                        fade = keep_fade;
                    } else {
                        cur.reset();
                        return false;
                    }
                    continue;
                }
                cur->stream->decode_block(block++, buf);
            }
            return false;
        }
        void mix(float* acc, std::size_t frames) {
            for (std::size_t f = 0; f < frames; ++f) {
                if (!ensure_frame()) return;
                const std::size_t ch = cur->stream->info().channels;
                float l, r;
                float gain = cur->params.volume * fade;
                if (ch == 1) {
                    const SpeakerGain sg = speaker_gain(quantise({1.0f, 1.0f, cur->params.azimuth, 0.0f}).azimuth);
                    l = buf[buf_pos] * sg.left * gain;
                    r = buf[buf_pos] * sg.right * gain;
                } else {
                    l = buf[buf_pos * ch] * gain;
                    r = buf[buf_pos * ch + 1] * gain;
                }
                acc[2 * f] += l;
                acc[2 * f + 1] += r;
                ++buf_pos;
                ++played;
                if (fade_rate != 0.0f) {
                    fade = std::clamp(fade + fade_rate, 0.0f, 1.0f);
                    if (fade >= 1.0f && fade_rate > 0.0f) fade_rate = 0.0f;
                    if (fade <= 0.0f && fade_rate < 0.0f) {
                        cur.reset();
                        queue.clear();
                        return;
                    }
                }
            }
        }
    };

    struct VehicleLink {
        VehicleSound model;
        std::array<VoiceId, kVehicleVoiceCount> voice{};
        std::string road_name;
        explicit VehicleLink(const VehicleConfig& cfg) : model(cfg) {}
    };

    std::mutex mu;
    std::vector<LoadedBank> banks;
    std::map<std::pair<const EaBank*, std::size_t>, std::shared_ptr<const EaSample>> decoded;
    std::vector<Voice> voices;
    VoiceId next_id = 1;
    StreamChannel music, speech;
    std::optional<VehicleLink> vehicle;
    std::vector<float> acc;
    PcmSink sink{kRate, [this](std::int16_t* out, std::size_t frames) { render(out, frames); }};

    // --- helpers (mu held) -------------------------------------------------------------------------------------
    struct Found {
        const LoadedBank* bank;
        const EaSound* sound;
    };
    std::optional<Found> find_by_slot(std::string_view role, int slot) const {
        for (const LoadedBank& b : banks) {
            if (!iequals(b.role, role)) continue;
            if (slot >= 0)
                if (const EaSound* s = b.bank->find(static_cast<std::size_t>(slot))) return Found{&b, s};
        }
        return std::nullopt;
    }
    std::optional<Found> find_by_name(std::string_view role, std::string_view name) const {
        for (const LoadedBank& b : banks) {
            if (!iequals(b.role, role)) continue;
            auto slot = b.index->lookup(name);
            if (!slot) continue;
            if (const EaSound* s = b.bank->find(static_cast<std::size_t>(*slot))) return Found{&b, s};
        }
        return std::nullopt;
    }
    SampleInfo info_of(const Found& f) {
        const auto smp = sample_of(f);
        SampleInfo i;
        i.role = f.bank->role;
        i.bank = f.bank->path;
        i.slot = static_cast<int>(f.sound->slot);
        i.rate = f.sound->sample_rate;
        i.xa = f.sound->xa;
        i.loops = smp->loops();
        i.first_pass_seconds = double(smp->first_pass()) / f.sound->sample_rate;
        return i;
    }
    std::shared_ptr<const EaSample> sample_of(const Found& f) {
        auto key = std::make_pair(f.bank->bank.get(), f.sound->slot);
        auto it = decoded.find(key);
        if (it == decoded.end())
            it = decoded.emplace(key, std::make_shared<const EaSample>(f.bank->bank->decode(*f.sound))).first;
        return it->second;
    }
    Voice& add_voice(const Found& f, bool persistent, bool once) {
        Voice v;
        v.id = next_id++;
        v.sample = sample_of(f);
        v.level = static_cast<float>(f.sound->volume_percent) / 100.0f;
        v.persistent = persistent;
        v.once = once;
        voices.push_back(std::move(v));
        return voices.back();
    }
    Voice* voice_of(VoiceId id) {
        for (Voice& v : voices)
            if (v.id == id) return &v;
        return nullptr;
    }
    void remove_voice(VoiceId id) {
        std::erase_if(voices, [id](const Voice& v) { return v.id == id; });
    }
    VoiceId start(const std::optional<Found>& f, const PlayParams& p) {
        if (!f) return 0;
        Voice& v = add_voice(*f, false, !p.loop);
        v.apply({p.volume, p.pitch, p.azimuth, 0.0f});
        if (v.finished) {
            VoiceId id = v.id;
            remove_voice(id);
            return id;
        }
        // Start at the target gain: a new voice does not fade in.
        v.gl = v.tl;
        v.gr = v.tr;
        return v.id;
    }

    // --- rendering -------------------------------------------------------------------------------------------
    void render(std::int16_t* out, std::size_t frames) {
        std::lock_guard lock(mu);
        acc.assign(frames * 2, 0.0f);
        for (Voice& v : voices) {
            if (!v.playing) {
                v.gl = v.gr = 0.0f;
                continue;
            }
            for (std::size_t f = 0; f < frames && v.playing; ++f) {
                v.gl += std::clamp(v.tl - v.gl, -kRampPerFrame, kRampPerFrame);
                v.gr += std::clamp(v.tr - v.gr, -kRampPerFrame, kRampPerFrame);
                const float s = v.read();
                acc[2 * f] += s * v.gl;
                acc[2 * f + 1] += s * v.gr;
                v.pos += v.step;
                v.settle();
            }
        }
        std::erase_if(voices, [](const Voice& v) { return v.finished && !v.persistent; });
        music.mix(acc.data(), frames);
        speech.mix(acc.data(), frames);
        for (std::size_t i = 0; i < acc.size(); ++i)
            out[i] = static_cast<std::int16_t>(std::clamp(std::lrintf(acc[i]), -32768L, 32767L));
    }

    // --- vehicle ---------------------------------------------------------------------------------------------
    void bind_road_noise(VehicleLink& v) {
        const auto slot = static_cast<std::size_t>(VehicleVoice::RoadNoise);
        if (v.voice[slot]) remove_voice(std::exchange(v.voice[slot], 0));
        v.road_name = std::string(v.model.road_noise_name());
        if (v.road_name.empty()) return;
        if (auto f = find_by_name("Generic", v.road_name)) v.voice[slot] = add_voice(*f, true, false).id;
    }
};

DrivingAudio::DrivingAudio() : impl_(std::make_unique<Impl>()) {}
DrivingAudio::~DrivingAudio() { close_device(); }

void DrivingAudio::load_level(DrivingMission& mission, std::string_view level) {
    const std::vector<BankRef>* refs = mission.banks_ini().find(level);
    if (!refs) throw FormatError(mission.stem() + ".VIV banks.ini has no section [" + std::string(level) + "]");
    std::vector<Impl::LoadedBank> loaded;
    auto add = [&](const std::string& role, const std::string& path) {
        for (const auto& b : loaded)
            if (b.path == path) return;
        loaded.push_back({role, path, mission.bank(path), mission.bank_index(path)});
    };
    for (const BankRef& r : *refs) add(r.role, r.path);
    if (mission.viv().find("data\\audio\\g_common.bnk")) add("Common", "g_common.bnk");

    std::lock_guard lock(impl_->mu);
    impl_->voices.clear();
    impl_->vehicle.reset();
    impl_->decoded.clear();
    impl_->banks = std::move(loaded);
}

std::optional<DrivingAudio::SampleInfo> DrivingAudio::find_sample(std::string_view role, std::string_view name) const {
    std::lock_guard lock(impl_->mu);
    auto f = impl_->find_by_name(role, name);
    return f ? std::optional(impl_->info_of(*f)) : std::nullopt;
}

std::optional<DrivingAudio::SampleInfo> DrivingAudio::find_slot(std::string_view role, int slot) const {
    std::lock_guard lock(impl_->mu);
    auto f = impl_->find_by_slot(role, slot);
    return f ? std::optional(impl_->info_of(*f)) : std::nullopt;
}

DrivingAudio::VoiceId DrivingAudio::play_sample(std::string_view role, std::string_view name, const PlayParams& p) {
    std::lock_guard lock(impl_->mu);
    return impl_->start(impl_->find_by_name(role, name), p);
}

DrivingAudio::VoiceId DrivingAudio::play_slot(std::string_view role, int slot, const PlayParams& p) {
    std::lock_guard lock(impl_->mu);
    return impl_->start(impl_->find_by_slot(role, slot), p);
}

void DrivingAudio::set_voice(VoiceId id, const VoiceParams& p) {
    std::lock_guard lock(impl_->mu);
    if (auto* v = impl_->voice_of(id)) v->apply(p);
}

void DrivingAudio::stop_voice(VoiceId id) {
    std::lock_guard lock(impl_->mu);
    impl_->remove_voice(id);
}

bool DrivingAudio::voice_playing(VoiceId id) const {
    std::lock_guard lock(impl_->mu);
    auto* v = impl_->voice_of(id);
    return v && v->playing;
}

std::size_t DrivingAudio::active_voices() const {
    std::lock_guard lock(impl_->mu);
    return static_cast<std::size_t>(std::count_if(impl_->voices.begin(), impl_->voices.end(), [](const auto& v) { return v.playing; }));
}

void DrivingAudio::start_vehicle(const VehicleConfig& cfg) {
    std::lock_guard lock(impl_->mu);
    if (impl_->vehicle)
        for (VoiceId id : impl_->vehicle->voice)
            if (id) impl_->remove_voice(id);
    impl_->vehicle.emplace(cfg);
    auto& link = *impl_->vehicle;
    for (std::size_t i = 0; i < kVehicleVoiceCount; ++i) {
        const auto v = static_cast<VehicleVoice>(i);
        if (v == VehicleVoice::RoadNoise) continue;
        const VoiceSource src = voice_source(v, cfg.player_skids);
        if (auto f = impl_->find_by_name(src.bank_role, src.name)) link.voice[i] = impl_->add_voice(*f, true, false).id;
    }
    impl_->bind_road_noise(link);
}

void DrivingAudio::update_vehicle(const VehicleState& state, const SoundPath& path) {
    std::lock_guard lock(impl_->mu);
    if (!impl_->vehicle) return;
    auto& link = *impl_->vehicle;
    link.model.update(state, path);
    if (link.model.road_noise_name() != link.road_name) impl_->bind_road_noise(link);
    for (std::size_t i = 0; i < kVehicleVoiceCount; ++i) {
        if (!link.voice[i]) continue;
        if (auto* v = impl_->voice_of(link.voice[i])) v->apply(link.model.voices()[i].params);
    }
    if (link.model.vacuum_triggered()) {
        PlayParams p;
        p.volume = VehicleSound::kVacuumVolume * link.model.config().mix.player_car * path.volume;
        p.pitch = path.pitch;
        p.azimuth = path.azimuth;
        p.loop = false;
        impl_->start(impl_->find_by_name("Engine", "SFX_vaccuum"), p);
    }
}

void DrivingAudio::stop_vehicle() {
    std::lock_guard lock(impl_->mu);
    if (!impl_->vehicle) return;
    for (VoiceId id : impl_->vehicle->voice)
        if (id) impl_->remove_voice(id);
    impl_->vehicle.reset();
}

const VehicleSound* DrivingAudio::vehicle() const { return impl_->vehicle ? &impl_->vehicle->model : nullptr; }

void DrivingAudio::play_stream(Channel c, std::shared_ptr<const EaStream> stream, const StreamParams& p) {
    if (stream->info().sample_rate != kRate) throw FormatError("driving streams are 48 kHz");
    std::lock_guard lock(impl_->mu);
    auto& ch = c == Channel::Music ? impl_->music : impl_->speech;
    ch.queue.clear();
    ch.begin({std::move(stream), p}, p.fade_in_s);
}

void DrivingAudio::queue_stream(Channel c, std::shared_ptr<const EaStream> stream, const StreamParams& p) {
    if (stream->info().sample_rate != kRate) throw FormatError("driving streams are 48 kHz");
    std::lock_guard lock(impl_->mu);
    auto& ch = c == Channel::Music ? impl_->music : impl_->speech;
    if (!ch.cur) ch.begin({std::move(stream), p}, p.fade_in_s);
    else ch.queue.push_back({std::move(stream), p});
}

void DrivingAudio::stop_stream(Channel c, float fade_out_s) {
    std::lock_guard lock(impl_->mu);
    auto& ch = c == Channel::Music ? impl_->music : impl_->speech;
    ch.queue.clear();
    if (!ch.cur) return;
    if (fade_out_s <= 0.0f) {
        ch.cur.reset();
        return;
    }
    ch.fade_rate = -1.0f / (fade_out_s * kRate);
}

bool DrivingAudio::stream_playing(Channel c) const {
    std::lock_guard lock(impl_->mu);
    return (c == Channel::Music ? impl_->music : impl_->speech).cur.has_value();
}

double DrivingAudio::stream_position_seconds(Channel c) const {
    std::lock_guard lock(impl_->mu);
    return double((c == Channel::Music ? impl_->music : impl_->speech).played) / kRate;
}

void DrivingAudio::render(std::int16_t* out, std::size_t frames) { impl_->render(out, frames); }

bool DrivingAudio::open_device() { return impl_->sink.open(); }
void DrivingAudio::close_device() { impl_->sink.close(); }
const std::string& DrivingAudio::last_error() const { return impl_->sink.error(); }

}  // namespace nf::audio

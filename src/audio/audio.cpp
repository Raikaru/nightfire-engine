#include "audio/audio.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

#include "audio/music_player.hpp"
#include "audio/reverb.hpp"
#include "audio/sfx_math.hpp"

namespace nf::audio {

namespace {

constexpr std::size_t kMaxItems = 40;         // SFXActive list (InitialiseSFXList)
constexpr std::size_t kMaxSlots = 10;         // voices per effect (SFXItem::VoiceHandle)
constexpr std::size_t kMaxVoices = 48;        // SPU2 voices (psiRequestVoiceHandle)
constexpr std::size_t kMaxCued = 50;          // SFXItem::CuedSamples
constexpr std::uint32_t kFrontEndLoop1 = 471;   // SFX_MUSIC_FRONT_END_LOOP_01 / _02 follow the music volume
constexpr std::uint32_t kFrontEndLoop2 = 1136;
constexpr int kStreamPitch = 1881;            // pitch of a STREAMS.BIN entry: 22050 Hz (StartSample)
constexpr float kSpuUnity = 16384.0f;         // SPU volume register value of gain 1.0
constexpr int kFadeFrames = 120;              // SFXFadeDown/Up take 2 s
constexpr int kFadeFull = 10000;
// BonbUnderWaterSFX in SFX.IRX: effects that keep their level while the listener is under water.
constexpr std::uint32_t kUnderwaterSfx[] = {335, 331, 332, 333, 334, 330, 279, 278, 277, 253, 1303, 137,
                                            1492, 1491, 136, 514, 10, 40, 33, 310, 311, 39, 9};
constexpr float kGainRamp = 1.0f / 256.0f;    // per output frame, hides volume steps between updates

struct Pcm {
    std::vector<std::int16_t> data;
    bool loops = false;
    std::size_t loop_start = 0;
};

// One playing sample of an effect.
struct Slot {
    bool active = false;
    std::shared_ptr<const Pcm> pcm;
    double pos = 0;   // input sample index
    double step = 0;  // input samples per output frame
    int base_volume = 0;
    int pan = 0;
    bool reverb_send = false;  // SFXParameters::reverb_send != 0: the voice also feeds the reverb
    float gain_l = 0, gain_r = 0;      // current
    float target_l = 0, target_r = 0;  // set by update()
};

// SFXItem: one started effect.
struct Item {
    SfxHandle handle = 0;
    std::int32_t tag = 0;
    std::uint32_t id = 0;
    std::shared_ptr<const SoundBank> bank;
    std::shared_ptr<const SfxEntry> direct;  // synthetic entry for play_stream
    const SfxEntry* entry = nullptr;
    SfxPoint real_pos, pos, vel;
    int inner = 0, outer = 0;
    int counter = 0;
    int volume_override = 100;
    bool delete_me = false;
    int cue_count = 0;   // samples still queued (sequential multi-sample)
    int cue_timer = 0;   // frames until the next queued sample / loop restart
    std::array<int, kMaxCued> cue_timers{};
    std::array<const PoolSample*, kMaxCued> cued{};
    std::array<Slot, kMaxSlots> slots;

    const SfxParams& params() const { return entry->params; }
};

std::int16_t to_s16(float v) {
    return static_cast<std::int16_t>(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f));
}

}  // namespace

struct AudioSystem::Impl {
    Impl(const SoundArchive& archive, std::uint32_t rate) : archive(archive), rate(rate) {}

    const SoundArchive& archive;
    std::uint32_t rate;
    mutable std::mutex mutex;

    // device
    SDL_AudioStream* stream = nullptr;
    bool sdl_audio_started = false;
    std::string error;
    std::vector<std::int16_t> device_buffer;

    // data
    std::map<int, std::shared_ptr<SoundBank>> banks;
    std::map<std::pair<int, std::size_t>, std::shared_ptr<const Pcm>> sample_cache;
    std::map<std::uint32_t, std::shared_ptr<const Pcm>> stream_cache;

    // effect state
    std::vector<std::unique_ptr<Item>> items;
    SfxHandle next_handle = 1;
    ListenerFrame listener;
    bool stereo = true;
    int pressure = 0;          // gp-32732: grows by 100 per started effect, decays 10% per update
    std::uint32_t rng_a = 0x1F123BB5u, rng_b = 0x159A55E5u;

    // volumes (percent)
    int sfx_volume = 100, music_volume = 70;
    int fade = kFadeFull, fade_dir = 0;
    int duck_level = 100, duck_target = 100, duck_frames_left = 0;
    bool sfx_paused = false, music_paused = false;

    // environment (SFXSetEnvironment / SFXUpdateEnvironment)
    int env_target = 0, env_level = 0;            // reverb return level 0..100, eased 16 up / 4 down per update
    int outdoor_target = 100, outdoor_level = 100;  // gain of effects flagged `outdoors`, eased 4 per update
    bool underwater = false;
    Reverb reverb;
    bool second_visit = false;  // Sound_Ready's toggle for level 0x07000048

    // music
    std::unique_ptr<MusicPlayer> music;
    std::uint32_t music_hash = 0;
    std::array<std::int16_t, kStereoBlockFrames * 2> music_a{}, music_b{};
    double music_pos = 0;  // frame index into music_a (native 32 kHz)
    std::array<std::int32_t, 64> events{};

    // --- helpers ----------------------------------------------------------------------------
    // SFXrnd: a lagged-Fibonacci style generator, returns 0..n-1.
    int rnd(int n) {
        if (n <= 0) return 0;
        rng_a += rng_b;
        rng_b += (static_cast<std::int32_t>(rng_a) >> 31) + rng_a;
        return static_cast<int>((rng_a >> 16) % static_cast<std::uint32_t>(n));
    }
    int signed_rnd() { return rnd(2049) - 1024; }  // SFXsfrnd
    int cue_delay(const SfxParams& p) { return p.min_delay + rnd(p.max_delay - p.min_delay); }

    std::shared_ptr<const Pcm> sample_pcm(const Item& item, const PoolSample& s, int& base_pitch) {
        if (s.is_stream()) {
            base_pitch = kStreamPitch;
            auto it = stream_cache.find(s.stream_index());
            if (it != stream_cache.end()) return it->second;
            if (s.stream_index() >= archive.stream_count()) return nullptr;
            StreamClip clip = archive.load_stream(s.stream_index());
            DecodedSample d = decode_spu_sample(Bytes(clip.audio));
            auto pcm = std::make_shared<Pcm>(Pcm{std::move(d.pcm), d.loops, d.loop_start});
            return stream_cache[s.stream_index()] = pcm;
        }
        const SampleHeader& h = item.bank->samples().at(static_cast<std::size_t>(s.file_ref));
        base_pitch = static_cast<int>(h.pitch);
        auto key = std::make_pair(item.bank->slot(), static_cast<std::size_t>(s.file_ref));
        auto it = sample_cache.find(key);
        if (it != sample_cache.end()) return it->second;
        DecodedSample d = item.bank->decode(static_cast<std::size_t>(s.file_ref));
        auto pcm = std::make_shared<Pcm>(Pcm{std::move(d.pcm), d.loops, d.loop_start});
        return sample_cache[key] = pcm;
    }

    int count_voices(const Item* except) const {
        int n = 0;
        for (const auto& it : items)
            if (it.get() != except)
                for (const auto& s : it->slots) n += s.active;
        return n;
    }
    int total_voices() const { return count_voices(nullptr); }

    // ES_GetMyPos, single listener: sound types 1 and 3 are placed relative to the listener.
    void refresh_position(Item& it) {
        it.pos = it.real_pos;
        if (it.params().tracking == Tracking::HeadLocked) {
            it.inner = 2;
            it.outer = 3;
            it.pos = {listener.pos.x + listener.dir.x, listener.pos.y + listener.dir.y, listener.pos.z + listener.dir.z};
        } else if (it.params().tracking == Tracking::Front) {
            int dx = (listener.pos.x - it.real_pos.x) / 100, dy = (listener.pos.y - it.real_pos.y) / 100,
                dz = (listener.pos.z - it.real_pos.z) / 100;
            int d = 100 * ps2_sqrt(dx * dx + dy * dy + dz * dz);
            it.pos = {listener.pos.x + d * listener.dir.x / 1000, listener.pos.y + d * listener.dir.y / 1000,
                      listener.pos.z + d * listener.dir.z / 1000};
        }
    }

    // ES_CombineVolumes: 0..100 in, 0..100 out, squared response.
    int combine_volumes(int base, const Item& it) const {
        int v;
        if (it.id == kFrontEndLoop1 || it.id == kFrontEndLoop2) v = base * music_volume / 100;
        else {
            v = base * sfx_volume / 100 * it.volume_override / 100;
            if (it.params().outdoors != 0) v = v * outdoor_level / 100;
            if (underwater && std::find(std::begin(kUnderwaterSfx), std::end(kUnderwaterSfx), it.id) == std::end(kUnderwaterSfx))
                v /= 2;
        }
        v = v * fade / kFadeFull * 100 / 100 * it.params().master_volume / 100;
        if (it.params().ducker == 0) v = v * (100 - (100 - duck_level) / 3) / 100;
        return v * v / 100;
    }

    void refresh_gains(Item& it) {
        refresh_position(it);
        const SfxParams& p = it.params();
        for (Slot& s : it.slots) {
            if (!s.active) continue;
            int vol = combine_volumes(s.base_volume, it);
            ChannelVolumes cv;
            switch (p.tracking) {
                case Tracking::Flat: cv = pan_2d(s.pan, vol, stereo); break;
                case Tracking::HeadLocked: cv = head_locked(vol); break;
                default:
                    cv = apply_volume(calculate_3d(listener, it.pos, it.inner, it.outer, stereo), vol);
                    break;
            }
            if (!stereo) cv.left = cv.right = (cv.left + cv.right) / 2;
            auto gain = [](int v) { return static_cast<float>(0x3FFF * std::clamp(v, 0, 100) / 100) / kSpuUnity; };
            s.target_l = gain(cv.left);
            s.target_r = gain(cv.right);
        }
    }

    // StartSample.
    bool start_sample(Item& it, const PoolSample& ps, std::size_t slot_index) {
        if (total_voices() >= static_cast<int>(kMaxVoices)) return false;
        int base_pitch = 0;
        auto pcm = sample_pcm(it, ps, base_pitch);
        if (!pcm || pcm->data.empty()) return false;

        int pitch = base_pitch + base_pitch * (ps.pitch_offset + ((signed_rnd() * ps.random_pitch_offset) >> 10)) / 12288;
        int volume = std::clamp(ps.base_volume + ((signed_rnd() * ps.random_volume_offset) >> 10), 0, 100);
        int pan = std::clamp(ps.pan + ((signed_rnd() * ps.random_pan) >> 10), -100, 100);

        Slot& s = it.slots[slot_index];
        s = Slot{};
        s.active = true;
        s.pcm = std::move(pcm);
        s.step = static_cast<double>(pitch) * 48000.0 / 4096.0 / rate;
        s.base_volume = volume;
        s.pan = pan;
        s.reverb_send = it.params().reverb_send != 0;
        refresh_gains(it);
        s.gain_l = s.target_l;  // a new voice starts at its volume, only changes ramp
        s.gain_r = s.target_r;
        return true;
    }

    // SFXSetup: apply the voice limits, then start the effect's first sample(s).
    bool setup(Item& it) {
        const SfxParams& p = it.params();
        for (Slot& s : it.slots) s = Slot{};
        pressure = std::min(pressure, 2000);
        it.delete_me = false;
        const int limit = 15 - pressure / 400;
        const int active = count_voices(&it);

        if (p.tracking == Tracking::Positional && p.priority < 100) {
            int dist = distance_units(listener, it.real_pos);
            bool cull = it.outer < dist;
            if (!cull) {
                int span = std::max(it.outer - it.inner, 1);
                int level = std::clamp(100 - 100 * (dist - it.inner) / span, 0, 100);
                cull = level * p.priority < 2000 * active * limit / (limit * limit);
            }
            if (cull) return it.delete_me = true, false;
        }
        if (limit < active) {
            for (int n = active - limit; n > 0; --n) {
                Item* victim = nullptr;
                int lowest = 10000;
                for (auto& other : items)
                    if (!other->delete_me && other->params().tracking != Tracking::HeadLocked &&
                        other->params().priority < lowest) {
                        lowest = other->params().priority;
                        victim = other.get();
                    }
                if (victim) victim->delete_me = true;
            }
            if (p.max_reject != 0) return it.delete_me = true, false;
            if (it.delete_me) return false;
        }
        if (p.max_voices != 0) {
            int same = 0;
            for (auto& other : items) same += other->id == it.id && other.get() != &it;
            if (same >= p.max_voices) {
                if (p.max_reject != 0) return it.delete_me = true, false;
                Item* oldest = nullptr;
                int age = -1;
                for (auto& other : items)
                    if (other->id == it.id && other.get() != &it && other->counter > age) {
                        age = other->counter;
                        oldest = other.get();
                    }
                oldest->delete_me = true;
            }
        }
        pressure += 100;

        if (p.ducker != 0 && (p.tracking == Tracking::Flat || p.tracking == Tracking::HeadLocked)) {
            duck_target = 100 - p.ducker;
            duck_frames_left = std::abs(p.ducker_length) * 60 / 1000;
        }

        const int count = static_cast<int>(p.samples.size());
        if (count == 0) return it.delete_me = true, false;
        if (p.multi_sample != 0) {
            if (p.polyphonic != 0) {
                for (int i = 0; i < count; ++i) {
                    it.cue_timers[i] = cue_delay(p);
                    it.cued[i] = &p.samples[i];
                }
            } else {
                it.cue_count = count;
                it.cue_timer = 0;
                if (p.shuffled != 0) {
                    std::vector<int> pool(count);
                    for (int i = 0; i < count; ++i) pool[i] = i;
                    for (int i = 0; i < count; ++i) {
                        int k = rnd(count - i);
                        it.cued[i] = &p.samples[pool[k]];
                        pool.erase(pool.begin() + k);
                    }
                } else {
                    for (int i = 0; i < count; ++i) it.cued[i] = &p.samples[count - 1 - i];
                }
                start_sample(it, *it.cued[--it.cue_count], 0);
                it.cue_timer = cue_delay(p);
            }
        } else {
            if (!start_sample(it, p.samples[rnd(count)], 0)) return it.delete_me = true, false;
            it.cue_timer = cue_delay(p);
        }
        return true;
    }

    Item* find_item(SfxHandle h) const {
        for (const auto& it : items)
            if (it->handle == h) return it.get();
        return nullptr;
    }

    SfxHandle start(std::uint32_t id, std::shared_ptr<const SoundBank> bank, const SfxEntry* entry,
                    std::shared_ptr<const SfxEntry> direct, const PlayOptions& opt) {
        if (items.size() >= kMaxItems) return 0;
        auto it = std::make_unique<Item>();
        it->handle = next_handle++;
        it->tag = opt.tag;
        it->id = id;
        it->bank = std::move(bank);
        it->direct = std::move(direct);
        it->entry = entry;
        it->real_pos = it->pos = to_sfx_point(opt.position.value_or(Vec3{}));
        it->vel = to_sfx_point(opt.velocity);
        it->inner = opt.inner_radius >= 0 ? opt.inner_radius : entry->params.inner_radius;
        it->outer = opt.outer_radius >= 0 ? opt.outer_radius : entry->params.outer_radius;
        Item* raw = it.get();
        items.push_back(std::move(it));
        SfxHandle handle = raw->handle;
        if (!setup(*raw)) {
            erase_deleted();
            return 0;
        }
        return handle;
    }

    void erase_deleted() {
        std::erase_if(items, [](const std::unique_ptr<Item>& i) { return i->delete_me; });
    }

    // SFXUpdateEnding: an effect whose samples are all done loops or is removed.
    void finish_or_loop(Item& it) {
        if (it.params().loop == 0) {
            it.delete_me = true;
        } else if (it.cue_timer-- <= 0) {
            setup(it);
        }
    }

    void tick_item(Item& it) {
        const SfxParams& p = it.params();
        ++it.counter;
        auto ended = [](const Slot& s) { return !s.active; };
        if (p.multi_sample != 0 && p.polyphonic != 0) {
            bool all_done = true;
            for (int j = 0; j < static_cast<int>(p.samples.size()); ++j) {
                int& timer = it.cue_timers[j];
                if (timer != 0) {
                    if (timer <= 0) {
                        if (!ended(it.slots[j])) all_done = false;
                    } else {
                        all_done = false;
                        --timer;
                    }
                } else {
                    start_sample(it, *it.cued[j], j);
                    timer = -1;
                    all_done = false;
                }
            }
            if (all_done) finish_or_loop(it);
        } else if (ended(it.slots[0])) {
            if (p.multi_sample != 0 && it.cue_count != 0) {
                if (it.cue_timer-- <= 0) {
                    start_sample(it, *it.cued[--it.cue_count], 0);
                    it.cue_timer = cue_delay(p);
                }
            } else {
                finish_or_loop(it);
            }
        }
        refresh_gains(it);
    }

    // --- music ------------------------------------------------------------------------------
    int music_gain_percent() const {
        if (!music) return 0;
        int v = music_volume * duck_level / 100 * fade / kFadeFull;
        v = v * static_cast<int>(music->track().map.base_volume) / 100;
        return v * v / 100;
    }

    void next_music_block() {
        music_a = music_b;
        music->next_block(music_b.data());
    }

    void mix_music(float* acc, std::size_t frames) {
        if (!music || music_paused) return;
        const float gain = static_cast<float>(music_gain_percent()) / 100.0f;
        const double step = static_cast<double>(kMusicSampleRate) / rate;
        for (std::size_t f = 0; f < frames; ++f) {
            std::size_t i = static_cast<std::size_t>(music_pos);
            float frac = static_cast<float>(music_pos - static_cast<double>(i));
            const std::int16_t* a = &music_a[i * 2];
            const std::int16_t* b = i + 1 < kStereoBlockFrames ? a + 2 : music_b.data();
            acc[f * 2] += (a[0] + (b[0] - a[0]) * frac) / 32768.0f * gain;
            acc[f * 2 + 1] += (a[1] + (b[1] - a[1]) * frac) / 32768.0f * gain;
            music_pos += step;
            while (music_pos >= kStereoBlockFrames) {
                music_pos -= kStereoBlockFrames;
                next_music_block();
            }
        }
    }

    void mix_slot(Slot& s, float* acc, float* wet, std::size_t frames) {
        const std::vector<std::int16_t>& d = s.pcm->data;
        const std::size_t n = d.size();
        for (std::size_t f = 0; f < frames; ++f) {
            s.gain_l += std::clamp(s.target_l - s.gain_l, -kGainRamp, kGainRamp);
            s.gain_r += std::clamp(s.target_r - s.gain_r, -kGainRamp, kGainRamp);
            std::size_t i = static_cast<std::size_t>(s.pos);
            float frac = static_cast<float>(s.pos - static_cast<double>(i));
            float a = d[i];
            float b = i + 1 < n ? d[i + 1] : (s.pcm->loops ? d[s.pcm->loop_start] : 0.0f);
            float v = (a + (b - a) * frac) / 32768.0f;
            acc[f * 2] += v * s.gain_l;
            acc[f * 2 + 1] += v * s.gain_r;
            if (s.reverb_send) {
                wet[f * 2] += v * s.gain_l;
                wet[f * 2 + 1] += v * s.gain_r;
            }
            s.pos += s.step;
            while (s.pos >= static_cast<double>(n)) {
                if (!s.pcm->loops) {
                    s.active = false;
                    return;
                }
                s.pos -= static_cast<double>(n - s.pcm->loop_start);
            }
        }
    }

    void render(std::int16_t* out, std::size_t frames) {
        std::lock_guard lock(mutex);
        std::vector<float> acc(frames * 2, 0.0f), wet(frames * 2, 0.0f);
        if (!sfx_paused)
            for (auto& it : items)
                for (Slot& s : it->slots)
                    if (s.active) mix_slot(s, acc.data(), wet.data(), frames);
        // psiSetReverb: effect volume register = 0x3FFF * (80% of the environment level).
        reverb.process(wet.data(), acc.data(), frames, 0x3FFF * (80 * env_level / 100) / 100);
        mix_music(acc.data(), frames);
        for (std::size_t i = 0; i < acc.size(); ++i) out[i] = to_s16(acc[i]);
    }

    // --- device -----------------------------------------------------------------------------
    static void SDLCALL device_callback(void* userdata, SDL_AudioStream* s, int additional, int) {
        auto* self = static_cast<Impl*>(userdata);
        std::size_t frames = static_cast<std::size_t>(additional) / (2 * sizeof(std::int16_t));
        if (frames == 0) return;
        self->device_buffer.resize(frames * 2);
        self->render(self->device_buffer.data(), frames);
        SDL_PutAudioStreamData(s, self->device_buffer.data(), static_cast<int>(frames * 2 * sizeof(std::int16_t)));
    }

    bool open_device() {
        std::lock_guard lock(mutex);
        if (stream) return true;
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            error = SDL_GetError();
            return false;
        }
        sdl_audio_started = true;
        SDL_AudioSpec spec{SDL_AUDIO_S16, 2, static_cast<int>(rate)};
        stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, device_callback, this);
        if (!stream) {
            error = SDL_GetError();
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
            sdl_audio_started = false;
            return false;
        }
        SDL_ResumeAudioStreamDevice(stream);
        return true;
    }

    void close_device() {
        SDL_AudioStream* s;
        {
            std::lock_guard lock(mutex);
            s = std::exchange(stream, nullptr);
        }
        if (s) SDL_DestroyAudioStream(s);  // joins the callback, so it must not hold the mutex
        if (std::exchange(sdl_audio_started, false)) SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
};

AudioSystem::AudioSystem(const SoundArchive& archive, std::uint32_t output_rate)
    : impl_(std::make_unique<Impl>(archive, output_rate)) {}

AudioSystem::~AudioSystem() { impl_->close_device(); }

bool AudioSystem::open_device() { return impl_->open_device(); }
void AudioSystem::close_device() { impl_->close_device(); }
bool AudioSystem::device_open() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->stream != nullptr;
}
const std::string& AudioSystem::last_error() const { return impl_->error; }
std::uint32_t AudioSystem::output_rate() const { return impl_->rate; }
void AudioSystem::render(std::int16_t* out, std::size_t frames) { impl_->render(out, frames); }

void AudioSystem::load_bank(int slot) {
    auto bank = std::make_shared<SoundBank>(impl_->archive.load_bank(slot));
    std::lock_guard lock(impl_->mutex);
    impl_->banks[slot] = std::move(bank);
}

void AudioSystem::load_bank_hash(std::uint32_t hash) {
    auto slot = impl_->archive.bank_slot(hash);
    if (!slot) throw FormatError("no sound bank with hash " + std::to_string(hash));
    load_bank(*slot);
}

void AudioSystem::unload_all_banks() {
    std::lock_guard lock(impl_->mutex);
    impl_->items.clear();
    impl_->banks.clear();
    impl_->sample_cache.clear();
    // SFXResetEnvironment
    impl_->env_target = impl_->env_level = 0;
    impl_->outdoor_target = impl_->outdoor_level = 100;
    impl_->underwater = false;
}

bool AudioSystem::enter_level(std::uint32_t level_id) {
    unload_all_banks();
    auto hash = SoundArchive::level_bank_hash(level_id, impl_->second_visit);
    if (level_id == 0x07000048) impl_->second_visit = !impl_->second_visit;
    if (!hash) return false;
    load_bank_hash(*hash);
    return true;
}

bool AudioSystem::bank_loaded(int slot) const {
    std::lock_guard lock(impl_->mutex);
    return impl_->banks.contains(slot);
}

void AudioSystem::set_listener(const Listener& l) {
    std::lock_guard lock(impl_->mutex);
    auto unit = [](const Vec3& v) { return to_sfx_point(v); };
    impl_->listener = {unit(l.position), unit(l.velocity), unit(l.dir), unit(l.up), unit(l.norm)};
}

void AudioSystem::set_environment(int room, bool indoors) {
    std::lock_guard lock(impl_->mutex);
    Impl& s = *impl_;
    s.underwater = room == kUnderwaterRoom;
    s.env_target = s.underwater ? 0 : std::clamp(room, 0, 100);
    s.outdoor_target = indoors ? 50 : 100;
}

void AudioSystem::set_stereo(bool stereo) {
    std::lock_guard lock(impl_->mutex);
    impl_->stereo = stereo;
}

SfxHandle AudioSystem::play_sfx(std::uint32_t id, const PlayOptions& options) {
    std::lock_guard lock(impl_->mutex);
    for (auto& [slot, bank] : impl_->banks)
        if (const SfxEntry* e = bank->find(id)) return impl_->start(id, bank, e, nullptr, options);
    return 0;
}

SfxHandle AudioSystem::play_sfx(std::string_view name, const PlayOptions& options) {
    auto id = impl_->archive.sfx_id(name);
    return id ? play_sfx(*id, options) : 0;
}

SfxHandle AudioSystem::play_stream(std::uint32_t index, const PlayOptions& options) {
    if (index >= impl_->archive.stream_count()) return 0;
    auto entry = std::make_shared<SfxEntry>();
    entry->id = 0xFFFF0000u | index;
    SfxParams& p = entry->params;
    p = {};
    p.tracking = options.position ? Tracking::Positional : Tracking::Flat;
    p.inner_radius = 2;
    p.outer_radius = 30;
    p.priority = 100;
    p.master_volume = 100;
    p.samples.push_back({-static_cast<std::int32_t>(index) - 1, 0, 0, 100, 0, 0, 0});
    std::lock_guard lock(impl_->mutex);
    const SfxEntry* raw = entry.get();
    const std::uint32_t id = entry->id;
    return impl_->start(id, nullptr, raw, std::move(entry), options);
}

void AudioSystem::stop_sfx(SfxHandle handle) {
    std::lock_guard lock(impl_->mutex);
    if (Item* it = impl_->find_item(handle)) it->delete_me = true;
    impl_->erase_deleted();
}

void AudioSystem::remove_sfx(std::int32_t tag, std::int64_t id) {
    std::lock_guard lock(impl_->mutex);
    for (auto& it : impl_->items)
        if ((it->tag == tag || tag == 0) && (id == -1 || it->id == id)) it->delete_me = true;
    impl_->erase_deleted();
}

void AudioSystem::set_position(const Vec3& position, const Vec3& velocity, std::int32_t tag, std::int64_t id) {
    std::lock_guard lock(impl_->mutex);
    for (auto& it : impl_->items)
        if ((it->tag == tag || tag == 0) && (id == -1 || it->id == id)) {
            it->real_pos = it->pos = to_sfx_point(position);
            it->vel = to_sfx_point(velocity);
        }
}

bool AudioSystem::is_playing(SfxHandle handle) const {
    std::lock_guard lock(impl_->mutex);
    Item* it = impl_->find_item(handle);
    return it && !it->delete_me;
}

bool AudioSystem::is_sfx_playing(std::uint32_t id) const {
    std::lock_guard lock(impl_->mutex);
    for (const auto& it : impl_->items)
        if (it->id == id && !it->delete_me) return true;
    return false;
}

std::size_t AudioSystem::active_effects() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->items.size();
}

void AudioSystem::start_music(std::uint32_t track_hash, std::uint32_t section) {
    auto number = impl_->archive.music_number(track_hash);
    if (!number) throw FormatError("no music track with hash " + std::to_string(track_hash));
    start_music_number(*number, section);
    std::lock_guard lock(impl_->mutex);
    impl_->music_hash = track_hash;
}

void AudioSystem::start_music_number(int number, std::uint32_t section) {
    auto track = std::make_shared<const MusicTrack>(impl_->archive.load_music(number));
    auto player = std::make_unique<MusicPlayer>(track);
    player->start(section);
    std::lock_guard lock(impl_->mutex);
    impl_->music = std::move(player);
    impl_->music_hash = impl_->archive.music_hash(static_cast<std::size_t>(number - 1));
    impl_->music->next_block(impl_->music_b.data());
    impl_->next_music_block();
    impl_->music_pos = 0;
}

void AudioSystem::stop_music() {
    std::lock_guard lock(impl_->mutex);
    impl_->music.reset();
}

void AudioSystem::jump_music(std::uint32_t section, bool instant) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->music) impl_->music->jump(section, instant);
}

MusicStatus AudioSystem::music_status() const {
    std::lock_guard lock(impl_->mutex);
    MusicStatus s;
    if (!impl_->music) return s;
    s.playing = !impl_->music->finished();
    s.track_hash = impl_->music_hash;
    s.now_playing = impl_->music->now_playing();
    s.last_jump = impl_->music->last_jump();
    s.pending_jump = impl_->music->pending_jump();
    s.seconds = music_seconds(impl_->music->position());
    return s;
}

void AudioSystem::music_event(std::uint32_t event, std::int32_t arg) {
    std::lock_guard lock(impl_->mutex);
    if (event < impl_->events.size()) impl_->events[event] = arg;
}

std::int32_t AudioSystem::music_event_value(std::uint32_t event) const {
    std::lock_guard lock(impl_->mutex);
    return event < impl_->events.size() ? impl_->events[event] : 0;
}

void AudioSystem::set_sfx_volume(int percent) {
    std::lock_guard lock(impl_->mutex);
    impl_->sfx_volume = std::clamp(percent, 0, 100);
}

void AudioSystem::set_music_volume(int percent) {
    std::lock_guard lock(impl_->mutex);
    impl_->music_volume = std::clamp(percent, 0, 100);
}

void AudioSystem::fade_down() {
    std::lock_guard lock(impl_->mutex);
    if (impl_->fade_dir != -1 && impl_->fade != 0) impl_->fade_dir = -1;
}

void AudioSystem::fade_up() {
    std::lock_guard lock(impl_->mutex);
    if (impl_->fade_dir != 1 && impl_->fade != kFadeFull) impl_->fade_dir = 1;
}

void AudioSystem::pause_sfx(bool paused) {
    std::lock_guard lock(impl_->mutex);
    impl_->sfx_paused = paused;
}

void AudioSystem::pause_music(bool paused) {
    std::lock_guard lock(impl_->mutex);
    impl_->music_paused = paused;
}

void AudioSystem::update() {
    std::lock_guard lock(impl_->mutex);
    Impl& s = *impl_;
    if (s.pressure > 0) s.pressure = 9 * s.pressure / 10;

    // SFXUpdateEnvironment
    s.env_level += std::clamp(s.env_target - s.env_level, -4, 16);
    s.outdoor_level += std::clamp(s.outdoor_target - s.outdoor_level, -4, 4);

    // SFXFadeUpdate
    if (s.fade_dir != 0) {
        s.fade = std::clamp(s.fade + s.fade_dir * kFadeFull / kFadeFrames, 0, kFadeFull);
        if (s.fade == 0 || s.fade == kFadeFull) s.fade_dir = 0;
    }
    // ES_UpdateDucker: ease towards the target; once the duck time has passed, back to full.
    if (s.duck_level != s.duck_target) {
        int gap = s.duck_target - s.duck_level;
        int move = gap * 1024 / (s.duck_frames_left > 0 ? 6 : 15) / 1024;
        s.duck_level += move != 0 ? move : (gap > 0 ? 1 : -1);
    }
    if (s.duck_frames_left > 0) {
        if (--s.duck_frames_left == 0) s.duck_target = 100;
    } else if (s.duck_target != 100) {
        s.duck_target = 100;
    }

    if (!s.sfx_paused) {
        std::vector<Item*> live;
        for (auto& it : s.items) live.push_back(it.get());
        for (Item* it : live)
            if (!it->delete_me) s.tick_item(*it);
    }
    s.erase_deleted();
}

}  // namespace nf::audio

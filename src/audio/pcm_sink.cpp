#include "audio/pcm_sink.hpp"

#include <SDL3/SDL.h>

#include <utility>

namespace nf::audio {

PcmSink::PcmSink(std::uint32_t rate, Fill fill) : rate_(rate), fill_(std::move(fill)) {}

PcmSink::~PcmSink() { close(); }

void PcmSink::callback(void* self, SDL_AudioStream* stream, int additional, int) {
    auto* sink = static_cast<PcmSink*>(self);
    const std::size_t frames = static_cast<std::size_t>(additional) / (2 * sizeof(std::int16_t));
    if (frames == 0) return;
    sink->buffer_.resize(frames * 2);
    sink->fill_(sink->buffer_.data(), frames);
    SDL_PutAudioStreamData(stream, sink->buffer_.data(), static_cast<int>(frames * 2 * sizeof(std::int16_t)));
}

bool PcmSink::open() {
    if (stream_) return true;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        error_ = SDL_GetError();
        return false;
    }
    sdl_started_ = true;
    SDL_AudioSpec spec{SDL_AUDIO_S16, 2, static_cast<int>(rate_)};
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &PcmSink::callback, this);
    if (!stream_) {
        error_ = SDL_GetError();
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        sdl_started_ = false;
        return false;
    }
    SDL_ResumeAudioStreamDevice(stream_);
    return true;
}

void PcmSink::close() {
    if (stream_) SDL_DestroyAudioStream(std::exchange(stream_, nullptr));  // joins the callback
    if (std::exchange(sdl_started_, false)) SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

}  // namespace nf::audio

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct SDL_AudioStream;

namespace nf::audio {

// A 16-bit stereo SDL3 playback device that pulls its samples from `fill` (called on SDL's audio thread with
// interleaved L,R frames to produce). Independent of AudioSystem: several sinks can exist at once.
class PcmSink {
public:
    using Fill = std::function<void(std::int16_t* out, std::size_t frames)>;

    PcmSink(std::uint32_t rate, Fill fill);
    ~PcmSink();
    PcmSink(const PcmSink&) = delete;
    PcmSink& operator=(const PcmSink&) = delete;

    bool open();  // false when there is no usable device; error() says why
    void close();
    bool is_open() const { return stream_ != nullptr; }
    const std::string& error() const { return error_; }

private:
    static void callback(void* self, SDL_AudioStream* stream, int additional, int total);

    std::uint32_t rate_;
    Fill fill_;
    SDL_AudioStream* stream_ = nullptr;
    bool sdl_started_ = false;
    std::string error_;
    std::vector<std::int16_t> buffer_;
};

}  // namespace nf::audio

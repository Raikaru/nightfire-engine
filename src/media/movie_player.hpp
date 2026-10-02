// PSS movie playback (MOVIES/30_FPS/*.PSS): MPEG-2 program streams with mpeg2video and
// Sony private-stream PCM audio. Video decodes through libavformat/libavcodec/libswscale;
// audio (0xBD packets: `ff a0 00 00` + s16le stereo, rate/channels from the SShd header of the
// first packet) is extracted by the small scanner below and played through SDL3 directly
// (AudioSystem untouched). See docs/formats.md "Movies".
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace nf::media {

struct MovieVideoInfo {
    int width = 0, height = 0;
    double fps = 30.0, duration = 0;  // duration from the container (0 unknown)
};

struct MovieAudioInfo {
    bool present = false;
    int sample_rate = 48000, channels = 2;
    double duration = 0;  // bytes / (rate * channels * 2)
};

struct MovieFrame {
    std::vector<std::uint32_t> rgba;  // R in low byte, width * height
    int width = 0, height = 0;
    double pts = 0;  // presentation time, seconds
};

class MoviePlayer {
public:
    MoviePlayer();
    ~MoviePlayer();
    MoviePlayer(const MoviePlayer&) = delete;
    MoviePlayer& operator=(const MoviePlayer&) = delete;

    // Opens path (a .PSS file). Returns false with describe() set on failure.
    bool open(const std::string& path);
    const std::string& error() const;
    const MovieVideoInfo& video() const;
    const MovieAudioInfo& audio() const;

    // Next video frame in presentation order (false at EOF/error). Skips nothing.
    bool next_video(MovieFrame& frame);
    // Up to `seconds` of stereo s16 audio from the current position (false at EOF).
    bool next_audio(std::vector<std::int16_t>& pcm, double seconds);
    double audio_position() const;  // seconds consumed so far

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace nf::media

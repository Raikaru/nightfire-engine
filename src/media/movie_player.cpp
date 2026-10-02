// PSS playback: libavformat demuxes the MPEG-2 program stream (video packet 0xE0), the
// mpeg2video decoder + swscale produce RGBA frames, and the audio scanner below pulls the Sony
// private-stream (0xBD) PCM: each PES payload is `ff a0 00 00` + s16le stereo (rate/channels from
// the first packet's SShd header; its 64 header bytes are skipped). Concatenated payloads decode
// gapless (track length matches the video duration to a frame).
#include "media/movie_player.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
}

namespace nf::media {

namespace {

// First audio packet: `ff a0 00 00` + SShd(24: magic, len, ?, rate u32, channels u32, ...) +
// padding + SSbd; its header bytes carry no samples, so the extractor skips them.
constexpr std::size_t kFirstPacketSkip = 64;
constexpr std::uint8_t kSShdMagic[4] = {0x53, 0x53, 0x68, 0x64};

struct AudioTrack {
    std::vector<std::int16_t> pcm;  // interleaved s16
    int sample_rate = 0, channels = 0;
    std::size_t position = 0;  // samples consumed, always frame-aligned
};

// Appends every 0xBD payload (minus the 4 sub-stream header bytes) to `out`; reads rate/channels
// from the first packet's SShd header. Returns false when the layout is not as expected.
bool extract_audio(const std::string& path, std::vector<std::int16_t>& out, int& rate, int& channels,
                   std::string& error) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        error = "cannot open " + path;
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<std::uint8_t> data(std::size_t(std::max<long>(size, 0)));
    if (std::fread(data.data(), 1, data.size(), f) != data.size()) {
        std::fclose(f);
        error = "short read of " + path;
        return false;
    }
    std::fclose(f);
    bool first = true;
    std::size_t i = 0;
    const std::size_t n = data.size();
    while (i + 16 < n) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1 && data[i + 3] == 0xBD) {
            const std::size_t len = (std::size_t(data[i + 4]) << 8) | data[i + 5];
            const std::size_t hl = data[i + 8];
            const std::size_t pay = i + 9 + hl, plen = len >= 3 + hl ? len - 3 - hl : 0;
            if (pay + plen > n) break;
            if (plen < 4 || data[pay] != 0xFF || data[pay + 1] != 0xA0) {
                i += 4;
                continue;
            }
            std::size_t start = pay + 4;
            if (first) {
                first = false;
                if (plen < 4 + 24 || std::memcmp(&data[pay + 4], kSShdMagic, 4) != 0) {
                    error = "missing SShd header";
                    return false;
                }
                const std::uint8_t* h = &data[pay + 4];
                rate = int(h[12] | (h[13] << 8) | (h[14] << 16) | (h[15] << 24));
                channels = int(h[16] | (h[17] << 8) | (h[18] << 16) | (h[19] << 24));
                if ((rate != 32000 && rate != 44100 && rate != 48000) || (channels != 1 && channels != 2)) {
                    error = "implausible SShd rate/channels";
                    return false;
                }
                start = pay + kFirstPacketSkip;
            }
            for (std::size_t p = start; p + 1 < pay + plen; p += 2)
                out.push_back(std::int16_t(data[p] | (data[p + 1] << 8)));
            i = pay + plen;
            continue;
        }
        ++i;
    }
    if (first) {
        error = "no audio packets";
        return false;
    }
    return true;
}

}  // namespace

struct MoviePlayer::Impl {
    AVFormatContext* format = nullptr;
    AVCodecContext* video_codec = nullptr;
    SwsContext* scaler = nullptr;
    AVFrame* decoded = nullptr;
    AVPacket* packet = nullptr;
    int video_stream = -1;
    double time_base = 0;
    MovieVideoInfo video_info;
    MovieAudioInfo audio_info;
    AudioTrack audio;
    std::string error;
    bool video_eof = false;
    bool flushed = false;  // the null-packet flush may only be sent once (re-flushing re-emits delayed frames)

    ~Impl() {
        if (scaler) sws_freeContext(scaler);
        if (decoded) av_frame_free(&decoded);
        if (packet) av_packet_free(&packet);
        if (video_codec) avcodec_free_context(&video_codec);
        if (format) avformat_close_input(&format);
    }
};

MoviePlayer::MoviePlayer() : impl_(std::make_unique<Impl>()) {}
MoviePlayer::~MoviePlayer() = default;

const std::string& MoviePlayer::error() const { return impl_->error; }
const MovieVideoInfo& MoviePlayer::video() const { return impl_->video_info; }
const MovieAudioInfo& MoviePlayer::audio() const { return impl_->audio_info; }
double MoviePlayer::audio_position() const {
    const Impl& s = *impl_;
    if (s.audio.sample_rate <= 0 || s.audio.channels <= 0) return 0;
    return double(s.audio.position) / double(s.audio.channels * s.audio.sample_rate);
}

bool MoviePlayer::open(const std::string& path) {
    Impl& s = *impl_;
    if (avformat_open_input(&s.format, path.c_str(), nullptr, nullptr) != 0) {
        s.error = "avformat cannot open " + path;
        return false;
    }
    if (avformat_find_stream_info(s.format, nullptr) < 0) {
        s.error = "avformat cannot probe " + path;
        return false;
    }
    s.video_stream = av_find_best_stream(s.format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (s.video_stream < 0) {
        s.error = "no video stream in " + path;
        return false;
    }
    AVStream* stream = s.format->streams[s.video_stream];
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        s.error = "no decoder for the video codec";
        return false;
    }
    s.video_codec = avcodec_alloc_context3(codec);
    if (!s.video_codec || avcodec_parameters_to_context(s.video_codec, stream->codecpar) < 0 ||
        avcodec_open2(s.video_codec, codec, nullptr) < 0) {
        s.error = "cannot open the video decoder";
        return false;
    }
    s.time_base = av_q2d(stream->time_base);
    s.video_info.width = s.video_codec->width;
    s.video_info.height = s.video_codec->height;
    if (stream->avg_frame_rate.num > 0 && stream->avg_frame_rate.den > 0)
        s.video_info.fps = av_q2d(stream->avg_frame_rate);
    else if (stream->r_frame_rate.num > 0 && stream->r_frame_rate.den > 0)
        s.video_info.fps = av_q2d(stream->r_frame_rate);
    if (s.format->duration != AV_NOPTS_VALUE) s.video_info.duration = double(s.format->duration) / AV_TIME_BASE;
    s.decoded = av_frame_alloc();
    s.packet = av_packet_alloc();
    if (!s.decoded || !s.packet) {
        s.error = "cannot allocate decode buffers";
        return false;
    }
    // Audio is demuxed manually (libavformat skips Sony's private audio): failure just means silence.
    std::vector<std::int16_t> pcm;
    int rate = 48000, channels = 2;
    std::string audio_error;
    if (extract_audio(path, pcm, rate, channels, audio_error)) {
        s.audio.pcm = std::move(pcm);
        s.audio.sample_rate = rate;
        s.audio.channels = channels;
        s.audio_info.present = true;
        s.audio_info.sample_rate = rate;
        s.audio_info.channels = channels;
        s.audio_info.duration = rate > 0 ? double(s.audio.pcm.size()) / double(channels * rate) : 0;
    }
    return true;
}

bool MoviePlayer::next_video(MovieFrame& frame) {
    Impl& s = *impl_;
    if (!s.format || s.video_eof) return false;
    bool framed = false;
    while (!framed) {
        if (av_read_frame(s.format, s.packet) < 0) {
            // End of the stream: flush the decoder once, then report EOF.
            if (s.flushed || avcodec_send_packet(s.video_codec, nullptr) < 0 ||
                avcodec_receive_frame(s.video_codec, s.decoded) != 0) {
                s.video_eof = true;
                return false;
            }
            s.flushed = true;
            framed = true;
            break;
        }
        if (s.packet->stream_index != s.video_stream) {
            av_packet_unref(s.packet);
            continue;
        }
        if (avcodec_send_packet(s.video_codec, s.packet) < 0) {
            av_packet_unref(s.packet);
            continue;
        }
        av_packet_unref(s.packet);
        framed = avcodec_receive_frame(s.video_codec, s.decoded) == 0;
    }
    if (!s.scaler) {
        s.scaler = sws_getContext(s.decoded->width, s.decoded->height, AVPixelFormat(s.decoded->format),
                                  s.decoded->width, s.decoded->height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr,
                                  nullptr, nullptr);
        if (!s.scaler) {
            s.error = "cannot create the scaler";
            return false;
        }
    }
    frame.width = s.decoded->width;
    frame.height = s.decoded->height;
    frame.rgba.resize(std::size_t(frame.width) * std::size_t(frame.height));
    std::uint8_t* dst[1] = {reinterpret_cast<std::uint8_t*>(frame.rgba.data())};
    const int stride[1] = {frame.width * 4};
    sws_scale(s.scaler, s.decoded->data, s.decoded->linesize, 0, s.decoded->height, dst, stride);
    // libswscale writes RGBA (R first); our pixels keep R in the low byte on little-endian.
    frame.pts = s.decoded->best_effort_timestamp == AV_NOPTS_VALUE
                    ? 0
                    : double(s.decoded->best_effort_timestamp) * s.time_base;
    return true;
}

bool MoviePlayer::next_audio(std::vector<std::int16_t>& pcm, double seconds) {
    Impl& s = *impl_;
    if (!s.audio_info.present) return false;
    const std::size_t channels = std::size_t(s.audio.channels);
    const std::size_t want = std::size_t(double(s.audio.sample_rate) * seconds) * channels;
    const std::size_t have = s.audio.pcm.size() - std::min(s.audio.position, s.audio.pcm.size());
    std::size_t n = std::min(want, have);
    n -= n % channels;  // keep frame alignment so `position` always advances
    if (n == 0) return false;
    const std::size_t at = s.audio.position;
    pcm.assign(s.audio.pcm.begin() + long(at), s.audio.pcm.begin() + long(at + n));
    s.audio.position += n;
    return true;
}

}  // namespace nf::media

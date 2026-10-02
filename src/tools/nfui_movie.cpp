// nfui movie: PSS playback (MOVIES/30_FPS/*.PSS) with video on the canvas and PCM audio
// through a short-lived SDL device.
//   nfui <gamedir> movie <id-or-path> [--at SEC] [--stats] [--shot out.bmp] [--press ...]
// <id> is a hex movie id (e.g. 0x73B0048) resolved under <gamedir>/MOVIES/30_FPS/%08X.PSS.
// --stats decodes everything headless (frames, duration, audio seconds) for verification;
// otherwise a window plays with A/V sync (Back quits) or --shot captures --at seconds.
#include <cmath>
#include <cstdio>
#include <stdexcept>

#include <SDL3/SDL.h>

#include "media/movie_player.hpp"
#include "tools/nfui_scene.hpp"

namespace nf {

namespace {

std::string resolve_movie(const std::string& gamedir, const std::string& what) {
    if (what.size() > 4 && (what.compare(what.size() - 4, 4, ".PSS") == 0 || what.compare(what.size() - 4, 4, ".pss") == 0))
        return what;
    // Movie ids are hex (`psiStartBackgroundMovie("%s%s%8.8x.pss%s")`); accept `0x` too.
    const std::uint32_t id = std::uint32_t(std::stoul(what, nullptr, 16));
    char name[16];
    std::snprintf(name, sizeof(name), "%08X.PSS", id);
    return gamedir + "/MOVIES/30_FPS/" + name;
}

class MovieScene : public Scene {
public:
    explicit MovieScene(SceneArgs& args) : gamedir_(args.gamedir) {
        std::string what;
        for (std::size_t i = 0; i < args.extra.size(); ++i) {
            const std::string& a = args.extra[i];
            if (a == "--at" && i + 1 < args.extra.size()) at_ = std::stod(args.extra[++i]);
            else if (a == "--stats") stats_ = true;
            else if (!a.empty() && a[0] != '-') what = a;
            else throw std::runtime_error("movie: unknown option " + a);
        }
        if (what.empty()) throw std::runtime_error("movie: need a movie id or .PSS path");
        path_ = resolve_movie(args.gamedir, what);
        if (!player_.open(path_)) throw std::runtime_error("movie: " + player_.error());
        // Audio device (best effort: headless environments may have none; time still advances).
        if (player_.audio().present) {
            SDL_AudioSpec spec{};
            spec.format = SDL_AUDIO_S16;
            spec.channels = player_.audio().channels;
            spec.freq = player_.audio().sample_rate;
            audio_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
            if (audio_) SDL_ResumeAudioStreamDevice(audio_);
        }
        // Seek the headless shot/stats position by decoding (movies are short).
        if (at_ > 0 || stats_) {
            double target = stats_ ? 1e9 : at_;
            media::MovieFrame frame;
            while (player_.next_video(frame)) {
                ++frames_;
                duration_ = frame.pts;
                if (!stats_ && frame.pts >= target) break;
                shot_ = std::move(frame);
            }
            if (!stats_) {
                // Drain the audio clock to the shot position too.
                std::vector<std::int16_t> pcm;
                while (player_.audio_position() < target && player_.next_audio(pcm, 0.5)) {
                }
            }
        }
        if (stats_) {
            std::vector<std::int16_t> pcm;
            while (player_.next_audio(pcm, 1.0)) {
            }
            std::printf("movie %s: %d frames %.2fs video", path_.c_str(), frames_, duration_);
            if (player_.audio().present)
                std::printf(", %.2fs audio (%d Hz x%d)", player_.audio_position(),
                            player_.audio().sample_rate, player_.audio().channels);
            std::printf("\n");
        }
    }

    ~MovieScene() override {
        if (audio_) SDL_DestroyAudioStream(audio_);
    }

    void update(const PadHistory& pad) override {
        if (finished_) return;
        if (pad.pressed(kPadCircle) || pad.pressed(kPadTriangle) || pad.pressed(kPadStart)) {
            finished_ = true;
            return;
        }
        // Keep ~0.25 s of audio queued; the audio clock drives the video.
        if (audio_ && !audio_eof_) {
            const int queued = SDL_GetAudioStreamQueued(audio_);
            const int bytes_per_sec = player_.audio().sample_rate * player_.audio().channels * 2;
            if (bytes_per_sec > 0 && queued < bytes_per_sec / 4) {
                std::vector<std::int16_t> pcm;
                if (player_.next_audio(pcm, 0.25))
                    SDL_PutAudioStreamData(audio_, pcm.data(), int(pcm.size() * 2));
                else
                    audio_eof_ = true;
            }
        }
        const double clock = clock_seconds();
        // Advance video while the next frame is due (pending_ holds the future frame).
        for (;;) {
            if (!pending_valid_) {
                if (!player_.next_video(pending_)) {
                    video_eof_ = true;
                    break;
                }
                pending_valid_ = true;
            }
            if (pending_.pts <= clock + 1e-3) {
                shot_ = std::move(pending_);
                pending_valid_ = false;
            } else {
                break;
            }
        }
        if (video_eof_ && !pending_valid_ && (!audio_ || audio_eof_)) finished_ = true;
    }

    void draw(ui::Renderer& renderer, ui::TextRenderer&) override {
        if (shot_.rgba.empty()) return;
        renderer.draw_frame({0, 0, ui::kScreenW, ui::kScreenH}, shot_.width, shot_.height, shot_.rgba.data());
    }

    bool finished() const override { return finished_; }

    double clock_seconds() const {
        if (audio_ && player_.audio().present && !audio_eof_) {
            const int queued = SDL_GetAudioStreamQueued(audio_);
            const int bytes_per_sec = player_.audio().sample_rate * player_.audio().channels * 2;
            if (bytes_per_sec > 0) return player_.audio_position() - double(queued) / double(bytes_per_sec);
        }
        return double(frames_shown_++) / 30.0;
    }

    std::string gamedir_, path_;
    double at_ = 0;
    bool stats_ = false, finished_ = false, audio_eof_ = false, video_eof_ = false;
    media::MoviePlayer player_;
    media::MovieFrame shot_, pending_;
    bool pending_valid_ = false;
    int frames_ = 0;
    double duration_ = 0;
    mutable int frames_shown_ = 0;
    SDL_AudioStream* audio_ = nullptr;
};

}  // namespace

std::unique_ptr<Scene> make_movie_scene(SceneArgs& args) { return std::make_unique<MovieScene>(args); }

int run_movie_stats(const std::string& gamedir, const std::vector<std::string>& extra) {
    std::string what;
    for (const std::string& a : extra)
        if (!a.empty() && a[0] != '-') what = a;
    if (what.empty()) {
        std::fprintf(stderr, "movie: need a movie id or .PSS path\n");
        return 2;
    }
    media::MoviePlayer player;
    if (!player.open(resolve_movie(gamedir, what))) {
        std::fprintf(stderr, "movie: %s\n", player.error().c_str());
        return 1;
    }
    int frames = 0;
    double duration = 0;
    media::MovieFrame frame;
    while (player.next_video(frame)) {
        ++frames;
        duration = std::max(duration, frame.pts);
    }
    std::vector<std::int16_t> pcm;
    while (player.next_audio(pcm, 1.0)) {
    }
    std::printf("movie %s: %dx%d %.2ffps %d frames %.2fs video", what.c_str(), player.video().width,
                player.video().height, player.video().fps, frames, duration);
    if (player.audio().present)
        std::printf(", %.2fs audio (%d Hz x%d)", player.audio_position(), player.audio().sample_rate,
                    player.audio().channels);
    std::printf("\n");
    return 0;
}

}  // namespace nf

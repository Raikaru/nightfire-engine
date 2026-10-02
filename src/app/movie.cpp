// Fullscreen PSS playback: video pacing, PCM audio, skip + headless shots.
#include "app/movie.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <deque>
#include <mutex>

#include "audio/audio.hpp"
#include "audio/pcm_sink.hpp"
#include "game/input.hpp"
#include "media/movie_player.hpp"

namespace nf::app {

struct MovieScreen::Impl {
    Impl(Window& w, ui::Renderer& u, ui::TextRenderer& t, audio::AudioSystem* m, SDL_Gamepad* g, std::string dir)
        : window(w), ui(u), text(t), music(m), gamepad(g), gamedir(std::move(dir)) {}

    Window& window;
    ui::Renderer& ui;
    ui::TextRenderer& text;
    audio::AudioSystem* music;
    SDL_Gamepad* gamepad;
    std::string gamedir;

    static std::string path_for(const std::string& gamedir, std::uint32_t id) {
        char name[16];
        std::snprintf(name, sizeof(name), "%08X.PSS", id);
        return gamedir + "/MOVIES/30_FPS/" + name;
    }

    // Queued PCM for the sink callback (filled by next_audio, drained on the audio thread).
    std::mutex pcm_mutex;
    std::deque<std::int16_t> pcm_queue;
    bool audio_eof = false;

    PadState menu_pad() {
        PadState s;
        const bool* k = SDL_GetKeyboardState(nullptr);
        if (k[SDL_SCANCODE_Z] || k[SDL_SCANCODE_RETURN] || k[SDL_SCANCODE_SPACE]) s.buttons |= kPadCross;
        if (k[SDL_SCANCODE_X] || k[SDL_SCANCODE_ESCAPE]) s.buttons |= kPadCircle;
        if (k[SDL_SCANCODE_BACKSPACE]) s.buttons |= kPadSelect;
        if (gamepad) {
            if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_SOUTH)) s.buttons |= kPadCross;
            if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_EAST)) s.buttons |= kPadCircle;
            if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_START)) s.buttons |= kPadStart;
            if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_BACK)) s.buttons |= kPadSelect;
        }
        return s;
    }
};

MovieScreen::MovieScreen(Window& window, ui::Renderer& ui, ui::TextRenderer& text, audio::AudioSystem* music,
                         SDL_Gamepad* gamepad, const std::string& gamedir)
    : impl_(std::make_unique<Impl>(window, ui, text, music, gamepad, gamedir)) {}

MovieScreen::~MovieScreen() = default;

bool MovieScreen::play(std::uint32_t id, const std::string& shot, long max_frames) {
    Impl& s = *impl_;
    media::MoviePlayer player;
    const std::string path = Impl::path_for(s.gamedir, id);
    if (!player.open(path)) {
        std::printf("movie %08X: %s (fallback transition)\n", id, player.error().c_str());
        return false;
    }
    std::printf("movie %08X: %dx%d %.1fs%s\n", id, player.video().width, player.video().height,
                player.video().duration, player.audio().present ? " +audio" : "");
    if (s.music) s.music->pause_music(true);
    struct MusicResume {
        audio::AudioSystem* m;
        ~MusicResume() {
            if (m) m->pause_music(false);
        }
    } resume{s.music};

    // PCM through the audio/ sink (best effort: headless may have no device; time still advances).
    std::unique_ptr<audio::PcmSink> sink;
    if (player.audio().present && shot.empty()) {
        auto fill = [&s](std::int16_t* out, std::size_t frames) {
            std::lock_guard<std::mutex> lock(s.pcm_mutex);
            for (std::size_t i = 0; i < frames * 2; ++i) {
                if (s.pcm_queue.empty()) {
                    out[i] = 0;
                    continue;
                }
                out[i] = s.pcm_queue.front();
                s.pcm_queue.pop_front();
            }
        };
        sink = std::make_unique<audio::PcmSink>(std::uint32_t(player.audio().sample_rate), fill);
        if (!sink->open()) {
            std::printf("movie: no audio device (%s)\n", sink->error().c_str());
            sink.reset();
        }
    }

    media::MovieFrame frame, pending;
    bool pending_valid = false, video_eof = false, done = false;
    long shown = 0;
    // Playback clock: the movie position everything shows up to. The old
    // shown/30 self-gate stalls forever on files whose first pts is not ~0
    // (07380048 starts at 0.0465s): pending stays valid, shown never advances,
    // EOF is never reached. Pace by wall time instead, tracking the fetched
    // audio position while the sink is live and fresh (A/V stay together);
    // muted, device-less, audio-finished or stalled-audio runs use the wall.
    const Uint64 t0 = SDL_GetTicksNS();
    PadHistory hist;
    while (!done) {
        // Keep ~0.25 s of PCM queued ahead of the sink, throttled to realtime so the
        // fetched audio position is a usable clock (unthrottled it races at loop speed).
        const double wall = double(SDL_GetTicksNS() - t0) * 1e-9;
        if (sink && !s.audio_eof) {
            std::size_t queued = 0;
            {
                std::lock_guard<std::mutex> lock(s.pcm_mutex);
                queued = s.pcm_queue.size() / 2;
            }
            const std::size_t want = std::size_t(player.audio().sample_rate / 4);
            if (queued < want && player.audio_position() < wall + 0.25) {
                std::vector<std::int16_t> pcm;
                if (player.next_audio(pcm, 0.25)) {
                    std::lock_guard<std::mutex> lock(s.pcm_mutex);
                    s.pcm_queue.insert(s.pcm_queue.end(), pcm.begin(), pcm.end());
                } else {
                    s.audio_eof = true;
                }
            }
        }
        if (!shot.empty()) {
            // Headless: take frames in decode order (no clock), bounded by max_frames/EOF.
            if (player.next_video(frame)) {
                ++shown;
                if (max_frames >= 0 && shown >= max_frames) done = true;
            } else {
                video_eof = true;
                done = true;
            }
        } else {
            // Interactive: show everything due up to the clock. The fetched audio
            // position paces the video while the sink is live and fresh; otherwise
            // (muted, no device, audio finished, or audio stalled > 2 s behind the
            // wall) the wall clock paces, so playback always advances.
            double clock = wall;
            if (sink && !s.audio_eof && wall - player.audio_position() < 2.0)
                clock = player.audio_position();
            for (;;) {
                if (!pending_valid) {
                    if (!player.next_video(pending)) {
                        video_eof = true;
                        break;
                    }
                    pending_valid = true;
                }
                if (pending.pts <= clock + 1e-3) {
                    frame = std::move(pending);
                    pending_valid = false;
                    ++shown;
                } else {
                    break;
                }
            }
            if (max_frames >= 0 && shown >= max_frames) done = true;
        }
        if (!shot.empty()) {
            if (video_eof || (max_frames >= 0 && shown >= max_frames)) done = true;
        } else {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_EVENT_QUIT) return true;
            }
            hist.push(s.menu_pad());
            // Skip on fresh Cross/Circle/Start/Select presses, like the menu movie pages.
            if (hist.pressed(kPadCross) || hist.pressed(kPadCircle) || hist.pressed(kPadStart) ||
                hist.pressed(kPadSelect))
                done = true;
            if (video_eof && !pending_valid) done = true;
        }
        int w, h;
        s.window.begin_frame(w, h);
        s.ui.begin(w, h);
        if (!frame.rgba.empty())
            s.ui.draw_frame({0, 0, ui::kScreenW, ui::kScreenH}, frame.width, frame.height, frame.rgba.data());
        s.ui.end();
        s.window.swap();
        if (shot.empty()) SDL_Delay(5);
    }
    if (!shot.empty()) {
        const bool ok = s.window.save_bmp(shot);
        std::printf("movie shot -> %s\n", ok ? shot.c_str() : SDL_GetError());
        if (!ok) throw std::runtime_error("cannot write shot");
    }
    return true;
}

}  // namespace nf::app

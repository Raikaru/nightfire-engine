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
    double clock = 0;
    long shown = 0;
    PadHistory hist;
    while (!done) {
        // Keep ~0.25 s of PCM queued ahead of the sink.
        if (sink && !s.audio_eof) {
            std::size_t queued = 0;
            {
                std::lock_guard<std::mutex> lock(s.pcm_mutex);
                queued = s.pcm_queue.size() / 2;
            }
            const std::size_t want = std::size_t(player.audio().sample_rate / 4);
            if (queued < want) {
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
            clock = double(shown) / 30.0;
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
        // Audio clock follows the sink when present.
        if (sink) clock = player.audio_position();
        else if (shot.empty()) clock = double(shown) / 30.0;
    }
    if (!shot.empty()) {
        const bool ok = s.window.save_bmp(shot);
        std::printf("movie shot -> %s\n", ok ? shot.c_str() : SDL_GetError());
        if (!ok) throw std::runtime_error("cannot write shot");
    }
    return true;
}

}  // namespace nf::app

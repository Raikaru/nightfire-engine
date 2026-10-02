// Fullscreen PSS movie playback for the `nightfire` app (boot intros, the
// frontend's take_movie_request pages, the results wingame): video on the UI
// canvas, PCM through an audio/ PcmSink with the game music paused, Back/
// Cross/Triangle/Start skips. Missing files fall back (returns false, the
// caller runs movie_finished() so the menu takes its fallback transition).
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "render/window.hpp"
#include "ui/renderer.hpp"
#include "ui/text.hpp"

struct SDL_Gamepad;

namespace nf {
namespace audio {
class AudioSystem;
}
namespace app {

class MovieScreen {
public:
    // `music` (may be null) is paused during playback and resumed after, so the
    // movie does not play over the menu music. Everything must outlive the call.
    MovieScreen(Window& window, ui::Renderer& ui, ui::TextRenderer& text, audio::AudioSystem* music,
                SDL_Gamepad* gamepad, const std::string& gamedir);
    ~MovieScreen();

    MovieScreen(const MovieScreen&) = delete;
    MovieScreen& operator=(const MovieScreen&) = delete;

    // Plays MOVIES/30_FPS/%08X.PSS. Interactive: skippable, ends at EOF.
    // Headless (`shot` set): decodes up to `max_frames` (< 0 = to EOF, bounded
    // by the file) with the audio clock simulated, then saves the frame.
    // Returns false when the file is missing/unplayable (caller falls back).
    bool play(std::uint32_t id, const std::string& shot = {}, long max_frames = -1);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace app
}  // namespace nf

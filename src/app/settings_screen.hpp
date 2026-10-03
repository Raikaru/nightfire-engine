#pragma once

#include <string>

#include "app/app.hpp"
#include "render/window.hpp"
#include "ui/accessibility.hpp"
#include "ui/renderer.hpp"
#include "ui/text.hpp"

namespace nf::audio {
class AudioSystem;
}

namespace nf::app {

// The accessibility options of `cfg` (ui::set_accessibility applies them process-wide).
ui::Accessibility accessibility_from_config(const AppConfig& cfg);
// Applies the display, audio and accessibility settings of `cfg` to the running window, renderer and menu audio.
void apply_settings(const AppConfig& cfg, Window& window, ui::Renderer& renderer, audio::AudioSystem* audio);

// The Settings screen (main menu Square): Graphics, Widescreen, Audio and Accessibility pages drawn in the front
// end's multiplayer-page style. Changes apply at once and are saved to nightfire.cfg when the screen closes.
// `press` replays `set-up`, `set-down`, `set-left`, `set-right`, `set-cross` and `set-circle` tokens (other tokens are
// skipped); with a script the screen draws once, saves `shot` when given, and returns.
void run_settings(const AppContext& ctx, Window& window, ui::Renderer& renderer, ui::TextRenderer& text,
                  AppConfig& cfg, audio::AudioSystem* audio, const std::string& press, const std::string& shot);

}  // namespace nf::app

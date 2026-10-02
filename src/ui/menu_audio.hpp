// Menu sounds and menu music for the front end (Menu_PlaySound, credits/attract music).
// Header-only; the game supplies the actual playback as a callback so nf_ui needs no audio
// dependency (see docs/ui.md "Menu sounds and music").
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "ui/menu.hpp"

namespace nf {

// Menu_PlaySound (ACTION.ELF): every menu effect is one of three 2D SFX ids at volume 100
// (Sound_Play(100.0, id, 0, 0)); Memo (9) is silent. Accept/Back/Alt/PageBack only sound when
// their Menu_EnableSounds bit is set (mask bits 1/2/4/8); the rest always sound.
inline std::uint32_t menu_sound_sfx(ui::MenuSound sound, unsigned enable_mask = 0xF) {
    using S = ui::MenuSound;
    switch (sound) {
        case S::Accept: return (enable_mask & 1) ? 0x1DA : 0;
        case S::Back: return (enable_mask & 2) ? 0x1DA : 0;
        case S::Alt: return (enable_mask & 4) ? 0x1DA : 0;
        case S::Left: case S::LeftRepeat: return 0x1D8;
        case S::Right: case S::RightRepeat: return 0x1D9;
        case S::Move: return 0x1DA;
        case S::PageBack: return (enable_mask & 8) ? 0x1D9 : 0;
        case S::Memo: return 0;
    }
    return 0;
}

// The menu ambience: the two loops the menu pages start when menu_movie is clear
// (SFXStart(0x1D7) + SFXStart(0x470) in P_ATTRACT/P_INTRO/P_NFRESULTS/P_MPDEBRIEFING/...).
// Play both once when the frontend opens.
constexpr std::uint32_t kMenuAmbientA = 0x1D7, kMenuAmbientB = 0x470;
// P_CREDITS starts music track 0x27 (SFXStartMusic) and restores the music volume after.
constexpr std::uint32_t kCreditsMusicTrack = 0x27;

// Plays every sound in `sounds` (e.g. Frontend::take_sounds()) through `play` (which should call
// AudioSystem::play_sfx(id) with the frontend bank loaded). Silent ids are skipped.
inline void pump_menu_sounds(const std::vector<ui::MenuSound>& sounds,
                             const std::function<void(std::uint32_t)>& play, unsigned enable_mask = 0xF) {
    for (ui::MenuSound s : sounds)
        if (std::uint32_t id = menu_sound_sfx(s, enable_mask)) play(id);
}

}  // namespace nf

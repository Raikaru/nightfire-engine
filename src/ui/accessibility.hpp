#pragma once

#include <cstdint>

namespace nf::ui {

// Opt-in presentation options from the Settings screen (nightfire.cfg). The defaults draw the original game.
enum class CrosshairStyle : int {
    Original = 0,  // HUDCrossCoords row of the weapon (the game's crosshair)
    Cross = 1,     // four open ticks
    Dot = 2,       // a single dot
    Ring = 3,      // ring with a centre dot
    Chevron = 4,   // an upward chevron
};
constexpr int kCrosshairStyleCount = 5;

struct Accessibility {
    CrosshairStyle crosshair = CrosshairStyle::Original;
    bool high_contrast = false;     // HUD text and menu prompts on dark plates with a solid outline
    bool colorblind_teams = false;  // Phoenix / MI6 in a colour-blind-safe pair, team shapes on the radar
};

// The process-wide options (set by the application at start-up and whenever the Settings screen changes them).
const Accessibility& accessibility();
void set_accessibility(const Accessibility& options);

// Team colours (0xRRGGBBAA, full-scale bytes): the game's red / blue, or vermillion / sky blue (Okabe-Ito), which stay
// distinct under protan, deutan and tritan colour vision. `team` 0 Phoenix, 1 MI6, anything else neutral grey.
std::uint32_t team_color(int team);
// The same for colour words the game builds on the GS scale (0x80 = 1.0): maps the two team words the arena and
// HUD use when the colour-blind option is on and returns `word` unchanged otherwise.
std::uint32_t remap_team_word(std::uint32_t word);

}  // namespace nf::ui

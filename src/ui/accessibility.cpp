#include "ui/accessibility.hpp"

namespace nf::ui {

namespace {
Accessibility g_options;
}  // namespace

const Accessibility& accessibility() { return g_options; }
void set_accessibility(const Accessibility& options) { g_options = options; }

std::uint32_t team_color(int team) {
    const bool safe = g_options.colorblind_teams;
    if (team == 0) return safe ? 0xE69F00FF : 0xD22D35FF;   // Phoenix: Okabe-Ito orange / the arena's red
    if (team == 1) return safe ? 0x56B4E9FF : 0x2D61D2FF;   // MI6: Okabe-Ito sky blue / the arena's blue
    return 0x9C9A9CFF;
}

std::uint32_t remap_team_word(std::uint32_t word) {
    if (!g_options.colorblind_teams) return word;
    // GS words (0x80 = 1.0): Okabe-Ito orange (230, 159, 0) and sky blue (86, 180, 233), and darker tag shades.
    switch (word) {
        case 0xD22D35FF:   // arena radar blips (MP_GetRadarObjects team colours)
        case 0xFF0000FF:   // score pane shadow (HUD_MPUpdatePane)
            return 0x734F00FF;
        case 0x2D61D2FF:
        case 0x0000FFFF:
            return 0x2B5A75FF;
        case 0x5A1414FF:   // radar name tags
            return 0x5A3E00FF;
        case 0x14145AFF:
            return 0x153F5CFF;
        default:
            return word;
    }
}

std::uint32_t player_color(int slot) {
    // Lightness-balanced hues (no navy / maroon / black: each must read on the translucent black plates).
    static constexpr std::uint32_t kDistinct[kPlayerColorCount] = {
        0xE6194BFF, 0x3CB44BFF, 0xFFE119FF, 0x4F7BEAFF, 0xF58231FF, 0xB04CE0FF, 0x42D4F4FF, 0xF032E6FF,
        0xBFEF45FF, 0xFABED4FF, 0x4FB3A9FF, 0xDCBEFFFF, 0xC08A3EFF, 0xFFFAC8FF, 0xAAFFC3FF, 0xB5B53AFF};
    // Okabe-Ito, with blue lifted and black replaced by white for the dark background.
    static constexpr std::uint32_t kSafe[8] = {0xE69F00FF, 0x56B4E9FF, 0x009E73FF, 0xF0E442FF,
                                               0x3A8FD6FF, 0xD55E00FF, 0xCC79A7FF, 0xF2F2F2FF};
    if (slot < 0) return 0x9C9A9CFF;
    return g_options.colorblind_teams ? kSafe[slot % 8] : kDistinct[slot % kPlayerColorCount];
}

bool player_hollow(int slot) { return g_options.colorblind_teams && slot >= 8; }

}  // namespace nf::ui

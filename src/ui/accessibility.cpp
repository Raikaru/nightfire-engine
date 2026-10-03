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
    switch (word) {
        case 0xD22D35FF:   // arena radar blips (MP_GetRadarObjects team colours)
        case 0xFF0000FF:   // score pane shadow (HUD_MPUpdatePane)
            return 0xFF9F00FF;
        case 0x2D61D2FF:
        case 0x0000FFFF:
            return 0x00A0FFFF;
        case 0x5A1414FF:   // radar name tags
            return 0x734F00FF;
        case 0x14145AFF:
            return 0x0F4F80FF;
        default:
            return word;
    }
}

}  // namespace nf::ui

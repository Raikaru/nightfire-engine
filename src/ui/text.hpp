#pragma once

#include <cstdint>
#include <string_view>

#include "assets/ui_fonts.hpp"
#include "ui/renderer.hpp"

namespace nf::ui {

enum class Align { Left, Center, Right };

// Font_DrawText's parameters. Colours are 0xRRGGBBAA words on the GS 0x80 = 1.0 scale.
struct TextStyle {
    int font = 1;                         // 1 NFont2, 2 SerpLight, 3 Medium (FontTable)
    std::uint32_t color = 0x7F7F7FFF;
    std::uint32_t shadow_color = 0x00000080;   // outline / drop shadow colour (label default: black)
    std::uint32_t highlight_color = 0xFFFFFFFF;
    bool drop_shadow = false;             // +1,+1 copy in shadow_color            (draw flag 0x1000)
    bool outline = false;                 // four +-1 copies in shadow_color       (draw flag 0x4000)
    bool highlight = false;               // -1,-1 copy in highlight_color         (draw flag 0x2000)
    Align align = Align::Left;
    float scale_x = 1, scale_y = 1;       // GPOINT
};

struct TextMetrics {
    float width = 0;   // widest line
    float height = 0;  // all lines including the trailing line gap
};

// Applies a menu format string (Font_ParseFormat): 0xFA r g b a (ignored: glyph colours come from the
// draw call), 0xFB/0xFC n (unused), 0xFE 2|3 = right|centre alignment, 0xFF n = font number.
TextStyle apply_format(TextStyle base, std::string_view format);
// Font_GetAlignment: the alignment a format string selects.
Align format_alignment(std::string_view format);

// Draws strings with the original bitmap fonts (Font_DrawText / Font_GetTextExtent). Text is raw
// game-encoded bytes: '\n' starts a new line, 0xA0 is a wide space, 0x99 draws a half-size "TM",
// `~X` inserts button icon X (`specialchar`), 0xBA toggles the highlight colours.
class TextRenderer {
public:
    TextRenderer(Renderer& renderer, const FontSet& fonts) : renderer_(&renderer), fonts_(fonts) {}
    // Measure-only instance (menu layout runs outside the draw pass); draw() requires a renderer.
    explicit TextRenderer(const FontSet& fonts) : fonts_(fonts) {}

    // Extent of `text` at scale 1 (or style.scale_*); draws nothing.
    TextMetrics measure(std::string_view text, const TextStyle& style) const;
    // (x, y) is the anchor the game passes: left edge (or centre / right edge for Center / Right
    // alignment) and the line reference y (see docs/formats.md). Returns the metrics of the text.
    TextMetrics draw(float x, float y, std::string_view text, const TextStyle& style);

    const FontSet& fonts() const { return fonts_; }

private:
    TextMetrics layout(float x, float y, std::string_view text, const TextStyle& style, Color color, bool emit);

    Renderer* renderer_ = nullptr;
    const FontSet& fonts_;
};

}  // namespace nf::ui

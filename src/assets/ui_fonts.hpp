#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "assets/elf.hpp"

namespace nf {

// Bitmap fonts live in ACTION.ELF's data segment (there is no font file on the disc). Each font is
// a `FONT<name>` descriptor plus a glyph table and a kerning table; glyph rectangles index a
// 512x128 (or 512x64) sprite texture registered under the font's texture hash.
//
//   descriptor (0x1C bytes): u32 kern_count; u32 kern_ptr; u16 first_char, last_char; u32 glyph_count;
//                            u32 glyph_ptr; f32 line_gap; f32 space_width
//   glyph (0x18 bytes):      u16 u, v, w, h; i16 y_offset, lead, trail; u16 kern_count; u32 kern_ptr;
//                            u16 code; u16 pad
//   kern pair (6 bytes):     u16 first (the owning glyph's code); u16 second; i8 amount; u8 pad
//
// (Font_DrawText, Font_GetKernAdjust, Font_ParseFormat, `FontTable`, `specialchar`.)
struct Glyph {
    std::uint16_t u, v, w, h;      // texel rectangle in the font texture
    std::int16_t y_offset;         // vertical placement relative to the line
    std::int16_t lead, trail;      // extra advance before (skipped on the first char) and after the glyph
    std::uint16_t code;            // character code, the table is sorted by it
    // Kerning applied when a glyph follows this one: (following code, amount in texels).
    std::vector<std::pair<std::uint16_t, std::int8_t>> kern_after;
};

struct Font {
    std::uint32_t texture_hash = 0;
    std::uint16_t first_char = 0, last_char = 0;
    float line_gap = 0, space_width = 0;
    std::vector<Glyph> glyphs;  // sorted by code

    const Glyph* find(std::uint32_t code) const;
    // Kerning between `prev` (0 = none) and `cur`.
    int kern(std::uint32_t prev, std::uint32_t cur) const;
};

// Inline button icon drawn for a `~X` escape in a string (`specialchar`, 0x18 bytes per entry).
struct SpecialChar {
    char key;                      // the character after '~'
    std::uint32_t texture_hash;
    std::uint16_t tex_width, tex_height;
    std::uint16_t u, v, w, h;      // texel rectangle in the icon texture
    std::int16_t advance, height;  // on-screen size at scale 1
};

// `FontTable`: font numbers 1..3 in a format string's 0xFF <n> escape.
struct FontSet {
    std::array<Font, 3> fonts;  // [0] NFont2 (1), [1] SerpLight (2), [2] Medium (3)
    std::vector<SpecialChar> special;

    const Font& font(int number) const { return fonts.at(std::size_t(number - 1)); }
    const SpecialChar* find_special(char key) const;
};

FontSet load_fonts(const Elf32& elf);

}  // namespace nf

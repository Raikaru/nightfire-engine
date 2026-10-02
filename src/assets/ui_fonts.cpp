#include "assets/ui_fonts.hpp"

#include <algorithm>

#include "assets/reader.hpp"

namespace nf {

namespace {

constexpr std::size_t kDescriptorSize = 0x1C;
constexpr std::size_t kGlyphSize = 0x18;
constexpr std::size_t kKernSize = 6;
constexpr std::size_t kSpecialSize = 0x18;

Font load_font(const Elf32& elf, std::uint32_t texture_hash, std::uint32_t descriptor) {
    Bytes d = elf.at(descriptor, kDescriptorSize);
    Font font;
    font.texture_hash = texture_hash;
    auto kern_count = load<std::uint32_t>(d, 0);
    auto kern_ptr = load<std::uint32_t>(d, 4);
    font.first_char = load<std::uint16_t>(d, 8);
    font.last_char = load<std::uint16_t>(d, 10);
    auto glyph_count = load<std::uint32_t>(d, 12);
    auto glyph_ptr = load<std::uint32_t>(d, 16);
    font.line_gap = load<float>(d, 20);
    font.space_width = load<float>(d, 24);

    Bytes kerns = elf.at(kern_ptr, kern_count * kKernSize);
    Bytes glyphs = elf.at(glyph_ptr, glyph_count * kGlyphSize);
    for (std::uint32_t i = 0; i < glyph_count; ++i) {
        std::size_t o = i * kGlyphSize;
        Glyph g;
        g.u = load<std::uint16_t>(glyphs, o);
        g.v = load<std::uint16_t>(glyphs, o + 2);
        g.w = load<std::uint16_t>(glyphs, o + 4);
        g.h = load<std::uint16_t>(glyphs, o + 6);
        g.y_offset = load<std::int16_t>(glyphs, o + 8);
        g.lead = load<std::int16_t>(glyphs, o + 10);
        g.trail = load<std::int16_t>(glyphs, o + 12);
        auto n = load<std::uint16_t>(glyphs, o + 14);
        auto ptr = load<std::uint32_t>(glyphs, o + 16);
        g.code = load<std::uint16_t>(glyphs, o + 20);
        if (g.code < font.first_char || g.code > font.last_char) throw FormatError("glyph code outside font range");
        if (i && g.code <= font.glyphs.back().code) throw FormatError("glyph table not sorted by code");
        if (g.u + g.w > 512 || g.v + g.h > 128) throw FormatError("glyph rectangle outside the 512x128 font texture");
        if (n) {
            if (ptr < kern_ptr || ptr + n * kKernSize > kern_ptr + kern_count * kKernSize)
                throw FormatError("glyph kerning list outside the kerning table");
            std::size_t k = ptr - kern_ptr;
            for (std::uint16_t j = 0; j < n; ++j, k += kKernSize) {
                // Font_GetKernAdjust(cur, prev) walks the list owned by `prev` for `cur`.
                if (load<std::uint16_t>(kerns, k) != g.code) throw FormatError("kerning pair owned by another glyph");
                g.kern_after.emplace_back(load<std::uint16_t>(kerns, k + 2), load<std::int8_t>(kerns, k + 4));
            }
        }
        font.glyphs.push_back(std::move(g));
    }
    if (font.glyphs.empty()) throw FormatError("font has no glyphs");
    return font;
}

}  // namespace

const Glyph* Font::find(std::uint32_t code) const {
    auto it = std::lower_bound(glyphs.begin(), glyphs.end(), code,
                               [](const Glyph& g, std::uint32_t c) { return g.code < c; });
    return it != glyphs.end() && it->code == code ? &*it : nullptr;
}

int Font::kern(std::uint32_t prev, std::uint32_t cur) const {
    if (!prev) return 0;
    const Glyph* g = find(prev);
    if (!g) return 0;
    for (const auto& [second, amount] : g->kern_after)
        if (second == cur) return amount;
    return 0;
}

const SpecialChar* FontSet::find_special(char key) const {
    for (const auto& s : special)
        if (s.key == key) return &s;
    return nullptr;
}

FontSet load_fonts(const Elf32& elf) {
    auto table = elf.symbol("FontTable");
    auto special = elf.symbol("specialchar");
    if (!table || !special) throw FormatError("ACTION.ELF lacks FontTable/specialchar");
    Bytes t = elf.at(table->value, 3 * 8);
    FontSet set;
    for (std::size_t i = 0; i < 3; ++i)
        set.fonts[i] = load_font(elf, load<std::uint32_t>(t, i * 8), load<std::uint32_t>(t, i * 8 + 4));

    Bytes s = elf.at(special->value, special->size);
    for (std::size_t o = 0; o + kSpecialSize <= s.size(); o += kSpecialSize) {
        SpecialChar c;
        c.key = static_cast<char>(s[o]);
        c.texture_hash = load<std::uint32_t>(s, o + 4);
        c.tex_width = load<std::uint16_t>(s, o + 8);
        c.tex_height = load<std::uint16_t>(s, o + 10);
        c.u = load<std::uint16_t>(s, o + 12);
        c.v = load<std::uint16_t>(s, o + 14);
        c.w = load<std::uint16_t>(s, o + 16);
        c.h = load<std::uint16_t>(s, o + 18);
        c.advance = load<std::int16_t>(s, o + 20);
        c.height = load<std::int16_t>(s, o + 22);
        set.special.push_back(c);
    }
    return set;
}

}  // namespace nf

#include "ui/text.hpp"

#include <algorithm>
#include <cctype>

#include "ui/art_sheet.hpp"
#include "ui/prompts.hpp"

namespace nf::ui {

namespace {

constexpr std::uint8_t kFmtColor = 0xFA, kFmtSkip1 = 0xFB, kFmtSkip2 = 0xFC, kFmtAlign = 0xFE, kFmtFont = 0xFF;
constexpr std::uint8_t kHighlightToggle = 0xBA;  // 'º'
constexpr std::uint32_t kHighlightColors[2] = {0x785A14FF, 0x7D6D59FF};

// Walks the escapes of a format string; `on_align` / `on_font` receive their operand.
template <typename Align, typename FontF>
void walk_format(std::string_view f, Align on_align, FontF on_font) {
    for (std::size_t i = 0; i < f.size();) {
        auto c = std::uint8_t(f[i]);
        if (c == kFmtColor) i += 5;
        else if (c == kFmtSkip1 || c == kFmtSkip2) i += 2;
        else if (c == kFmtAlign) on_align(i + 1 < f.size() ? std::uint8_t(f[i + 1]) : 0), i += 2;
        else if (c == kFmtFont) on_font(i + 1 < f.size() ? std::uint8_t(f[i + 1]) : 0), i += 2;
        else ++i;
    }
}

}  // namespace

Align format_alignment(std::string_view format) {
    Align result = Align::Left;
    walk_format(format, [&](std::uint8_t v) { result = v == 3 ? Align::Center : v == 2 ? Align::Right : Align::Left; },
                [](std::uint8_t) {});
    return result;
}

TextStyle apply_format(TextStyle base, std::string_view format) {
    walk_format(format, [&](std::uint8_t v) { base.align = v == 3 ? Align::Center : v == 2 ? Align::Right : Align::Left; },
                [&](std::uint8_t n) {
                    if (n >= 1 && n <= 3) base.font = n;
                });
    return base;
}

TextMetrics TextRenderer::layout(float x0, float y0, std::string_view text, const TextStyle& style, Color color,
                                 bool emit) {
    const Font& font = fonts_.font(style.font);
    const Glyph& g0 = font.glyphs.front();  // line metrics come from the first glyph
    const float sx = style.scale_x, sy = style.scale_y;
    const float line_height = (g0.h + font.line_gap + 1.0f) * sy;

    float x = x0, y = y0, width = 0;
    std::uint32_t prev = 0;
    int highlights = 0;

    // One glyph: kerning against the previous character, lead-in (not before the first character),
    // the quad, then the trail-out advance.
    auto put = [&](const Glyph& g, std::uint32_t code, bool half) {
        float w = g.w * sx * (half ? 0.5f : 1.0f), h = g.h * sy * (half ? 0.5f : 1.0f);
        x += font.kern(prev, code) * sx;
        if (prev) x += g.lead * sx;
        if (emit) {
            Rect dst{x, y + g.y_offset * sy - (g0.h + g0.y_offset) * sy, w, h};
            renderer_->draw(font.texture_hash, dst, {float(g.u), float(g.v), float(g.w), float(g.h)}, color);
        }
        x += g.trail * sx + w;
        width = std::max(width, x - x0);
    };

    for (std::size_t i = 0; i < text.size(); ++i) {
        std::uint32_t c = std::uint8_t(text[i]);
        if (c == '\n') {
            x = x0;
            y += line_height;
        } else if (c == ' ' || c == 0xA0) {
            x += font.space_width * sx * 1.25f;
        } else if (c == kHighlightToggle) {
            if (highlights < 2) color = Color::from_rgba(kHighlightColors[highlights]);
            ++highlights;
        } else if (c == '~') {
            char key = ++i < text.size() ? text[i] : 0;
            int player = -1;   // `~<n>X`: button X on local player n's device (the MP join page)
            if (key >= '1' && key <= '4' && i + 1 < text.size()) player = key - '1', key = text[++i];
            const SpecialChar* s = prompts_ ? (player >= 0 ? prompts_->find(key, player) : prompts_->find(key))
                                            : fonts_.find_special(char(std::toupper(static_cast<unsigned char>(key))));
            if (s) {
                if (emit) {
                    Rect dst{x, y - (s->height + g0.h * sy) * 0.5f, s->advance * sx, s->height * sy};
                    renderer_->draw(s->texture_hash, dst, {float(s->u), float(s->v), float(s->w), float(s->h)},
                                   Color{0x7F, 0x7F, 0x7F, color.a});
                }
                // The original does not scale the icon advance; the engine's own (wider) glyphs advance by their
                // drawn width so keycap labels never run into the following text.
                x += s->texture_hash >= kArtHashBase ? s->advance * sx : float(s->advance);
                width = std::max(width, x - x0);
            }
        } else {
            std::uint32_t code = c == 0x92 ? '\'' : c == 0x85 ? '.' : c;
            if (code == 0x99) {  // trademark sign: half-size "TM"
                for (std::uint32_t ch : {std::uint32_t('T'), std::uint32_t('M')})
                    if (const Glyph* g = font.find(ch)) put(*g, 0x99, true);
            } else if (const Glyph* g = font.find(code)) {
                put(*g, code, false);
            }
        }
        prev = c;
    }
    return {width, (y - y0) + g0.h * sy + font.line_gap + 1.0f};
}

TextMetrics TextRenderer::measure(std::string_view text, const TextStyle& style) const {
    return const_cast<TextRenderer*>(this)->layout(0, 0, text, style, {}, false);
}

TextMetrics TextRenderer::draw(float x, float y, std::string_view text, const TextStyle& style) {
    TextMetrics m = measure(text, style);
    if (style.align == Align::Center) x -= m.width * 0.5f;
    else if (style.align == Align::Right) x -= m.width;

    const Color shadow = Color::from_rgba(style.shadow_color);
    if (style.drop_shadow) {
        layout(x + 1, y + 1, text, style, shadow, true);
    } else if (style.outline) {
        for (float dy : {-1.0f, 1.0f})
            for (float dx : {-1.0f, 1.0f}) layout(x + dx, y + dy, text, style, shadow, true);
    }
    if (style.highlight) layout(x - 1, y - 1, text, style, Color::from_rgba(style.highlight_color), true);
    layout(x, y, text, style, Color::from_rgba(style.color), true);
    return m;
}

}  // namespace nf::ui

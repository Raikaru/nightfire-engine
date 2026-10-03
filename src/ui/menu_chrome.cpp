#include "ui/menu_chrome.hpp"

#include <algorithm>
#include <vector>

#include "ui/menu.hpp"

namespace nf::ui {

namespace {

constexpr float kScaleX = 1.25f;                // MenuManager::draw: the 512-wide buffer onto the canvas
constexpr std::string_view kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWX";   // Label_Update's line extent

// The agent panel texture is 256x128: a 22-texel opaque header strip and 24-texel rounded corners.
// P_MPJOIN draws it into the script box 262x134, which fixup_resolution makes 261.25x125 on the canvas.
constexpr float kPanelTexW = 256.0f, kPanelTexH = 128.0f, kPanelCorner = 24.0f, kPanelHeader = 22.0f;
constexpr float kPanelScaleX = 261.25f / kPanelTexW, kPanelScaleY = 125.0f / kPanelTexH;

}  // namespace

MenuChrome::MenuChrome(Renderer& renderer, TextRenderer& text, const MenuFile& menu)
    : renderer_(renderer), text_(text), menu_(menu), layout_(Layout::from_canvas_width(renderer.canvas_width())),
      left_(layout_.x(authored(50, 0, 540, 1).x, HorizontalAnchor::Left)),
      right_(layout_.x(authored(50, 0, 540, 1).x + authored(50, 0, 540, 1).w, HorizontalAnchor::Right)) {}

Rect MenuChrome::authored(int x, int y, int w, int h) {
    const Box b = fixup_resolution({x, y, w, h}, false);
    return {float(b.x) * kScaleX, float(b.y), float(b.w) * kScaleX, float(b.h)};
}

Rect MenuChrome::span(int y, int h) const {
    Rect r = authored(50, y, 540, h);
    r.x = left_;
    r.w = right_ - left_;
    return r;
}

TextStyle MenuChrome::style(int font, Align align, std::uint32_t color) {
    TextStyle s;
    s.font = font;
    s.align = align;
    s.color = color;
    s.shadow_color = std::min<std::uint32_t>(color & 0xFF, 0x80);
    s.outline = true;
    s.scale_x = kScaleX;
    return s;
}

void MenuChrome::label(Rect box, std::string_view text, int font, Align align, std::uint32_t color) {
    const TextStyle s = style(font, align, color);
    const float text_h = text_.measure(kAlphabet, s).height;
    const float x = align == Align::Center ? box.x + box.w * 0.5f : align == Align::Right ? box.x + box.w : box.x;
    const float y = float(int(float(int(box.y) + (int(box.h) >> 1)) + text_h * 0.5f - 1.0f));
    text_.draw(x, y, text, s);
}

std::string MenuChrome::clip(std::string_view text, int font, float width) const {
    const TextStyle s = style(font, Align::Left, menu_style::kLabelColor);
    std::size_t n = text.size();
    while (n > 0 && text_.measure(text.substr(0, n), s).width > width) --n;
    return std::string(text.substr(0, n));
}

float MenuChrome::text_width(std::string_view text, int font) const {
    return text_.measure(text, style(font, Align::Left, menu_style::kLabelColor)).width;
}

void MenuChrome::title(std::string_view text) {
    Rect box = authored(50, 40, 376, 17);
    box.x = left_;
    label(box, text, 1, Align::Left, menu_style::kLabelColor);
}

void MenuChrome::logo() {
    Rect box = authored(434, 38, 156, 43);
    box.x = right_ - box.w;
    sprite(menu_style::kLogo, box, {0, 0, 256, 128});
}

void MenuChrome::prompts(std::string_view text) {
    label(authored(50, 419, 540, 21), text, 2, Align::Center, menu_style::kLabelColor);
}

float MenuChrome::panel_header_height() { return kPanelHeader * kPanelScaleY; }

void MenuChrome::panel(Rect box, std::string_view header) {
    const float cw = kPanelCorner * kPanelScaleX, ch = kPanelCorner * kPanelScaleY;
    const float src_x[3] = {0, kPanelCorner, kPanelTexW - kPanelCorner};
    const float src_w[3] = {kPanelCorner, kPanelTexW - 2 * kPanelCorner, kPanelCorner};
    const float src_y[3] = {0, kPanelCorner, kPanelTexH - kPanelCorner};
    const float src_h[3] = {kPanelCorner, kPanelTexH - 2 * kPanelCorner, kPanelCorner};
    const float dst_x[3] = {box.x, box.x + cw, box.x + box.w - cw};
    const float dst_w[3] = {cw, box.w - 2 * cw, cw};
    const float dst_y[3] = {box.y, box.y + ch, box.y + box.h - ch};
    const float dst_h[3] = {ch, box.h - 2 * ch, ch};
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            sprite(menu_style::kPanel, {dst_x[column], dst_y[row], dst_w[column], dst_h[row]},
                   {src_x[column], src_y[row], src_w[column], src_h[row]});
    // The header label: script box 70,105 226x16 inside the panel at 50,103.
    if (!header.empty())
        label({box.x + 20.0f, box.y + 2.0f, box.w - 40.0f, 14.0f}, header, 2, Align::Center, menu_style::kLabelColor);
}

void MenuChrome::skin(std::uint16_t set, std::uint16_t component, Rect box, unsigned mask) {
    const MenuComponentSet* skins = menu_.skin(set);
    if (!skins || component >= skins->components.size()) return;
    std::vector<DrawCmd> cmds;
    append_skin(skins->components[component], mask, int(box.x / kScaleX), int(box.y), int(box.w / kScaleX),
                int(box.h), 0, cmds);
    for (const DrawCmd& c : cmds)
        renderer_.draw(c.hash, {c.dst.x * kScaleX, c.dst.y, c.dst.w * kScaleX, c.dst.h}, c.src, c.color);
}

void MenuChrome::selection(Rect box) {
    // Atlas rows 33..38: orange, alpha rising left to right; mirrored so the bar fades towards the right.
    sprite(menu_style::kAtlas, box, {64, 33, -64, 6}, 0x7F7F7F50);
}

void MenuChrome::sprite(std::uint32_t hash, Rect dst, Rect src, std::uint32_t color) {
    renderer_.draw(hash, dst, src, Color::from_rgba(color));
}

}  // namespace nf::ui

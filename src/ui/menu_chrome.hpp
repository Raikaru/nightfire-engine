#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "assets/menu_file.hpp"
#include "ui/layout.hpp"
#include "ui/renderer.hpp"
#include "ui/text.hpp"

namespace nf::ui {

// The furniture of the front end's multiplayer pages (07000048.bin: P_MPJOIN 0x40000019, P_MPSCEN
// 0x4000001a, the codename keyboard 0x40000020, the message box 0x4000002f) for screens drawn outside
// the menu script. Boxes are the script's authored 640x480 rectangles through fixup_resolution, text
// follows Label_Update, and the page margins (script x 50 / 590) are anchored to the screen edges so a
// page spans any aspect while the 4:3 composition stays exactly the original one.
namespace menu_style {
constexpr std::uint32_t kLabelColor = 0x7D6D59FF;   // every MP page label (messages 0x1b/0x1c)
constexpr std::uint32_t kItemColor = 0x73330F80;    // unselected buttons and keys (P_MAIN, P_CNCREATE)
constexpr std::uint32_t kHighContrastColor = 0x80807CFF;   // Settings > Accessibility > High contrast (near white)
constexpr std::uint32_t kLogo = 0x0300016D;         // "007 nightfire", page box 434,38 156x43
constexpr std::uint32_t kPanel = 0x03000199;        // P_MPJOIN agent panel: header strip + translucent body
constexpr std::uint32_t kAtlas = 0x03000042;        // frame atlas (orange selection gradient at v 33)
constexpr std::uint32_t kIcons = 0x03000075;        // button icons, keyboard arrows and tick
constexpr std::uint32_t kRing = 0x030000B3;         // P_MPSCEN ring with its selection bar
constexpr std::uint32_t kRingFrame = 0x030000BF;    // gold frame over the ring picture
constexpr std::uint32_t kEmblem = 0x03000135;       // 007 gun-barrel picture (P_MPJOIN "press to join")
constexpr std::uint16_t kKeyboardSkin = 7;          // P_CNCREATE key frame
constexpr std::uint16_t kFieldSkin = 8;             // P_CNCREATE name field (rounded tab corner)
constexpr float kRowPitch = 21.0f;                  // Menu_CreateOptionBox list row height (0x15)
}  // namespace menu_style

// Nine-slice: `src` split `corner` texels in from each edge, corners drawn corner_w x corner_h canvas units.
void draw_nine_slice(Renderer& renderer, std::uint32_t hash, Rect src, float corner, Rect dst, float corner_w,
                     float corner_h, Color color);
// The P_MPJOIN agent panel (menu_style::kPanel) stretched over `dst` (MenuChrome::panel without the header text).
void draw_agent_panel(Renderer& renderer, Rect dst);

class MenuChrome {
public:
    MenuChrome(Renderer& renderer, TextRenderer& text, const MenuFile& menu);

    // A script box (640x480 authored) on the canvas, horizontally as authored (centre-anchored).
    static Rect authored(int x, int y, int w, int h);

    const Layout& layout() const { return layout_; }
    // The page margins: script x 50 and 590 kept 50 units from the screen edges.
    float left() const { return left_; }
    float right() const { return right_; }
    float centre() const { return Layout::kDesignWidth * 0.5f; }
    // `authored(50, y, 540, h)` stretched between the margins.
    Rect span(int y, int h) const;

    static TextStyle style(int font, Align align, std::uint32_t color);
    // Label_Update placement of `text` in `box` (vertically centred on the alphabet extent).
    void label(Rect box, std::string_view text, int font, Align align, std::uint32_t color);
    // Menu_ClipString: the longest prefix of `text` that fits `width`.
    std::string clip(std::string_view text, int font, float width) const;
    float text_width(std::string_view text, int font) const;
    // A sprite of the engine's own art sheets (assets/ui, tools/art) drawn 1:1 with its top-left at (x, y); returns
    // the drawn rectangle (empty when the sheet has no such sprite).
    Rect art(std::string_view sheet, std::string_view name, float x, float y, std::uint32_t color = 0x7F7F7FFF);
    // The busy spinner (online.png spinner_0..11) centred on (x, y), `scale` canvas units per texel; `frame`
    // counts 30 Hz updates.
    void spinner(float x, float y, unsigned frame, float scale = 1.0f);

    void title(std::string_view text);     // page title, script box 50,40 376x17, font 1
    void logo();                           // script box 434,38 156x43, kept on the right margin
    void prompts(std::string_view text);   // glyph prompt row, script box 50,419 540x21, font 2 centred
    // P_MPJOIN agent panel stretched to `box` (nine-slice), with an optional centred header.
    void panel(Rect box, std::string_view header = {});
    // Header strip height of `panel` on the canvas.
    static float panel_header_height();
    // A component of a script skin set laid out by Component_SetupInstance.
    void skin(std::uint16_t set, std::uint16_t component, Rect box, unsigned mask = 0x10);
    // The orange selection gradient across `box`, strongest at the left like the scenario ring's bar.
    void selection(Rect box);
    void sprite(std::uint32_t hash, Rect dst, Rect src, std::uint32_t color = 0x7F7F7FFF);

private:
    Renderer& renderer_;
    TextRenderer& text_;
    const MenuFile& menu_;
    Layout layout_;
    float left_, right_;
};

}  // namespace nf::ui

// nfui font: the three font atlases plus sample text exercising kerning, alignment, outline, icons and TM.
#include "tools/nfui_scene.hpp"

namespace nf {

namespace {

class FontScene : public Scene {
public:
    void update(const PadHistory&) override {}

    void draw(ui::Renderer& r, ui::TextRenderer& text) override {
        const FontSet& fonts = text.fonts();
        r.fill({0, 0, ui::kScreenW, ui::kScreenH}, {0x20, 0x28, 0x30, 0x80});
        float y = 8;
        for (int n = 1; n <= 3; ++n) {
            const Font& f = fonts.font(n);
            auto [w, h] = r.texture_size(f.texture_hash);
            r.fill({4, y - 2, 308, float(h) * 0.6f + 4}, {0, 0, 0, 0x80});
            r.draw(f.texture_hash, {6, y, float(w) * 0.6f, float(h) * 0.6f}, {0x80, 0x80, 0x80, 0x80});
            y += float(h) * 0.6f + 10;
        }
        ui::TextStyle s;
        const float x = 326;
        const char* samples[] = {"The quick brown fox 0123456789", "AVATAR To Yo Ty WA Wa Kerning",
                                 "Nightfire\x99 \x92s Golden Gun\n  second line", "Press ~A to fire, ~B to jump"};
        y = 24;
        for (int n = 1; n <= 3; ++n) {
            s.font = n;
            s.scale_x = s.scale_y = 0.7f;
            for (const char* t : samples) {
                text.draw(x, y, t, s);
                y += text.measure(t, s).height + 2;
            }
            y += 4;
        }
        s.font = 1;
        s.scale_x = s.scale_y = 0.7f;
        s.outline = true;
        text.draw(x, y, "Outlined (labels)", s);
        s.outline = false;
        s.drop_shadow = true;
        text.draw(x, y + 16, "Drop shadow", s);
        s.drop_shadow = false;
        s.align = ui::Align::Center;
        text.draw(480, y + 32, "Centred", s);
        s.align = ui::Align::Right;
        text.draw(630, y + 48, "Right aligned", s);
    }
};

}  // namespace

std::unique_ptr<Scene> make_font_scene(SceneArgs&) { return std::make_unique<FontScene>(); }

}  // namespace nf

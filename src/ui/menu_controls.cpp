// Control behaviour: Label_/Button_/Radio_/Scroll_/Memo_/List_/Window_ SendMessage + Update and the
// skin ("component") instancing that draws their frames (Component_SetupInstance).
#include <algorithm>
#include <cmath>

#include "ui/menu.hpp"

namespace nf::ui {

namespace {

using namespace menu_msg;

constexpr std::uint32_t kDefaultColor = 0x7F7F7FC0;   // Menu_InitializeSprite when no colour is given
constexpr const char* kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWX";   // the strings Font_GetTextExtent measures for line heights

TextStyle style_of(const std::string& fmt, std::uint32_t color) {
    TextStyle s = apply_format(TextStyle{}, fmt);
    s.color = color;
    s.shadow_color = std::min<std::uint32_t>(color & 0xFF, 0x80);
    s.outline = false;
    return s;
}

DrawCmd image_cmd(std::uint32_t hash, Rect dst, Rect src, std::uint32_t color, int layer) {
    DrawCmd c;
    c.hash = hash, c.dst = dst, c.src = src, c.color = Color::from_rgba(color), c.layer = layer;
    return c;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Skins

const MenuComponent* MenuManager::component(const Control& c, std::uint16_t comp) const {
    const MenuComponentSet* set = file_.skin(c.skin_set);
    if (!set || comp >= set->components.size()) return nullptr;
    return &set->components[comp];
}

// Component_SetupInstance: lays the instances of component `comp` out inside (cx, cy, W, H) and emits
// the ones visible in state `mask` (0x10 idle, 0x20 selected, 0x40/0x80 pressed).
void MenuManager::skin_commands(Page& page, Control& c, std::uint16_t comp_index, unsigned mask, int cx, int cy, int W,
                                int H, int layer, std::vector<DrawCmd>& out) {
    const MenuComponent* comp = component(c, comp_index);
    if (!comp) return;
    int right = 0, left = W, bottom = 0, top = H;   // iStack_cc, iStack_c8, iStack_c4, iVar14
    for (const MenuInstance& in : comp->instances) {
        if ((in.flags & mask) == 0) continue;
        int w = in.w, h = in.h;
        const float wf = in.width_factor, hf = in.height_factor;
        if (in.flags & 0x100) w = hf == 0 ? in.uh + 1 : int(float(top - bottom) * hf);
        int ew;
        if (wf == 0.0f) {
            if (!(in.flags & 0x400)) {
                if (!(in.flags & 2)) right = std::max(right, in.x + w);
                else left = std::min(left, W - in.x - w);
            }
            ew = w;
        } else {
            const float avail = float(left - right), t = avail * wf;
            ew = t < 0 ? 0 : int(std::min(t, avail));
        }
        if (in.flags & 0x200) h = wf == 0.0f ? w : int(float(left - right) * wf);
        int eh;
        if (hf == 0.0f) {
            if (!(in.flags & 0x800)) {
                if (!(in.flags & 1)) bottom = std::max(bottom, in.y + h);
                else top = std::min(top, H - in.y - h);
            }
            eh = h;
        } else {
            const float avail = float(top - bottom), t = avail * hf;
            eh = t < 0 ? 0 : int(std::min(t, avail));
        }
        int x, y;
        const int bx = page.self->x + cx, by = page.self->y + cy;
        if (!(in.flags & 2)) x = (in.flags & 4) ? bx + ((W - ew) >> 1) : bx + in.x;
        else x = bx + W - ew - in.x;
        if (!(in.flags & 1)) y = (in.flags & 8) ? by + ((H - eh) >> 1) : by + in.y;
        else y = by + H - eh - in.y;
        if (ew <= 0 || eh <= 0) continue;
        Rect src{float(in.u), float(in.v), float(in.uw + 1), float(in.uh + 1)};
        out.push_back(image_cmd(comp->texture, {float(x), float(y), float(ew), float(eh)}, src,
                                in.color ? in.color : kDefaultColor, layer));
    }
}

// ---------------------------------------------------------------------------------------------
// Labels

// Menu_ClipString: drops the characters of each line that do not fit `width`.
void MenuManager::clip_string(std::string& text, const LabelState& l, int width) const {
    const TextStyle style = style_of(l.format, 0);
    std::string out, line;
    auto flush = [&] {
        std::size_t n = 0;
        while (n < line.size() && measurer_.measure(line.substr(0, n + 1), style).width <= float(width)) ++n;
        out += line.substr(0, n);
        line.clear();
    };
    for (char ch : text) {
        if (ch == '\n') {
            flush();
            out += ch;
        } else {
            line += ch;
        }
    }
    flush();
    text = std::move(out);
}

// Label_Update for the label state `l` of `owner` (a label control, or the label inside a button/radio)
// laid out in `r`; appends the sprite to `out`.
void MenuManager::update_label(Page& page, Control& owner, LabelState& l, Rect r, bool hover, int) {
    std::vector<DrawCmd>& out = owner.cmds;
    const bool embedded = owner.type != std::uint8_t(ControlType::Label);
    const int layer = (embedded ? owner.layer - 1 : owner.layer) - page.layer;
    const bool label_hover = !embedded && cursor_over(owner);
    // colour selection (selected / normal / pulse)
    if (l.colours_set) {
        bool selected = false;
        if (!label_hover && !hover) {
            const Control* cur = page.current;
            const bool same = cur && owner.id == cur->id && (owner.index == cur->index || l.always_selected);
            if (same) selected = true;
            else if (l.fade_state == 0) {
                if ((embedded ? 0u : owner.age) < 6) l.color = l.color_normal;
                else fade(owner, l.color_normal, 15, true);
                l.fade_state = 1;
            }
        } else {
            selected = true;
        }
        if (selected) {
            l.fade_state = 0;
            if (!l.pulse || owner.state != 0) {
                l.color = l.color_selected;
            } else {
                const int a = alpha_;
                auto ch = [&](int shift) { return std::uint32_t(std::clamp(int((l.pulse_base >> shift) & 0xFF) + a, 0, 255)); };
                l.color = ch(24) << 24 | ch(16) << 16 | ch(8) << 8 | (l.pulse_base & 0xFF);
            }
        }
    }

    if (l.text_mode) {
        std::string text = l.text;
        if (l.text_hash != 0xFFFFFFFF) {
            text = std::string(strings_.label(l.text_hash));
            if (text.empty()) text = " ";
        }
        const TextStyle base = style_of(l.format, l.color);
        const float text_h = measurer_.measure(kAlphabet, base).height;
        const bool multiline = text.find('\n') != std::string::npos;
        DrawCmd c;
        c.is_text = true;
        const Align align = format_alignment(l.format);
        c.x = float(page.self->x) + r.x + (align == Align::Center ? float(int(r.w) >> 1) : align == Align::Right ? r.w : 0);
        c.y = float(int(float(int(r.y) + (int(r.h) >> 1)) + text_h * (multiline ? 0.0f : 1.0f) * 0.5f + float(page.self->y) - 1.0f));
        clip_string(text, l, int(r.w));
        c.text = std::move(text);
        c.style = base;
        c.style.outline = l.outline;
        c.layer = layer;
        if (!c.text.empty()) out.push_back(std::move(c));
    } else if (l.sprite) {
        Rect dst{float(page.self->x) + r.x, float(page.self->y) + r.y, r.w, r.h};
        out.push_back(image_cmd(l.sprite, dst, {float(l.u), float(l.v), float(l.uw) + 1, float(l.vh) + 1}, l.color, layer));
    }
}

// ---------------------------------------------------------------------------------------------
// Activation: the accept / back / alt presses of a control that is the page's current one.

void MenuManager::emit_activation(Page& page, Control& c, int ev) {
    if (!active_ || input_lock_) return;
    switch (ev) {
        case 1: play_sound(MenuSound::Accept); page_message(page, Msg{kAccept, 0, 0, {}, &c}); break;
        case 2: play_sound(MenuSound::Back); page_message(page, Msg{kBack, 0, 0, {}, &c}); break;
        case 3: play_sound(MenuSound::Alt); page_message(page, Msg{kAlt, 0, 0, {}, &c}); break;
        default: break;
    }
}

// ---------------------------------------------------------------------------------------------
// Buttons

void MenuManager::update_button(Page& page, Control& c) {
    c.cmds.clear();
    if ((page.state & 5) || c.hidden()) {
        c.state |= 4;
        update_label(page, c, c.label, {}, cursor_over(c));
        return;
    }
    c.state &= ~4;
    const MenuComponent* comp = own_component(c);
    const int p0 = comp ? comp->params[0] : 0, p1 = comp ? comp->params[1] : 0, p2 = comp ? comp->params[2] : 0,
              p3 = comp ? comp->params[3] : 0;
    Rect lr{float(c.x + p0), float(c.y + p1), float(c.w - p0 - p2), float(c.h - p1 - p3)};
    unsigned mask = 0x10;
    int ev = 0;   // 1 accept, 2 repeat, 3 back, 4 alt
    const Control* cur = page.current;
    if (cur && cur->id == c.id) {
        mask = 0x20;
        if (cursor_over(c) || cur->index == c.index) {
            using namespace menu_action;
            if (act(kActAccept, kPressed)) ev = 1;
            if (act(kActAccept, kRepeat)) ev = 2;
            if (act(kActBack, kPressed)) ev = 3;
            mask = 0x80;
            if (act(kActAlt, kPressed)) ev = 4;
            if (act(kActAccept, kHeld)) mask = 0x40;
        }
    }
    skin_commands(page, c, c.skin_comp, mask, c.x, c.y, c.w, c.h, c.layer - page.layer, c.cmds);
    update_label(page, c, c.label, lr, cursor_over(c));
    if (!active_ || input_lock_) return;
    switch (ev) {
        case 1: play_sound(MenuSound::Accept); page_message(page, Msg{kAccept, 0, 0, {}, &c}); break;
        case 2: play_sound(MenuSound::Accept); page_message(page, Msg{kRepeat, 0, 0, {}, &c}); break;
        case 3: play_sound(MenuSound::Back); page_message(page, Msg{kBack, 0, 0, {}, &c}); break;
        case 4: play_sound(MenuSound::Alt); page_message(page, Msg{kAlt, 0, 0, {}, &c}); break;
        default: break;
    }
}

// ---------------------------------------------------------------------------------------------
// Scrolls (also the arrow/slider part of radios, memos and lists)

void MenuManager::notify_parent(Control& c, std::uint32_t type, std::uint32_t a, std::uint32_t b) {
    (void)a;
    if (c.type == std::uint8_t(ControlType::Scroll)) {
        page_message(c.page, Msg{type, 0, b, {}, &c});   // a standalone scroll reports to its page
    } else {
        Msg m{type, 0, b, {}, &c};                        // the scroll inside a radio/list reports to its owner
        control_message(c, m);
    }
}

// The input half of Scroll_Update for scroll state `s` owned by `owner` (the radio/memo/list or the scroll
// control itself): arrow presses change the value and notify the owner with 0x49.
void MenuManager::scroll_input(Page& page, Control& c, ScrollState& s, Control& owner, bool selected) {
    using namespace menu_action;
    (void)owner;
    const bool up = act(kActUp, kHeld), down = act(kActDown, kHeld),
               left = act(kActLeft, kHeld), right = act(kActRight, kHeld);
    const bool vert_key = up || down, horiz_key = left || right;
    const int rate = s.hold > 0x1F9 ? 2 : 5;
    if (!selected || !active_ || input_lock_) {
        s.pressed = 0;
        s.hold = 0;
        return;
    }
    int step = 0;
    unsigned sound = 0;
    auto back = [&](int first, bool held, MenuSound snd, MenuSound rep, int dir) {
        if (!held) return false;
        if (s.pressed == 0) {
            s.pressed = first;
            s.hold = 0;
            step = dir;
            play_sound(snd);
        } else if (s.pressed == first && s.hold >= 0x10) {
            ++s.hold;
            if (s.hold % rate == 0) {
                step = dir;
                play_sound(rep);
            }
            return true;
        } else {
            ++s.hold;
            return true;
        }
        return true;
    };
    (void)sound;
    if (!s.vertical) {
        if (left && !vert_key) back(0x40, true, MenuSound::Left, MenuSound::LeftRepeat, -1);
        else if (right && !vert_key) back(0x80, true, MenuSound::Right, MenuSound::RightRepeat, +1);
        else s.pressed = 0;
    } else {
        if (up && !horiz_key) back(0x40, true, MenuSound::Left, MenuSound::LeftRepeat, -1);
        else if (down && !horiz_key) back(0x80, true, MenuSound::Right, MenuSound::RightRepeat, +1);
        else s.pressed = 0;
    }
    if (step) {
        int v = s.value + step;
        if (v < s.min || v > s.max) {
            if (!s.wrap) {
                return;
            }
            v = step < 0 ? s.max : s.min;
        }
        s.value = v;
        notify_parent(c, kValueChanged, 0, 0);
    }
    (void)page;
}

void MenuManager::update_scroll(Page& page, Control& c) {
    c.cmds.clear();
    ScrollState& s = c.scroll;
    if ((page.state & 5) || c.hidden()) {
        c.state |= 4;
        return;
    }
    const bool selected = page.current && page.current->id == c.id;
    scroll_input(page, c, s, c, selected);
    unsigned mask = 0x10;
    if (selected) mask = s.pressed ? unsigned(s.pressed) : 0x20u;
    const MenuComponent* arrows = own_component(c);
    skin_commands(page, c, c.skin_comp, mask, c.x, c.y, c.w, c.h, c.layer - page.layer, c.cmds);
    if (!s.embedded) {
        // Scroll_Update's second half: the thumb (or, in slider mode, the filled part) is component 8.
        const MenuComponent* tc = component(c, 8);
        if (tc) {
            const int q0 = tc->params[0], q1 = tc->params[1], q2 = tc->params[2], q3 = tc->params[3];
            const int count = std::max(1, s.max - s.min + 1);
            const int span = std::max(1, s.max - s.min);
            const float frac = float(std::clamp(s.value, s.min, s.max) - s.min) / float(span);
            if (!s.vertical) {
                const int a = c.h + (arrows ? arrows->params[0] : 0), b = c.h + (arrows ? arrows->params[2] : 0);
                const int inner = c.w - a - b;
                int tw = std::max(c.h, inner / count);
                int tx = c.x + a + int(float(inner - tw) * frac);
                if (s.slider) {
                    tx = c.x + a;
                    tw = int(float(inner) * frac);
                }
                skin_commands(page, c, 8, mask, tx + q0, c.y + q1, tw - q0 - q2, c.h - q1 - q3, c.layer - page.layer, c.cmds);
            } else {
                const int a = c.w + (arrows ? arrows->params[1] : 0), b = c.w + (arrows ? arrows->params[3] : 0);
                const int inner = c.h - a - b;
                int th = std::max(c.w, inner / count);
                int ty = c.y + a + int(float(inner - th) * frac);
                if (s.slider) {
                    ty = c.y + a;
                    th = int(float(inner) * frac);
                }
                skin_commands(page, c, 8, mask, c.x + q0, ty + q1, c.w - q0 - q2, th - q1 - q3, c.layer - page.layer, c.cmds);
            }
        }
    }
    c.state &= ~4;
    if (selected && active_ && !input_lock_) {
        using namespace menu_action;
        if (act(kActAccept, kPressed)) { play_sound(MenuSound::Accept); page_message(page, Msg{kAccept, 0, 0, {}, &c}); }
        else if (act(kActBack, kPressed)) { play_sound(MenuSound::Back); page_message(page, Msg{kBack, 0, 0, {}, &c}); }
        else if (act(kActAlt, kPressed)) { play_sound(MenuSound::Alt); page_message(page, Msg{kAlt, 0, 0, {}, &c}); }
        else if (act(kActPageBack, kPressed)) { play_sound(MenuSound::Memo); page_message(page, Msg{kAltOnControl, 0, 0, {}, &c}); }
    }
}

// ---------------------------------------------------------------------------------------------
// Radios

void MenuManager::update_radio(Page& page, Control& c) {
    c.cmds.clear();
    if ((page.state & 5) || c.hidden()) {
        c.state |= 4;
        update_label(page, c, c.label, {}, cursor_over(c));
        return;
    }
    c.state &= ~4;
    const MenuComponent* comp = own_component(c);
    const int p0 = comp ? comp->params[0] : 0, p2 = comp ? comp->params[2] : 0;
    Rect lr{float(c.x + p0), float(c.y - 1), float(c.w - p0 - p2), float(c.h)};
    // the embedded scroll (skin component 6: the two arrows) runs on the radio's rectangle
    ScrollState& s = c.scroll;
    s.min = 0;
    s.max = std::max<int>(0, int(c.rows.size()) - 1);
    if (c.current_row >= 0) s.value = c.current_row;
    const bool selected = page.current && page.current->id == c.id;   // Scroll_Update compares control ids only
    scroll_input(page, c, s, c, selected);
    unsigned mask = 0x10;
    if (selected) mask = s.pressed ? unsigned(s.pressed) : 0x20u;
    skin_commands(page, c, 6, mask, c.x, c.y, c.w, c.h, c.layer - page.layer, c.cmds);
    skin_commands(page, c, c.skin_comp, 0x10 | mask, c.x, c.y, c.w, c.h, c.layer - page.layer, c.cmds);
    c.label.text = c.current_row >= 0 && c.current_row < int(c.rows.size()) ? c.rows[std::size_t(c.current_row)].cell[0] : " ";
    c.label.text_hash = 0xFFFFFFFF;
    update_label(page, c, c.label, lr, cursor_over(c));
    if (selected && active_ && !input_lock_) {
        using namespace menu_action;
        if (act(kActAccept, kPressed)) { play_sound(MenuSound::Accept); Msg m{kAccept, 0, 0, {}, &c}; control_message(c, m); }
        else if (act(kActBack, kPressed)) { play_sound(MenuSound::Back); Msg m{kBack, 0, 0, {}, &c}; control_message(c, m); }
        else if (act(kActAlt, kPressed)) { play_sound(MenuSound::Alt); Msg m{kAlt, 0, 0, {}, &c}; control_message(c, m); }
    }
}

// ---------------------------------------------------------------------------------------------
// Memos (word-wrapped text boxes)

void MenuManager::memo_wrap(Control& c) {
    // Font_WordWrapString: greedy wrap at spaces to the box width minus the frame insets and the 16 pixel scroll bar.
    const MenuComponent* comp = own_component(c);
    const int p0 = comp ? comp->params[0] : 0, p2 = comp ? comp->params[2] : 0;
    const float width = float(c.w - (p0 + 16) - p2);
    const TextStyle style = style_of(c.memo_format, 0);
    c.rows.clear();
    std::string rest = c.memo_text;
    std::size_t pos = 0;
    while (pos <= rest.size() && !rest.empty()) {
        std::size_t nl = rest.find('\n', pos);
        std::string para = rest.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        std::size_t start = 0;
        do {
            std::size_t end = start, last_space = std::string::npos;
            while (end < para.size()) {
                const std::string probe = para.substr(start, end - start + 1);
                if (measurer_.measure(probe, style).width > width && end > start) break;
                if (para[end] == ' ') last_space = end;
                ++end;
            }
            std::size_t cut = end;
            if (end < para.size() && last_space != std::string::npos) cut = last_space;
            Row r;
            r.cell[0] = para.substr(start, cut - start);
            c.rows.push_back(std::move(r));
            start = cut < para.size() && para[cut] == ' ' ? cut + 1 : cut;
        } while (start < para.size());
        if (nl == std::string::npos) break;
        pos = nl + 1;
        if (pos >= rest.size()) break;
    }
}

int MenuManager::memo_line_count(Control& c) {
    if (c.memo_line_height <= 0) c.memo_line_height = measurer_.measure(kAlphabet, style_of(c.memo_format, c.memo_color)).height * 1.5f;
    memo_wrap(c);
    return int(c.rows.size());
}

void MenuManager::update_memo(Page& page, Control& c) {
    c.cmds.clear();
    if ((page.state & 5) || c.hidden()) {
        c.state |= 4;
        return;
    }
    c.state &= ~4;
    const TextStyle style = style_of(c.memo_format, c.memo_color);
    if (c.memo_line_height <= 0) c.memo_line_height = measurer_.measure(kAlphabet, style).height * 1.5f;
    memo_wrap(c);
    const MenuComponent* comp = own_component(c);
    const int p0 = comp ? comp->params[0] : 0, p1 = comp ? comp->params[1] : 0;
    c.memo_lines = int(c.rows.size());
    c.memo_visible = int(float(c.h) / c.memo_line_height);
    if (c.memo_top + c.memo_visible > c.memo_lines && c.memo_visible < c.memo_lines) c.memo_top = c.memo_lines - c.memo_visible;
    if (c.memo_lines <= c.memo_visible) c.memo_top = 0;
    const bool selected = page.current && page.current->id == c.id;
    // scroll bar
    ScrollState& s = c.scroll;
    s.min = 0;
    s.max = std::max(0, c.memo_lines - c.memo_visible);
    if (selected) {
        using namespace menu_action;
        if (act(kActUp, kHeld) && c.memo_top > 0 && active_) --c.memo_top;
        else if (act(kActDown, kHeld) && c.memo_top < s.max && active_) ++c.memo_top;
    }
    s.value = c.memo_top;
    skin_commands(page, c, c.skin_comp, selected ? 0x20u : 0x10u, c.x, c.y, c.w, c.h, c.layer - page.layer, c.cmds);
    const int first = c.memo_top;
    const int shown = std::min(c.memo_visible, c.memo_lines - first);
    const float block = c.memo_line_height * float(std::max(shown, 0));
    const float voff = c.memo_vcentre ? (float(c.h) - block) * 0.5f : 0.0f;
    for (int i = 0; i < shown; ++i) {
        const std::string& line = c.rows[std::size_t(first + i)].cell[0];
        DrawCmd d;
        d.is_text = true;
        d.style = style;
        d.style.align = Align::Left;
        d.style.outline = true;
        float x = float(page.self->x + c.x + p0 + 2);
        if (c.memo_centre) {
            const float lw = measurer_.measure(line, style).width;
            x += (float(c.w) - lw) * 0.5f - 0;
            x = float(page.self->x + c.x) + (float(c.w) - lw) * 0.5f;
        }
        d.x = x;
        d.y = float(int(float(i) * c.memo_line_height + float(c.y) + float(p1) + c.memo_line_height + voff)) + float(page.self->y);
        d.text = line;
        d.layer = c.layer - page.layer - 5;
        c.cmds.push_back(std::move(d));
    }
    // Memo_Update's own accept / back / alt when it is the topmost control under the cursor
    if (cursor_over(c) && active_ && !input_lock_) {
        if (act(menu_action::kActAccept, menu_action::kPressed)) emit_activation(page, c, 1);
        else if (act(menu_action::kActBack, menu_action::kPressed)) emit_activation(page, c, 2);
        else if (act(menu_action::kActAlt, menu_action::kPressed)) emit_activation(page, c, 3);
    }
    if (selected && active_ && !input_lock_ && act(menu_action::kActPageBack, menu_action::kPressed))
        page_message(page, Msg{kMemoBack, 0, 0, {}, &c});
    if (c.memo_lines > c.memo_visible) {
        // Memo_Update's embedded scroll: the two arrows (skin component 6) at the right edge, mid height.
        skin_commands(page, c, 6, selected ? 0x20u : 0x10u, c.x + c.w - 0x12, c.y + (c.h >> 1) - 8, 0x24, 0x10,
                      c.layer - page.layer, c.cmds);
    }
}

// ---------------------------------------------------------------------------------------------
// Lists

void MenuManager::update_list(Page& page, Control& c) {
    c.cmds.clear();
    if ((page.state & 5) || c.hidden()) {
        c.state |= 4;
        return;
    }
    c.state &= ~4;
    const bool selected_list = page.current && page.current->id == c.id;
    const std::string& fmt = c.list_format == 3 ? std::string("\xFF\x03") : c.list_format == 2 ? std::string("\xFF\x01") : std::string("\xFF\x02");
    const TextStyle base = style_of(fmt, kDefaultColor);
    const float row_h = measurer_.measure("ABCDEFGHIJKLMNOP", base).height * 1.1f;
    c.list_visible = int(float(c.h) / row_h);
    const int n = int(c.rows.size());
    if (n < c.list_top + c.list_visible && c.list_visible < n) c.list_top = n - c.list_visible;
    ScrollState& s = c.scroll;
    s.min = 0;
    s.max = std::max(0, n - 1);
    if (c.current_row >= 0 && c.current_row < n) s.value = c.current_row;
    // keep the current row visible
    if (c.current_row >= 0) {
        if (c.current_row < c.list_top) c.list_top = c.current_row;
        else if (c.current_row >= c.list_top + c.list_visible) c.list_top = c.current_row - c.list_visible + 1;
    }
    const bool selected = selected_list;
    s.vertical = true;
    scroll_input(page, c, s, c, selected);
    const int layer = c.layer - page.layer;
    skin_commands(page, c, c.skin_comp, selected ? 0x20u : 0x10u, c.x, c.y, c.w, c.h, layer, c.cmds);
    const MenuComponent* comp = own_component(c);
    const int p0 = comp ? comp->params[0] : 0, p1 = comp ? comp->params[1] : 0;
    int column_x[6] = {0, 0, 0, 0, 0, 0};
    for (int col = 0; col < c.columns; ++col)
        column_x[col + 1] = column_x[col] + int(float(c.column_width[std::size_t(col)]) * float(c.w - 0x10) * 0.01f);
    for (int i = c.list_top; i < n && i < c.list_top + c.list_visible; ++i) {
        const Row& row = c.rows[std::size_t(i)];
        const bool current = i == c.current_row && !(c.state & 2);
        const float top = float(i - c.list_top) * row_h + float(c.y) + float(p1);
        if (current && c.list_mode == 1) {
            // the selection bar: component 4 of the list's skin
            const int hx = c.x + p0;
            skin_commands(page, c, 4, 0x20, hx, int(top + 3.0f), c.w - 2 * p0 - (c.list_visible < n ? 0x10 : 0), int(row_h), layer + 1, c.cmds);
        }
        for (int col = 0; col < c.columns; ++col) {
            std::string text = row.cell[std::size_t(col)];
            if (text.empty()) continue;
            std::uint32_t color = kDefaultColor;
            if (current && c.list_mode != 0) color = c.list_selected_color;
            TextStyle st = style_of(fmt, color);
            const float colw = float(c.column_width[std::size_t(col)]) * float(c.w) * 0.01f;
            LabelState tmp;
            tmp.format = fmt;
            clip_string(text, tmp, int(colw - 16.0f));
            const float tw = measurer_.measure(text, st).width;
            float x = float(page.self->x + c.x + p0 + 2 + column_x[col]);
            if (c.column_align[std::size_t(col)] == 2) x += float(int(colw)) - tw;
            else if (c.column_align[std::size_t(col)] == 3) x += (float(int(colw)) - tw) * 0.5f;
            DrawCmd d;
            d.is_text = true;
            d.text = text;
            d.style = st;
            d.style.outline = true;
            d.x = x;
            d.y = float(int(top + row_h)) + float(page.self->y);
            d.layer = layer - 7;
            c.cmds.push_back(std::move(d));
        }
    }
    if (selected && active_ && !input_lock_) {
        using namespace menu_action;
        if (act(kActAccept, kPressed)) { play_sound(MenuSound::Accept); page_message(page, Msg{kAccept, 0, 0, {}, &c}); }
        else if (act(kActBack, kPressed)) { play_sound(MenuSound::Back); page_message(page, Msg{kBack, 0, 0, {}, &c}); }
        else if (act(kActAlt, kPressed)) { play_sound(MenuSound::Alt); page_message(page, Msg{kAlt, 0, 0, {}, &c}); }
        else if (act(kActPageBack, kPressed)) { play_sound(MenuSound::Memo); page_message(page, Msg{kAltOnControl, 0, 0, {}, &c}); }
    }
}

// ---------------------------------------------------------------------------------------------
// Dispatch by control type

void MenuManager::control_update(Page& page, Control& c) {
    switch (ControlType(c.type)) {
        case ControlType::Button: update_button(page, c); break;
        case ControlType::Label: {
            c.cmds.clear();
            if ((page.state & 5) || c.hidden()) {
                c.state |= 4;
                break;
            }
            c.state &= ~4;
            update_label(page, c, c.label, c.box(), cursor_over(c));
            // Label_Update's own accept/back/alt when the label is the topmost control under the cursor
            if (cursor_over(c) && active_ && !input_lock_) {
                using namespace menu_action;
                if (act(kActAccept, kPressed)) emit_activation(page, c, 1);
                else if (act(kActBack, kPressed)) emit_activation(page, c, 2);
                else if (act(kActAlt, kPressed)) emit_activation(page, c, 3);
            }
            break;
        }
        case ControlType::Radio: update_radio(page, c); break;
        case ControlType::Scroll: update_scroll(page, c); break;
        case ControlType::Memo: update_memo(page, c); break;
        case ControlType::List: update_list(page, c); break;
        default: c.cmds.clear(); break;   // windows draw nothing
    }
}

// ---------------------------------------------------------------------------------------------
// Messages

Row& MenuManager::add_row(Control& c) {
    c.rows.emplace_back();
    return c.rows.back();
}

int MenuManager::control_message(Control& c, const Msg& m) {
    using CT = ControlType;
    LabelState& l = c.label;
    const CT type = CT(c.type);
    // messages every type with a script handles
    switch (m.type) {
        case kExitLoop:
            c.run.flags &= ~8u;
            return 1;
        case kPlayScript: return script_start(c, m.a, m.b != 0) ? 1 : 0;
        case kAttachScript:
            if (m.a < 7) {
                const MenuScript* s = nullptr;
                for (const MenuScript& sc : c.def->scripts)
                    if (sc.id == m.b) s = &sc;
                c.slots[m.a] = s;
                return 1;
            }
            return 0;
        case kGetControl: return 1;
        case kGetUserIndex: return int(c.index);
        default: break;
    }
    const bool has_label = type == CT::Label || type == CT::Button || type == CT::Radio;
    if (has_label) {
        switch (m.type) {
            case kSetText:
                l.text_mode = true;
                if (m.a == 0) {
                    if (m.b == 0) { l.text = " "; l.text_hash = 0xFFFFFFFF; }
                    else { l.text_hash = m.b; }
                } else {
                    l.text = m.text;
                    l.text_hash = 0xFFFFFFFF;
                }
                return 1;
            case kSetColor:
                l.color = m.a;
                if (m.b == 0) {
                    l.colours_set = false;
                    l.color_selected = l.color_normal = m.a;
                } else {
                    l.colours_set = true;
                }
                return 1;
            case kSetSelectedColor: l.colours_set = true; l.color_selected = m.a; l.color = m.a; return 1;
            case kSetNormalColor: l.colours_set = true; l.color_normal = m.a; l.color = m.a; return 1;
            case kSetFormat: l.format = m.text + std::string("\xFC") + "d" + "\xFB" + "d"; return 1;
            case kSetSprite:
                if (m.a) { l.text_mode = false; l.sprite = m.a; }
                return 1;
            case kSetOutlineColor: l.pulse_base = m.a; return 1;
            case kSetUv: l.u = std::uint16_t(m.a >> 16), l.v = std::uint16_t(m.a), l.uw = std::uint16_t(m.b >> 16), l.vh = std::uint16_t(m.b); return 1;
            case kOutline: l.outline = (m.a & 0xFF) != 0; return 1;
            case kPulse: l.pulse = (m.a & 0xFF) != 0; return 0;
            case 0x76: l.always_selected = (m.a & 0xFF) != 0; return 0;
            case kGetColor: return int(l.color);
            case 0x31: return int(l.color_selected);
            case 0x32: return int(l.color_normal);
            default: break;
        }
    }
    switch (type) {
        case CT::Label:
            if (m.type == kSetFlags) { c.state = std::uint8_t(m.a); return 1; }
            return 0;
        case CT::Window: return 0;
        case CT::Button:
            switch (m.type) {
                case kSetUserIndex: c.index = m.a; return 1;
                case kSetFlags: c.state = std::uint8_t(m.a); return 1;
                case 0x3d: return int(c.index);
                default: return 0;
            }
        case CT::Radio:
            switch (m.type) {
                case kAddItem: case kAddItemLabel: {
                    Row& r = add_row(c);
                    r.cell[0] = m.type == kAddItem ? m.text : std::string(strings_.label(m.a));
                    r.value = m.b;
                    if (c.rows.size() == 1) c.current_row = 0;
                    return 1;
                }
                case kClear: c.rows.clear(); c.current_row = -1; c.scroll.value = 0; return 0;
                case kSetIndex: {
                    const int idx = int(m.a);
                    if (idx < 0 || idx >= int(c.rows.size())) return 0;
                    c.current_row = idx;
                    c.scroll.value = idx;
                    Msg n{kValueChanged, 0, 0, {}, &c};
                    page_message(c.page, n);
                    return 0;
                }
                case kSelectValue:
                    for (std::size_t i = 0; i < c.rows.size(); ++i)
                        if (c.rows[i].value == m.a) {
                            c.current_row = int(i);
                            c.scroll.value = int(i);
                            if (m.b == 0) {
                                Msg n{kValueChanged, 0, 0, {}, &c};
                                page_message(c.page, n);
                            }
                            return 1;
                        }
                    if (m.b == 0) {
                        Msg n{kValueChanged, 0, 0, {}, &c};
                        return page_message(c.page, n);
                    }
                    return 1;
                case kSetUserIndex:
                    if (c.current_row < 0) return 0;
                    c.rows[std::size_t(c.current_row)].value = m.a;
                    return 1;
                case kSetFlags: c.state = std::uint8_t(m.a); return 1;
                case kGetCount: return int(c.rows.size()) - 1;
                case 0x34: case 0x40: return c.current_row;
                case kGetValue: return c.current_row < 0 ? 0 : int(c.rows[std::size_t(c.current_row)].value);
                case 0x3b: return c.current_row < 0 ? 0 : 1;
                case kValueChanged: case kAccept: case 0x52: case kBack: case kAlt: {
                    // the arrows changed the value (or the radio was activated): mirror it, tell the page
                    if (m.type == kValueChanged) c.current_row = std::clamp(c.scroll.value, 0, std::max(0, int(c.rows.size()) - 1));
                    if (c.rows.empty()) c.current_row = -1;
                    Msg n{m.type, 0, 0, {}, &c};
                    page_message(c.page, n);
                    return 1;
                }
                case kScrollChanged:
                    c.current_row = std::clamp(c.scroll.value, 0, std::max(0, int(c.rows.size()) - 1));
                    if (c.rows.empty()) c.current_row = -1;
                    page_message(c.page, Msg{kValueChanged, 0, 1, {}, &c});
                    return 1;
                default: return 0;
            }
        case CT::Scroll:
            switch (m.type) {
                case kScrollGrow: c.scroll.max += int(m.a); return 0;
                case kScrollWrap: c.scroll.wrap = false; return 1;
                case kSetUserIndex: c.index = m.a; return 1;
                case kScrollMax:
                    if (int(m.a) < c.scroll.min) return 0;
                    c.scroll.max = int(m.a);
                    c.scroll.value = std::min(c.scroll.value, c.scroll.max);
                    return 1;
                case kScrollMin:
                    if (c.scroll.max < int(m.a)) return 0;
                    c.scroll.min = int(m.a);
                    c.scroll.value = std::max(c.scroll.value, c.scroll.min);
                    return 1;
                case kScrollRange:
                    if (int(m.b) < int(m.a)) return 0;
                    c.scroll.min = int(m.a), c.scroll.max = int(m.b);
                    c.scroll.value = std::clamp(c.scroll.value, c.scroll.min, c.scroll.max);
                    return 1;
                case kSetFlags: c.state = std::uint8_t(m.a); return 1;
                case kScrollSet:
                    if (int(m.a) > c.scroll.max || int(m.a) < c.scroll.min) return 0;
                    c.scroll.value = int(m.a);
                    page_message(c.page, Msg{kScrollChanged, 0, 0, {}, &c});
                    return 1;
                case kScrollGetMax: return c.scroll.max;
                case kScrollGetMin: return c.scroll.min;
                case kScrollGet: return std::clamp(c.scroll.value, c.scroll.min, c.scroll.max);
                case 0x69: c.scroll.slider = true; c.scroll.wrap = false; return 1;
                default: return 0;
            }
        case CT::Memo:
            switch (m.type) {
                case kSetText: c.memo_text = m.a == 0 && m.text.empty() ? std::string(strings_.label(m.b)) : m.text; c.memo_top = 0; return 1;
                case kSetSelectedColor: c.memo_color = m.a; return 1;
                case kSetNormalColor: return 1;
                case kSetFormat: c.memo_format = m.text; return 1;
                case kSetFlags: c.state = std::uint8_t(m.a); return 1;
                case 100: c.memo_centre = (m.a & 0xFF) != 0; return 1;
                case 0x66: c.memo_vcentre = (m.a & 0xFF) != 0; return 0;
                case kLineHeight: {
                    c.memo_line_height = measurer_.measure(kAlphabet, style_of(c.memo_format, 0)).height * 0.01f * float(int(m.a));
                    return 0;
                }
                case 0x65: return std::min<int>(c.memo_lines, c.memo_visible);
                case kAccept: page_message(c.page, Msg{kAccept, 0, 0, {}, &c}); return 1;
                case 0x2f: return 1;
                default: return 0;
            }
        case CT::List:
            switch (m.type) {
                case kAddItem: case kAddItemLabel: {
                    Row& r = add_row(c);
                    r.cell[0] = m.type == kAddItem ? m.text : std::string(strings_.label(m.a));
                    r.value = m.b;
                    if (c.current_row < 0) c.current_row = 0;
                    return int(c.rows.size()) - 1;
                }
                case kClear:
                    c.rows.clear(), c.current_row = -1, c.list_top = 0, c.scroll.value = 0;
                    c.columns = 1, c.column_width = {100, 0, 0, 0, 0}, c.column_align = {1, 1, 1, 1, 1};
                    return 1;
                case kSetIndex:
                    if (m.a >= c.rows.size()) return 0;
                    c.current_row = int(m.a);
                    dispatch(kValueChanged, c, nullptr, 0, 0);
                    return 1;
                case kSelectValue:
                    for (std::size_t i = 0; i < c.rows.size(); ++i)
                        if (c.rows[i].value == m.a) {
                            c.current_row = int(i);
                            if (m.b == 0) dispatch(kValueChanged, c, nullptr, 0, 0);
                            return 1;
                        }
                    return 0;
                case kSetRowValue:
                    if (c.current_row < 0) return 0;
                    c.rows[std::size_t(c.current_row)].value = m.a;
                    return 1;
                case kSetFlags: c.state = std::uint8_t(m.a); return 1;
                case kSetSelectedColor: case kSetNormalColor: return 1;
                case 0x29: c.list_selected_color = m.a; return 1;
                case kSetFormat: c.list_format = int(m.a); return 1;
                case 0x0E:   // add a column {width percent a, alignment b}
                    if (c.columns < 5) {
                        c.column_width[std::size_t(c.columns)] = int(m.a);
                        c.column_align[std::size_t(c.columns)] = int(m.b);
                        ++c.columns;
                        return 1;
                    }
                    return 0;
                case 0x28:
                    if (int(m.a) < c.columns) { c.column_width[m.a] = std::min<int>(int(m.b), 100); return 1; }
                    return 0;
                case 0x59:
                    if (int(m.a) < c.columns) { c.column_align[m.a] = int(m.b); return 1; }
                    return 0;
                case kListSetCell: {
                    const std::size_t row = m.a >> 16, col = m.a & 0xFFFF;
                    if (row < c.rows.size() && col < 5) { c.rows[row].cell[col] = m.text; return 1; }
                    return 0;
                }
                case 0x2c: c.list_top = int(m.a); return 1;
                case 0x2a: c.list_mode = int(m.a & 0xFF); return 0;
                case kGetCount: return int(c.rows.size());
                case 0x34: return c.current_row;
                case kGetValue: return c.current_row < 0 ? 0 : int(c.rows[std::size_t(c.current_row)].value);
                case 0x3c: return c.list_top;
                case 0x41: return c.list_visible;
                case kValueChanged: case 0x52: {
                    // the embedded scroll moved: skip disabled rows, tell the page
                    int v = std::clamp(c.scroll.value, 0, std::max(0, int(c.rows.size()) - 1));
                    if (!c.rows.empty()) {
                        const int dir = v > c.current_row ? 1 : -1;
                        while (v >= 0 && v < int(c.rows.size()) && c.rows[std::size_t(v)].disabled) v += dir;
                        v = std::clamp(v, 0, int(c.rows.size()) - 1);
                        c.current_row = v;
                        c.scroll.value = v;
                    }
                    page_message(c.page, Msg{m.type, 0, 0, {}, &c});
                    return 1;
                }
                case kAccept: case kBack: case kAlt: case kAltOnControl:
                    return page_message(c.page, Msg{m.type, 0, 0, {}, &c});
                default: return 0;
            }
        default: return 0;
    }
}

}  // namespace nf::ui

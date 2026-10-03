// The menu manager: pages, control creation, message routing, page transitions, cursor navigation
// (MenuManager_Create/Update, Manager_SendMessage, Page_*, Menu_GetControl, Menu_FindControl,
// Page_Update). Field offsets in the comments are those of the original structs.
#include <algorithm>
#include <cmath>

#include "ui/accessibility.hpp"
#include "ui/layout.hpp"

#include "ui/menu.hpp"
namespace nf::ui {

namespace {

constexpr std::uint32_t kAllControls = 0xFFFFFFFF;

}  // namespace

// FixupResolution(NEW_CONTROL / M_KEYFRAME): authored 640x480 -> the 512x448 buffer.
Box fixup_resolution(Box b, bool scroll) {
    int w = int(float(b.w) * 0.8f);
    if (w < 2) w = 2;
    int x = int((float(b.x + (b.w >> 1) - 320) * 0.8f + 256.0f) - float(w >> 1));
    int h = int(float(b.h) * 0.9333334f);
    if (scroll) {
        if (h < 16) h = 16;
    } else if (h < 2) {
        h = 2;
    }
    int y = int((float(b.y + (b.h >> 1) - 240) * 0.9333334f + 224.0f) - float(h >> 1));
    return {x, y, w, h};
}

// ---------------------------------------------------------------------------------------------
// MenuInput

void MenuInput::update(const PadState& pad) {
    const bool axis_left = pad.lx < 0x40, axis_right = pad.lx > 0xBF, axis_up = pad.ly < 0x40, axis_down = pad.ly > 0xBF;
    std::array<bool, kCount> down{};
    down[menu_action::kActUp - kFirst] = pad.held(kPadUp) || axis_up;
    down[menu_action::kActDown - kFirst] = pad.held(kPadDown) || axis_down;
    down[menu_action::kActLeft - kFirst] = pad.held(kPadLeft) || axis_left;
    down[menu_action::kActRight - kFirst] = pad.held(kPadRight) || axis_right;
    down[menu_action::kActStart - kFirst] = pad.held(kPadStart);
    down[menu_action::kActAccept - kFirst] = pad.held(kPadCross);
    down[menu_action::kActBack - kFirst] = pad.held(kPadSquare);
    down[menu_action::kActAlt - kFirst] = pad.held(kPadCircle);
    down[menu_action::kActPageBack - kFirst] = pad.held(kPadTriangle);
    down[0] = pad.held(kPadStart) || pad.held(kPadCross);   // action 25 (skip movie)
    down_ = down;
    for (int a = 0; a < kCount; ++a) {   // Input_Update
        std::uint8_t state = 0;
        if (!down[a]) {
            hold_[a] = 0;
        } else {
            state = flags_[a] == 0 ? 5 : 1;
            hold_[a] = std::uint16_t(std::min<int>(hold_[a] + 1, 0x7FFF));
            if (hold_[a] >= 0x2E) state = (hold_[a] & 7) != 0 ? state & ~8 : state | 8;
        }
        flags_[a] = state;
    }
    ++frame_;
}

void MenuInput::clear() {
    flags_.fill(0);
    hold_.fill(0);
}

bool MenuInput::raw(int action, unsigned flags) const {
    const int i = action - kFirst;
    return i >= 0 && i < kCount && down_[i] && (flags_[i] & flags) != 0;
}

bool MenuInput::action(int action, unsigned flags, int status) const {
    using namespace menu_action;
    auto hit = [&](bool v) {
        if (v) last_input_ = frame_;
        return v;
    };
    // Holding triangle suppresses accept/start, holding accept or start suppresses triangle.
    if (action == kActStart || action == kActAccept) {
        if (hit(raw(kActPageBack, kHeld))) return false;
    }
    if (action == kActPageBack) {
        if (hit(raw(kActAccept, kHeld)) || hit(raw(kActStart, kHeld))) return false;
    }
    if ((action == kActStart || action == kActAccept) && status >= 4) {
        if (hit(raw(kActAccept, flags)) || hit(raw(kActStart, flags))) return true;
    }
    return hit(raw(action, flags));
}

// ---------------------------------------------------------------------------------------------
// Control / Page

Control::Control(Page& p, const MenuControl& d) : page(p), def(&d), id(d.id), type(d.type), layer(d.layer), index(d.index) {
    Box b{d.x, d.y, d.w, d.h};
    const bool is_scroll = type == std::uint8_t(ControlType::Scroll);
    if (type == std::uint8_t(ControlType::Label) && b.x < 1 && b.y == 0 && b.w >= 0x280 && b.h >= 0x1E0) {
        b.w = 0x200, b.h = 0x1C0;    // the full-screen backdrop label
    } else if (id == 0x10000224) {
        b.x = 0x154, b.y = 0xC6;
    } else if (type != std::uint8_t(ControlType::Window)) {
        b = fixup_resolution(b, is_scroll);
    }
    x = b.x, y = b.y, w = b.w, h = b.h;
    skin_set = d.skin;
    switch (ControlType(type)) {
        case ControlType::Radio:
            skin_comp = 5;
            scroll.embedded = true;
            break;
        case ControlType::Memo: skin_comp = 3; break;
        case ControlType::List:
            skin_comp = 3;
            list_mode = int(d.a & 0xFF);
            break;
        case ControlType::Scroll:
            scroll.min = int(d.a), scroll.max = int(d.b), scroll.value = int(d.a);
            scroll.vertical = b.w < b.h;
            skin_comp = scroll.vertical ? 7 : 6;
            break;
        default: skin_comp = 0; break;
    }
}

namespace {
MenuControl page_control(std::uint32_t id) {
    MenuControl m{};
    m.id = id;
    m.type = 8;
    return m;
}
}  // namespace

Page::Page(MenuManager& m, const MenuPage& d) : manager(m), def(&d), id(d.id), self_def(page_control(d.id)) {
    self = std::make_unique<Control>(*this, self_def);
    self->type = 8;
    self->x = 0, self->y = 0, self->w = 512, self->h = 448;
    movie = d.extra;
}

Control* Page::find(std::uint32_t control_id) {
    for (auto& c : controls)
        if (c->id == control_id) return c.get();
    return nullptr;
}

// ---------------------------------------------------------------------------------------------
// MenuManager: creation

MenuManager::MenuManager(const MenuFile& file, const StringTable& strings, const FontSet& fonts, MenuHost& host,
                         std::uint32_t menu_id, int status)
    : file_(file), strings_(strings), host_(host), measurer_(fonts), menu_id_(menu_id), status_(status) {}

// MenuManager_Create: build every page of the menu (their initial messages already reach the handlers), show
// the first page and select the main menu's first button.
void MenuManager::start(std::uint32_t start_page) {
    // Every page of the file is created: level scripts carry pages of several menus (the pause
    // menu is 0x80000002, P_ENDMISSION is 0x80000004, 0x40000021 is 0x80000003) and the game
    // navigates between them through one manager (Menu_GetPage searches the whole file).
    for (const MenuPage& p : file_.pages) create_page(p);
    (void)menu_id_;
    change_page(start_page, 1);
    send_manager(menu_msg::kSelectControl, 0x10000002);
}

MenuManager::~MenuManager() = default;

void MenuManager::create_page(const MenuPage& def) {
    Page& page = *pages_.emplace_back(std::make_unique<Page>(*this, def));
    for (const MenuControl& c : def.controls) create_control(page, c);
}

Control& MenuManager::create_control(Page& page, const MenuControl& def) {
    Control& c = *page.controls.emplace_back(std::make_unique<Control>(page, def));
    // Message 0x23 (the format string) arrives as its own token; the rest replay in order.
    if (!def.format.empty()) {
        Msg m{menu_msg::kSetFormat, 0, 0, def.format};
        send_to(c, m);
    }
    for (const MenuMessage& mm : def.messages) {
        if (mm.type == 0x1D) continue;
        Msg m{mm.type, mm.a, mm.b};
        if (mm.type >= 0x1A && mm.type < 0x1D && mm.a == 0x7D6D59FF) m.a = 0x645A49FF;   // the shipped PS2 colour
        send_to(c, m);
    }
    dispatch(menu_msg::kControlDone, c, nullptr, 0, 0);   // Manager 0x51: the control is complete
    return c;
}

// ---------------------------------------------------------------------------------------------
// Sending

Page* MenuManager::find_page(std::uint32_t id) {
    for (auto& p : pages_)
        if (p->id == id) return p.get();
    return nullptr;
}

std::uint32_t MenuManager::current_page_id() const { return current_ ? current_->id : 0; }

Control* MenuManager::find(std::uint32_t control_id) {
    Control* found = nullptr;
    for (auto& p : pages_)
        for (auto& c : p->controls)
            if (c->id == control_id) found = c.get();   // __Menu_Send keeps the last match
    return found;
}

Control* MenuManager::find_ex(std::uint32_t control_id, std::uint32_t index) {
    Control* found = nullptr;
    for (auto& p : pages_)
        for (auto& c : p->controls)
            if (c->id == control_id && c->index == index) found = c.get();
    return found;
}

int MenuManager::send_to(Control& c, const Msg& msg) {
    if (c.type == 8) return page_message(c.page, msg);
    return control_message(c, msg);
}

int MenuManager::send(std::uint32_t control_id, std::uint32_t type, std::uint32_t a, std::uint32_t b) {
    int result = 0;
    for (auto& p : pages_)
        for (auto& c : p->controls)
            if (c->id == control_id || control_id == kAllControls) {
                int r = send_to(*c, Msg{type, a, b});
                if (r) result = r;
            }
    return result;
}

int MenuManager::send(std::uint32_t control_id, std::uint32_t type, std::string text, std::uint32_t b) {
    int result = 0;
    for (auto& p : pages_)
        for (auto& c : p->controls)
            if (c->id == control_id || control_id == kAllControls) {
                Msg m{type, 0, b, text};
                m.a = text.empty() ? 0 : 1;
                int r = send_to(*c, m);
                if (r) result = r;
            }
    return result;
}

int MenuManager::send_ex(std::uint32_t control_id, std::uint32_t index, std::uint32_t type, std::uint32_t a, std::uint32_t b) {
    int result = 0;
    for (auto& p : pages_)
        for (auto& c : p->controls)
            if (c->id == control_id && c->index == index) {
                int r = send_to(*c, Msg{type, a, b});
                if (r) result = r;
            }
    return result;
}

int MenuManager::send_ex(std::uint32_t control_id, std::uint32_t index, std::uint32_t type, std::string text, std::uint32_t b) {
    int result = 0;
    for (auto& p : pages_)
        for (auto& c : p->controls)
            if (c->id == control_id && c->index == index) {
                Msg m{type, text.empty() ? 0u : 1u, b, text};
                int r = send_to(*c, m);
                if (r) result = r;
            }
    return result;
}

void MenuManager::send_delayed(int frames, std::uint32_t control_id, std::uint32_t type, std::uint32_t a, std::uint32_t b) {
    if (!find(control_id)) return;
    if (delayed_.size() >= 128) return;
    delayed_.push_back({int(frame_) + frames, false, control_id, type, a, b, {}});
}

void MenuManager::send_delayed_text(int frames, std::uint32_t control_id, std::uint32_t type, std::string text) {
    if (!find(control_id) || delayed_.size() >= 128) return;
    Delayed d{int(frame_) + frames, false, control_id, type, 1, 0, std::move(text)};
    delayed_.push_back(std::move(d));
}

void MenuManager::send_delayed_manager(int frames, std::uint32_t type, std::uint32_t a, std::uint32_t b) {
    if (delayed_.size() >= 128) return;
    delayed_.push_back({int(frame_) + frames, true, 0, type, a, b, {}});
}

void MenuManager::clear_delayed() { delayed_.clear(); }

std::vector<MenuSound> MenuManager::take_sounds() {
    std::vector<MenuSound> s;
    s.swap(sounds_);
    return s;
}

// Manager_SendMessage's default branch: the handler chain, then the control's own script.
int MenuManager::dispatch(std::uint32_t type, Control& a, Control* b, std::uint32_t av, std::uint32_t bv) {
    Msg m{type, av, bv};
    m.ca = &a, m.cb = b;
    const bool handled = host_.handle(*this, a, m);
    switch (type) {
        case 0x49: case 0x4A: case 0x4B: case 0x4E: case 0x4F: case 0x52: case 0x53:
            if (a.type != 8) return script_play_default(a, type) ? 1 : 0;
            return 1;
        default:
            if (handled) return 0;
            if (current_) {
                Msg fwd = m;
                fwd.b = 1;   // Page_SendMessage(.., 1): scripts only
                return page_message(*current_, fwd);
            }
            return 0;
    }
}

// Page_SendMessage.
int MenuManager::page_message(Page& page, const Msg& msg) {
    Control* a = msg.ca;
    switch (msg.type) {
        case 0x49: case 0x4E: case 0x4F: case 0x51:
            if (a) script_play_default(*a, msg.type);
            return msg.b == 0 && a ? dispatch(msg.type, *a, nullptr, 0, 0) : 0;
        case 0x4A: case 0x52: case 0x53: case 0x54: case 0x61:
            return msg.b == 0 && a ? dispatch(msg.type, *a, nullptr, 0, 0) : 0;
        case 0x4B: case 0x5D: case 0x5E: case 0x6D:
            if (msg.b == 0) dispatch(msg.type, *page.self, a, 0, 0);
            if (a) script_play_default(*a, msg.type);
            return msg.b == 0 && a ? dispatch(msg.type, *a, nullptr, 0, 0) : 0;
        case menu_msg::kPageMinDelay:
            page.min_frames = msg.a;
            return 0;
        case menu_msg::kSetNavigation:
            page.nav = int(msg.a);
            return 0;
        default:
            return 0;
    }
}

int MenuManager::send_manager(std::uint32_t type, std::uint32_t a, std::uint32_t b, Control* cb) {
    using namespace menu_msg;
    switch (type) {
        case kSelectControl: return select_control(a, cb);
        case kChangePage: return change_page(a, b);
        case kPageBack: return page_back(a, b);
        case kInputLock:
            input_lock_ = a != 0;
            clear_input();
            return 1;
        case 0x5A: return assign_pad_control(a, b);
        case menu_msg::kSetNavigation:
            if (a == 1 && current_) current_->nav = int(b);
            return current_ ? 1 : 0;
        case kGetPageId: return current_ ? int(overlay_ && under_ ? under_->id : current_->id) : 0;
        default: return 0;
    }
}

// ---------------------------------------------------------------------------------------------
// Pages

void MenuManager::set_cursor(const Control& c) {
    cursor_x_ = c.x + (c.w >> 1);
    cursor_y_ = c.y + (c.h >> 1);
}

// Manager 0x22.
int MenuManager::select_control(std::uint32_t id, Control* target) {
    if (!current_) return 0;
    for (auto& c : current_->controls) {
        const bool match = id == 0 ? c.get() == target : c->id == id;
        if (!match || c->state == 2) continue;
        set_cursor(*c);
        current_->current = c.get();
        return 1;
    }
    return 0;
}

// Manager 0x44.
int MenuManager::change_page(std::uint32_t id, unsigned flags) {
    clear_input();
    Page* target = find_page(id);
    if (!target) {
        if (current_) page_update(*current_, false);
        return 1;
    }
    const bool overlay = (flags & 2) != 0, push = (flags & 1) == 0, silent = (flags & 8) != 0;
    if (push && current_) history_.push_back({current_, current_->current});
    if (id == 0x40000002 && (flags & 4) == 0) history_.clear();
    Page* old = current_;
    const std::uint32_t old_id = old ? old->id : 0;
    if (old) {
        old->state = 1;
        page_update(*old, false);
        if (!overlay) {
            if (!silent) dispatch(menu_msg::kPageHidden, *old->self, nullptr, 0, 0);
            under_ = nullptr;
            overlay_ = false;
            send_manager(menu_msg::kInputLock, 0);
        } else {
            overlay_ = true;
            under_ = old;
            clear_delayed();
        }
    }
    current_ = target;
    target->layer = 5;
    target->state = 0;
    target->frames = 0;
    if (under_) {
        under_->layer = -5;
        under_->state = 2;
    }
    if (!silent) {
        dispatch(menu_msg::kPageShown, *target->self, nullptr, old_id, 0);
        script_play_default(*target->self, menu_msg::kPageShown);
    } else if (!(flags & 0x10)) {
        dispatch(menu_msg::kFadeInPage, *target->self, nullptr, old_id, 0);
    }
    page_update(*target, false);
    return 1;
}

// Manager 0x5a: makes control `id` with index `pad` the cursor control of controller `pad`.
int MenuManager::assign_pad_control(std::uint32_t id, std::uint32_t pad) {
    if (!current_ || pad >= 4) return 0;
    for (Page* page : {current_, under_}) {
        if (!page) continue;
        for (auto& c : page->controls) {
            if (c->id != id || c->index != pad) continue;
            if (Control* old = page->pad_control[pad]) dispatch(0x60, *old, nullptr, 0, 0);
            page->pad_control[pad] = c.get();
            dispatch(0x5C, *c, nullptr, 0, 0);
            return 1;
        }
    }
    return 0;
}

// Manager 0x5f.
int MenuManager::page_back(std::uint32_t a, std::uint32_t pad) {
    Page* page = current_;
    if (a == 0 && page) {
        if (pad < 4 && page->pad_control[pad]) {
            Msg m{menu_msg::kBackVeto, 0, 0};
            m.ca = page->pad_control[pad];
            host_.handle(*this, *page->pad_control[pad], m);
            if (m.veto) return 1;
        }
        Msg m{menu_msg::kBackVeto, 0, 0};
        m.ca = page->self.get();
        host_.handle(*this, *page->self, m);
        if (m.veto) return 1;
    }
    if (history_.empty()) return 0;
    Frame top = history_.back();
    bool scripted = false;
    if (page) scripted = script_play_default(*page->self, menu_msg::kPageBackScript);
    if (!scripted) {
        change_page(top.page->id, overlay_ ? 0xD : 5);
        if (top.control) select_control(top.control->id, top.control);
    }
    history_.pop_back();
    return 1;
}

// ---------------------------------------------------------------------------------------------
// Cursor / navigation

bool MenuManager::cursor_over(const Control& c) const {
    // Menu_CursorOverMe: the control with the smallest layer among the visible ones under the cursor.
    if (!current_) return false;
    const Control* best = nullptr;
    for (auto& o : current_->controls) {
        if (o->type == 13 || o->state != 0) continue;
        if (cursor_x_ < o->x || cursor_x_ > o->x + o->w || cursor_y_ < o->y || cursor_y_ > o->y + o->h) continue;
        if (!best || std::uint16_t(o->layer) < std::uint16_t(best->layer)) best = o.get();
    }
    return best == &c;
}

Control* MenuManager::first_control(Page& page) {
    if (page.current) return page.current;
    for (auto& c : page.controls)
        if (c->type != 13 && c->state == 0) return c.get();
    return nullptr;
}

// Menu_GetControl(0): the smallest visible control under the cursor.
Control* MenuManager::control_under_cursor() {
    if (!current_) return nullptr;
    Control* best = nullptr;
    float best_score = 0;
    for (auto& o : current_->controls) {
        if (!(o->x < cursor_x_ && cursor_x_ < o->x + o->w && o->y < cursor_y_ && cursor_y_ < o->y + o->h)) continue;
        if (o->state != 0 || o->type == 13) continue;
        const float score = (512.0f - float(o->w)) + (448.0f - float(o->h));
        if (!best || score > best_score || (score == best_score && std::uint16_t(o->layer) < std::uint16_t(best->layer))) {
            best = o.get();
            best_score = score;
        }
    }
    return best ? best : first_control(*current_);
}

// Menu_FindControl: directions 1 up, 2 down, 4 left, 8 right.
void MenuManager::find_control(int dir) {
    if (!current_) return;
    Control* cur = control_under_cursor();
    if (!cur) cur = current_->window();
    if (!cur) return;
    struct Cand { Control* c; int score; };
    std::vector<Cand> primary, wrap;
    for (auto& o : current_->controls) {
        if (o.get() == cur || o->state != 0 || o->type == 13) continue;
        if (o->w * o->h == 0) continue;
        const float dx = float(cur->x - o->x), dxr = float((cur->x + cur->w) - o->x);
        const float dy = float(cur->y - o->y), dyb = float((cur->y + cur->h) - o->y);
        const bool xover = dx < float(o->w) && dxr > 0.0f;
        const bool yover = dyb > 0.0f && dy < float(o->h);
        int s1 = 9999, s2 = 9999;
        if (dir == 2) {
            if (dy < float(o->h) && xover) s1 = int(std::fabs(dyb) + std::fabs(dyb) + std::fabs(dx) * 0.1f);
            if (float(o->h) < dy && xover) s2 = int(-dy + -dy + std::fabs(dx) * 0.1f);
        } else if (dir == 1) {
            if (float(o->h) < dy && xover) s1 = int(std::fabs(dy) + std::fabs(dy) + std::fabs(dx) * 0.1f);
            if (dyb < 0.0f && xover) s2 = int(dyb + dyb + std::fabs(dx) * 0.1f);
        } else if (dir == 4) {
            if (float(o->w) < dx && yover) s1 = int(std::fabs(dx) + std::fabs(dy));
            if (dxr < 0.0f && yover) s2 = int(dxr + std::fabs(dy));
        } else if (dir == 8) {
            if (dx < float(o->w) && yover) s1 = int(std::fabs(dxr) + std::fabs(dy));
            if (float(o->w) < dx && yover) s2 = int(std::fabs(dy) - dx);
        }
        if (s1 != 9999) primary.push_back({o.get(), std::int16_t(s1)});
        if (s2 != 9999) wrap.push_back({o.get(), std::int16_t(s2)});
        if (primary.size() >= 0x3F) break;
    }
    auto by_score = [](const Cand& l, const Cand& r) { return l.score < r.score; };
    Control* best = cur;
    if (!primary.empty()) {
        std::stable_sort(primary.begin(), primary.end(), by_score);
        best = primary.front().c;
    } else if (!wrap.empty()) {
        std::stable_sort(wrap.begin(), wrap.end(), by_score);
        best = wrap.front().c;
    }
    set_cursor(*best);
}

// ---------------------------------------------------------------------------------------------
// Page_Update

void MenuManager::page_update(Page& page, bool active) {
    if (page.controls.empty()) return;
    active_ = active;
    page.frames += kStepsPerUpdate;
    const bool is_current = &page == current_;
    if (is_current) {
        Control* c = control_under_cursor();
        if (c != page.current) {
            Control* old = page.current;
            page.current = c;
            if (c) c->age = 0;
            if (active_ && c) {
                if (old) dispatch(menu_msg::kDeselected, *old, nullptr, 0, 0);
                dispatch(menu_msg::kSelected, *c, nullptr, 0, 0);
                play_sound(MenuSound::Move);
            }
        }
    }
    for (auto& cp : page.controls) {
        Control& c = *cp;
        if (page.current && c.id == page.current->id) c.age = page.current->age;
        for (int step = 0; step < kStepsPerUpdate; ++step) run_script_frame(c);
        if (c.hidden()) c.cmds.clear();
        if (!c.hidden()) {
            ++c.age;
            const bool saved_active = active_;
            const int saved_controller = controller_;
            Control* saved_current = page.current;
            const int saved_cx = cursor_x_, saved_cy = cursor_y_;
            if (page.frames < page.min_frames) active_ = false;
            if (is_current)
                for (int u = 0; u < 4; ++u)
                    if (page.pad_control[std::size_t(u)] == &c) {   // the controller's own cursor control
                        controller_ = u;
                        cursor_x_ = c.x + 1, cursor_y_ = c.y + 1;
                        page.current = &c;
                        break;
                    }
            control_update(page, c);
            controller_ = saved_controller;
            active_ = saved_active;
            if (controller_ == saved_controller && page.current != saved_current && is_current) {
                page.current = saved_current;
                cursor_x_ = saved_cx, cursor_y_ = saved_cy;
            }
        }
    }
    if (is_current && active_ && !overlay_) dispatch(menu_msg::kIdle, *page.self, nullptr, 0, 0);
}

// ---------------------------------------------------------------------------------------------
// MenuManager_Update

void MenuManager::process_delayed() {
    for (std::size_t i = 0; i < delayed_.size();) {
        if (delayed_[i].due == int(frame_)) {
            Delayed d = delayed_[i];
            delayed_.erase(delayed_.begin() + long(i));
            if (d.manager) send_manager(d.type, d.a, d.b);
            else if (!d.text.empty()) send(d.control_id, d.type, d.text);
            else send(d.control_id, d.type, d.a, d.b);
        } else if (delayed_[i].due < int(frame_)) {
            delayed_.erase(delayed_.begin() + long(i));
        } else {
            ++i;
        }
    }
}

void MenuManager::clear_input() {
    for (auto& i : inputs_) i.clear();
}

bool MenuManager::act(int action, unsigned flags) const {
    if (controller_ >= 0 && controller_ < 4) return inputs_[std::size_t(controller_)].action(action, flags, status_);
    bool any = false;
    for (const auto& i : inputs_) any = i.action(action, flags, status_) || any;
    return any;
}

unsigned MenuManager::idle_frames() const {
    unsigned n = ~0u;
    for (const auto& i : inputs_) n = std::min(n, i.idle_frames());
    return n == ~0u ? n : n * kStepsPerUpdate;
}

void MenuManager::update(const PadState& pad) {
    PadInputs pads;
    pads[0] = pad;
    update(pads);
}

void MenuManager::update(const PadInputs& pads) {
    for (std::size_t i = 0; i < 4; ++i) inputs_[i].update(pads[i]);
    // The menu logic counts 60 Hz steps: with PS2FramesToSkip == 2 (one update per 30 Hz frame) the original
    // runs Menu_AlphaUpdate / Menu_ProcessDelayedMessages and the scripts twice.
    for (int step = 0; step < kStepsPerUpdate; ++step) {
        alpha_phase_ += 0.2f;
        if (alpha_phase_ >= 6.2831855f) alpha_phase_ = 0;
        alpha_ = int(std::sin(alpha_phase_) * 64.0f);
        ++frame_;
        process_delayed();
    }
    if (!current_) return;

    if (!input_lock_) {
        // triangle: the controller's cursor control script, else the history
        for (int u = 0; u < 4; ++u) {
            const int saved = controller_;
            controller_ = u;
            const bool pressed = act(menu_action::kActPageBack, menu_action::kPressed);
            controller_ = saved;
            if (!pressed) continue;
            Control* pc = current_->pad_control[std::size_t(u)];
            Control* target = pc ? pc : current_->current;
            if (target && script_play_default(*target, menu_msg::kScriptEvent)) continue;
            if (!overlay_) {
                play_sound(MenuSound::PageBack);
                send_manager(menu_msg::kPageBack, 0, std::uint32_t(u));
                return;
            }
        }
        bool pad_controls = false;
        for (Control* c : current_->pad_control) pad_controls = pad_controls || c;
        if (!pad_controls) {
            using namespace menu_action;
            const unsigned f = kPressed | kRepeat;
            if (current_->nav == 3 || current_->nav == 1) {
                if (act(kActLeft, f)) find_control(4);
                if (act(kActRight, f)) find_control(8);
            }
            if (current_->nav == 2 || current_->nav == 1) {
                if (act(kActUp, f)) find_control(1);
                if (act(kActDown, f)) find_control(2);
            }
        }
    }
    if (current_) page_update(*current_, true);
    if (under_ && under_ != current_) page_update(*under_, false);
}

// ---------------------------------------------------------------------------------------------
// Drawing

void MenuManager::draw(Renderer& renderer, TextRenderer& text) const {
    constexpr float kScaleX = 1.25f;   // the 512-wide buffer is stretched to 4:3
    const Layout layout = Layout::from_canvas_width(renderer.canvas_width());
    struct Item {
        const DrawCmd* cmd;
        int layer;
        std::size_t order;
    };
    std::vector<Item> items;
    std::size_t order = 0;
    for (const auto& p : pages_) {
        if (p->state == 1) continue;
        for (const auto& c : p->controls)
            for (const DrawCmd& d : c->cmds) items.push_back({&d, d.layer, order++});
    }
    // Lower layers are in front (the fade label of layer 20 covers the layer 25 controls).
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.layer > b.layer; });
    for (const Item& it : items) {
        const DrawCmd& d = *it.cmd;
        const float x = d.is_text ? d.x : d.dst.x;
        const float y = d.is_text ? d.y : d.dst.y;
        const bool top_chrome = y >= 24.0f && y <= 72.0f;
        const bool left_anchor = x < 256.0f;
        if (d.is_text) {
            TextStyle st = d.style;
            st.scale_x = kScaleX;
            float draw_x = d.x * kScaleX;
            if (top_chrome) draw_x = layout.x(draw_x, left_anchor ? HorizontalAnchor::Left : HorizontalAnchor::Right);
            // Settings > Accessibility > High contrast: the button-prompt row (glyph escapes, script y 419) is drawn
            // near-white on a dark plate.
            if (accessibility().high_contrast && d.y >= 395.0f && d.text.find('~') != std::string::npos) {
                const TextMetrics m = text.measure(d.text, st);
                const float left = st.align == Align::Center ? draw_x - m.width * 0.5f
                                   : st.align == Align::Right ? draw_x - m.width : draw_x;
                renderer.fill({left - 8.0f, d.y - 16.0f, m.width + 16.0f, 22.0f}, {4, 2, 4, 0x78});
                st.color = 0x80807CFF;
                st.shadow_color = 0x00000080;
                st.outline = true;
            }
            text.draw(draw_x, d.y, d.text, st);
        } else {
            Rect dst{d.dst.x * kScaleX, d.dst.y, d.dst.w * kScaleX, d.dst.h};
            if (top_chrome && dst.w <= 200.0f)
                dst.x = layout.x(dst.x + (left_anchor ? 0.0f : dst.w),
                                 left_anchor ? HorizontalAnchor::Left : HorizontalAnchor::Right) -
                        (left_anchor ? 0.0f : dst.w);
            renderer.draw(d.hash, dst, d.src, d.color);
        }
    }
}

}  // namespace nf::ui

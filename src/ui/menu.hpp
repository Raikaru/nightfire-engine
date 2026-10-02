#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "assets/menu_file.hpp"
#include "assets/menu_messages.hpp"
#include "assets/strings.hpp"
#include "assets/ui_fonts.hpp"
#include "game/input.hpp"
#include "ui/renderer.hpp"
#include "ui/text.hpp"

namespace nf::ui {

// The runtime of the original front end (MenuManager_*, Manager_/Page_/Button_/Label_/List_/Radio_/
// Scroll_/Memo_/Window_SendMessage, Component_*, Script_*; docs/ui.md "Front-end runtime"). All
// geometry is in the 512x448 buffer the PS2 draws into; `MenuManager::draw` scales x by 1.25 onto the
// 640x448 renderer canvas.

class Control;
class Page;
class MenuManager;

// One control message. `text` carries the string payloads (0x18 literal, 0x10 row text, 0x23 format);
// `ca`/`cb` the pointer-valued arguments (the original passes control pointers in a/b).
struct Msg {
    Msg() = default;
    Msg(std::uint32_t t, std::uint32_t a_ = 0, std::uint32_t b_ = 0, std::string s = {}, Control* ca_ = nullptr, Control* cb_ = nullptr)
        : type(t), a(a_), b(b_), text(std::move(s)), ca(ca_), cb(cb_) {}
    std::uint32_t type = 0;
    std::uint32_t a = 0, b = 0;
    std::string text;
    Control* ca = nullptr;
    Control* cb = nullptr;
    mutable bool veto = false;    // 0x6b: a handler sets it to cancel the back navigation
};

// Sound effects the menus request (MENU_SOUND of Menu_PlaySound), collected for the application.
enum class MenuSound : std::uint8_t {
    Accept = 0, Back = 1, Alt = 2, Left = 3, Right = 4, LeftRepeat = 5, RightRepeat = 6, Move = 7, PageBack = 8, Memo = 9,
};

// Menu_InputAction: the digital menu actions 26..34 of PlayerSetting with Input_Update's flag byte.
namespace menu_action {
constexpr int kActUp = 0x1a, kActDown = 0x1b, kActLeft = 0x1c, kActRight = 0x1d;
constexpr int kActStart = 0x1e, kActAccept = 0x1f /* cross */, kActBack = 0x20 /* square */, kActAlt = 0x21 /* circle */,
              kActPageBack = 0x22 /* triangle */;
constexpr unsigned kHeld = 1, kPressed = 4, kRepeat = 8;
}  // namespace menu_action

class MenuInput {
public:
    void update(const PadState& pad);   // once per 30 Hz frame (Input_Update)
    void clear();                       // Input_ClearAllActions: a held button counts as newly pressed again
    // Menu_InputAction(any controller, action, flags); `status` is MenuManager_GetStatus (>= 4 for the
    // multiplayer pause menu, which aliases the start and accept actions).
    bool action(int action, unsigned flags, int status = 0) const;
    // Frames since a menu action last fired (Menu_GetNoInputCount).
    unsigned idle_frames() const { return frame_ - last_input_; }
    void reset_idle() { last_input_ = frame_; }

private:
    bool raw(int action, unsigned flags) const;
    static constexpr int kFirst = 0x19, kCount = 10;
    std::array<std::uint8_t, kCount> flags_{};
    std::array<std::uint16_t, kCount> hold_{};
    std::array<bool, kCount> down_{};
    unsigned frame_ = 0;
    mutable unsigned last_input_ = 0;
};

// Where the handlers plug in. `handle` is Handler_HandleMessage: it is offered every message
// addressed to a page or a control (`ctrl` is the page for page-level messages) and returns true
// when it consumed it.
class MenuHost {
public:
    virtual ~MenuHost() = default;
    virtual bool handle(MenuManager& mgr, Control& ctrl, const Msg& msg) = 0;
};

struct DrawCmd {
    int layer = 0;
    bool is_text = false;
    std::uint32_t hash = 0;          // image
    Rect dst, src;
    Color color;
    std::string text;                // text
    TextStyle style;
    float x = 0, y = 0;
};

// One row of a radio (value selector) or list.
struct Row {
    std::array<std::string, 5> cell;   // cell[0] is the radio text
    std::uint32_t value = 0;           // +0x2c4
    bool disabled = false;             // +0x2c8 (list rows that cannot be selected)
};

struct LabelState {
    std::string text;                                  // +0xf4
    std::uint32_t text_hash = 0xFFFFFFFF;              // +0xf0 label hash rebound on every update, -1 literal
    std::string format = "\xFF\x02\xFE\x03";           // +0xc0 (default: font 2, centred)
    std::uint32_t sprite = 0;                          // +0xf8
    bool text_mode = true;                             // +0x108 bit 1
    bool colours_set = false;                          // +0x108 bit 0
    std::uint32_t color_selected = 0xFF;               // +0xfc (0x1b)
    std::uint32_t color_normal = 0xFFFFFF7F;           // +0x100 (0x1c)
    std::uint32_t color = 0x7F7F7FC0;                  // +0xb4 current sprite colour
    std::uint32_t pulse_base = 0x808080FF;             // +0x104 (0x29)
    std::uint16_t u = 0, v = 0, uw = 0, vh = 0;        // +0xd4..0xda (0x2d)
    bool outline = true;                               // +0x10e (0x62)
    bool pulse = false;                                // +0x10f (0x70)
    bool always_selected = false;                      // +0x110 (0x76)
    int fade_state = 0;                                // +0x10c
};

struct ScrollState {
    int min = 0, max = 0, value = 0;                   // +0x128, +0x12c, +0x130
    bool wrap = true;                                  // +0x144
    bool slider = false;                               // +0x145 (0x69)
    bool vertical = false;                             // +0x13c bit 1
    bool embedded = false;                             // +0x13c bit 0: drawn by its owner
    int hold = 0;                                      // +0x138
    int pressed = 0;                                   // +0x142 (0x40 first arrow, 0x80 second)
};

// A running script (Script_Play*): control+0x38..0x58.
struct ScriptRun {
    const MenuScript* script = nullptr;
    int next = -1;             // +0x3c: keyframe reached next, -1 = finished
    int loop_keyframe = -1;    // +0x40
    int loop_index = 0;        // +0x44
    int index = -1;            // +0x4c
    int frame = 0;             // +0x54
    std::uint32_t flags = 0;   // script parameter (+0x1c of the script) with the loop bit 8
    std::uint32_t result = 0;  // +0x50 result of the previous keyframe message
};

struct Process {              // Process_Create: a 0xE1 colour fade
    std::uint32_t target = 0, start = 0;
    int elapsed = 0, duration = 0;
    bool flag = false;
};

class Control {
public:
    Control(Page& page, const MenuControl& def);

    Page& page;
    const MenuControl* def;
    std::uint32_t id;
    std::uint8_t type;
    int x, y, w, h;                    // +0x70..+0x76
    int layer;                         // +0x78
    std::uint32_t index;               // +0x20
    std::uint16_t skin_set = 0;        // +0x14 high half
    std::uint16_t skin_comp = 0;       // +0x14 low half
    std::uint8_t state = 0;            // +0x7a: bit 0 hidden, bit 2 idle-hidden
    std::uint32_t age = 0;             // +0x24
    std::array<const MenuScript*, 7> slots{};   // +0x7c..
    ScriptRun run;
    std::vector<Process> processes;

    LabelState label;                  // labels and the label inside buttons/radios
    ScrollState scroll;                // scroll controls, radios, memos, lists
    std::vector<Row> rows;             // radio / list
    int current_row = -1;              // radio: +0x304, list: +0x560
    int list_top = 0;                  // +0x534
    int list_visible = 0;              // +0x500
    std::array<int, 5> column_width{100, 0, 0, 0, 0};   // +0x508 (percent)
    int columns = 1;                   // +0x504
    std::array<int, 5> column_align{1, 1, 1, 1, 1};   // +0x51c (1 left, 2 right, 3 centre)
    int list_format = 1;               // 0x23 a: 1 font 2, 2 font 1, 3 font 3
    int list_mode = 1;                 // +0x564: 1 selection bar + colour, 2 colour only
    std::uint32_t list_selected_color = 0x7F7F7FFF;   // +0x540
    // memo
    std::string memo_text;             // +0x1dc
    std::string memo_format = "\xFF\x02";     // +0x1e0
    float memo_line_height = 0;        // +0x26c
    int memo_lines = 0, memo_visible = 0, memo_top = 0;   // +0x268, +0x264, +0x270
    bool memo_centre = true;           // +0x280 (0x64)
    bool memo_vcentre = true;          // +0x282 (0x66)
    std::uint32_t memo_color = 0xFF;   // +0x274 (0x1b)

    std::vector<DrawCmd> cmds;         // sprites of the last update

    bool hidden() const { return (state & 1) != 0; }
    ui::Rect box() const { return {float(x), float(y), float(w), float(h)}; }
};

class Page {
public:
    Page(MenuManager& mgr, const MenuPage& def);

    MenuManager& manager;
    const MenuPage* def;
    std::uint32_t id;
    std::vector<std::unique_ptr<Control>> controls;   // controls[0] is the window
    int layer = 0;                     // +0x78
    std::uint8_t state = 0;            // +0x7a: 0 shown, 1 hidden, 2 shown below an overlay
    Control* current = nullptr;        // +0xac
    std::array<Control*, 4> pad_control{};   // +0xb0: per-controller cursor controls (multiplayer join/setup pages)
    int nav = 1;                       // +0xc0: 1 both axes, 2 vertical only, 3 horizontal only
    std::uint32_t frames = 0;          // +0xc8
    std::uint32_t min_frames = 0;      // +0xcc
    std::uint32_t movie = 0;           // +0xc4 (the page's "extra": 0x7350048 / 0x7330048 background 3D scene id)

    MenuControl self_def;              // the page seen as a type-8 control (what handlers receive)
    std::unique_ptr<Control> self;

    Control* window() { return controls.empty() ? nullptr : controls.front().get(); }
    Control* find(std::uint32_t control_id);
    bool visible() const { return (state & 5) == 0; }
};

// The whole state of one menu (MenuManager index 0).
class MenuManager {
public:
    static constexpr int kStepsPerUpdate = 2;   // 60 Hz menu steps per 30 Hz update (PS2FramesToSkip == 2)
    // `menu_id` selects the pages of `file` (MenuManager_Create): pages of other menus are ignored.
    MenuManager(const MenuFile& file, const StringTable& strings, const FontSet& fonts, MenuHost& host,
                std::uint32_t menu_id, int status = 0);
    // Creates the pages and shows `start_page` (Manager 0x44, flags 1). Separate from the constructor because the
    // handlers reach the manager through their owner while pages are created and shown.
    void start(std::uint32_t start_page);
    ~MenuManager();

    void update(const PadInputs& pads);               // MenuManager_Update, 30 Hz (all four controllers)
    void update(const PadState& pad);                 // controller 0 only
    void draw(Renderer& renderer, TextRenderer& text) const;

    const StringTable& strings() const { return strings_; }
    const MenuFile& file() const { return file_; }
    // Menu_InputAction for the controller being served (-1: any of the four).
    bool act(int action, unsigned flags) const;
    void input_reset_idle() { for (auto& i : inputs_) i.reset_idle(); }   // Menu_ResetNoInputCount
    unsigned idle_frames() const;                      // Menu_GetNoInputCount
    int controller() const { return controller_; }
    std::uint32_t current_page_id() const;
    Page* current_page() { return current_; }
    Page* find_page(std::uint32_t id);
    bool input_locked() const { return input_lock_; }
    int status() const { return status_; }
    unsigned frame() const { return frame_; }
    int alpha() const { return alpha_; }              // Menu_AlphaGet
    std::vector<MenuSound> take_sounds();

    // Manager_SendMessage: change page (0x44), lock input (0x68), select control (0x22), history back (0x5f).
    int send_manager(std::uint32_t type, std::uint32_t a = 0, std::uint32_t b = 0, Control* cb = nullptr);
    // __Menu_Send: to every control with `control_id` on every page (-1: all); the last non-zero result.
    int send(std::uint32_t control_id, std::uint32_t type, std::uint32_t a = 0, std::uint32_t b = 0);
    int send(std::uint32_t control_id, std::uint32_t type, std::string text, std::uint32_t b = 0);
    // Menu_SendEx: only the controls whose index (control+0x20) equals `index`.
    int send_ex(std::uint32_t control_id, std::uint32_t index, std::uint32_t type, std::uint32_t a = 0, std::uint32_t b = 0);
    int send_ex(std::uint32_t control_id, std::uint32_t index, std::uint32_t type, std::string text, std::uint32_t b = 0);
    // The control with `control_id` and index `index` on the current page (nullptr when absent).
    Control* find_ex(std::uint32_t control_id, std::uint32_t index);
    // The single control with `control_id` on the current page (or the first page that has one).
    Control* find(std::uint32_t control_id);
    // __Menu_SendMessage to one control.
    int send_to(Control& ctrl, const Msg& msg);
    // __Menu_SendDelayed: after `frames` updates (128 slots).
    void send_delayed(int frames, std::uint32_t control_id, std::uint32_t type, std::uint32_t a = 0, std::uint32_t b = 0);
    void send_delayed_text(int frames, std::uint32_t control_id, std::uint32_t type, std::string text);
    void send_delayed_manager(int frames, std::uint32_t type, std::uint32_t a = 0, std::uint32_t b = 0);
    void clear_delayed();
    void play_sound(MenuSound s) { sounds_.push_back(s); }
    // Font_GetTextExtent for the layout code and the handlers.
    TextMetrics measure(std::string_view text, const TextStyle& style) const { return measurer_.measure(text, style); }
    // Starts the fade of a control's colour (Process_Create with a 0xE1 message).
    void fade(Control& c, std::uint32_t target, int frames, bool flag);
    bool script_play_default(Control& c, std::uint32_t event);   // Script_PlayDefault
    // Wraps a memo's text now and returns its line count (message 0x65 after Memo_Update).
    int memo_line_count(Control& memo);
    const MenuFile& menu_file() const { return file_; }

private:
    friend class Control;
    friend class Page;

    // --- pages, transitions (menu_manager.cpp) ---
    int change_page(std::uint32_t id, unsigned flags);
    int select_control(std::uint32_t id, Control* target);
    int page_back(std::uint32_t a, std::uint32_t pad);
    int assign_pad_control(std::uint32_t id, std::uint32_t pad);
    void clear_input();
    void process_delayed();
    void page_update(Page& page, bool active);
    Control* control_under_cursor();
    Control* first_control(Page& page);
    void find_control(int direction);
    bool cursor_over(const Control& c) const;
    void set_cursor(const Control& c);
    void delete_all();
    // Manager_SendMessage default: the handler chain.
    int dispatch(std::uint32_t type, Control& a, Control* b, std::uint32_t av, std::uint32_t bv);
    int page_message(Page& page, const Msg& msg);
    void create_page(const MenuPage& def);
    Control& create_control(Page& page, const MenuControl& def);

    // --- scripts (menu_script.cpp) ---
    void run_script_frame(Control& c);
    void interpolate(Control& c, const ScriptRun& run, float t);
    bool script_start(Control& c, std::uint32_t script_id, bool fast_forward);
    void send_keyframe_message(Control& c, ScriptRun& run, const MenuKeyframeMessage& m, int frames_left);
    void run_processes(Control& c);

    // --- per control type (menu_controls.cpp) ---
    int control_message(Control& c, const Msg& msg);
    void control_update(Page& page, Control& c);
    void update_label(Page& page, Control& owner, LabelState& l, ui::Rect r, bool hover, int = 0);
    void update_button(Page& page, Control& c);
    void update_radio(Page& page, Control& c);
    void update_scroll(Page& page, Control& c);
    void update_memo(Page& page, Control& c);
    void update_list(Page& page, Control& c);
    void skin_commands(Page& page, Control& c, std::uint16_t comp, unsigned state, int x, int y, int w, int h,
                       int layer, std::vector<DrawCmd>& out);
    const MenuComponent* component(const Control& c, std::uint16_t comp) const;
    const MenuComponent* own_component(const Control& c) const { return component(c, c.skin_comp); }
    void clip_string(std::string& text, const LabelState& l, int width) const;
    void scroll_input(Page& page, Control& c, ScrollState& s, Control& owner, bool selected);
    void notify_parent(Control& c, std::uint32_t type, std::uint32_t a, std::uint32_t b);
    void emit_activation(Page& page, Control& c, int ev);
    Row& add_row(Control& c);
    void memo_wrap(Control& c);

    const MenuFile& file_;
    const StringTable& strings_;
    MenuHost& host_;
    TextRenderer measurer_;
    std::uint32_t menu_id_;
    int status_;
    std::vector<std::unique_ptr<Page>> pages_;
    Page* current_ = nullptr;
    Page* under_ = nullptr;            // +0x1c0: the page kept below an overlay
    bool overlay_ = false;             // +0x1d3
    bool active_ = false;              // +0x1d2 Page_Update's "accept input" flag
    bool back_vetoed_ = false;
    bool input_lock_ = false;          // +0x1d4
    struct Frame { Page* page; Control* control; };
    std::vector<Frame> history_;       // +0xa8 stack
    std::array<MenuInput, 4> inputs_;
    int controller_ = -1;              // +0x1cc: the controller whose input the controls read (-1 any)
    struct Delayed { int due = 0; bool manager = false; std::uint32_t control_id = 0, type = 0, a = 0, b = 0; std::string text; };
    std::vector<Delayed> delayed_;
    unsigned frame_ = 0;               // uGpffff8b20
    float alpha_phase_ = 0;
    int alpha_ = 0;                    // Menu_AlphaGet
    int cursor_x_ = 0, cursor_y_ = 0;  // the cursor sprite (+0x94): centre of the selected control
    std::vector<MenuSound> sounds_;
};

}  // namespace nf::ui

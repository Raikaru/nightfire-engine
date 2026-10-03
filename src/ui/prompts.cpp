#include "ui/prompts.hpp"

#include <algorithm>
#include <cctype>
#include <string>

#include "ui/art_sheet.hpp"
#include "ui/text.hpp"

namespace nf::ui {

namespace {

constexpr std::uint16_t kDirs = kPadUp | kPadDown | kPadLeft | kPadRight;

std::optional<SpecialChar> art(std::string_view name) {
    const ArtSprite* s = art_sprite("prompts", name);
    if (!s) return std::nullopt;
    SpecialChar c{};
    c.texture_hash = s->hash;
    c.u = std::uint16_t(s->src.x);
    c.v = std::uint16_t(s->src.y);
    c.w = std::uint16_t(s->src.w);
    c.h = std::uint16_t(s->src.h);
    c.advance = std::int16_t(s->src.w);
    c.height = std::int16_t(s->src.h);
    return c;
}

std::optional<SpecialChar> original(const FontSet& fonts, char key) {
    const SpecialChar* s = fonts.find_special(key);
    if (!s) return std::nullopt;
    return *s;
}

// The original glyph of a gamepad control (PlayStation layout).
char ps_key(const DeviceInput& in) {
    if (in.kind == DeviceInput::PadAxis) return in.code == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? 'C' : 'D';
    switch (SDL_GamepadButton(in.code)) {
        case SDL_GAMEPAD_BUTTON_SOUTH: return 'A';
        case SDL_GAMEPAD_BUTTON_EAST: return 'X';
        case SDL_GAMEPAD_BUTTON_WEST: return 'Y';
        case SDL_GAMEPAD_BUTTON_NORTH: return 'B';
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return 'L';
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return 'R';
        case SDL_GAMEPAD_BUTTON_LEFT_STICK: return 'F';
        case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return 'E';
        case SDL_GAMEPAD_BUTTON_START: return 'S';
        case SDL_GAMEPAD_BUTTON_BACK: return 'T';
        case SDL_GAMEPAD_BUTTON_DPAD_UP:
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return 'V';
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return 'H';
        default: return 0;
    }
}

// The Xbox-style art of a gamepad control; face buttons carry the letter the pad itself prints there.
std::string xbox_name(const DeviceInput& in, SDL_GamepadType type) {
    if (in.kind == DeviceInput::PadAxis) return in.code == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? "pad_lt" : "pad_rt";
    const auto b = SDL_GamepadButton(in.code);
    switch (b) {
        case SDL_GAMEPAD_BUTTON_SOUTH:
        case SDL_GAMEPAD_BUTTON_EAST:
        case SDL_GAMEPAD_BUTTON_WEST:
        case SDL_GAMEPAD_BUTTON_NORTH:
            switch (SDL_GetGamepadButtonLabelForType(type, b)) {
                case SDL_GAMEPAD_BUTTON_LABEL_A: return "pad_a";
                case SDL_GAMEPAD_BUTTON_LABEL_B: return "pad_b";
                case SDL_GAMEPAD_BUTTON_LABEL_X: return "pad_x";
                case SDL_GAMEPAD_BUTTON_LABEL_Y: return "pad_y";
                default:   // no letters (unknown pads): the Xbox positions
                    return b == SDL_GAMEPAD_BUTTON_SOUTH ? "pad_a" : b == SDL_GAMEPAD_BUTTON_EAST ? "pad_b"
                           : b == SDL_GAMEPAD_BUTTON_WEST ? "pad_x" : "pad_y";
            }
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return "pad_lb";
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return "pad_rb";
        case SDL_GAMEPAD_BUTTON_LEFT_STICK: return "pad_ls";
        case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return "pad_rs";
        case SDL_GAMEPAD_BUTTON_START: return "pad_menu";
        case SDL_GAMEPAD_BUTTON_BACK: return "pad_view";
        case SDL_GAMEPAD_BUTTON_DPAD_UP: return "dpad_up";
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return "dpad_down";
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return "dpad_left";
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return "dpad_right";
        default: return {};
    }
}

// The keycap or mouse art of a keyboard/mouse input.
std::string keyboard_name(const DeviceInput& in) {
    if (in.kind == DeviceInput::Mouse) {
        static constexpr const char* kMouse[] = {"mouse_left", "mouse_right", "mouse_middle", "mouse_x1",
                                                 "mouse_x2",   "mouse_wheel_up", "mouse_wheel_down"};
        return kMouse[in.code];
    }
    const auto k = SDL_Scancode(in.code);
    if (k >= SDL_SCANCODE_A && k <= SDL_SCANCODE_Z) return "key_" + std::string(1, char('a' + (k - SDL_SCANCODE_A)));
    if (k >= SDL_SCANCODE_1 && k <= SDL_SCANCODE_9) return "key_" + std::string(1, char('1' + (k - SDL_SCANCODE_1)));
    if (k >= SDL_SCANCODE_F1 && k <= SDL_SCANCODE_F12) return "key_f" + std::to_string(1 + (k - SDL_SCANCODE_F1));
    if (k >= SDL_SCANCODE_KP_1 && k <= SDL_SCANCODE_KP_9) return "key_kp_" + std::to_string(1 + (k - SDL_SCANCODE_KP_1));
    switch (k) {
        case SDL_SCANCODE_0: return "key_0";
        case SDL_SCANCODE_RETURN: return "key_return";
        case SDL_SCANCODE_ESCAPE: return "key_escape";
        case SDL_SCANCODE_BACKSPACE: return "key_backspace";
        case SDL_SCANCODE_TAB: return "key_tab";
        case SDL_SCANCODE_SPACE: return "key_space";
        case SDL_SCANCODE_MINUS: return "key_minus";
        case SDL_SCANCODE_EQUALS: return "key_equals";
        case SDL_SCANCODE_LEFTBRACKET: return "key_leftbracket";
        case SDL_SCANCODE_RIGHTBRACKET: return "key_rightbracket";
        case SDL_SCANCODE_BACKSLASH: return "key_backslash";
        case SDL_SCANCODE_SEMICOLON: return "key_semicolon";
        case SDL_SCANCODE_APOSTROPHE: return "key_apostrophe";
        case SDL_SCANCODE_GRAVE: return "key_grave";
        case SDL_SCANCODE_COMMA: return "key_comma";
        case SDL_SCANCODE_PERIOD: return "key_period";
        case SDL_SCANCODE_SLASH: return "key_slash";
        case SDL_SCANCODE_CAPSLOCK: return "key_capslock";
        case SDL_SCANCODE_INSERT: return "key_insert";
        case SDL_SCANCODE_HOME: return "key_home";
        case SDL_SCANCODE_PAGEUP: return "key_pageup";
        case SDL_SCANCODE_DELETE: return "key_delete";
        case SDL_SCANCODE_END: return "key_end";
        case SDL_SCANCODE_PAGEDOWN: return "key_pagedown";
        case SDL_SCANCODE_RIGHT: return "key_right";
        case SDL_SCANCODE_LEFT: return "key_left";
        case SDL_SCANCODE_DOWN: return "key_down";
        case SDL_SCANCODE_UP: return "key_up";
        case SDL_SCANCODE_KP_DIVIDE: return "key_kp_divide";
        case SDL_SCANCODE_KP_MULTIPLY: return "key_kp_multiply";
        case SDL_SCANCODE_KP_MINUS: return "key_kp_minus";
        case SDL_SCANCODE_KP_PLUS: return "key_kp_plus";
        case SDL_SCANCODE_KP_ENTER: return "key_kp_enter";
        case SDL_SCANCODE_KP_0: return "key_kp_0";
        case SDL_SCANCODE_KP_PERIOD: return "key_kp_period";
        case SDL_SCANCODE_LCTRL: return "key_lctrl";
        case SDL_SCANCODE_LSHIFT: return "key_lshift";
        case SDL_SCANCODE_LALT: return "key_lalt";
        case SDL_SCANCODE_RCTRL: return "key_rctrl";
        case SDL_SCANCODE_RSHIFT: return "key_rshift";
        case SDL_SCANCODE_RALT: return "key_ralt";
        default: return "key_unknown";
    }
}

bool is_key(const std::optional<DeviceInput>& in, SDL_Scancode k) {
    return in && in->kind == DeviceInput::Key && in->code == int(k);
}
bool is_mouse(const std::optional<DeviceInput>& in, MouseInput m) {
    return in && in->kind == DeviceInput::Mouse && in->code == int(m);
}
bool is_dpad(const std::optional<DeviceInput>& in) {
    return in && in->kind == DeviceInput::PadButton && in->code >= SDL_GAMEPAD_BUTTON_DPAD_UP &&
           in->code <= SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
}

}  // namespace

std::uint16_t prompt_buttons(char key) {
    switch (key) {
        case 'A': return kPadCross;
        case 'B': return kPadTriangle;
        case 'X': return kPadCircle;
        case 'Y': return kPadSquare;
        case 'L': return kPadL1;
        case 'R': return kPadR1;
        case 'C': return kPadL2;
        case 'D': return kPadR2;
        case 'S': return kPadStart;
        case 'T': return kPadSelect;
        case 'V': return kPadUp | kPadDown;
        case 'H': return kPadLeft | kPadRight;
        case 'W': return kDirs;
        case 'F': return kPadL3;   // left stick
        case 'E': return kPadR3;   // right stick
        default: return 0;
    }
}

char prompt_key(std::uint16_t button) {
    for (char k : std::string_view("ABXYLRCDSTFE"))
        if (prompt_buttons(k) == button) return k;
    return 0;
}

std::uint16_t action_button(Action action, int style) {
    // Ask the style's psiInput_MapInputs which single button sets the action (sticks centred, so only buttons count).
    PadState centred;
    centred.rx = centred.ry = centred.lx = centred.ly = 0x7F;
    for (int bit = 0; bit < 16; ++bit) {
        PadState p = centred;
        p.buttons = std::uint16_t(1u << bit);
        if (map_inputs(p, style, 1)[std::size_t(action)] != 0.0f) return p.buttons;
    }
    return 0;
}

void select_prompts(int player, InputContext context, bool text_entry) {
    if (PromptGlyphs* p = TextRenderer::prompts()) p->select(player, context, text_entry);
}

void PromptGlyphs::select(int player, InputContext context, bool text_entry) {
    if (player == player_ && context == context_ && text_entry == text_entry_) return;
    player_ = player;
    context_ = context;
    text_entry_ = text_entry;
    for (Cache& c : cache_) c.built = 0;
}

const SpecialChar* PromptGlyphs::find(char key, int player) const {
    const bool upper = key >= 'A' && key <= 'Z', lower = key >= 'a' && key <= 'z';
    if (!upper && !lower) return fonts_.find_special(key);
    player = std::clamp(player, 0, InputDevices::kPlayers - 1);
    Cache& c = cache_[std::size_t(player)];
    if (c.built != input_devices().generation()) {
        for (int i = 0; i < 26; ++i) {
            c.glyphs[std::size_t(i)] = resolve(char('A' + i), player);
            c.glyphs[std::size_t(26 + i)] = resolve(char('a' + i), player);
        }
        c.built = input_devices().generation();
    }
    const auto& slot = c.glyphs[std::size_t(upper ? key - 'A' : 26 + key - 'a')];
    return slot ? &*slot : nullptr;
}

std::optional<SpecialChar> PromptGlyphs::resolve(char key, int player) const {
    const bool gameplay = key >= 'a' && key <= 'z';
    const char k = char(std::toupper(static_cast<unsigned char>(key)));
    const InputContext context = gameplay ? InputContext::OnFoot : context_;
    if (prompt_buttons(k) == 0) return original(fonts_, k);
    std::optional<SpecialChar> out = input_devices().device(player) == InputDevice::KeyboardMouse
                                         ? resolve_keyboard(k, context)
                                         : resolve_pad(k, context, player);
    return out ? out : original(fonts_, k);
}

std::optional<SpecialChar> PromptGlyphs::resolve_pad(char key, InputContext context, int player) const {
    const InputBindings& b = input_bindings();
    const bool ps = input_devices().device(player) == InputDevice::PlayStation;
    const SDL_GamepadType type = input_devices().gamepad_type(player);
    const std::uint16_t buttons = prompt_buttons(key);
    if (key == 'E' || key == 'F') {   // sticks are fixed: physical left / right stick
        if (ps) return original(fonts_, key);
        return art(key == 'F' ? "pad_ls" : "pad_rs");
    }
    if (buttons & (buttons - 1)) {    // V, H, W: a D-pad group
        const auto first = b.first(context, std::uint16_t(buttons & -buttons), false);
        if (!is_dpad(first)) return std::nullopt;
        if (ps) return original(fonts_, key);
        return art(key == 'V' ? "dpad_updown" : key == 'H' ? "dpad_leftright" : "dpad_all");
    }
    const auto in = b.first(context, buttons, false);
    if (!in) return std::nullopt;
    if (ps) {
        const char k = ps_key(*in);
        return k ? original(fonts_, k) : std::nullopt;
    }
    const std::string name = xbox_name(*in, type);
    return name.empty() ? std::nullopt : art(name);
}

std::optional<SpecialChar> PromptGlyphs::resolve_keyboard(char key, InputContext context) const {
    const InputBindings& b = input_bindings();
    const std::uint16_t buttons = prompt_buttons(key);
    if (key == 'F') return art("key_wasd");     // left stick: W/S + A/D (LocalPad)
    if (key == 'E') return art("mouse_move");   // right stick: mouse look
    auto first = [&](std::uint16_t button) { return b.first(context, button, true, text_entry_); };
    if (key == 'V' || key == 'H' || key == 'W') {
        const auto up = first(key == 'H' ? kPadLeft : kPadUp), down = first(key == 'H' ? kPadRight : kPadDown);
        if (key == 'W') {
            if (is_key(up, SDL_SCANCODE_UP) && is_key(down, SDL_SCANCODE_DOWN) && is_key(first(kPadLeft), SDL_SCANCODE_LEFT) &&
                is_key(first(kPadRight), SDL_SCANCODE_RIGHT))
                return art("key_arrows");
        } else if (key == 'V' && is_key(up, SDL_SCANCODE_UP) && is_key(down, SDL_SCANCODE_DOWN)) {
            return art("key_updown");
        } else if (key == 'H' && is_key(up, SDL_SCANCODE_LEFT) && is_key(down, SDL_SCANCODE_RIGHT)) {
            return art("key_leftright");
        } else if (key == 'V' && is_mouse(up, MouseInput::WheelUp) && is_mouse(down, MouseInput::WheelDown)) {
            return art("mouse_wheel");
        }
        if (up) return art(keyboard_name(*up));
        return down ? art(keyboard_name(*down)) : std::nullopt;
    }
    if (const auto in = first(buttons)) return art(keyboard_name(*in));
    // The sessions pause on Esc (a key event, not a binding).
    if (buttons == kPadStart && (context == InputContext::OnFoot || context == InputContext::Driving))
        return art("key_escape");
    return std::nullopt;
}

}  // namespace nf::ui

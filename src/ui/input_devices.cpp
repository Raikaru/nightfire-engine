#include "ui/input_devices.hpp"

#include <algorithm>
#include <cstdlib>
#include <ostream>
#include <string_view>

namespace nf {

namespace {

constexpr std::string_view kContextNames[] = {"menu", "browser", "onfoot", "driving"};

struct ButtonName {
    std::string_view name;
    std::uint16_t bit;
};
constexpr ButtonName kButtonNames[] = {
    {"cross", kPadCross}, {"circle", kPadCircle}, {"square", kPadSquare}, {"triangle", kPadTriangle},
    {"up", kPadUp},       {"down", kPadDown},     {"left", kPadLeft},     {"right", kPadRight},
    {"start", kPadStart}, {"select", kPadSelect}, {"l1", kPadL1},         {"l2", kPadL2},
    {"r1", kPadR1},       {"r2", kPadR2},         {"l3", kPadL3},         {"r3", kPadR3}};

constexpr std::string_view kMouseNames[] = {"Mouse Left", "Mouse Right", "Mouse Middle", "Mouse X1",
                                            "Mouse X2",   "Wheel Up",    "Wheel Down"};

constexpr int kTriggerThreshold = 16000;   // LocalPad: a trigger pressed past half

DeviceInput key(SDL_Scancode s) { return {DeviceInput::Key, int(s)}; }
DeviceInput mouse(MouseInput m) { return {DeviceInput::Mouse, int(m)}; }
DeviceInput pad(SDL_GamepadButton b) { return {DeviceInput::PadButton, int(b)}; }
DeviceInput axis(SDL_GamepadAxis a) { return {DeviceInput::PadAxis, int(a)}; }

std::string_view trim(std::string_view s) {
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

std::optional<DeviceInput> parse_input(std::string_view name, bool keyboard_mouse) {
    const std::string n(name);
    if (keyboard_mouse) {
        for (std::size_t i = 0; i < std::size(kMouseNames); ++i)
            if (SDL_strcasecmp(n.c_str(), std::string(kMouseNames[i]).c_str()) == 0) return mouse(MouseInput(i));
        const SDL_Scancode s = SDL_GetScancodeFromName(n.c_str());
        if (s != SDL_SCANCODE_UNKNOWN) return key(s);
        return std::nullopt;
    }
    const SDL_GamepadButton b = SDL_GetGamepadButtonFromString(n.c_str());
    if (b != SDL_GAMEPAD_BUTTON_INVALID) return pad(b);
    const SDL_GamepadAxis a = SDL_GetGamepadAxisFromString(n.c_str());
    if (a == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || a == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) return axis(a);
    return std::nullopt;
}

std::string input_name(const DeviceInput& in) {
    switch (in.kind) {
        case DeviceInput::Key: return SDL_GetScancodeName(SDL_Scancode(in.code));
        case DeviceInput::Mouse: return std::string(kMouseNames[std::size_t(in.code)]);
        case DeviceInput::PadButton: return SDL_GetGamepadStringForButton(SDL_GamepadButton(in.code));
        case DeviceInput::PadAxis: return SDL_GetGamepadStringForAxis(SDL_GamepadAxis(in.code));
    }
    return {};
}

}  // namespace

bool is_playstation_pad(SDL_GamepadType type) {
    return type == SDL_GAMEPAD_TYPE_PS3 || type == SDL_GAMEPAD_TYPE_PS4 || type == SDL_GAMEPAD_TYPE_PS5;
}

InputBindings::InputBindings() {
    auto add = [this](InputContext c, std::uint16_t button, std::initializer_list<DeviceInput> inputs) {
        for (const DeviceInput& in : inputs) bindings_[std::size_t(c)].push_back({button, in});
    };
    const auto menu_dirs = [&](InputContext c) {
        add(c, kPadUp, {key(SDL_SCANCODE_UP), pad(SDL_GAMEPAD_BUTTON_DPAD_UP)});
        add(c, kPadDown, {key(SDL_SCANCODE_DOWN), pad(SDL_GAMEPAD_BUTTON_DPAD_DOWN)});
        add(c, kPadLeft, {key(SDL_SCANCODE_LEFT), pad(SDL_GAMEPAD_BUTTON_DPAD_LEFT)});
        add(c, kPadRight, {key(SDL_SCANCODE_RIGHT), pad(SDL_GAMEPAD_BUTTON_DPAD_RIGHT)});
    };
    // Menus: the nfui keyboard layout, gamepad face buttons by position.
    using enum InputContext;
    menu_dirs(Menu);
    add(Menu, kPadCross, {key(SDL_SCANCODE_RETURN), key(SDL_SCANCODE_Z), pad(SDL_GAMEPAD_BUTTON_SOUTH)});
    add(Menu, kPadCircle, {key(SDL_SCANCODE_X), pad(SDL_GAMEPAD_BUTTON_EAST)});
    add(Menu, kPadSquare, {key(SDL_SCANCODE_A), pad(SDL_GAMEPAD_BUTTON_WEST)});
    add(Menu, kPadTriangle, {key(SDL_SCANCODE_S), pad(SDL_GAMEPAD_BUTTON_NORTH)});
    add(Menu, kPadStart, {key(SDL_SCANCODE_SPACE), pad(SDL_GAMEPAD_BUTTON_START)});
    add(Menu, kPadSelect, {key(SDL_SCANCODE_BACKSPACE), pad(SDL_GAMEPAD_BUTTON_BACK)});
    add(Menu, kPadL1, {key(SDL_SCANCODE_Q), pad(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)});
    add(Menu, kPadR1, {key(SDL_SCANCODE_E), pad(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)});
    // Online browser: Enter joins, Esc/C opens the address keyboard (or leaves it), T/Backspace refreshes or
    // erases, S leaves the browser.
    menu_dirs(Browser);
    add(Browser, kPadCross,
        {key(SDL_SCANCODE_RETURN), key(SDL_SCANCODE_KP_ENTER), key(SDL_SCANCODE_X), pad(SDL_GAMEPAD_BUTTON_SOUTH)});
    add(Browser, kPadCircle, {key(SDL_SCANCODE_ESCAPE), key(SDL_SCANCODE_C), pad(SDL_GAMEPAD_BUTTON_EAST)});
    add(Browser, kPadTriangle, {key(SDL_SCANCODE_T), key(SDL_SCANCODE_BACKSPACE), pad(SDL_GAMEPAD_BUTTON_NORTH)});
    add(Browser, kPadSquare, {key(SDL_SCANCODE_S), pad(SDL_GAMEPAD_BUTTON_WEST)});
    add(Browser, kPadR1, {key(SDL_SCANCODE_F), pad(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)});   // favourite server
    // On foot (LocalPad). Gamepads are positional, so the game's controller style (psiInput_MapInputs; Classic Bond
    // by default) decides the actions exactly as on the PS2: south cross, east circle, west square, north triangle,
    // LB/RB L1/R1, LT/RT L2/R2, stick clicks L3/R3, Back select. The keyboard has its own (Classic Bond) layout.
    const auto positional = [&](InputContext c) {
        add(c, kPadCross, {pad(SDL_GAMEPAD_BUTTON_SOUTH)});
        add(c, kPadCircle, {pad(SDL_GAMEPAD_BUTTON_EAST)});
        add(c, kPadSquare, {pad(SDL_GAMEPAD_BUTTON_WEST)});
        add(c, kPadTriangle, {pad(SDL_GAMEPAD_BUTTON_NORTH)});
        add(c, kPadL1, {pad(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)});
        add(c, kPadR1, {pad(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)});
        add(c, kPadL2, {axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER)});
        add(c, kPadR2, {axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)});
        add(c, kPadL3, {pad(SDL_GAMEPAD_BUTTON_LEFT_STICK)});
        add(c, kPadR3, {pad(SDL_GAMEPAD_BUTTON_RIGHT_STICK)});
        add(c, kPadUp, {pad(SDL_GAMEPAD_BUTTON_DPAD_UP)});
        add(c, kPadDown, {pad(SDL_GAMEPAD_BUTTON_DPAD_DOWN)});
        add(c, kPadLeft, {pad(SDL_GAMEPAD_BUTTON_DPAD_LEFT)});
        add(c, kPadRight, {pad(SDL_GAMEPAD_BUTTON_DPAD_RIGHT)});
        add(c, kPadStart, {pad(SDL_GAMEPAD_BUTTON_START)});
        add(c, kPadSelect, {pad(SDL_GAMEPAD_BUTTON_BACK)});
    };
    positional(OnFoot);
    add(OnFoot, kPadTriangle, {key(SDL_SCANCODE_SPACE)});
    add(OnFoot, kPadL2, {key(SDL_SCANCODE_C), key(SDL_SCANCODE_LCTRL)});
    add(OnFoot, kPadCross, {key(SDL_SCANCODE_R)});
    add(OnFoot, kPadSquare, {key(SDL_SCANCODE_TAB)});
    add(OnFoot, kPadCircle, {key(SDL_SCANCODE_E)});
    add(OnFoot, kPadLeft, {key(SDL_SCANCODE_Q)});
    add(OnFoot, kPadUp, {mouse(MouseInput::WheelUp)});
    add(OnFoot, kPadR2, {mouse(MouseInput::WheelDown)});
    add(OnFoot, kPadR1, {mouse(MouseInput::Left)});
    add(OnFoot, kPadL1, {mouse(MouseInput::Right)});
    add(OnFoot, kPadSelect, {key(SDL_SCANCODE_N)});   // vision mode
    // Esc pauses through the sessions' key event (fixed, not a pad bit); the prompts know it (ui/prompts.cpp).
    // Driving: positional pads; keyboard W/Up accelerate (cross), S/Down brake (square), Space fires (circle),
    // C (triangle), Q (L2).
    positional(Driving);
    add(Driving, kPadCross, {key(SDL_SCANCODE_W), key(SDL_SCANCODE_UP)});
    add(Driving, kPadSquare, {key(SDL_SCANCODE_S), key(SDL_SCANCODE_DOWN)});
    add(Driving, kPadCircle, {key(SDL_SCANCODE_SPACE)});
    add(Driving, kPadTriangle, {key(SDL_SCANCODE_C)});
    add(Driving, kPadL2, {key(SDL_SCANCODE_Q)});
}

std::uint16_t InputBindings::keyboard_buttons(InputContext context, const bool* keys, SDL_MouseButtonFlags mouse_mask,
                                              int wheel) const {
    std::uint16_t out = 0;
    for (const Binding& b : bindings_[std::size_t(context)]) {
        bool down = false;
        if (b.input.kind == DeviceInput::Key) {
            down = keys && keys[b.input.code];
        } else if (b.input.kind == DeviceInput::Mouse) {
            switch (MouseInput(b.input.code)) {
                case MouseInput::Left: down = (mouse_mask & SDL_BUTTON_LMASK) != 0; break;
                case MouseInput::Right: down = (mouse_mask & SDL_BUTTON_RMASK) != 0; break;
                case MouseInput::Middle: down = (mouse_mask & SDL_BUTTON_MMASK) != 0; break;
                case MouseInput::X1: down = (mouse_mask & SDL_BUTTON_X1MASK) != 0; break;
                case MouseInput::X2: down = (mouse_mask & SDL_BUTTON_X2MASK) != 0; break;
                case MouseInput::WheelUp: down = wheel > 0; break;
                case MouseInput::WheelDown: down = wheel < 0; break;
            }
        }
        if (down) out |= b.button;
    }
    return out;
}

std::uint16_t InputBindings::gamepad_buttons(InputContext context, SDL_Gamepad* p) const {
    if (!p) return 0;
    std::uint16_t out = 0;
    for (const Binding& b : bindings_[std::size_t(context)]) {
        if (b.input.kind == DeviceInput::PadButton && SDL_GetGamepadButton(p, SDL_GamepadButton(b.input.code)))
            out |= b.button;
        else if (b.input.kind == DeviceInput::PadAxis &&
                 SDL_GetGamepadAxis(p, SDL_GamepadAxis(b.input.code)) > kTriggerThreshold)
            out |= b.button;
    }
    return out;
}

PadState InputBindings::sample(InputContext context, SDL_Gamepad* p) const {
    PadState s;
    s.buttons = std::uint16_t(keyboard_buttons(context, SDL_GetKeyboardState(nullptr)) | gamepad_buttons(context, p));
    return s;
}

bool InputBindings::types_text(SDL_Scancode k) {
    return (k >= SDL_SCANCODE_A && k <= SDL_SCANCODE_0) || k == SDL_SCANCODE_SPACE ||
           (k >= SDL_SCANCODE_MINUS && k <= SDL_SCANCODE_SLASH) ||
           (k >= SDL_SCANCODE_KP_DIVIDE && k <= SDL_SCANCODE_KP_PERIOD && k != SDL_SCANCODE_KP_ENTER);
}

std::uint16_t InputBindings::button_for_key(InputContext context, SDL_Scancode k, bool text_entry) const {
    if (text_entry && types_text(k)) return 0;
    for (const Binding& b : bindings_[std::size_t(context)])
        if (b.input == key(k)) return b.button;
    return 0;
}

std::optional<DeviceInput> InputBindings::first(InputContext context, std::uint16_t button, bool keyboard_mouse,
                                                bool text_entry) const {
    for (const Binding& b : bindings_[std::size_t(context)]) {
        if (b.button != button || b.input.keyboard_mouse() != keyboard_mouse) continue;
        if (text_entry && b.input.kind == DeviceInput::Key && types_text(SDL_Scancode(b.input.code))) continue;
        return b.input;
    }
    return std::nullopt;
}

bool InputBindings::set(const std::string& k, const std::string& value) {
    const bool keyboard_mouse = k.starts_with("bind_");
    if (!keyboard_mouse && !k.starts_with("pad_")) return false;
    const std::string_view rest = std::string_view(k).substr(keyboard_mouse ? 5 : 4);
    const std::size_t us = rest.find('_');
    if (us == std::string_view::npos) return false;
    const auto ctx = std::find(std::begin(kContextNames), std::end(kContextNames), rest.substr(0, us));
    const auto btn = std::find_if(std::begin(kButtonNames), std::end(kButtonNames),
                                  [&](const ButtonName& b) { return b.name == rest.substr(us + 1); });
    if (ctx == std::end(kContextNames) || btn == std::end(kButtonNames)) return false;
    std::vector<DeviceInput> inputs;
    for (std::string_view v = value; !v.empty();) {
        const std::size_t comma = v.find(',');
        const std::string_view item = trim(v.substr(0, comma));
        v = comma == std::string_view::npos ? std::string_view{} : v.substr(comma + 1);
        if (item.empty()) continue;
        const auto in = parse_input(item, keyboard_mouse);
        if (!in) return false;
        inputs.push_back(*in);
    }
    auto& list = bindings_[std::size_t(ctx - std::begin(kContextNames))];
    std::erase_if(list, [&](const Binding& b) { return b.button == btn->bit && b.input.keyboard_mouse() == keyboard_mouse; });
    for (const DeviceInput& in : inputs) list.push_back({btn->bit, in});
    return true;
}

void InputBindings::save(std::ostream& out) const {
    for (std::size_t c = 0; c < bindings_.size(); ++c)
        for (const bool keyboard_mouse : {true, false})
            for (const ButtonName& b : kButtonNames) {
                std::string line;
                for (const Binding& x : bindings_[c])
                    if (x.button == b.bit && x.input.keyboard_mouse() == keyboard_mouse)
                        line += (line.empty() ? "" : ",") + input_name(x.input);
                if (line.empty()) continue;
                out << (keyboard_mouse ? "bind_" : "pad_") << kContextNames[c] << '_' << b.name << '=' << line << '\n';
            }
}

InputBindings& input_bindings() {
    static InputBindings bindings;
    return bindings;
}

// ---------------------------------------------------------------------------------------------- devices

namespace {

bool SDLCALL watch_events(void* self, SDL_Event* e) {
    static_cast<InputDevices*>(self)->observe(*e);
    return true;
}

}  // namespace

void InputDevices::install() {
    int count = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
        for (int i = 0; i < count; ++i)
            if (SDL_Gamepad* g = SDL_OpenGamepad(ids[i])) open_.push_back(g);
        SDL_free(ids);
    }
    if (!open_.empty()) {
        const SDL_GamepadType type = SDL_GetGamepadType(open_.front());
        use(0, is_playstation_pad(type) ? InputDevice::PlayStation : InputDevice::Xbox, type);
    } else {
        use(0, InputDevice::KeyboardMouse, SDL_GAMEPAD_TYPE_STANDARD);
    }
    SDL_AddEventWatch(watch_events, this);
}

void InputDevices::shutdown() {
    SDL_RemoveEventWatch(watch_events, this);
    for (SDL_Gamepad* g : open_) SDL_CloseGamepad(g);
    open_.clear();
    slots_.clear();
}

void InputDevices::assign(std::span<const SlotDevice> slots) {
    slots_.assign(slots.begin(), slots.begin() + std::min<std::ptrdiff_t>(std::ssize(slots), kPlayers));
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        const SlotDevice& s = slots_[i];
        if (s.gamepad != 0 && !s.keyboard_mouse) use(int(i), device_of(s.gamepad), SDL_GetGamepadTypeForID(s.gamepad));
        else if (s.keyboard_mouse && s.gamepad == 0) use(int(i), InputDevice::KeyboardMouse, {});
    }
    ++generation_;
}

InputDevice InputDevices::device_of(SDL_JoystickID id) {
    return is_playstation_pad(SDL_GetGamepadTypeForID(id)) ? InputDevice::PlayStation : InputDevice::Xbox;
}

void InputDevices::force(std::optional<InputDevice> device) {
    forced_ = device;
    ++generation_;
}

InputDevice InputDevices::device(int player) const {
    if (forced_) return *forced_;
    return device_[std::size_t(std::clamp(player, 0, kPlayers - 1))];
}

SDL_GamepadType InputDevices::gamepad_type(int player) const {
    if (forced_) return *forced_ == InputDevice::PlayStation ? SDL_GAMEPAD_TYPE_PS4 : SDL_GAMEPAD_TYPE_XBOXONE;
    return type_[std::size_t(std::clamp(player, 0, kPlayers - 1))];
}

int InputDevices::player_of(SDL_JoystickID id) const {
    if (slots_.empty()) return 0;
    for (std::size_t i = 0; i < slots_.size(); ++i)
        if (slots_[i].gamepad == id) return int(i);
    return -1;
}

int InputDevices::keyboard_player() const {
    if (slots_.empty()) return 0;
    for (std::size_t i = 0; i < slots_.size(); ++i)
        if (slots_[i].keyboard_mouse) return int(i);
    return -1;
}

void InputDevices::use(int player, InputDevice device, SDL_GamepadType type) {
    const std::size_t p = std::size_t(player);
    if (device_[p] == device && (device == InputDevice::KeyboardMouse || type_[p] == type)) return;
    device_[p] = device;
    if (device != InputDevice::KeyboardMouse) type_[p] = type;
    ++generation_;
}

void InputDevices::observe(const SDL_Event& e) {
    switch (e.type) {
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_WHEEL:
            if (keyboard_player() >= 0) use(keyboard_player(), InputDevice::KeyboardMouse, {});
            break;
        case SDL_EVENT_MOUSE_MOTION:
            if (keyboard_player() >= 0 && std::abs(e.motion.xrel) + std::abs(e.motion.yrel) > 3.0f)
                use(keyboard_player(), InputDevice::KeyboardMouse, {});
            break;
        case SDL_EVENT_GAMEPAD_ADDED:
            if (std::none_of(open_.begin(), open_.end(), [&](SDL_Gamepad* g) { return SDL_GetGamepadID(g) == e.gdevice.which; }))
                if (SDL_Gamepad* g = SDL_OpenGamepad(e.gdevice.which)) open_.push_back(g);
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            std::erase_if(open_, [&](SDL_Gamepad* g) {
                if (SDL_GetGamepadID(g) != e.gdevice.which) return false;
                SDL_CloseGamepad(g);
                return true;
            });
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
            const SDL_JoystickID id = e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ? e.gbutton.which : e.gaxis.which;
            if (e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && std::abs(int(e.gaxis.value)) < kTriggerThreshold) break;
            const int player = player_of(id);
            if (player < 0) break;
            use(player, device_of(id), SDL_GetGamepadTypeForID(id));
            break;
        }
        default: break;
    }
}

InputDevices& input_devices() {
    static InputDevices devices;
    return devices;
}

}  // namespace nf

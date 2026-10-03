#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "game/input.hpp"

namespace nf {

// The kind of controller a player last used; it picks the button-prompt glyph set (ui/prompts.hpp).
enum class InputDevice : std::uint8_t { PlayStation, Xbox, KeyboardMouse };

// The screens that map PC devices onto the DualShock 2 buttons differently.
enum class InputContext : std::uint8_t {
    Menu,      // front end, pause and result pages (MenuManager pads)
    Browser,   // the online server browser and its address/password keyboards
    OnFoot,    // first-person gameplay (LocalPad)
    Driving,   // driving missions
    Count,
};

enum class MouseInput : std::uint8_t { Left, Right, Middle, X1, X2, WheelUp, WheelDown };

// One physical key, mouse button or gamepad control.
struct DeviceInput {
    enum Kind : std::uint8_t { Key, Mouse, PadButton, PadAxis } kind;
    int code;   // SDL_Scancode, MouseInput, SDL_GamepadButton or SDL_GamepadAxis (triggers, pressed past half)
    bool keyboard_mouse() const { return kind == Key || kind == Mouse; }
    bool operator==(const DeviceInput&) const = default;
};

// Which PC inputs press which DualShock 2 button, per context. The samplers of every screen read it and the
// button prompts resolve through it, so a rebinding in nightfire.cfg changes both:
//   bind_<context>_<button>=Return,Z,Mouse Left,Wheel Up   (keyboard and mouse; SDL key names)
//   pad_<context>_<button>=a,leftshoulder,lefttrigger      (gamepad; SDL gamepad button/axis names)
// context: menu browser onfoot driving; button: cross circle square triangle up down left right start select
// l1 l2 r1 r2 l3 r3. An empty value unbinds. Sticks (movement, look, steering) are not rebindable.
class InputBindings {
public:
    InputBindings();   // the built-in defaults

    // Buttons held on the keyboard/mouse. `mouse` is the SDL mouse button mask (0 when the screen does not
    // take mouse buttons), `wheel` the wheel step since the last sample (-1, 0, 1).
    std::uint16_t keyboard_buttons(InputContext context, const bool* keys, SDL_MouseButtonFlags mouse = 0,
                                   int wheel = 0) const;
    std::uint16_t gamepad_buttons(InputContext context, SDL_Gamepad* pad) const;
    // Keyboard state plus `pad` (may be null): the menu pad of every menu loop.
    PadState sample(InputContext context, SDL_Gamepad* pad) const;
    // The button a key press stands for (event-driven screens), 0 when unbound. With `text_entry` keys that
    // type text are ignored (they belong to the text field).
    std::uint16_t button_for_key(InputContext context, SDL_Scancode key, bool text_entry) const;
    // The first input of the device family bound to `button` (prompt resolution).
    std::optional<DeviceInput> first(InputContext context, std::uint16_t button, bool keyboard_mouse,
                                     bool text_entry = false) const;

    // nightfire.cfg keys; set() returns false for a key that is not a binding (or an unknown name).
    bool set(const std::string& key, const std::string& value);
    void save(std::ostream& out) const;

    static bool types_text(SDL_Scancode key);

private:
    struct Binding {
        std::uint16_t button;
        DeviceInput input;
    };
    std::array<std::vector<Binding>, std::size_t(InputContext::Count)> bindings_;
};

// The process-wide table (nightfire.cfg overrides apply to it).
InputBindings& input_bindings();

// The physical devices of one local player slot.
struct SlotDevice {
    bool keyboard_mouse = false;
    SDL_JoystickID gamepad = 0;   // 0 = none
    bool empty() const { return !keyboard_mouse && gamepad == 0; }
};

// Last used device per local player, updated from every SDL event (an event watch, so each screen's own event
// loop feeds it). Without slot assignments every device drives player 0 (front end, single player); with them
// (split-screen, the MP join page) each device drives the slot it is assigned to.
class InputDevices {
public:
    static constexpr int kPlayers = 4;

    // Registers the event watch and opens every connected gamepad; the initial device of player 0 is the first
    // gamepad if one is connected, else keyboard and mouse.
    void install();
    // Removes the event watch and closes the gamepads (before SDL_Quit, which would otherwise report their
    // removal to the watch while it tears them down).
    void shutdown();
    // slots[i] are local player i's devices; empty span: every device drives player 0.
    void assign(std::span<const SlotDevice> slots);
    // Forces one device for every player (`--prompts`); nullopt returns to tracking.
    void force(std::optional<InputDevice> device);

    InputDevice device(int player) const;
    // Face-button letters of player's last gamepad (Xbox layout when none).
    SDL_GamepadType gamepad_type(int player) const;
    // Bumped whenever any player's device changes (prompt caches compare it).
    std::uint32_t generation() const { return generation_; }
    // The player a gamepad / the keyboard and mouse drive, -1 for none.
    int player_of(SDL_JoystickID id) const;
    int keyboard_player() const;
    // Every connected gamepad, in connection order (opened by install).
    const std::vector<SDL_Gamepad*>& gamepads() const { return open_; }
    // The device kind of one gamepad.
    static InputDevice device_of(SDL_JoystickID id);

    void observe(const SDL_Event& e);

private:
    void use(int player, InputDevice device, SDL_GamepadType type);

    std::array<InputDevice, kPlayers> device_{};
    std::array<SDL_GamepadType, kPlayers> type_{};
    std::vector<SlotDevice> slots_;
    std::vector<SDL_Gamepad*> open_;
    std::optional<InputDevice> forced_;
    std::uint32_t generation_ = 1;
};

InputDevices& input_devices();

// The PlayStation-family check behind InputDevice::PlayStation.
bool is_playstation_pad(SDL_GamepadType type);

}  // namespace nf

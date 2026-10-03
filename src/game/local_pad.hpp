#pragma once

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

#include "game/actions.hpp"
#include "game/input.hpp"
#include "ui/input_devices.hpp"

namespace nf {

// One local player's controller: an optional keyboard + mouse (nfgame's keyboard layout, docs/gameplay.md "nfgame")
// and an optional gamepad read positionally as a DualShock 2. Buttons come from the shared binding table
// (InputContext::OnFoot, rebindable in nightfire.cfg); sticks come out in game form (post dead zone).
class LocalPad {
public:
    LocalPad(bool keyboard_and_mouse, SDL_Gamepad* pad) : keyboard_(keyboard_and_mouse), pad_(pad) {}

    void handle_event(const SDL_Event& e) {
        if (!keyboard_) return;
        if (e.type == SDL_EVENT_MOUSE_MOTION && captured_) {
            mouse_dx_ += e.motion.xrel;
            mouse_dy_ += e.motion.yrel;
        } else if (e.type == SDL_EVENT_MOUSE_WHEEL) {
            wheel_ = e.wheel.y > 0 ? 1 : e.wheel.y < 0 ? -1 : 0;
        }
    }
    void set_captured(bool c) { captured_ = c; }
    bool has_gamepad() const { return pad_ != nullptr; }
    // The devices of this player (for the prompt glyphs, InputDevices::assign).
    SlotDevice device() const { return {keyboard_, pad_ ? SDL_GetGamepadID(pad_) : 0}; }
    // This player's devices read through the menu bindings (their pause menu).
    PadState sample_menu() const {
        PadState s;
        const InputBindings& bindings = input_bindings();
        if (keyboard_) s.buttons = bindings.keyboard_buttons(InputContext::Menu, SDL_GetKeyboardState(nullptr));
        s.buttons |= bindings.gamepad_buttons(InputContext::Menu, pad_);
        return s;
    }

    PadState sample() {
        PadState raw;
        const InputBindings& bindings = input_bindings();
        if (keyboard_) {
            const bool* k = SDL_GetKeyboardState(nullptr);
            auto axis = [](bool neg, bool pos) { return std::uint8_t(neg == pos ? 0x80 : neg ? 0x00 : 0xFF); };
            raw.ly = axis(k[SDL_SCANCODE_W] || k[SDL_SCANCODE_UP], k[SDL_SCANCODE_S] || k[SDL_SCANCODE_DOWN]);
            raw.rx = axis(k[SDL_SCANCODE_A], k[SDL_SCANCODE_D]);
            raw.lx = axis(k[SDL_SCANCODE_LEFT], k[SDL_SCANCODE_RIGHT]);
            raw.ry = axis(k[SDL_SCANCODE_PAGEUP], k[SDL_SCANCODE_PAGEDOWN]);
            const SDL_MouseButtonFlags mouse = captured_ ? SDL_GetMouseState(nullptr, nullptr) : 0;
            raw.buttons |= bindings.keyboard_buttons(InputContext::OnFoot, k, mouse, wheel_);
            wheel_ = 0;
        }
        if (pad_) {
            apply_gamepad_sticks(raw);
            raw.buttons |= bindings.gamepad_buttons(InputContext::OnFoot, pad_);
        }
        PadState out = compensate_sticks(raw);
        if (keyboard_ && (mouse_dx_ != 0 || mouse_dy_ != 0)) {
            out.lx = stick_from_delta(mouse_dx_, out.lx);
            out.ry = stick_from_delta(mouse_dy_, out.ry);
        }
        mouse_dx_ = mouse_dy_ = 0;
        return out;
    }

private:
    static std::uint8_t stick_from_delta(float delta, std::uint8_t current) {
        constexpr float kUnitsPerPixel = 4.0f;
        return std::uint8_t(std::clamp(int(std::lround(float(current) + delta * kUnitsPerPixel)), 0, 255));
    }

    // Positional, as the DualShock 2 report: the left stick is lx/ly, the right stick rx/ry (the controller style
    // decides what they do; Classic Bond: left = move/turn, right = strafe/look).
    void apply_gamepad_sticks(PadState& raw) {
        auto stick = [this](SDL_GamepadAxis a) {
            return std::uint8_t(std::clamp(int(std::lround(SDL_GetGamepadAxis(pad_, a) / 32767.0 * 127.0)) + 0x80, 0, 255));
        };
        const auto lx = stick(SDL_GAMEPAD_AXIS_LEFTX), ly = stick(SDL_GAMEPAD_AXIS_LEFTY);
        const auto rx = stick(SDL_GAMEPAD_AXIS_RIGHTX), ry = stick(SDL_GAMEPAD_AXIS_RIGHTY);
        if (lx != 0x80 || ly != 0x80 || rx != 0x80 || ry != 0x80) {
            raw.lx = lx;
            raw.ly = ly;
            raw.rx = rx;
            raw.ry = ry;
        }
    }

    bool keyboard_;
    SDL_Gamepad* pad_;
    bool captured_ = false;
    float mouse_dx_ = 0, mouse_dy_ = 0;
    int wheel_ = 0;
};

// Split-screen device assignment: with at least as many gamepads as players every player gets one (player 1 keeps the
// keyboard and mouse too); otherwise player 1 plays on keyboard + mouse and the others take the gamepads in order.
inline std::vector<LocalPad> open_local_pads(int players) {
    std::vector<SDL_Gamepad*> pads;
    int count = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
        for (int i = 0; i < count; ++i)
            if (SDL_Gamepad* g = SDL_OpenGamepad(ids[i])) pads.push_back(g);
        SDL_free(ids);
    }
    const bool one_each = int(pads.size()) >= players;
    std::vector<LocalPad> out;
    for (int p = 0; p < players; ++p) {
        const int index = one_each ? p : p - 1;
        SDL_Gamepad* g = index >= 0 && index < int(pads.size()) ? pads[std::size_t(index)] : nullptr;
        out.emplace_back(p == 0, g);
    }
    return out;
}

// The devices the P_MPJOIN slots claimed (FrontendResult::slot_devices), one pad per player.
inline std::vector<LocalPad> open_local_pads(std::span<const SlotDevice> slots, int players) {
    std::vector<LocalPad> out;
    for (int p = 0; p < players; ++p) {
        const SlotDevice d = p < int(slots.size()) ? slots[std::size_t(p)] : SlotDevice{};
        out.emplace_back(d.keyboard_mouse, d.gamepad != 0 ? SDL_OpenGamepad(d.gamepad) : nullptr);
    }
    return out;
}

}  // namespace nf

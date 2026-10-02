#pragma once

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "game/actions.hpp"
#include "game/input.hpp"

namespace nf {

// One local player's controller for split-screen play: an optional keyboard + mouse and an optional gamepad, mapped
// like nfgame's single-player input (docs/gameplay.md "nfgame"). Sticks come out in game form (post dead zone).
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

    PadState sample() {
        PadState raw;
        if (keyboard_) {
            const bool* k = SDL_GetKeyboardState(nullptr);
            auto axis = [](bool neg, bool pos) { return std::uint8_t(neg == pos ? 0x80 : neg ? 0x00 : 0xFF); };
            raw.ly = axis(k[SDL_SCANCODE_W] || k[SDL_SCANCODE_UP], k[SDL_SCANCODE_S] || k[SDL_SCANCODE_DOWN]);
            raw.rx = axis(k[SDL_SCANCODE_A], k[SDL_SCANCODE_D]);
            raw.lx = axis(k[SDL_SCANCODE_LEFT], k[SDL_SCANCODE_RIGHT]);
            raw.ry = axis(k[SDL_SCANCODE_PAGEUP], k[SDL_SCANCODE_PAGEDOWN]);
            if (k[SDL_SCANCODE_SPACE]) raw.buttons |= kPadTriangle;
            if (k[SDL_SCANCODE_C] || k[SDL_SCANCODE_LCTRL]) raw.buttons |= kPadL2;
            if (k[SDL_SCANCODE_R]) raw.buttons |= kPadCross;
            if (k[SDL_SCANCODE_TAB]) raw.buttons |= kPadSquare;
            if (k[SDL_SCANCODE_E]) raw.buttons |= kPadCircle;
            if (k[SDL_SCANCODE_Q]) raw.buttons |= kPadLeft;
            if (wheel_ != 0) {
                raw.buttons |= wheel_ > 0 ? kPadUp : kPadR2;
                wheel_ = 0;
            }
        }
        if (pad_) apply_gamepad(raw);
        PadState out = compensate_sticks(raw);
        if (keyboard_ && (mouse_dx_ != 0 || mouse_dy_ != 0)) {
            out.lx = stick_from_delta(mouse_dx_, out.lx);
            out.ry = stick_from_delta(mouse_dy_, out.ry);
        }
        if (keyboard_ && captured_) {
            const Uint32 mb = SDL_GetMouseState(nullptr, nullptr);
            if (mb & SDL_BUTTON_LMASK) out.buttons |= kPadR1;
            if (mb & SDL_BUTTON_RMASK) out.buttons |= kPadL1;
        }
        mouse_dx_ = mouse_dy_ = 0;
        return out;
    }

private:
    static std::uint8_t stick_from_delta(float delta, std::uint8_t current) {
        constexpr float kUnitsPerPixel = 4.0f;
        return std::uint8_t(std::clamp(int(std::lround(float(current) + delta * kUnitsPerPixel)), 0, 255));
    }

    void apply_gamepad(PadState& raw) {
        auto stick = [this](SDL_GamepadAxis a) {
            return std::uint8_t(std::clamp(int(std::lround(SDL_GetGamepadAxis(pad_, a) / 32767.0 * 127.0)) + 0x80, 0, 255));
        };
        auto pressed = [this](SDL_GamepadButton b) { return SDL_GetGamepadButton(pad_, b); };
        const auto lx = stick(SDL_GAMEPAD_AXIS_LEFTX), ly = stick(SDL_GAMEPAD_AXIS_LEFTY);
        const auto rx = stick(SDL_GAMEPAD_AXIS_RIGHTX), ry = stick(SDL_GAMEPAD_AXIS_RIGHTY);
        if (lx != 0x80 || ly != 0x80 || rx != 0x80 || ry != 0x80) {
            raw.rx = lx;
            raw.ly = ly;
            raw.lx = rx;
            raw.ry = ry;
        }
        if (pressed(SDL_GAMEPAD_BUTTON_SOUTH)) raw.buttons |= kPadTriangle;
        if (pressed(SDL_GAMEPAD_BUTTON_EAST)) raw.buttons |= kPadL2;
        if (SDL_GetGamepadAxis(pad_, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) raw.buttons |= kPadL1;
        if (SDL_GetGamepadAxis(pad_, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) raw.buttons |= kPadR1;
        if (pressed(SDL_GAMEPAD_BUTTON_WEST)) raw.buttons |= kPadCross;
        if (pressed(SDL_GAMEPAD_BUTTON_NORTH)) raw.buttons |= kPadCircle;
        if (pressed(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) raw.buttons |= kPadSquare;
        if (pressed(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) raw.buttons |= kPadR2;
        if (pressed(SDL_GAMEPAD_BUTTON_START)) raw.buttons |= kPadStart;
        if (pressed(SDL_GAMEPAD_BUTTON_BACK)) raw.buttons |= kPadSelect;
        if (pressed(SDL_GAMEPAD_BUTTON_DPAD_UP)) raw.buttons |= kPadUp;
        if (pressed(SDL_GAMEPAD_BUTTON_DPAD_DOWN)) raw.buttons |= kPadDown;
        if (pressed(SDL_GAMEPAD_BUTTON_DPAD_LEFT)) raw.buttons |= kPadLeft;
        if (pressed(SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) raw.buttons |= kPadRight;
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

}  // namespace nf

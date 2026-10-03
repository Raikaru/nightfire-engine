#include "app/frontend_pads.hpp"

#include <vector>

namespace nf::app {

namespace {

constexpr std::uint32_t kPageMain = 0x40000002, kPageMpJoin = 0x40000019;

bool claimed(const std::array<SlotDevice, 4>& claims, const SlotDevice& d) {
    for (const SlotDevice& c : claims)
        if ((d.keyboard_mouse && c.keyboard_mouse) || (d.gamepad != 0 && c.gamepad == d.gamepad)) return true;
    return false;
}

}  // namespace

void FrontendPads::set_joining(bool on) {
    if (on == joining_) return;
    joining_ = on;
    claims_ = {};
    if (!on) input_devices().assign({});
}

// Join-page preview: claimed slots keep their device, the free slots show the devices that could still join
// (so each slot's "Press ~A to join" carries that device's glyph and the slot counts as present).
void FrontendPads::publish(Frontend& frontend) {
    if (!joining_) return;
    std::array<SlotDevice, 4> preview = claims_;
    std::vector<SlotDevice> waiting;
    if (!claimed(claims_, {true, 0})) waiting.push_back({true, 0});
    for (SDL_Gamepad* g : input_devices().gamepads())
        if (const SlotDevice d{false, SDL_GetGamepadID(g)}; !claimed(claims_, d)) waiting.push_back(d);
    std::size_t next = 0;
    for (SlotDevice& s : preview)
        if (s.empty() && next < waiting.size()) s = waiting[next++];
    input_devices().assign(preview);
    for (std::size_t slot = 1; slot < 4; ++slot) frontend.set_controller_present(slot, !preview[slot].empty());
}

const std::array<PadHistory, 4>& FrontendPads::sample(Frontend& frontend, std::uint16_t injected_keyboard) {
    const std::uint32_t page = frontend.page_id();
    if (page == kPageMpJoin) set_joining(true);
    else if (page == kPageMain) set_joining(false);

    const InputBindings& b = input_bindings();
    struct Device {
        SlotDevice id;
        std::uint16_t now, prev;
    };
    std::vector<Device> devices;
    const std::uint16_t keyboard =
        std::uint16_t(b.keyboard_buttons(InputContext::Menu, SDL_GetKeyboardState(nullptr)) | injected_keyboard);
    devices.push_back({{true, 0}, keyboard, keyboard_prev_});
    keyboard_prev_ = keyboard;
    for (SDL_Gamepad* g : input_devices().gamepads()) {
        const SDL_JoystickID id = SDL_GetGamepadID(g);
        const std::uint16_t now = b.gamepad_buttons(InputContext::Menu, g);
        devices.push_back({{false, id}, now, pad_prev_[id]});
        pad_prev_[id] = now;
    }

    std::array<std::uint16_t, 4> slots{};
    for (const Device& d : devices) {
        if (!joining_) {
            slots[0] |= d.now;
            continue;
        }
        int slot = -1;
        for (std::size_t s = 0; s < 4; ++s)
            if ((d.id.keyboard_mouse && claims_[s].keyboard_mouse) || (d.id.gamepad != 0 && claims_[s].gamepad == d.id.gamepad))
                slot = int(s);
        const bool cross = (d.now & kPadCross) && !(d.prev & kPadCross);
        if (slot < 0 && page == kPageMpJoin && cross) {
            for (std::size_t s = 0; s < 4 && slot < 0; ++s)
                if (claims_[s].empty()) claims_[s] = d.id, slot = int(s);
        }
        // Before anyone joined, any device may still leave the page (slot 0's back button).
        if (slot < 0 && claims_[0].empty()) slot = 0;
        if (slot >= 0) slots[std::size_t(slot)] |= d.now;
    }
    publish(frontend);
    for (std::size_t s = 0; s < 4; ++s) pads_[s].push({slots[s]});
    frontend.update(pads_);
    return pads_;
}

const std::array<PadHistory, 4>& FrontendPads::push_slot0(Frontend& frontend, std::uint16_t buttons) {
    const std::uint32_t page = frontend.page_id();
    if (page == kPageMpJoin) set_joining(true);
    else if (page == kPageMain) set_joining(false);
    publish(frontend);
    pads_[0].push({buttons});
    for (std::size_t s = 1; s < 4; ++s) pads_[s].push({});
    frontend.update(pads_);
    return pads_;
}

}  // namespace nf::app

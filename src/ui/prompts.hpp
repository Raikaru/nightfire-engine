#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include "assets/ui_fonts.hpp"
#include "game/actions.hpp"
#include "ui/input_devices.hpp"

namespace nf::ui {

// Button prompts that follow the player's device. The game's strings name DualShock 2 buttons with `~X`
// escapes (`specialchar`: A cross, B triangle, X circle, Y square, L/R L1/R1, C/D L2/R2, S start, T select,
// V up/down, H left/right, W the D-pad, F/E left/right stick). PromptGlyphs resolves each escape through
// the binding table of the active context (ui/input_devices.hpp) to the input the player presses on their last
// used device: the original glyph for PlayStation pads, the engine's own art (assets/ui/prompts.png) for
// Xbox-style pads and keyboard/mouse. A lowercase escape (`~a`) names the same button as bound for gameplay
// (InputContext::OnFoot) whatever the screen's context; the controls pages use it.
class PromptGlyphs {
public:
    explicit PromptGlyphs(const FontSet& fonts) : fonts_(fonts) {}

    // The player whose device and the context whose bindings the next draws use. `text_entry`: keys that type
    // text belong to a text field, so prompts skip them.
    void select(int player, InputContext context, bool text_entry = false);
    int player() const { return player_; }
    InputContext context() const { return context_; }
    InputDevice device() const { return input_devices().device(player_); }

    // The glyph of `~key` for the selected player (or `player`, the `~<n>X` escape), nullptr when the key is
    // no glyph. Recomputed when a device changes.
    const SpecialChar* find(char key) const { return find(key, player_); }
    const SpecialChar* find(char key, int player) const;

private:
    std::optional<SpecialChar> resolve(char key, int player) const;
    std::optional<SpecialChar> resolve_pad(char key, InputContext context, int player) const;
    std::optional<SpecialChar> resolve_keyboard(char key, InputContext context) const;

    const FontSet& fonts_;
    int player_ = 0;
    InputContext context_ = InputContext::Menu;
    bool text_entry_ = false;
    struct Cache {
        std::uint32_t built = 0;   // InputDevices generation of the entries (0 = stale)
        std::array<std::optional<SpecialChar>, 52> glyphs{};   // A..Z, a..z
    };
    mutable std::array<Cache, InputDevices::kPlayers> cache_{};
};

// Selects player/context on the active resolver (TextRenderer::prompts), if any: one call per draw site.
void select_prompts(int player, InputContext context, bool text_entry = false);
// The DualShock 2 buttons an original escape stands for (0 for the non-button glyphs I and J; E/F are sticks).
std::uint16_t prompt_buttons(char key);
// The escape key of a single button (0 when none).
char prompt_key(std::uint16_t button);
// The button that triggers a gameplay action in controller style `style` (psiInput_MapInputs), 0 for analogue actions.
std::uint16_t action_button(Action action, int style);

}  // namespace nf::ui

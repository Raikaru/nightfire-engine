#pragma once

#include <cstdint>
#include <string_view>

#include "assets/sprites.hpp"
#include "ui/renderer.hpp"

namespace nf::ui {

// The engine's own UI art (button prompts for PC devices, online/settings icons, ...): sheets drawn by the
// scripts in tools/art/, checked in as assets/ui/<sheet>.png + <sheet>.txt and compiled into nf_ui. Each sheet
// is one sprite texture; sheet i (sorted by file name) has hash kArtHashBase + i.
constexpr std::uint32_t kArtHashBase = 0x7F000000;

struct ArtSprite {
    std::uint32_t hash;   // texture hash in the sprite library
    Rect src;             // texel rectangle (draw 1:1 on the 640x448 canvas)
};

// The sprite `name` of sheet `sheet`, or nullptr. Works before register_art_sheets (manifests only).
const ArtSprite* art_sprite(std::string_view sheet, std::string_view name);
// Decodes every sheet and registers it in `sprites` (once per library, after load_ui_assets).
void register_art_sheets(SpriteLibrary& sprites);

}  // namespace nf::ui

#pragma once

#include <filesystem>
#include <string_view>

#include "assets/game_files.hpp"
#include "assets/sprites.hpp"
#include "assets/strings.hpp"
#include "assets/ui_fonts.hpp"

namespace nf {

// The level bin that holds the front end: menu script (entry type 8), the menu backdrop map and the
// shared UI chunk files (fonts, window skins, menu sprites). LoaderProcess hands its type-8 entry to
// MenuManager_Load.
constexpr std::string_view kFrontEndBin = "07000048.bin";

// Everything 2D that does not depend on a level: fonts (ACTION.ELF data), English strings and the
// front end's sprite textures.
struct UiAssets {
    FontSet fonts;
    StringTable strings;
    SpriteLibrary sprites;
};

// `language_file` is a FILES.BIN `<LANG>Txt.dat` (the US disc's English table is USATxt.dat).
UiAssets load_ui_assets(const std::filesystem::path& gamedir, GameFiles& files,
                        std::string_view language_file = "USATxt.dat");

// Adds the sprite textures of every chunk file in a level `.bin` (HUD, pickups, level-specific
// sprites) to `sprites`; entries registered later replace earlier ones.
void add_level_sprites(SpriteLibrary& sprites, Bytes bin);

}  // namespace nf

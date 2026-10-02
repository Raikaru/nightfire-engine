#pragma once

#include <cstddef>
#include <filesystem>

#include "assets/game_files.hpp"

namespace nf {

// Loads every HUD table from ACTION.ELF and checks that each sprite hash the HUD draws with exists
// in the sprite set of every gameplay level bin. Prints one line per failure; returns their count.
std::size_t validate_hud(GameFiles& files, const std::filesystem::path& gamedir);

}  // namespace nf

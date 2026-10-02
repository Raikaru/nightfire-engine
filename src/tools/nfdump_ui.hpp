#pragma once

#include <filesystem>

#include "assets/game_files.hpp"

namespace nf {

// Loads the fonts (ACTION.ELF), every localised string table (`*Txt.dat`, `*TxtU.dat`) and every
// level bin's UI sprites; checks the font/icon textures resolve in every level and that the two menu
// scripts (front end 08000002, in-game 08000001) parse and are identical across bins. Prints a summary and returns the failure count.
std::size_t validate_ui(GameFiles& files, const std::filesystem::path& gamedir);

}  // namespace nf

#pragma once

#include <cstddef>
#include <filesystem>

#include "assets/game_files.hpp"
#include "assets/menu_file.hpp"

namespace nf {

// The menu script of a level `.bin` (its type-8 entry, `MenuManager_Load`). Throws FormatError when the
// bin has none.
MenuFile load_menu_from_bin(Bytes bin);

// nfdump validate: parses the front end's menu script and the one every level bin carries and checks that
// every skin texture and label sprite exists in the bin's sprite set, every control and keyframe message is one
// the runtime implements, keyframe page changes and the handler ids resolve. Prints a summary and returns the
// number of problems (0 = pass).
std::size_t validate_menu(GameFiles& files, const std::filesystem::path& gamedir);

}  // namespace nf

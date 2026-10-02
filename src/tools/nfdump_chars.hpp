#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "assets/game_files.hpp"

namespace nf {

// `nfdump <gamedir> chars [level.bin [skin]]`: skin / skeleton / animation catalog of the disc, of one
// level .bin, or the full description of one skin (hex hash or model name).
int cmd_chars(GameFiles& files, const std::vector<std::string>& args);

// Loads every skeleton, skin, sequence and script of every level .bin, decodes every skinned and rigid
// mesh a skin references and samples every frame of every sequence. Prints a summary and returns the
// number of failures. Also exercises CharacterInstance: facial layers + morph targets, blend_to, and the AnimSet
// locomotion blender (tables read from `gamedir`/ACTION.ELF).
std::size_t validate_chars(GameFiles& files, const std::filesystem::path& gamedir);

}  // namespace nf

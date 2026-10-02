#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "assets/game_files.hpp"

namespace nf {

// `nfdump <gamedir> weapons [id]`: the weapon_data table decoded from ACTION.ELF's static initializer.
int cmd_weapons(GameFiles& files, const std::filesystem::path& gamedir, const std::vector<std::string>& args);
// Decodes the table and checks it (ranges, labels resolve, rows the spec dump pins). Returns the failure count.
std::size_t validate_weapons(GameFiles& files, const std::filesystem::path& gamedir);

}  // namespace nf

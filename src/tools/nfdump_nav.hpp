#pragma once

#include <string>

#include "assets/game_files.hpp"

namespace nf {

// Parses map blocks 0x05 / 0x19 of every level .bin and checks the spec invariants
// (docs/spec-arena-ai.md Part 2B section 1). Returns the number of failures.
int validate_nav(GameFiles& files, const std::string& gamedir);

// `nfdump <gamedir> nav <level.bin> [map.bmp]`: counts, connectivity, sample A*, emitters, MoveTest agreement.
int cmd_nav(GameFiles& files, const std::string& level_name, const std::string& bmp_path = {});

}  // namespace nf

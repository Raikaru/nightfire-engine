#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "assets/game_files.hpp"

namespace nf {

// Multiplayer arena data (docs/gameplay.md "Multiplayer arena"): reads spawn markers, pickup placements and
// MP objects of the eight arena maps, checks PickupMatrix / UseableGuns against ACTION.ELF, the placement
// counts against the numbers the reverse-engineering spec lists, that every pickup resolves to a weapon id /
// model, and that every spawn stands on a floor. Returns the failure count.
std::size_t validate_arena(GameFiles& files, const std::filesystem::path& gamedir);

// `nfdump <gamedir> arena [level.bin]`: prints every spawn / pickup / MP object of the arena maps.
int cmd_arena(GameFiles& files, const std::filesystem::path& gamedir, const std::vector<std::string>& args);

}  // namespace nf

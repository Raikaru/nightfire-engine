#pragma once

#include <cstddef>
#include <filesystem>

#include "assets/game_files.hpp"

namespace nf {

// Validates all single-player script data on the disc (mission tables, cutscene scripts, dynamic
// objects): mission rows resolve to level bins, objectives carry valid channels and resolvable
// labels, every type-7 entry of every level bin parses and decodes, every non-world static's
// class is known to `parsemap_create_dynamic_objects`, LoadLevel destinations exist, and door
// path refs land on real tracks. Prints one line per issue plus per-level census lines; returns
// the failure count (dangling script/movie refs that the original tolerates are notes, not
// failures).
std::size_t validate_script(GameFiles& files, const std::filesystem::path& gamedir);

// Dumps one level's mission row, objectives, object census and script entries.
int dump_script(GameFiles& files, const std::filesystem::path& gamedir, const std::string& level_bin);

}  // namespace nf

#pragma once

#include <filesystem>

#include "assets/game_files.hpp"

namespace nf {

// Loads the multiplayer tables (ACTION.ELF + USATxt.dat), checks every level bin, skin chunk file, label
// and sprite they name, then drives MpSetup through a full setup for every scenario on every map (with the
// rewards unlocked) and through MpMatch's end conditions. Prints a summary and returns the failure count.
std::size_t validate_mp(GameFiles& files, const std::filesystem::path& gamedir);

}  // namespace nf

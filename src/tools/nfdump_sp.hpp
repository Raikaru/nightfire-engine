#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

#include "assets/game_files.hpp"

namespace nf {

// Single-player enemy data (docs/ai-sp.md): reads DroneTypeSettings / DroneModeSettings, the behaviour bit layout and
// the NDrone2_init_DMODE_* functions out of ACTION.ELF and compares them with the spec, then parses the enemy
// placement (NPCs 0x0f, cover nodes 0xe5/0xe6, AI points 0xe9, spawners 0xf1, AI volumes 0xf5/0xf8) of every single
// player level, checks that every skin id resolves in the level's character bank and that every placed NPC resolves
// at all three difficulties. Prints a summary and returns the failure count.
std::size_t validate_sp(GameFiles& files, const std::filesystem::path& gamedir);

// `nfdump <dir> sp [level.bin]`: per-level placement tables (all levels when no name is given).
int dump_sp(GameFiles& files, const std::filesystem::path& gamedir, const std::string& level_bin);

}  // namespace nf

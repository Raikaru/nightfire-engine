#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "assets/game_files.hpp"

namespace nf {

// `nfdump <gamedir> sounds [banks|bank <slot>|music [n]|streams|maps]`
int cmd_sounds(GameFiles& files, const std::filesystem::path& gamedir, const std::vector<std::string>& args);

// Decodes every sound bank, music track and stream on the disc and checks every table against the data.
// Prints a summary and returns the number of failures.
std::size_t validate_sounds(GameFiles& files, const std::filesystem::path& gamedir);

}  // namespace nf

#pragma once

#include <cstddef>
#include <filesystem>

namespace nf {

// Reads every member of every DRIVING/*.VIV mission archive (BIGF + RefPack), loads each `.crp` (CARP
// directory + embedded ELF object), builds the world mesh and collision of every track, decodes the vehicle
// models and parses the camera/attribute text files. Prints a per-archive summary and returns the number of
// failures (0 = everything decoded). Skipped with a note when the gamedir has no DRIVING directory.
std::size_t validate_driving(const std::filesystem::path& gamedir);

}  // namespace nf

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace nf {

// `nfdump <gamedir> ssh <viv> <member> [outdir]`: lists the images of an .ssh inside DRIVING/<viv>.VIV (or the
// TAR bindings of a .crp) and, with `outdir`, writes every image as <index>_<name>.bmp.
// <viv> is the archive name without extension (MIS01, RACE, MISC, ...); <member> a path like
// car/model/vanquish.ssh.
int cmd_ssh(const std::filesystem::path& gamedir, const std::vector<std::string>& args);

// Parses every .ssh of every DRIVING/*.VIV archive (and the two SHPS blobs embedded in DRIVING.ELF), then
// checks every texture symbol of every .crp against the .ssh files named by its `sn` entries. Prints a summary
// and returns the number of failures.
std::size_t validate_ssh(const std::filesystem::path& gamedir);

}  // namespace nf

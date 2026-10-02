#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace nf {

struct BigEntry {
    std::string path;        // backslash-separated, e.g. "data\\track\\paris_mis01.crp"
    std::uint32_t offset;
    std::uint32_t size;
};

// EA "BIGF" archive (DRIVING/*.VIV on the disc), read by DRIVING.ELF's BIG_* file layer:
//   char magic[4] = "BIGF"; u32 file_size (LE); u32 count (BE); u32 header_size (BE);
//   count x { u32 offset (BE); u32 size (BE); char path[] (NUL-terminated) }.
class BigArchive {
public:
    explicit BigArchive(const std::filesystem::path& file);

    const std::vector<BigEntry>& entries() const { return entries_; }
    // Case-insensitive; '/' and '\\' are interchangeable. nullptr if absent.
    const BigEntry* find(std::string_view path) const;
    // File bytes, RefPack-decompressed when the entry is packed.
    std::vector<std::uint8_t> read(const BigEntry& entry);
    std::vector<std::uint8_t> read_raw(const BigEntry& entry);

private:
    std::vector<BigEntry> entries_;
    std::ifstream file_;
    std::filesystem::path name_;
};

}  // namespace nf

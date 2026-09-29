#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace nf {

// FILES.BIN has no header. ACTION.ELF's `FileList` (118 x 32 bytes) indexes it:
//   char name[16]; u32 byte_offset; u32 byte_size; u32 pad[2]
// (psiFileOpen: sector = offset >> 11).
struct GameFile {
    std::string name;
    std::uint32_t offset;
    std::uint32_t size;
};

class GameFiles {
public:
    // `dir` must contain ACTION.ELF and FILES.BIN extracted from the PS2 disc.
    explicit GameFiles(const std::filesystem::path& dir);

    const std::vector<GameFile>& files() const { return files_; }
    const GameFile* find(std::string_view name) const;  // case-insensitive, like psiFileOpen's strupr/strcmp
    std::vector<std::uint8_t> read(const GameFile& file);

private:
    std::vector<GameFile> files_;
    std::ifstream bin_;
};

std::vector<std::uint8_t> read_file(const std::filesystem::path& path);

}  // namespace nf

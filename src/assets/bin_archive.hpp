#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "assets/reader.hpp"

namespace nf {

// Entry types, from LoaderProcess's switch on DirFileType.
enum class EntryType : std::uint8_t {
    MapShared = 0,       // parsemap(hash, 0)
    Map = 1,             // parsemap(hash, 1): the level itself
    MapShared2 = 2,      // parsemap(hash, 0)
    Anim3 = 3,           // AnimLoadFile
    Anim4 = 4,
    Anim5 = 5,
    Skeleton = 6,        // AnimSkeletonProcess
    Script = 7,          // Script_Load
    Menu = 8,            // MenuManager_Load
    MapShared11 = 11,    // parsemap(hash, 0) with MemType = 1
    LoadableList = 12,
    Icon = 15,           // SetUpIconFile
    Woman = 16,          // PS2SetUpWoman
};

inline bool is_map_chunk_file(EntryType t) {
    return t == EntryType::MapShared || t == EntryType::Map || t == EntryType::MapShared2 ||
           t == EntryType::MapShared11;
}

struct BinEntry {
    EntryType type;
    std::string name;     // e.g. "01000093"; basename's leading hex digits are the hash
    std::uint32_t hash;   // DirFileHash
    Bytes data;
};

// Level .bin container (LoaderLoad):
//   0x000: u32 dir_size, u32 count, (hash/pad to 0x800)
//   0x800: count x { u32 length; u8 type; char name[] (NUL-terminated) }
//   0x800 + dir_size: file data, concatenated in directory order.
// A dir_size of 0 marks an empty placeholder file.
std::vector<BinEntry> parse_bin_archive(Bytes data);

}  // namespace nf

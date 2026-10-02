#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "assets/reader.hpp"

namespace nf {

// Localised string table (`<LANG>Txt.dat` narrow, `<LANG>TxtU.dat` UTF-16; Txt_LoadLanguage):
//
//   u32 blob_size; u8 blob[blob_size]; pad to 4 (a full extra 4 when already aligned)
//   u32 count; u32 offset[count]           offsets into the blob (bytes; u16 units in the wide files)
//   u32 fixup_count; u32 fixup[fixup_count]
//
// The runtime bank is `count` entries with entry 0 a dummy, so string id `n` is offset[n - 1]. The
// last offset word is really `fixup_count`; there are `count - 1` strings. A label hash used by menus
// and HUD sprites (Txt_BindLabel) is `(kind << 24) | index` and resolves to id `fixup[kind] + index`.
// Narrow strings are raw bytes in the game's font encoding (CP1252-like); wide files are UTF-8 here.
class StringTable {
public:
    static StringTable parse(Bytes file, bool wide);

    std::size_t size() const { return strings_.size(); }        // number of real strings (ids 1..size)
    std::string_view get(std::uint32_t id) const;               // id 0 or out of range: empty
    // Txt_BindLabel; 0xFFFFFFFF or an out-of-range hash yields an empty string.
    std::string_view label(std::uint32_t hash) const;
    const std::vector<std::uint32_t>& fixups() const { return fixups_; }

private:
    std::vector<std::string> strings_;
    std::vector<std::uint32_t> fixups_;
};

}  // namespace nf

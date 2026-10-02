#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "assets/reader.hpp"

namespace nf {

// One record of a CARP directory (`.crp`, EA/Redwood Shores "TAR" object file used by DRIVING.ELF for
// tracks and vehicles; loaded by sub_22F898 -> sub_1A9D70 -> sub_1A9600 / sub_2CD8C0).
//
//   file:   u32 magic (bytes "PRAC" = multi-char constant 'CARP'; every tag is stored byte-reversed);
//           u32 head_flags (head_count = head_flags >> 5); u32 plain_count; u32 1; then 16-byte records:
//           head_count sub-TAR heads, plain_count plain records, then the members of every head, group
//           after group. A head's `offset` is in 16-byte units relative to the head record and locates the
//           first of its `count` member records ("Arti" heads = one per 3D article, "CDat"/"Map "/"RNgp" =
//           world groups). Member records carry the same format; their payload offsets are byte offsets.
//   record: u32 id; u32 flags; u32 count; u32 offset
//     id     bit 0 of flags clear: four ASCII chars (e.g. "Arti", "Base", "ELFd");
//            bit 0 set: u16 index + two ASCII tag chars (e.g. index 5, tag "ni")
//     flags  bit 0 = indexed id, bit 1 = `offset` is relative to this record, bits 8.. = payload size
//     count  element count (0 for opaque blobs; for "Arti" the byte length of the string)
struct CarpEntry {
    std::string tag;            // "Arti", "Base", ... or the two-character tag of an indexed id
    std::int32_t index = -1;    // >= 0 for indexed ids
    std::uint32_t flags = 0;
    std::uint32_t count = 0;
    std::size_t offset = 0;     // absolute payload offset in the file (heads: unused)
    std::uint32_t size = 0;     // payload bytes as declared (flags >> 8)
    bool is_head = false;       // sub-TAR head
    std::size_t first_member = 0;  // heads: record number of the first member
    std::int32_t group = -1;    // members: record number of the owning head, -1 for plain records
};

// The relocatable ELF32 object embedded in a CARP file ("ELFd" carries the header and `.data`, "ELFr"
// the section names, symbol table, string table and `.rel.data`), loaded at runtime by the EAGL
// DynamicLoader (sub_29E518 resolves R_MIPS_32 relocations against the symbol pool).
struct ElfSymbol {
    std::string name;
    std::uint32_t value = 0;    // offset into `data` when defined
    std::uint32_t size = 0;
    bool defined = false;       // shndx == .data
};

struct ElfReloc {
    std::uint32_t offset;       // location in `data` (R_MIPS_32 word: section-relative pointer, or addend)
    std::uint32_t symbol;       // index into `symbols`; 1 = the .data section symbol (internal pointer)
    std::uint32_t type;         // R_MIPS_32 == 2
};

struct ElfImage {
    std::vector<std::uint8_t> data;
    std::vector<ElfSymbol> symbols;
    std::vector<ElfReloc> relocs;
    // Offsets (into data) of words that are pointers to other .data locations (relocation against the
    // section symbol): the stored value is the target offset.
    std::vector<std::uint8_t> internal_pointer;   // one flag per 4-byte word of data
    bool is_pointer(std::size_t byte_offset) const { return internal_pointer.at(byte_offset / 4) != 0; }
};

class CarpFile {
public:
    // `file` is the RefPack-decompressed .crp; the caller keeps it alive.
    explicit CarpFile(Bytes file);

    const std::vector<CarpEntry>& entries() const { return entries_; }
    Bytes file() const { return file_; }
    Bytes payload(const CarpEntry& e) const;                     // e.size bytes
    Bytes payload(const CarpEntry& e, std::size_t bytes) const;  // explicit length (counted blobs)

    // All entries with a tag (and optionally index).
    std::vector<const CarpEntry*> find(std::string_view tag) const;
    const CarpEntry* find(std::string_view tag, std::int32_t index) const;

    ElfImage load_elf() const;

private:
    Bytes file_;
    std::vector<CarpEntry> entries_;
};

}  // namespace nf

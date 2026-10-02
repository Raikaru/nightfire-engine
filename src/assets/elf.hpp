#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "assets/reader.hpp"

namespace nf {

// Minimal ELF32 LE reader: enough to pull symbol-addressed data tables out of ACTION.ELF.
class Elf32 {
public:
    explicit Elf32(std::vector<std::uint8_t> image);

    struct Symbol {
        std::uint32_t value;
        std::uint32_t size;
    };
    std::optional<Symbol> symbol(std::string_view name) const;
    // Bytes backing [vaddr, vaddr+size) in a PT_LOAD segment's file image.
    Bytes at(std::uint32_t vaddr, std::uint32_t size) const;
    // Memory image of [vaddr, vaddr+size): file-backed bytes of every PT_LOAD segment, zero elsewhere (.bss).
    std::vector<std::uint8_t> image_range(std::uint32_t vaddr, std::uint32_t size) const;

private:
    struct Segment {
        std::uint32_t offset, vaddr, filesz;
    };
    std::vector<std::uint8_t> image_;
    std::vector<Segment> segments_;
    Bytes symtab_, strtab_;
};

}  // namespace nf

#include "assets/elf.hpp"

namespace nf {

namespace {
constexpr std::uint32_t PT_LOAD = 1;
constexpr std::uint32_t SHT_SYMTAB = 2;
}  // namespace

Elf32::Elf32(std::vector<std::uint8_t> image) : image_(std::move(image)) {
    Bytes data(image_);
    if (data.size() < 0x34 || load<std::uint32_t>(data, 0) != 0x464C457F || data[4] != 1 || data[5] != 1)
        throw FormatError("not a 32-bit little-endian ELF");

    auto phoff = load<std::uint32_t>(data, 0x1C);
    auto phentsize = load<std::uint16_t>(data, 0x2A);
    auto phnum = load<std::uint16_t>(data, 0x2C);
    for (std::uint32_t i = 0; i < phnum; ++i) {
        std::size_t ph = phoff + std::size_t(i) * phentsize;
        if (load<std::uint32_t>(data, ph) != PT_LOAD) continue;
        segments_.push_back({load<std::uint32_t>(data, ph + 4), load<std::uint32_t>(data, ph + 8),
                             load<std::uint32_t>(data, ph + 16)});
    }

    auto shoff = load<std::uint32_t>(data, 0x20);
    auto shentsize = load<std::uint16_t>(data, 0x2E);
    auto shnum = load<std::uint16_t>(data, 0x30);
    for (std::uint32_t i = 0; i < shnum; ++i) {
        std::size_t sh = shoff + std::size_t(i) * shentsize;
        if (load<std::uint32_t>(data, sh + 4) != SHT_SYMTAB) continue;
        symtab_ = slice(data, load<std::uint32_t>(data, sh + 16), load<std::uint32_t>(data, sh + 20));
        std::size_t link = load<std::uint32_t>(data, sh + 24);
        std::size_t strsh = shoff + link * shentsize;
        strtab_ = slice(data, load<std::uint32_t>(data, strsh + 16), load<std::uint32_t>(data, strsh + 20));
        break;
    }
}

std::optional<Elf32::Symbol> Elf32::symbol(std::string_view name) const {
    for (std::size_t off = 0; off + 16 <= symtab_.size(); off += 16) {
        if (load_cstr(strtab_, load<std::uint32_t>(symtab_, off)) == name)
            return Symbol{load<std::uint32_t>(symtab_, off + 4), load<std::uint32_t>(symtab_, off + 8)};
    }
    return std::nullopt;
}

Bytes Elf32::at(std::uint32_t vaddr, std::uint32_t size) const {
    for (const auto& seg : segments_) {
        if (vaddr >= seg.vaddr && vaddr - seg.vaddr + std::uint64_t(size) <= seg.filesz)
            return slice(Bytes(image_), seg.offset + (vaddr - seg.vaddr), size);
    }
    throw FormatError("vaddr not backed by a loaded segment");
}

}  // namespace nf

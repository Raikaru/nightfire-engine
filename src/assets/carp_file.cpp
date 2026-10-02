#include "assets/carp_file.hpp"

#include <algorithm>

namespace nf {
namespace {


struct Shdr {
    std::uint32_t name, type, flags, addr, offset, size, link, info, align, entsize;
};

}  // namespace

CarpFile::CarpFile(Bytes file) : file_(file) {
    if (load<std::uint32_t>(file, 0) != 0x43415250) throw FormatError("not a CARP file");
    const std::uint32_t head_count = load<std::uint32_t>(file, 4) >> 5;  // sub-TAR heads
    const std::uint32_t plain_count = load<std::uint32_t>(file, 8);       // ordinary top-level records
    const std::size_t top = std::size_t(head_count) + plain_count;

    auto read_record = [&](std::size_t pos) {
        CarpEntry e;
        e.flags = load<std::uint32_t>(file, pos + 4);
        e.count = load<std::uint32_t>(file, pos + 8);
        const std::uint32_t off = load<std::uint32_t>(file, pos + 12);
        e.size = e.flags >> 8;
        if (e.flags & 1) {
            e.index = load<std::uint16_t>(file, pos);
            e.tag = {char(file[pos + 3]), char(file[pos + 2])};
        } else {
            e.tag = {char(file[pos + 3]), char(file[pos + 2]), char(file[pos + 1]), char(file[pos])};
        }
        e.offset = (e.flags & 2) ? pos + off : off;
        return std::pair{std::move(e), off};
    };

    // Heads first; their member tables follow the top-level records, group after group.
    std::size_t total = top;
    std::vector<std::pair<std::size_t, std::size_t>> ranges;  // [first, count) per head, in record numbers
    for (std::size_t h = 0; h < head_count; ++h) {
        const std::size_t pos = 16 + h * 16;
        const auto [e, off] = read_record(pos);
        const std::size_t first = (pos + std::size_t(off) * 16 - 16) / 16;
        ranges.emplace_back(first, e.count);
        total = std::max(total, first + e.count);
    }
    if (16 + total * 16 > file.size()) throw FormatError("CARP directory past end of file");
    entries_.reserve(total);
    for (std::size_t i = 0; i < total; ++i) {
        auto [e, off] = read_record(16 + i * 16);
        if (i < head_count) {
            e.is_head = true;
            e.offset = 0;  // heads carry no payload of their own; `members` addresses the table
        }
        entries_.push_back(std::move(e));
    }
    for (std::size_t h = 0; h < head_count; ++h) {
        const auto [first, count] = ranges[h];
        entries_[h].first_member = first;
        for (std::size_t m = first; m < first + count; ++m) entries_[m].group = static_cast<std::int32_t>(h);
    }
}

Bytes CarpFile::payload(const CarpEntry& e) const { return slice(file_, e.offset, e.size); }
Bytes CarpFile::payload(const CarpEntry& e, std::size_t bytes) const { return slice(file_, e.offset, bytes); }

std::vector<const CarpEntry*> CarpFile::find(std::string_view tag) const {
    std::vector<const CarpEntry*> out;
    for (const CarpEntry& e : entries_)
        if (e.tag == tag) out.push_back(&e);
    return out;
}

const CarpEntry* CarpFile::find(std::string_view tag, std::int32_t index) const {
    for (const CarpEntry& e : entries_)
        if (e.tag == tag && e.index == index) return &e;
    return nullptr;
}

ElfImage CarpFile::load_elf() const {
    const auto d = find("ELFd"), r = find("ELFr");
    if (d.size() != 1 || r.size() != 1) throw FormatError("CARP file has no single ELFd/ELFr pair");
    const std::size_t data_base = d[0]->offset, data_size = d[0]->size;
    // ELF file offsets below the ELFd size live in ELFd; the rest continue at the start of ELFr.
    auto abs = [&](std::size_t f) { return f < data_size ? data_base + f : r[0]->offset + (f - data_size); };

    Bytes elf = slice(file_, data_base, 52);
    if (load<std::uint32_t>(elf, 0) != 0x464C457F) throw FormatError("embedded ELF magic missing");
    const std::uint32_t shoff = load<std::uint32_t>(elf, 32);
    const std::uint16_t shnum = load<std::uint16_t>(elf, 48);
    std::vector<Shdr> sh(shnum);
    for (std::uint16_t i = 0; i < shnum; ++i) {
        const std::size_t p = abs(shoff) + std::size_t(i) * 40;
        std::uint32_t w[10];
        for (int k = 0; k < 10; ++k) w[k] = load<std::uint32_t>(file_, p + 4 * k);
        sh[i] = {w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9]};
    }
    const Shdr *data = nullptr, *symtab = nullptr, *rel = nullptr, *strtab = nullptr;
    for (const Shdr& s : sh) {
        if (s.type == 1 && !data) data = &s;          // SHT_PROGBITS: .data
        else if (s.type == 2) symtab = &s;            // SHT_SYMTAB
        else if (s.type == 9) rel = &s;               // SHT_REL
    }
    if (symtab && symtab->link < sh.size()) strtab = &sh[symtab->link];
    if (!data) throw FormatError("embedded ELF lacks .data");
    if (symtab && !strtab) throw FormatError("embedded ELF .symtab has no string table");

    ElfImage img;
    const Bytes dbytes = slice(file_, abs(data->offset), data->size);
    img.data.assign(dbytes.begin(), dbytes.end());
    img.internal_pointer.assign(img.data.size() / 4 + 1, 0);

    if (!symtab) return img;  // objects without symbols (e.g. some weapon models)

    const Bytes str = slice(file_, abs(strtab->offset), strtab->size);
    const std::size_t nsym = symtab->size / 16;
    img.symbols.reserve(nsym);
    for (std::size_t i = 0; i < nsym; ++i) {
        const std::size_t p = abs(symtab->offset) + i * 16;
        ElfSymbol s;
        s.name = std::string(load_cstr(str, load<std::uint32_t>(file_, p)));
        s.value = load<std::uint32_t>(file_, p + 4);
        s.size = load<std::uint32_t>(file_, p + 8);
        s.defined = load<std::uint16_t>(file_, p + 14) == 1;
        img.symbols.push_back(std::move(s));
    }
    const std::size_t nrel = rel ? rel->size / 8 : 0;
    img.relocs.reserve(nrel);
    for (std::size_t i = 0; i < nrel; ++i) {
        const std::size_t p = abs(rel->offset) + i * 8;
        const std::uint32_t off = load<std::uint32_t>(file_, p), info = load<std::uint32_t>(file_, p + 4);
        if ((info >> 8) >= img.symbols.size() || off + 4 > img.data.size())
            throw FormatError("relocation out of range");
        img.relocs.push_back({off, info >> 8, info & 0xFF});
        if ((info & 0xFF) == 2 && (info >> 8) == 1) img.internal_pointer[off / 4] = 1;
    }
    return img;
}

}  // namespace nf

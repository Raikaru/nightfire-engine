#include "assets/strings.hpp"

namespace nf {

namespace {

void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out += char(cp);
    } else if (cp < 0x800) {
        out += char(0xC0 | (cp >> 6));
        out += char(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += char(0xE0 | (cp >> 12));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    } else {
        out += char(0xF0 | (cp >> 18));
        out += char(0x80 | ((cp >> 12) & 0x3F));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    }
}

std::string read_string(Bytes blob, std::size_t unit_offset, bool wide) {
    if (!wide) return std::string(load_cstr(blob, unit_offset));
    std::string out;
    for (std::size_t o = unit_offset * 2;; o += 2) {
        std::uint32_t c = load<std::uint16_t>(blob, o);
        if (!c) return out;
        if (c >= 0xD800 && c < 0xDC00) {
            std::uint32_t lo = load<std::uint16_t>(blob, o + 2);
            if (lo < 0xDC00 || lo > 0xDFFF) throw FormatError("bad UTF-16 surrogate pair");
            c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
            o += 2;
        }
        append_utf8(out, c);
    }
}

}  // namespace

StringTable StringTable::parse(Bytes file, bool wide) {
    auto blob_size = load<std::uint32_t>(file, 0);
    Bytes blob = slice(file, 4, blob_size);
    std::size_t p = 4 + std::size_t(blob_size) + (4 - (blob_size & 3));
    auto count = load<std::uint32_t>(file, p);
    if (count < 2) throw FormatError("string table has no strings");
    std::size_t offsets = p + 4;
    StringTable t;
    std::uint32_t prev = 0;
    for (std::uint32_t i = 0; i + 1 < count; ++i) {
        auto off = load<std::uint32_t>(file, offsets + std::size_t(i) * 4);
        if (i && off <= prev) throw FormatError("string offsets are not increasing");
        prev = off;
        t.strings_.push_back(read_string(blob, off, wide));
    }
    std::size_t f = offsets + std::size_t(count - 1) * 4;
    auto fixup_count = load<std::uint32_t>(file, f);
    if (fixup_count == 0 || fixup_count > 256) throw FormatError("implausible string fixup count");
    for (std::uint32_t i = 0; i < fixup_count; ++i) {
        auto v = load<std::uint32_t>(file, f + 4 + std::size_t(i) * 4);
        if (v > count) throw FormatError("string fixup past the end of the table");
        t.fixups_.push_back(v);
    }
    if (f + 4 + std::size_t(fixup_count) * 4 > file.size()) throw FormatError("string table truncated");
    return t;
}

std::string_view StringTable::get(std::uint32_t id) const {
    return id >= 1 && id <= strings_.size() ? std::string_view(strings_[id - 1]) : std::string_view();
}

std::string_view StringTable::label(std::uint32_t hash) const {
    if (hash == 0xFFFFFFFFu) return {};
    std::uint32_t kind = hash >> 24;
    if (kind >= fixups_.size()) return {};
    return get(fixups_[kind] + (hash & 0xFFFFFF));
}

}  // namespace nf

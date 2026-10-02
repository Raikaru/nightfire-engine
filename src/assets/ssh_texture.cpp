#include "assets/ssh_texture.hpp"

#include <algorithm>
#include <cctype>

namespace nf {
namespace {

// Record codes (low byte of the record's first word). Bits per texel come from byte_3438C8[code & 0x7F]
// (sub_260058) and map to GS formats through sub_27F0E8: 4 -> PSMT4, 8 -> PSMT8, 24 -> PSMCT24, 32 -> PSMCT32.
constexpr std::uint8_t kPalette16 = 32, kPalette32 = 33, kMetalBin = 105, kName = 112;

// Bits 28..31 of the record's +12 flag dword (the top nibble of word14) hold mip levels - 1 (sub_27CD68); bit 12
// of the same dword would put the texel data at record + word16 instead of record + 16, which no shipped file uses.
constexpr std::uint16_t kExternalData = 0x1000;

constexpr std::size_t kHeader = 16;

struct Record {
    std::uint8_t code;
    std::size_t offset;
    std::size_t next;   // absolute offset of the next record, or 0 for the last one
};

// Follows the chain of records of one directory entry (the walk done by sub_306EF0 / sub_27F190).
std::vector<Record> read_chain(Bytes file, std::size_t offset) {
    std::vector<Record> chain;
    for (;;) {
        const auto word = load<std::uint32_t>(file, offset);
        Record r{static_cast<std::uint8_t>(word & 0xFF), offset, 0};
        if (word >> 8) {
            r.next = offset + (word >> 8);
            if (r.next <= offset) throw FormatError("SSH record chain does not advance");
        }
        chain.push_back(r);
        if (!r.next) return chain;
        if (chain.size() > 16) throw FormatError("SSH record chain too long");
        offset = r.next;
    }
}

std::size_t bits_per_texel(std::uint8_t code) {
    switch (code) {
        case 1: return 4;
        case 2: return 8;
        case 4: return 24;
        case 5: return 32;
        default: throw FormatError("unsupported SSH image record code " + std::to_string(code));
    }
}

std::string_view trim_padding(std::string_view s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.remove_suffix(1);
    return s;
}

// 16 * ((w * h * bits + 127) >> 7): the level's size in 16-byte quadwords, as in sub_27CD68's mip walk.
std::size_t level_bytes(std::size_t w, std::size_t h, std::size_t bits) { return 16 * ((w * h * bits + 127) >> 7); }

std::uint32_t pack(std::uint32_t r, std::uint32_t g, std::uint32_t b, std::uint32_t a) {
    return r | g << 8 | b << 16 | a << 24;
}

// PS2 alpha 0x80 is 1.0.
std::uint32_t alpha_from_ps2(std::uint32_t a) { return std::min<std::uint32_t>(255, a * 2); }

// The palette is uploaded as a 16x16 (or 8x2 for PSMT4) PSMCT32 image and read back through CSM1, which swaps
// bit 3 and bit 4 of the entry index for 8-bit palettes. Returns the index into the stored palette.
std::size_t clut_slot(std::size_t index, std::size_t bits) {
    if (bits != 8) return index;
    return (index & 0xE7) | ((index & 0x10) >> 1) | ((index & 0x08) << 1);
}

struct Palette {
    std::vector<std::uint32_t> colors;   // as stored, RGBA8
    std::uint8_t bits = 0;
};

Palette read_palette(Bytes file, const Record& r, std::uint32_t& entries_out) {
    const std::uint32_t entries = load<std::uint16_t>(file, r.offset + 4);
    const std::size_t entry_bytes = r.code == kPalette32 ? 4 : 2;
    const Bytes data = slice(file, r.offset + kHeader, entries * entry_bytes);
    Palette p;
    p.bits = static_cast<std::uint8_t>(entry_bytes * 8);
    p.colors.reserve(entries);
    for (std::uint32_t i = 0; i < entries; ++i) {
        if (entry_bytes == 4) {
            p.colors.push_back(pack(data[4 * i], data[4 * i + 1], data[4 * i + 2], alpha_from_ps2(data[4 * i + 3])));
        } else {
            // PSMCT16: R bits 0-4, G 5-9, B 10-14, A bit 15.
            const std::uint32_t v = data[2 * i] | data[2 * i + 1] << 8;
            auto c5 = [](std::uint32_t c) { return (c << 3) | (c >> 2); };
            p.colors.push_back(pack(c5(v & 31), c5(v >> 5 & 31), c5(v >> 10 & 31), v & 0x8000 ? 255 : 0));
        }
    }
    entries_out = entries;
    return p;
}

std::vector<std::uint32_t> decode_level(Bytes data, SshFormat format, std::size_t w, std::size_t h,
                                        const Palette& palette) {
    std::vector<std::uint32_t> out(w * h);
    auto lookup = [&](std::size_t index) -> std::uint32_t {
        const std::size_t slot = clut_slot(index, format == SshFormat::Indexed8 ? 8 : 4);
        return slot < palette.colors.size() ? palette.colors[slot] : 0;   // beyond the stored entries: unset CLUT
    };
    switch (format) {
        case SshFormat::Indexed4:
            // GS host transfers fill PSMT4 low nibble first.
            for (std::size_t i = 0; i < out.size(); ++i)
                out[i] = lookup(i & 1 ? data[i / 2] >> 4 : data[i / 2] & 15);
            break;
        case SshFormat::Indexed8:
            for (std::size_t i = 0; i < out.size(); ++i) out[i] = lookup(data[i]);
            break;
        case SshFormat::Rgb24:
            for (std::size_t i = 0; i < out.size(); ++i) out[i] = pack(data[3 * i], data[3 * i + 1], data[3 * i + 2], 255);
            break;
        case SshFormat::Rgba32:
            for (std::size_t i = 0; i < out.size(); ++i)
                out[i] = pack(data[4 * i], data[4 * i + 1], data[4 * i + 2], alpha_from_ps2(data[4 * i + 3]));
            break;
    }
    return out;
}

SshImage parse_entry(Bytes file, std::string name, std::size_t offset, std::size_t entry_end) {
    SshImage image;
    image.name = std::move(name);
    const std::vector<Record> chain = read_chain(file, offset);
    const Record* pixels = nullptr;
    const Record* pal = nullptr;
    for (const Record& r : chain) {
        const std::size_t end = r.next ? r.next : entry_end;
        if (end < r.offset + kHeader || end > file.size()) throw FormatError("SSH record extends past its entry");
        switch (r.code) {
            case 1: case 2: case 4: case 5:
                if (pixels) throw FormatError("SSH entry has two image records");
                pixels = &r;
                break;
            case kPalette16: case kPalette32:
                if (pal) throw FormatError("SSH entry has two palette records");
                pal = &r;
                break;
            case kMetalBin: case kName: {
                SshAttachment a;
                a.code = r.code;
                // sub_260090 prefers the 'p' record's name over the directory name; the two agree up to padding
                // (directory names pad with spaces, the record with NULs).
                const Bytes rec_name = slice(file, r.offset + 4, 4);
                if (r.code == kName &&
                    trim_padding({reinterpret_cast<const char*>(rec_name.data()), 4}) != trim_padding(image.name))
                    throw FormatError("SSH name record of '" + image.name + "' disagrees with the directory");
                a.word4 = load<std::uint16_t>(file, r.offset + 4);
                a.word6 = load<std::uint16_t>(file, r.offset + 6);
                const Bytes body = slice(file, r.offset + kHeader, end - r.offset - kHeader);
                a.data.assign(body.begin(), body.end());
                image.attachments.push_back(std::move(a));
                break;
            }
            default:
                throw FormatError("unknown SSH record code " + std::to_string(r.code));
        }
    }
    if (!pixels) throw FormatError("SSH entry '" + image.name + "' has no image record");

    const std::size_t bits = bits_per_texel(pixels->code);
    image.format = static_cast<SshFormat>(pixels->code);
    const auto width = load<std::int16_t>(file, pixels->offset + 4);   // the game reads both as s16
    const auto height = load<std::int16_t>(file, pixels->offset + 6);
    if (width <= 0 || height <= 0) throw FormatError("SSH image '" + image.name + "' has no size");
    image.width = static_cast<std::uint32_t>(width);
    image.height = static_cast<std::uint32_t>(height);
    image.word8 = load<std::uint16_t>(file, pixels->offset + 8);
    image.word10 = load<std::uint16_t>(file, pixels->offset + 10);
    image.word12 = load<std::uint16_t>(file, pixels->offset + 12);
    image.word14 = load<std::uint16_t>(file, pixels->offset + 14);
    if (image.word12 & kExternalData) throw FormatError("SSH image '" + image.name + "' uses external data offsets");
    const std::size_t levels = std::min<std::size_t>((image.word14 >> 12) + 1, 7);
    if (image.width == 1 && image.height == 1 && image.format == SshFormat::Rgba32 && image.word8 && image.word10)
        image.sprite = SshSprite{image.word8, image.word10, image.word12, static_cast<std::uint16_t>(image.word14 & 0xFFF)};

    Palette palette;
    if (bits <= 8) {
        if (!pal) throw FormatError("indexed SSH image '" + image.name + "' has no palette");
        palette = read_palette(file, *pal, image.palette_size);
        image.palette_bits = palette.bits;
    }

    const std::size_t pixel_end = pixels->next ? pixels->next : entry_end;
    std::size_t pos = pixels->offset + kHeader;
    std::size_t w = image.width, h = image.height;
    for (std::size_t level = 0; level < levels; ++level) {
        if (!w || !h) throw FormatError("SSH image '" + image.name + "' has more mip levels than its size allows");
        const std::size_t size = level_bytes(w, h, bits);
        if (pos + size > pixel_end)
            throw FormatError("SSH image '" + image.name + "' level " + std::to_string(level) + " overruns its record");
        std::vector<std::uint32_t> rgba = decode_level(slice(file, pos, size), image.format, w, h, palette);
        if (level == 0) {
            image.rgba = std::move(rgba);
        } else {
            image.mips.push_back({static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), std::move(rgba)});
        }
        pos += size;
        w >>= 1;
        h >>= 1;
    }
    // The record chain's next pointer is exactly the end of the last level (every shipped image); only a final
    // record may be followed by entry padding.
    if (pixels->next && pos != pixel_end)
        throw FormatError("SSH image '" + image.name + "' levels do not fill their record");
    return image;
}

}  // namespace

std::optional<std::size_t> SshFile::find(std::string_view name) const {
    for (std::size_t i = 0; i < images.size(); ++i)
        if (images[i].name == name) return i;
    return std::nullopt;
}

SshMip crop_sprite(const SshFile& file, const SshImage& image) {
    if (!image.sprite) throw FormatError("SSH image '" + image.name + "' is not an atlas sprite");
    const auto atlas_index = file.find("MAIN");
    if (!atlas_index) throw FormatError("SSH file has no MAIN atlas for sprite '" + image.name + "'");
    const SshImage& atlas = file.images[*atlas_index];
    const SshSprite& s = *image.sprite;
    if (std::size_t(s.x) + s.width > atlas.width || std::size_t(s.y) + s.height > atlas.height)
        throw FormatError("sprite '" + image.name + "' lies outside the atlas");
    SshMip out;
    out.width = s.width;
    out.height = s.height;
    out.rgba.reserve(std::size_t(s.width) * s.height);
    for (std::uint32_t y = 0; y < s.height; ++y)
        for (std::uint32_t x = 0; x < s.width; ++x) out.rgba.push_back(atlas.rgba[(s.y + y) * atlas.width + s.x + x]);
    return out;
}

SshFile parse_ssh(Bytes data) {
    const auto magic = load<std::uint32_t>(data, 0);
    if (magic != 0x53504853 && magic != 0x45504853) throw FormatError("not an SHPS file");  // "SHPS" / "SHPE"
    if (load<std::uint32_t>(data, 4) != data.size()) throw FormatError("SHPS size field disagrees with file size");
    const auto count = load<std::uint32_t>(data, 8);
    SshFile file;
    file.gx_version.assign(reinterpret_cast<const char*>(slice(data, 12, 4).data()), 4);

    struct Dir {
        std::string name;
        std::size_t offset;
        std::size_t index;
    };
    std::vector<Dir> dir;
    dir.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t pos = 16 + std::size_t(i) * 8;
        const Bytes name = slice(data, pos, 4);
        dir.push_back({std::string(reinterpret_cast<const char*>(name.data()), 4), load<std::uint32_t>(data, pos + 4), i});
    }
    // An entry ends where the next-higher offset begins (or at the end of the file).
    std::vector<std::size_t> starts;
    for (const Dir& d : dir) starts.push_back(d.offset);
    std::sort(starts.begin(), starts.end());
    file.images.resize(count);
    for (const Dir& d : dir) {
        const auto after = std::upper_bound(starts.begin(), starts.end(), d.offset);
        const std::size_t end = after == starts.end() ? data.size() : *after;
        file.images[d.index] = parse_entry(data, d.name, d.offset, end);
    }
    return file;
}

std::optional<TarSymbol> parse_tar_symbol(std::string_view symbol) {
    constexpr std::string_view prefix = "__EAGL::TAR:::{";
    if (!symbol.starts_with(prefix)) return std::nullopt;
    symbol.remove_prefix(prefix.size());
    // <kind:2><sep:1><id:8 hex>}TEX<slot>::<name>
    if (symbol.size() < 12 || symbol[2 + 1 + 8] != '}') return std::nullopt;
    TarSymbol out;
    out.kind = std::string(symbol.substr(0, 2));
    out.separator = symbol[2];
    if (out.separator != '@' && out.separator != '|') return std::nullopt;
    for (char c : symbol.substr(3, 8)) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return std::nullopt;
        out.id = out.id << 4 | static_cast<std::uint32_t>(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
    }
    symbol.remove_prefix(12);
    if (!symbol.starts_with("TEX")) return std::nullopt;
    symbol.remove_prefix(3);
    const std::size_t digits = symbol.find_first_not_of("0123456789");
    if (digits == 0 || digits == std::string_view::npos || !symbol.substr(digits).starts_with("::")) return std::nullopt;
    out.slot = static_cast<std::uint32_t>(std::stoul(std::string(symbol.substr(0, digits))));
    out.name = std::string(symbol.substr(digits + 2));
    return out;
}

std::string tar_shape_name(Bytes tar_object) {
    const Bytes name = slice(tar_object, 4, 4);
    return std::string(reinterpret_cast<const char*>(name.data()), 4);
}

std::vector<TarBinding> tar_bindings(const ElfImage& elf) {
    std::vector<TarBinding> out;
    for (const ElfSymbol& s : elf.symbols) {
        std::optional<TarSymbol> parsed = parse_tar_symbol(s.name);
        if (!parsed || !s.defined) continue;
        TarBinding b;
        b.symbol = s.name;
        b.parsed = std::move(*parsed);
        b.data_offset = s.value;
        b.shape_name = tar_shape_name(slice(Bytes(elf.data), s.value, kTarObjectSize));
        out.push_back(std::move(b));
    }
    return out;
}

std::optional<SshRef> find_shape(std::span<const SshFile* const> files, std::string_view name) {
    for (std::size_t f = 0; f < files.size(); ++f)
        if (const auto image = files[f]->find(name)) return SshRef{f, *image};
    return std::nullopt;
}

}  // namespace nf

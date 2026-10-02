#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "assets/carp_file.hpp"
#include "assets/reader.hpp"

namespace nf {

// EA "SHPS" shape/texture library (`.ssh`) as used by the driving executable (DRIVING.ELF). See
// docs/driving-textures.md for the reversed layout and the function addresses it comes from.
//
//   file:   char magic[4] = "SHPS" ("SHPE" is also accepted by the loader, sub_306CE0);
//           u32 file_size; u32 entry_count; char gx[4] = "G354" etc. (GX tool version, read by sub_306E68);
//           entry_count x { char name[4]; u32 offset }.
//   entry:  a chain of 16-byte-headed records starting at `offset`:
//           u32 code_and_next (low byte record code, upper 24 bits = byte distance to the next record,
//           0 = last); then a code specific 12-byte header.  Codes:
//             1, 2, 4, 5  image, PSMT4 / PSMT8 / PSMCT24 / PSMCT32 raster (byte_3438C8[code] = bits per texel)
//             32, 33      palette, 16-bit / 32-bit entries
//             105 'i'     "EAGL240 metal bin" runtime attachment
//             112 'p'     name record ending the chain
enum class SshFormat : std::uint8_t { Indexed4 = 1, Indexed8 = 2, Rgb24 = 4, Rgba32 = 5 };

// One mip level below the base image.
struct SshMip {
    std::uint32_t width = 0, height = 0;
    std::vector<std::uint32_t> rgba;
};

// A non-image, non-palette record of an entry (the 'i' metal bin and the 'p' name record).
struct SshAttachment {
    std::uint8_t code = 0;
    std::uint16_t word4 = 0, word6 = 0;   // record header +4 / +6 (for 'i': 240 and 0x10/0x90 flags)
    std::vector<std::uint8_t> data;       // bytes between the record header and the next record
};

// The particle libraries (partic01/02, ears_/sav_partic01) hold one large "MAIN" PSMCT32 atlas plus 1x1 PSMCT32
// placeholder records whose header words describe a cell of it: width, height, x, y (in texels).
struct SshSprite {
    std::uint16_t width = 0, height = 0, x = 0, y = 0;
};

struct SshImage {
    std::string name;                     // directory name, 4 characters (e.g. "rrvw", "$$00", "Isk4")
    SshFormat format = SshFormat::Indexed8;
    std::uint32_t width = 0, height = 0;
    // Base level, row-major top row first, R in the low byte then G, B, A. Palette alpha 0x80 (PS2 1.0)
    // and 16-bit palette alpha bit 1 map to 255; PSMCT24 texels are opaque.
    std::vector<std::uint32_t> rgba;
    std::vector<SshMip> mips;             // levels 1.. (each half the size of the previous, min 1)
    // The four u16 of the image record header at +8..+15, raw. Bits 12..15 of word14 are the mip count - 1 and
    // bit 12 / 13 of word12 are the "data offset follows" / 0x2000 flags read by sub_27CD68; the other bits are
    // the sprite rectangle below when `sprite` is set, and 0 or 1 in ordinary images.
    std::uint16_t word8 = 0, word10 = 0, word12 = 0, word14 = 0;
    std::optional<SshSprite> sprite;      // particle atlas cell (see SshSprite)
    std::uint32_t palette_size = 0;       // palette entries (0 for direct colour)
    std::uint8_t palette_bits = 0;        // 16 or 32 for indexed images, else 0
    std::vector<SshAttachment> attachments;
};

struct SshFile {
    std::string gx_version;               // "G354": header bytes 12..15
    std::vector<SshImage> images;         // directory order

    // Directory-name lookup (exact 4-character match, first hit).
    std::optional<std::size_t> find(std::string_view name) const;
};

// The RGBA texels of an atlas cell: `sprite` of `image` cut out of the file's "MAIN" image.
SshMip crop_sprite(const SshFile& file, const SshImage& image);

SshFile parse_ssh(Bytes data);

// --- Texture symbols of a CARP object --------------------------------------------------------------------
//
// A `.crp` model/track object *defines* one 96-byte EAGL::TAR object per material texture, exported under
//   __EAGL::TAR:::{T0@41ed1954}TEX0::rrvw        (hash form, shape name is always 4 characters)
//   __EAGL::TAR:::{T0|19f91200}TEX0::pb01_14     (index form, name may carry a `_<n>` suffix)
// (see docs/driving-textures.md). The TAR object is what the runtime binds to a shape: sub_27C6A0 takes the
// four characters at TAR + 4, builds `shape_<name>` and looks it up in the shape pool, which holds the entries
// of the .ssh files named by the .crp's `sn` directory entries (sub_1A9600 registers them, sub_29FDA0); a name
// that is not in the pool falls back to the built-in "fail" shape at vaddr 0x3441F0. The symbol's `{..}` id is
// an exporter-side unique per-TAR id and is not used for the lookup; for index-form symbols the name is not the
// shape name either, so the shape name MUST come from the TAR object bytes.
struct TarSymbol {
    std::string kind;                     // "T0", "Tb", "Td", "T1", ...
    char separator = '@';                 // '@' hash form, '|' index form
    std::uint32_t id = 0;
    std::uint32_t slot = 0;               // `TEX<slot>`: index of the .crp `sn` entry naming the .ssh
    std::string name;                     // text after `TEX<slot>::`
};

// Parses a symbol name; nullopt for other symbols (GAME::..., EAGL::EnvironmentMap, ...).
std::optional<TarSymbol> parse_tar_symbol(std::string_view symbol);

constexpr std::size_t kTarObjectSize = 0x60;

// The shape name of a TAR object: its four characters at +4, space padded exactly like .ssh directory names.
std::string tar_shape_name(Bytes tar_object);

struct TarBinding {
    std::string symbol;                   // full ELF symbol name
    TarSymbol parsed;
    std::uint32_t data_offset = 0;        // TAR object offset in the ELF `.data`
    std::string shape_name;               // from the TAR object
};

// Every defined texture symbol of `elf`, with the shape name read from its TAR object.
std::vector<TarBinding> tar_bindings(const ElfImage& elf);

struct SshRef {
    std::size_t file = 0;                 // index into the pool's file list
    std::size_t image = 0;
};

// The shape pool of one .crp load: `files` are the .ssh files of its `sn` entries in slot order, and the first
// file that has an entry called `name` wins (sub_29FDA0 only adds names that are not registered yet).
// nullopt means the runtime substitutes the built-in "fail" texture.
std::optional<SshRef> find_shape(std::span<const SshFile* const> files, std::string_view name);

}  // namespace nf

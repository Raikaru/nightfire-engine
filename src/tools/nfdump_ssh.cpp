#include "tools/nfdump_ssh.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <set>

#include "assets/big_archive.hpp"
#include "assets/carp_file.hpp"
#include "assets/elf.hpp"
#include "assets/game_files.hpp"
#include "assets/ssh_texture.hpp"

namespace nf {
namespace {

// DRIVING.ELF embeds two SHPS blobs: the "dot " image and the "fail" shape that sub_27C6A0 substitutes for a
// missing texture (vaddr 0x3441F0 + the entry offset word at 0x344204).
struct EmbeddedShps {
    std::uint32_t vaddr, size;
};
constexpr EmbeddedShps kEmbedded[] = {{0x33B1E0, 1424}, {0x3441F0, 16784}};

std::string lower(std::string s) {
    for (char& c : s) c = c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool ends_with(const std::string& s, std::string_view suffix) { return s.ends_with(suffix); }

std::vector<std::filesystem::path> list_vivs(const std::filesystem::path& gamedir) {
    std::vector<std::filesystem::path> vivs;
    for (const auto& e : std::filesystem::directory_iterator(gamedir / "DRIVING"))
        if (e.path().extension() == ".VIV") vivs.push_back(e.path());
    std::sort(vivs.begin(), vivs.end());
    return vivs;
}

// Directory part of an archive path including the trailing backslash.
std::string dir_of(const std::string& path) {
    const std::size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

void write_le(std::FILE* f, std::uint32_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) std::fputc(int(v >> (8 * i) & 0xFF), f);
}

// 32-bit top-down BMP (BGRA).
void write_bmp(const std::filesystem::path& path, std::uint32_t width, std::uint32_t height,
               const std::vector<std::uint32_t>& rgba) {
    std::FILE* f = std::fopen(path.string().c_str(), "wb");
    if (!f) throw FormatError("cannot write " + path.string());
    const std::uint32_t bytes = width * height * 4;
    std::fputs("BM", f);
    write_le(f, 54 + bytes, 4);
    write_le(f, 0, 4);
    write_le(f, 54, 4);
    write_le(f, 40, 4);
    write_le(f, width, 4);
    write_le(f, static_cast<std::uint32_t>(-static_cast<std::int32_t>(height)), 4);
    write_le(f, 1, 2);
    write_le(f, 32, 2);
    write_le(f, 0, 4);
    write_le(f, bytes, 4);
    write_le(f, 2835, 4);
    write_le(f, 2835, 4);
    write_le(f, 0, 4);
    write_le(f, 0, 4);
    for (std::uint32_t p : rgba) {
        std::fputc(int(p >> 16 & 0xFF), f);
        std::fputc(int(p >> 8 & 0xFF), f);
        std::fputc(int(p & 0xFF), f);
        std::fputc(int(p >> 24 & 0xFF), f);
    }
    std::fclose(f);
}

std::string safe_name(const std::string& name) {
    std::string s;
    for (char c : name) s += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
    return s;
}

const char* format_name(SshFormat f) {
    switch (f) {
        case SshFormat::Indexed4: return "PSMT4";
        case SshFormat::Indexed8: return "PSMT8";
        case SshFormat::Rgb24: return "PSMCT24";
        case SshFormat::Rgba32: return "PSMCT32";
    }
    return "?";
}

void print_images(const SshFile& file) {
    std::printf("%s, %zu images\n", file.gx_version.c_str(), file.images.size());
    for (std::size_t i = 0; i < file.images.size(); ++i) {
        const SshImage& im = file.images[i];
        std::printf("%3zu %-4s %4ux%-4u %-7s mips %zu pal %u/%u words %04x %04x %04x %04x", i, im.name.c_str(),
                    im.width, im.height, format_name(im.format), im.mips.size(), im.palette_size, im.palette_bits,
                    im.word8, im.word10, im.word12, im.word14);
        if (im.sprite)
            std::printf("  sprite %ux%u at %u,%u", im.sprite->width, im.sprite->height, im.sprite->x, im.sprite->y);
        std::printf("\n");
    }
}

enum class Where { OwnSlot, OtherSlot, Render, Missing };

using SshLookup = std::function<const SshFile*(const std::string& lower_path)>;

// Classifies every texture symbol of a .crp (archive path `crp_path`) the way the runtime resolves them: the
// shape pool holds the .ssh files of the `sn` entries, slot order; `render` are the level-wide libraries.
// Calls `visit(binding, where)` for each symbol; throws when an `sn` file is missing or a hash-form symbol name
// disagrees with its TAR object.
void resolve_crp(const std::string& crp_path, const CarpFile& crp, const SshLookup& lookup,
                 const std::vector<const SshFile*>& render,
                 const std::function<void(const TarBinding&, Where)>& visit) {
    std::vector<const SshFile*> slots;   // by slot index, nullptr for gaps
    for (const CarpEntry* sn : crp.find("sn")) {
        const std::string name(load_cstr(crp.payload(*sn), 0));
        const SshFile* file = lookup(lower(dir_of(crp_path)) + lower(name));
        if (!file) throw FormatError("sn " + std::to_string(sn->index) + " names missing " + name);
        if (slots.size() <= std::size_t(sn->index)) slots.resize(sn->index + 1, nullptr);
        slots[sn->index] = file;
    }
    std::vector<const SshFile*> pool;   // registration order
    for (const SshFile* f : slots)
        if (f) pool.push_back(f);

    for (const TarBinding& b : tar_bindings(crp.load_elf())) {
        if (b.parsed.separator == '@' && b.parsed.name != b.shape_name)
            throw FormatError("symbol " + b.symbol + " disagrees with its TAR object '" + b.shape_name + "'");
        const SshFile* slot_file = b.parsed.slot < slots.size() ? slots[b.parsed.slot] : nullptr;
        Where where = Where::Missing;
        if (slot_file && slot_file->find(b.shape_name)) where = Where::OwnSlot;
        else if (find_shape(pool, b.shape_name)) where = Where::OtherSlot;
        else if (std::any_of(render.begin(), render.end(), [&](const SshFile* f) { return f->find(b.shape_name).has_value(); }))
            where = Where::Render;
        visit(b, where);
    }
}

const char* where_name(Where w) {
    switch (w) {
        case Where::OwnSlot: return "own slot";
        case Where::OtherSlot: return "other sn file";
        case Where::Render: return "data/render only";
        case Where::Missing: return "MISSING -> fail texture";
    }
    return "?";
}

}  // namespace

int cmd_ssh(const std::filesystem::path& gamedir, const std::vector<std::string>& args) {
    if (args.size() < 2) {
        std::fprintf(stderr, "usage: nfdump <gamedir> ssh <viv> <member> [outdir]\n");
        return 2;
    }
    BigArchive archive(gamedir / "DRIVING" / (args[0] + ".VIV"));
    const BigEntry* entry = archive.find(args[1]);
    if (!entry) entry = archive.find("data/" + args[1]);
    if (!entry) throw FormatError("no such member " + args[1]);
    const std::vector<std::uint8_t> bytes = archive.read(*entry);

    if (ends_with(lower(entry->path), ".crp")) {
        const CarpFile crp(bytes);
        std::map<std::string, std::unique_ptr<SshFile>> parsed;   // lower-case path -> file, loaded on demand
        auto lookup = [&](const std::string& path) -> const SshFile* {
            auto it = parsed.find(path);
            if (it == parsed.end()) {
                const BigEntry* e = archive.find(path);
                it = parsed.emplace(path, e ? std::make_unique<SshFile>(parse_ssh(archive.read(*e))) : nullptr).first;
            }
            return it->second.get();
        };
        std::vector<const SshFile*> render;
        for (const BigEntry& e : archive.entries())
            if (ends_with(lower(e.path), ".ssh") && lower(e.path).starts_with("data/render/")) render.push_back(lookup(lower(e.path)));
        std::size_t counts[4] = {};
        std::map<std::string, std::size_t> missing;
        resolve_crp(entry->path, crp, lookup, render, [&](const TarBinding& b, Where where) {
            ++counts[static_cast<int>(where)];
            if (where == Where::Missing) ++missing[b.shape_name];
            std::printf("%-2s%c%08x TEX%u %-22s shape '%s' %s\n", b.parsed.kind.c_str(), b.parsed.separator, b.parsed.id,
                        b.parsed.slot, b.parsed.name.c_str(), b.shape_name.c_str(), where_name(where));
        });
        std::printf("%zu symbols: %zu own slot, %zu other sn file, %zu data/render only, %zu missing", counts[0] + counts[1] + counts[2] + counts[3],
                    counts[0], counts[1], counts[2], counts[3]);
        for (const auto& [name, n] : missing) std::printf(" %s:%zu", name.c_str(), n);
        std::printf("\n");
        return 0;
    }

    const SshFile file = parse_ssh(bytes);
    print_images(file);
    if (args.size() >= 3) {
        const std::filesystem::path out = args[2];
        std::filesystem::create_directories(out);
        for (std::size_t i = 0; i < file.images.size(); ++i) {
            char prefix[32];
            std::snprintf(prefix, sizeof prefix, "%03u_", static_cast<unsigned>(i));
            const SshImage& im = file.images[i];
            const std::string path = prefix + safe_name(im.name) + ".bmp";
            if (im.sprite) {   // particle atlas cell: write the cut-out rather than the 1x1 placeholder
                const SshMip cell = crop_sprite(file, im);
                write_bmp(out / path, cell.width, cell.height, cell.rgba);
            } else {
                write_bmp(out / path, im.width, im.height, im.rgba);
            }
        }
        std::printf("wrote %zu bmp files to %s\n", file.images.size(), out.string().c_str());
    }
    return 0;
}

std::size_t validate_ssh(const std::filesystem::path& gamedir) {
    std::size_t failures = 0;
    auto fail = [&](const std::string& what, const std::exception& e) {
        std::printf("FAIL %s: %s\n", what.c_str(), e.what());
        ++failures;
    };

    std::size_t files = 0, images = 0, mips = 0, formats[6] = {}, pixels = 0;
    auto count = [&](const SshFile& f) {
        ++files;
        images += f.images.size();
        for (const SshImage& im : f.images) {
            mips += im.mips.size();
            ++formats[static_cast<int>(im.format)];
            pixels += im.rgba.size();
        }
    };

    // The blobs inside DRIVING.ELF.
    {
        const Elf32 elf(read_file(gamedir / "DRIVING.ELF"));
        for (const EmbeddedShps& e : kEmbedded) {
            try {
                count(parse_ssh(elf.at(e.vaddr, e.size)));
            } catch (const std::exception& ex) {
                fail("DRIVING.ELF @" + std::to_string(e.vaddr), ex);
            }
        }
    }

    std::size_t crps = 0, elfless = 0, symbols = 0, own_slot = 0, other_slot = 0, render_set = 0, unresolved = 0;
    std::map<std::string, std::size_t> missing;
    for (const auto& viv : list_vivs(gamedir)) {
        BigArchive archive(viv);
        const std::string archive_name = viv.stem().string();
        std::map<std::string, std::unique_ptr<SshFile>> ssh;   // lower-case path -> parsed file (pixels released)
        const std::size_t files_before = files, images_before = images, failures_before = failures;
        for (const BigEntry& e : archive.entries()) {
            const std::string path = lower(e.path);
            if (!ends_with(path, ".ssh")) continue;
            try {
                auto parsed = std::make_unique<SshFile>(parse_ssh(archive.read(e)));
                count(*parsed);
                for (SshImage& im : parsed->images) {   // only the directory is needed below
                    im.rgba = {};
                    im.mips = {};
                }
                ssh[path] = std::move(parsed);
            } catch (const std::exception& ex) {
                fail(archive_name + ":" + e.path, ex);
            }
        }
        std::printf("%-7s %3zu ssh files, %5zu images", archive_name.c_str(), files - files_before,
                    images - images_before);

        std::vector<const SshFile*> render;   // the level-wide libraries (common, ext, partic01, ...)
        for (const auto& [path, file] : ssh)
            if (path.find("data/render/") == 0) render.push_back(file.get());

        std::size_t archive_symbols = 0;
        for (const BigEntry& e : archive.entries()) {
            const std::string path = lower(e.path);
            if (!ends_with(path, ".crp")) continue;
            try {
                const std::vector<std::uint8_t> bytes = archive.read(e);
                const CarpFile crp(bytes);
                ++crps;
                if (crp.find("ELFd").empty()) {
                    ++elfless;
                    continue;
                }
                const SshLookup lookup = [&](const std::string& p) -> const SshFile* {
                    const auto it = ssh.find(p);
                    return it == ssh.end() ? nullptr : it->second.get();
                };
                resolve_crp(e.path, crp, lookup, render, [&](const TarBinding& b, Where where) {
                    ++symbols;
                    ++archive_symbols;
                    switch (where) {
                        case Where::OwnSlot: ++own_slot; break;
                        case Where::OtherSlot: ++other_slot; break;
                        case Where::Render: ++render_set; break;
                        case Where::Missing:
                            ++unresolved;
                            ++missing[b.shape_name];
                            break;
                    }
                });
            } catch (const std::exception& ex) {
                fail(archive_name + ":" + e.path, ex);
            }
        }
        std::printf(", %5zu texture symbols, %zu failures\n", archive_symbols, failures - failures_before);
    }

    std::printf("ssh: %zu files (incl. 2 embedded in DRIVING.ELF), %zu images, %zu extra mip levels, %zu texels\n", files,
                images, mips, pixels);
    std::printf("formats: PSMT4 %zu, PSMT8 %zu, PSMCT24 %zu, PSMCT32 %zu\n", formats[1], formats[2], formats[4], formats[5]);
    std::printf("crp: %zu files (%zu without ELF), %zu texture symbols: %zu in own slot, %zu in another sn file, "
                "%zu only in data/render, %zu unresolved (built-in fail texture)\n",
                crps, elfless, symbols, own_slot, other_slot, render_set, unresolved);
    if (!missing.empty()) {
        std::printf("unresolved shape names:");
        for (const auto& [name, n] : missing) std::printf(" %s:%zu", name.c_str(), n);
        std::printf("\n");
    }
    std::printf("failures %zu\n", failures);
    return failures;
}

}  // namespace nf

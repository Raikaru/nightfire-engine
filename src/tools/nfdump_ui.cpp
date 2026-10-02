#include "tools/nfdump_ui.hpp"

#include <cstdio>
#include <exception>
#include <map>
#include <string>

#include "assets/bin_archive.hpp"
#include "assets/menu_file.hpp"
#include "assets/sprites.hpp"
#include "assets/strings.hpp"
#include "assets/ui_fonts.hpp"

namespace nf {

std::size_t validate_ui(GameFiles& files, const std::filesystem::path& gamedir) {
    std::size_t failures = 0;
    auto fail = [&](const std::string& what, const std::exception& e) {
        std::printf("FAIL %s: %s\n", what.c_str(), e.what());
        ++failures;
    };

    FontSet fonts;
    std::size_t glyphs = 0, kerns = 0;
    try {
        fonts = load_fonts(Elf32(read_file(gamedir / "ACTION.ELF")));
        for (const auto& f : fonts.fonts) {
            glyphs += f.glyphs.size();
            for (const auto& g : f.glyphs) kerns += g.kern_after.size();
        }
    } catch (const std::exception& e) {
        fail("fonts", e);
    }

    std::size_t tables = 0, strings = 0, labels = 0;
    for (const auto& f : files.files()) {
        if (f.name.size() < 7 || f.name == "TuningVars.txt") continue;
        std::string lower;
        for (char c : f.name) lower += char(std::tolower(static_cast<unsigned char>(c)));
        if (!lower.ends_with("txt.dat") && !lower.ends_with("txtu.dat")) continue;
        try {
            auto data = files.read(f);
            StringTable t = StringTable::parse(Bytes(data), lower.ends_with("txtu.dat"));
            ++tables;
            strings += t.size();
            // Every fixup section must start inside the table and sections must be increasing.
            for (std::size_t i = 1; i < t.fixups().size(); ++i)
                if (t.fixups()[i] < t.fixups()[i - 1]) throw FormatError("string fixups not increasing");
            for (std::uint32_t kind = 0; kind + 1 < t.fixups().size(); ++kind)
                for (std::uint32_t i = t.fixups()[kind]; i < t.fixups()[kind + 1]; ++i, ++labels)
                    if (t.label(kind << 24 | (i - t.fixups()[kind])).data() != t.get(i).data())
                        throw FormatError("label hash does not resolve to its string");
        } catch (const std::exception& e) {
            fail(f.name, e);
        }
    }

    std::size_t bins = 0, font_bins = 0, menu_pages = 0, menu_controls = 0;
    std::map<std::string, std::size_t> menus;  // menu script entry name -> byte size (identical copies per name)
    for (const auto& f : files.files()) {
        if (f.name.size() <= 4 || !f.name.ends_with(".bin")) continue;
        try {
            auto data = files.read(f);
            auto entries = parse_bin_archive(Bytes(data));
            if (entries.empty()) continue;
            ++bins;
            SpriteLibrary sprites;
            for (const auto& e : entries) {
                if (is_map_chunk_file(e.type)) sprites.add(parse_map_chunk(e.data));
                if (e.type != EntryType::Menu) continue;
                MenuFile menu = parse_menu_file(e.data);
                auto [it, fresh] = menus.emplace(e.name, e.data.size());
                if (!fresh && it->second != e.data.size()) throw FormatError("menu script copies differ in size");
                menu_pages += menu.pages.size();
                for (const auto& p : menu.pages) menu_controls += p.controls.size();
            }
            // Every gameplay level (and the front end) carries the shared UI chunk with the fonts.
            if (sprites.find(fonts.fonts[0].texture_hash)) {
                ++font_bins;
                for (const auto& font : fonts.fonts)
                    if (!sprites.find(font.texture_hash)) throw FormatError("font texture missing");
                for (const auto& s : fonts.special) {
                    const Texture* t = sprites.find(s.texture_hash);
                    if (t && (t->width != s.tex_width || t->height != s.tex_height))
                        throw FormatError("button icon texture size differs from specialchar");
                }
            }
        } catch (const std::exception& e) {
            fail(f.name, e);
        }
    }
    // The front end carries 08000002; every gameplay level carries the same in-game script 08000001.
    if (menus.size() != 2 || !menus.count("08000001") || !menus.count("08000002")) {
        std::printf("FAIL expected menu scripts 08000001 and 08000002 on the disc, found %zu kinds\n", menus.size());
        ++failures;
    }
    std::printf("ui: %zu glyphs (%zu kerning pairs) in 3 fonts, %zu icons; %zu string tables, %zu strings, %zu labels; "
                "%zu bins (%zu with fonts); menu script: %zu pages, %zu controls; failures %zu\n",
                glyphs, kerns, fonts.special.size(), tables, strings, labels, bins, font_bins, menu_pages,
                menu_controls, failures);
    return failures;
}

}  // namespace nf

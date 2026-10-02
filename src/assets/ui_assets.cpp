#include "assets/ui_assets.hpp"

#include "assets/bin_archive.hpp"

namespace nf {

void add_level_sprites(SpriteLibrary& sprites, Bytes bin) {
    for (const auto& entry : parse_bin_archive(bin))
        if (is_map_chunk_file(entry.type)) sprites.add(parse_map_chunk(entry.data));
}

UiAssets load_ui_assets(const std::filesystem::path& gamedir, GameFiles& files, std::string_view language_file) {
    UiAssets ui;
    ui.fonts = load_fonts(Elf32(read_file(gamedir / "ACTION.ELF")));

    const GameFile* text = files.find(language_file);
    if (!text) throw FormatError("FILES.BIN has no " + std::string(language_file));
    auto text_data = files.read(*text);
    // The `...TxtU.dat` tables are the UTF-16 variants.
    bool wide = language_file.size() >= 5 && (language_file[language_file.size() - 5] | 0x20) == 'u';
    ui.strings = StringTable::parse(Bytes(text_data), wide);

    const GameFile* front = files.find(kFrontEndBin);
    if (!front) throw FormatError("FILES.BIN has no front end bin " + std::string(kFrontEndBin));
    auto bin = files.read(*front);
    add_level_sprites(ui.sprites, Bytes(bin));
    return ui;
}

}  // namespace nf

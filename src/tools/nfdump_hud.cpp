#include "tools/nfdump_hud.hpp"

#include <cstdio>
#include <exception>
#include <set>
#include <string>

#include "assets/bin_archive.hpp"
#include "assets/hud_data.hpp"
#include "assets/sprites.hpp"

namespace nf {

namespace {

// Level bins only carry the textures of the HUD panes their scripts can enable (the space pane's lamps
// are only in 0x0700001B, the camera pane's frames only where the camera gadget exists, ...). So the
// checks are: the panes HUD_Reset leaves enabled must be complete in every level bin, and every
// hash the HUD draws with must exist in some level bin.
bool always_enabled(HudPane slot) {
    switch (slot) {
        case HudPane::Ammo:
        case HudPane::Health:
        case HudPane::MissionStatus:
        case HudPane::ObjectiveStatus:
        case HudPane::InfoStatus:
        case HudPane::LensFlare:
        case HudPane::PickupStatus:
        case HudPane::MpScore:
        case HudPane::Radar:
            return true;
        default:
            return false;
    }
}

}  // namespace

std::size_t validate_hud(GameFiles& files, const std::filesystem::path& gamedir) {
    std::size_t failures = 0;
    HudData data;
    try {
        data = load_hud_data(Elf32(read_file(gamedir / "ACTION.ELF")));
    } catch (const std::exception& e) {
        std::printf("FAIL hud tables: %s\n", e.what());
        return 1;
    }

    std::size_t panes = 0, sprites = 0;
    for (const HudPaneList* list : {&data.single_player, &data.multi_player})
        for (const auto& p : *list)
            if (p) ++panes, sprites += p->sprites.size();

    std::set<std::uint32_t> seen;      // hashes found in at least one gameplay level
    std::size_t levels = 0, multiplayer_levels = 0;
    for (const auto& f : files.files()) {
        // Gameplay levels are the 0x0700xxxx bins; the shared HUD chunk carries the crosshair texture.
        if (f.name.size() != 12 || f.name.rfind("0700", 0) != 0 || !f.name.ends_with(".bin")) continue;
        try {
            auto bin = files.read(f);
            SpriteLibrary library;
            for (const auto& e : parse_bin_archive(Bytes(bin)))
                if (is_map_chunk_file(e.type)) library.add(parse_map_chunk(e.data));
            if (!library.find(data.crosshair.texture)) continue;  // test stubs without HUD chunk
            ++levels;
            // The multiplayer maps are the ones that carry the radar texture.
            const bool multiplayer = library.find(data.multi_player[std::size_t(HudPane::Radar)]->sprites[0].texture);
            multiplayer_levels += multiplayer;
            for (std::uint32_t hash : hud_texture_hashes(data))
                if (library.find(hash)) seen.insert(hash);

            const HudPaneList& list = multiplayer ? data.multi_player : data.single_player;
            for (std::size_t slot = 0; slot < kHudPaneCount; ++slot) {
                if (!list[slot]) continue;
                std::size_t have = 0, total = 0;
                for (const auto& s : list[slot]->sprites)
                    if (s.texture) ++total, have += library.find(s.texture) != nullptr;
                if (have != total && always_enabled(HudPane(slot))) {
                    std::printf("FAIL %s: %s pane has %zu of %zu textures\n", f.name.c_str(),
                                list[slot]->name.c_str(), have, total);
                    ++failures;
                }
            }
            if (!library.find(data.crosshair.texture)) {
                std::printf("FAIL %s: crosshair texture missing\n", f.name.c_str());
                ++failures;
            }
        } catch (const std::exception& e) {
            std::printf("FAIL %s: %s\n", f.name.c_str(), e.what());
            ++failures;
        }
    }
    for (std::uint32_t hash : hud_texture_hashes(data))
        if (!seen.count(hash) && !hud_sprites::is_unused_slot(hash)) {
            std::printf("FAIL HUD sprite %08x is in no level bin\n", hash);
            ++failures;
        }
    std::printf("hud: %zu panes / %zu sprite infos, %zu textures, %zu gameplay level bins (%zu multiplayer), %zu failures\n",
                panes, sprites, hud_texture_hashes(data).size(), levels, multiplayer_levels, failures);
    return failures;
}

}  // namespace nf

#include "tools/nfdump_weapons.hpp"

#include <cstdio>
#include <string>

#include "assets/elf.hpp"
#include "assets/strings.hpp"
#include "assets/weapon_data.hpp"

namespace nf {

namespace {

StringTable load_text(GameFiles& files) {
    const GameFile* f = files.find("USATxt.dat");
    if (!f) throw FormatError("FILES.BIN has no USATxt.dat");
    const auto data = files.read(*f);
    return StringTable::parse(Bytes(data), false);
}

}  // namespace

int cmd_weapons(GameFiles& files, const std::filesystem::path& gamedir, const std::vector<std::string>& args) {
    const Elf32 elf(read_file(gamedir / "ACTION.ELF"));
    const WeaponTable table = WeaponTable::from_elf(elf);
    const StringTable text = load_text(files);
    const int only = args.empty() ? -1 : std::atoi(args[0].c_str());
    std::printf("id  base sel alt cat  dmg   blast  cls   pel range  speed  spr int dly zoom ammo rps clip  name / mode\n");
    for (const WeaponDef& w : table.weapons()) {
        if (only >= 0 && w.id != only) continue;
        std::printf("%3u %4u %3u %3d %3u %6.2f %5.1f 0x%03x %3u %6.1f %6.2f %5.1f %3u %3u %4.1f %4u %3u %4d  %s / %s\n", w.id,
                    w.base, w.selectable, w.alt, w.category, w.damage, w.blast_radius, w.class_flags, w.pellets, w.range,
                    w.speed, w.spread, w.fire_interval, w.fire_delay, w.zoom_max, w.ammo_type, w.rounds_per_shot, w.clip_size,
                    std::string(text.label(w.name_label)).c_str(), std::string(text.label(w.mode_label)).c_str());
        if (only >= 0) {
            std::printf("flags1 %08x flags2 %08x flags3 %08x model %08x pickup %08x muzzle rgb %02x%02x%02x\n", w.flags1, w.flags2,
                        w.flags3, w.model_gfx, w.pickup_celglist, w.flash_r, w.flash_g, w.flash_b);
            std::printf("anims idle %08x reload %08x fire %08x fire_alt %08x draw %08x holster %08x aim %08x\n", w.anim_idle,
                        w.anim_reload, w.anim_fire, w.anim_fire_alt, w.anim_draw, w.anim_holster, w.anim_aim);
        }
    }
    if (only < 0) {
        std::printf("ammo: idx start max  name\n");
        for (int i = 0; i < WeaponTable::kAmmoCount; ++i) {
            const AmmoDef& a = table.ammo(i);
            std::printf("      %3d %5d %4d  %s\n", i, a.start, a.max, std::string(text.label(a.name_label)).c_str());
        }
    }
    return 0;
}

std::size_t validate_weapons(GameFiles& files, const std::filesystem::path& gamedir) {
    std::size_t failures = 0;
    const auto fail = [&](const std::string& what) {
        std::printf("FAIL weapons: %s\n", what.c_str());
        ++failures;
    };
    try {
        const Elf32 elf(read_file(gamedir / "ACTION.ELF"));
        const WeaponTable t = WeaponTable::from_elf(elf);
        const StringTable text = load_text(files);
        if (t.weapons().size() != WeaponTable::kWeaponCount) fail("row count");
        std::size_t named = 0;
        for (const WeaponDef& w : t.weapons()) {
            if (w.base >= WeaponTable::kWeaponCount) fail("base out of range for " + std::to_string(w.id));
            if (w.ammo_type >= WeaponTable::kAmmoCount) fail("ammo type out of range for " + std::to_string(w.id));
            if (w.selectable && text.label(w.name_label).empty()) fail("unnamed selectable weapon " + std::to_string(w.id));
            named += !text.label(w.name_label).empty();
        }
        // Values the spec's independent emulation of the same initializer produced (docs/spec-weapons.md 5.1).
        const WeaponDef& pp7 = t.weapon(2);
        if (pp7.damage != 3.5f || pp7.clip_size != 7 || pp7.ammo_type != 1 || pp7.fire_interval != 8 || pp7.range != 50.0f)
            fail("PP7 row differs from the spec dump");
        const WeaponDef& sniper = t.weapon(30);
        if (sniper.damage != 40.0f || sniper.zoom_max != 10.0f || sniper.clip_size != 5) fail("sniper row differs from the spec dump");
        const WeaponDef& mgl = t.weapon(42);
        if (mgl.blast_radius != 6.0f || mgl.damage != 75.0f) fail("MGL row differs from the spec dump");
        if (t.ammo(1).max != 70 || t.ammo(14).max != 4) fail("ammo_data differs from the spec dump");
        if (text.label(pp7.name_label) != "Wolfram PP7") fail("PP7 label resolves to '" + std::string(text.label(pp7.name_label)) + "'");
        if (t.upgrade(6) != 2 || t.upgrade(30) != 30) fail("Upgrade tables");
        if (t.best_weapons().size() != 18 || t.best_weapons()[0] != 66) fail("BestWeapon");
        std::printf("weapons: %zu rows, %zu named, table from the static initializer\n", t.weapons().size(), named);
    } catch (const std::exception& e) {
        fail(e.what());
    }
    std::printf("weapons failures %zu\n", failures);
    return failures;
}

}  // namespace nf

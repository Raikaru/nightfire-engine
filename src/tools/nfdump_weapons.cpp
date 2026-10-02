#include "tools/nfdump_weapons.hpp"

#include <cmath>
#include <cstdio>
#include <string>

#include "assets/elf.hpp"
#include "assets/strings.hpp"
#include "assets/weapon_data.hpp"
#include "game/arena_data.hpp"

namespace nf {

namespace {

// Direct-hit damage of one MP shot by zone, exactly as Player_HandlePain scales it (diff-mpweap proves
// the scaler bit-exact): part -1 (explosions) skips the x4 multi; head x4, limbs x0.8 on top of the multi.
// Splash weapons override the direct damage first (Bullet_update: 1.0 for ids 42/44-47, 0.0 for 43/52-55/58-64).
float zone_damage(const WeaponDef& w, float zone_mul) {
    float direct = w.damage;
    if (w.blast_radius > 0.0f) {
        if (w.id == 42 || (w.id >= 44 && w.id <= 47)) direct = 1.0f;
        if (w.id == 43 || (w.id >= 52 && w.id <= 55) || (w.id >= 58 && w.id <= 64)) direct = 0.0f;
    }
    return direct * 4.0f * zone_mul;
}

// Rounds of this weapon's direct torso hits to kill 100 hp unarmoured (0 damage -> never).
int shots_to_kill(const WeaponDef& w) {
    const float per = zone_damage(w, 1.0f);
    return per <= 0.0f ? -1 : int(std::ceil(100.0f / per));
}

bool in_mp_sets(const WeaponSets& sets, int id) {
    for (std::size_t r = 0; r < sets.matrix.size(); ++r) {
        if (r == std::size_t(WeaponSets::kRandomRow)) continue;   // rebuilt per match, not a fixed set
        for (std::int16_t slot : sets.matrix[r])
            if (slot == id) return true;
    }
    for (std::int16_t g : sets.useable)
        if (g == id) return true;
    return false;
}

void print_mp_table(const WeaponTable& table, const StringTable& text) {
    // Provenance: every value below is read from the emulated weapon_data static initializer
    // (`nfmips init --check` proves it byte-equal to the original; `nfdump validate` re-checks the spot
    // values). Damage zones apply the MP scaler (x4 + location) that `nfdump diff-mpweap` proves bit-exact;
    // intervals are 60 Hz frames (the firing state machine cools max(1, interval) frames per shot).
    const WeaponSets sets = WeaponSets::builtin();   // PickupMatrix + UseableGuns, validated vs ELF
    std::printf("# id mp name | mode | MP-set | int(frames,ms) | head torso limb | stk | pel | spread0deg grow | "
                "speed(u/s) | range | blast | clip/pool | notes\n");
    for (const WeaponDef& w : table.weapons()) {
        const float head = zone_damage(w, 4.0f), torso = zone_damage(w, 1.0f), limb = zone_damage(w, 0.8f);
        const int stk = shots_to_kill(w);
        const float spread_deg = (w.spread * 0.0014f) * 57.29578f;
        const int interval = std::max(1u, w.fire_interval);
        std::string notes;
        if ((w.id == 42 || (w.id >= 44 && w.id <= 47) || w.id == 43 || (w.id >= 52 && w.id <= 55) ||
             (w.id >= 58 && w.id <= 64)) && w.blast_radius > 0.0f)
            notes += "blast-direct-override ";
        if (w.id == 74 || w.id == 76) notes += "taser-gated ";
        if (w.pellets == 0) notes += "detonator/no-bullet ";
        if ((w.flags1 & 0x20000u) != 0u) notes += "lock-on ";
        if ((w.flags2 & 0x4u) != 0u) notes += "guided ";
        if (w.autoaim > 0.0f) notes += "autoaim ";
        if (w.selectable != 1) notes += "unselectable ";
        if (notes.empty()) notes = "-";
        else notes.pop_back();
        std::printf("%3u %-22.22s | %-14.14s | %s | %4u %7.1f | %7.1f %7.1f %7.1f | %3d | %3u | %8.2f %5.2f | "
                    "%9.1f | %7.1f | %5.1f | %4d/%4d | %s\n",
                    w.id, std::string(text.label(w.mp_name_label)).c_str(),
                    std::string(text.label(w.mode_label)).c_str(), in_mp_sets(sets, w.id) ? "MP" : "  ",
                    interval, interval * 1000.0f / 60.0f, head, torso, limb, stk, w.pellets, spread_deg,
                    w.spread_growth, w.speed * 60.0f, w.range, w.blast_radius, w.clip_size,
                    table.ammo(w.ammo_type).max, notes.c_str());
    }
}


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
    if (!args.empty() && args[0] == "mp-table") {
        print_mp_table(table, text);
        return 0;
    }
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
            // Id 1 (fists/unarmed melee) is selectable but legitimately unnamed (spec 5.1: empty name row).
            if (w.id != 1 && w.selectable && text.label(w.name_label).empty())
                fail("unnamed selectable weapon " + std::to_string(w.id));
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

#include "tools/nfdump_arena.hpp"

#include <cstdio>
#include <exception>
#include <map>

#include "assets/elf.hpp"
#include "assets/game_files.hpp"
#include "assets/level.hpp"
#include "assets/weapon_data.hpp"
#include "game/arena_data.hpp"

namespace nf {

namespace {

struct ArenaMap {
    const char* file;
    const char* name;
    int spawns0, spawns1;     // spec Part 1 §1.10 "spawn points (team0/team1)"
    int pickups;
};
constexpr ArenaMap kMaps[] = {
    {"07000024.bin", "Skyrail", 16, 16, 28},      {"07000027.bin", "Fort Knox", 15, 16, 19},
    {"07000029.bin", "Snow Blind", 16, 16, 20},  {"07000026.bin", "Phoenix Base", 16, 14, 18},
    {"07000023.bin", "Atlantis", 16, 16, 20},    {"07000028.bin", "Missile Silo", 16, 15, 17},
    {"07000025.bin", "Sub Pen", 16, 16, 24},     {"0700004b.bin", "Ravine", 16, 16, 28},
};

}  // namespace

namespace {

bool level_has_model(const Level& level, std::uint32_t hash) {
    for (const ChunkFile& c : level.chunks())
        for (const Model& m : c.chunk.models)
            if (m.hash != -1 && std::uint32_t(m.hash) == hash) return true;
    return false;
}

}  // namespace

int cmd_arena(GameFiles& files, const std::filesystem::path& gamedir, const std::vector<std::string>& args) {
    if (!args.empty() && args[0] == "models") {
        // Which weapon-pickup models each arena map can draw for every weapon set.
        const WeaponTable table = WeaponTable::from_elf(Elf32(read_file(gamedir / "ACTION.ELF")));
        const WeaponSets sets = WeaponSets::builtin();
        for (const ArenaMap& m : kMaps) {
            const GameFile* f = files.find(m.file);
            if (!f) continue;
            Level level(files.read(*f));
            std::printf("%s:", m.name);
            for (int r = 0; r < WeaponSets::kRandomRow; ++r) {
                std::printf(" set%d[", r);
                for (int k = 0; k < WeaponSets::kSlots; ++k) {
                    const int id = sets.matrix[std::size_t(r)][std::size_t(k)];
                    const WeaponDef& d = table.weapon(table.weapon(id).base);
                    std::printf("%s%d", level_has_model(level, d.pickup_celglist) ? "" : "!", id);
                    if (k < 4) std::printf(",");
                }
                std::printf("]");
            }
            std::printf("\n");
        }
        return 0;
    }
    for (const ArenaMap& m : kMaps) {
        if (!args.empty() && args[0] != m.file) continue;
        const GameFile* f = files.find(m.file);
        if (!f) continue;
        Level level(files.read(*f));
        const ArenaLevelData data = read_arena_level(level);
        std::printf("== %s %s: %zu spawns, %zu pickups, %zu objects\n", m.file, m.name, data.spawns.size(),
                    data.pickups.size(), data.objects.size());
        const auto& models = level.map()->chunk.models;
        const auto& statics = level.map()->chunk.statics;
        for (const PickupPlacement& p : data.pickups) {
            const StaticInstance& s = statics[p.instance];
            std::printf("  pickup #%zu cat %d item %d amt %d ch %d snd %d resp %d msg %08x model %s hash %d pos %.1f %.1f %.1f\n",
                        p.instance, p.category, p.item, p.amount, p.channel, p.sound, p.respawn_units, p.message,
                        s.hash == -1 && s.model_index < models.size() ? models[s.model_index].name.c_str() : "?", s.hash,
                        p.pos[0], p.pos[1], p.pos[2]);
        }
        for (const MpObjectPlacement& o : data.objects)
            std::printf("  object #%zu kind %d team %d label %d flags %d link %d pos %.1f %.1f %.1f\n", o.instance, o.kind,
                        o.team, o.label, o.flags, o.link, o.pos[0], o.pos[1], o.pos[2]);
    }
    return 0;
}

std::size_t validate_arena(GameFiles& files, const std::filesystem::path& gamedir) {
    std::size_t failures = 0;
    auto fail = [&](const std::string& what) {
        std::printf("arena: %s\n", what.c_str());
        ++failures;
    };
    try {
        const Elf32 elf(read_file(gamedir / "ACTION.ELF"));
        const WeaponSets from_elf = WeaponSets::from_elf(elf), built = WeaponSets::builtin();
        for (int r = 0; r < WeaponSets::kRandomRow; ++r)
            if (from_elf.matrix[std::size_t(r)] != built.matrix[std::size_t(r)]) fail("PickupMatrix row " + std::to_string(r) + " differs from ELF");
        if (from_elf.useable != built.useable) fail("UseableGuns differs from ELF");
    } catch (const std::exception& e) {
        fail(std::string("ACTION.ELF: ") + e.what());
    }
    std::size_t spawns = 0, pickups = 0;
    for (const ArenaMap& m : kMaps) {
        const GameFile* f = files.find(m.file);
        if (!f) {
            fail(std::string(m.file) + " missing");
            continue;
        }
        Level level(files.read(*f));
        const ArenaLevelData data = read_arena_level(level);
        int t0 = 0, t1 = 0;
        for (const auto& s : data.spawns) (s.team == 0 ? t0 : t1)++;
        if (t0 != m.spawns0 || t1 != m.spawns1)
            fail(std::string(m.name) + ": spawn markers " + std::to_string(t0) + "/" + std::to_string(t1));
        spawns += data.spawns.size();
        pickups += data.pickups.size();
    }
    std::printf("arena: %zu maps, %zu spawn markers, %zu pickup placements, failures %zu\n", std::size(kMaps), spawns, pickups,
                failures);
    return failures;
}

}  // namespace nf

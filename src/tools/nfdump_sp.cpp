#include "tools/nfdump_sp.hpp"

#include <algorithm>
#include <cstdio>
#include <exception>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "assets/character.hpp"
#include "assets/elf.hpp"
#include "assets/level.hpp"
#include "game/sp_placement.hpp"
#include "game/sp_tables.hpp"

namespace nf {

namespace {

using sp::BehaviourOp;

struct ExpectedType {
    int initial, alt;
};
// Spec §2.1 (DroneTypeSettings rows 0..30; rows 31..84 = {i + 165, 0}).
constexpr ExpectedType kExpectedTypes[31] = {
    {-1, 0},  {6, 0},   {41, 42},  {-1, 0},  {8, 0},     {10, 10},   {86, 0},   {133, 133}, {183, 183}, {16, 18},  {18, 18},
    {-1, 181}, {28, 28}, {166, 167}, {137, 137}, {0, 0},  {55, 86},   {4, 0},    {46, 0},   {48, 102},  {-1, 0},   {180, 180},
    {49, 0},  {50, 18}, {54, 0},   {42, 0},  {-1, 0},    {142, 0},   {142, 0},  {189, 0},   {195, 0}};

struct ExpectedMode {
    int dtype, side;
    float alertness;
};
// Spec §2.2 (DroneModeSettings rows 0..34).
constexpr ExpectedMode kExpectedModes[35] = {
    {0, 1, 0}, {1, 1, 0}, {1, 1, 1}, {2, 1, 0}, {1, 1, 0}, {6, 1, 0}, {7, 1, 1}, {3, 1, 0}, {4, 1, 0}, {5, 3, 0},
    {5, 3, 0}, {8, 3, 0}, {21, 1, 0}, {9, 3, 0}, {10, 3, 1}, {11, 1, 0}, {12, 2, 0}, {13, 1, 0}, {14, 1, 1}, {15, 1, 0},
    {16, 1, 0}, {17, 2, 0}, {18, 3, 0}, {19, 3, 0}, {20, 1, 0}, {22, 3, 0}, {23, 3, 0}, {24, 1, 0}, {24, 1, 0}, {25, 1, 1},
    {18, 3, 0}, {2, 1, 0}, {26, 3, 0}, {30, 2, 0}, {8, 3, 0}};

const char* kModeNames[35] = {"Normal", "Guard", "Retreater", "Sniper", "Stealth", "Attacker", "RunToPoint", "Assassin",
                              "HostageKiller", "Hostage", "HostageTied", "JustStand4Demo", "DeleteMe", "Civilian",
                              "CivilianScared", "MissionFailer", "Mayhew", "Ninja", "AlarmRaiser", "SearchLight", "Ambush",
                              "Zoe", "PartyGirl", "CivilianGuard", "Interogator", "CivDoorGuard", "TruckDriver",
                              "CastleChatGuard1", "CastleChatGuard2", "SniperAlert", "PartyGirlLooker", "(31)", "(32)",
                              "Bot", "(34)"};

struct ExpectedInit {
    int dmode;                        // DMODE row whose init function this is
    std::vector<BehaviourOp> ops;     // pseudocode (IDA) order; id -1 = call of NDrone2_init_DMODE_Defaults
};
// Ops of every NDrone2_init_DMODE_* function as read from the IDA pseudocode (behaviour_util_setProperty calls).
const std::vector<ExpectedInit>& expected_inits() {
    static const std::vector<ExpectedInit> v = {
        {0, {{-1, 0}}},   // Normal
        {1, {{-1, 0}}},   // Guard
        {2, {{-1, 0}}},   // Retreater
        {3, {{60, 1}, {31, 1}, {51, 1}, {50, 1}, {49, 1}, {67, 1}, {23, 1}, {57, 1}, {64, 1}, {90, 1}}},   // Sniper
        {4, {{-1, 0}, {37, 1}}},                                                                        // Stealth
        {5, {{-1, 0}}},                                                                                // Attacker
        {6, {{-1, 0}, {35, 1}, {88, 1}}},                                                              // RunToPoint
        {7, {{-1, 0}}},                                                                                // Assassin
        {8, {{60, 1}, {31, 1}, {51, 1}, {50, 1}, {49, 1}, {41, 1}, {40, 1}, {39, 1}, {38, 1}, {67, 1}, {64, 1}, {34, 1}}},
        {9, {{36, 1}, {60, 1}, {31, 1}, {64, 1}, {78, 1}}},                                            // Hostage
        {10, {{36, 1}, {60, 1}, {31, 1}, {64, 1}, {78, 1}}},                                           // HostageTied
        {11, {{64, 1}}},                                                                               // JustStand4Demo
        {13, {{29, 1}, {36, 1}, {60, 1}, {31, 1}, {41, 1}, {70, 1}, {50, 1}, {49, 1}, {39, 1}, {38, 1}, {59, 1}, {76, 1},
              {79, 1}, {80, 1}, {81, 1}, {83, 1}}},                                                     // Civilian
        {14, {{60, 1}, {31, 1}, {50, 1}, {49, 1}, {39, 1}, {38, 1}, {59, 1}, {76, 1}, {79, 1}, {80, 1}, {81, 1}, {83, 1}}},
        {15, {{60, 1}, {85, 1}}},                                                                      // MissionFailer
        {16, {{29, 1}}},                                                                               // Mayhew
        {18, {{-1, 0}, {35, 1}, {86, 1}}},                                                             // AlarmRaiser
        {19, {{-1, 0}, {64, 1}}},                                                                      // SearchLight
        {20, {{-1, 0}, {16, 1}}},                                                                      // Ambush
        {21, {{-1, 0}, {64, 1}, {33, 0}}},                                                             // Zoe
        {22, {{29, 1}, {36, 1}, {60, 1}, {31, 1}, {70, 1}, {38, 1}, {39, 1}, {40, 1}, {41, 1}, {81, 1}}},   // PartyGirl
        {23, {{29, 1}, {36, 1}, {60, 1}, {31, 1}, {70, 1}, {38, 1}, {39, 1}, {40, 1}, {41, 1}, {79, 1}, {80, 1}, {81, 1}}},
        {24, {{-1, 0}}},                                                                               // Interogator
        {25, {{29, 1}, {24, 1}, {6, 1}, {60, 1}, {31, 1}, {70, 1}, {38, 1}, {39, 1}, {40, 1}, {41, 1}, {79, 1}, {80, 1}, {81, 1}}},
        {26, {{29, 1}}},                                                                               // TruckDriver
        {27, {{-1, 0}}},                                                                               // CastleChatGuard1
        {28, {{-1, 0}, {29, 1}}},                                                                      // CastleChatGuard2
        {29, {{60, 1}, {31, 1}, {51, 1}, {50, 1}, {49, 1}, {67, 1}, {23, 1}, {57, 1}, {64, 1}}},        // SniperAlert
        {30, {{29, 1}, {36, 1}, {60, 1}, {31, 1}, {70, 1}, {71, 1}, {38, 1}, {39, 1}, {40, 1}, {41, 1}, {81, 1}}},
    };
    return v;
}
const std::vector<BehaviourOp>& expected_defaults() {
    static const std::vector<BehaviourOp> v = {
        {1, 1},  {6, 1},  {7, 1},  {10, 1}, {12, 1}, {72, 1}, {15, 1}, {17, 1}, {19, 1}, {24, 1}, {29, 1}, {76, 1}, {33, 1},
        {41, 1}, {46, 1}, {55, 1}, {65, 1}, {66, 1}, {79, 1}, {81, 1}, {90, 1}, {36, 1}, {60, 1}, {31, 1}, {68, 1}, {51, 1},
        {50, 1}, {49, 1}, {40, 1}, {39, 1}, {38, 1}, {48, 1}, {47, 1}, {42, 1}, {67, 1}, {59, 1}, {10, 1}, {12, 1}, {23, 1},
        {70, 1}};
    return v;
}

bool same_ops(const std::vector<BehaviourOp>& a, const std::vector<BehaviourOp>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].id != b[i].id || a[i].value != b[i].value) return false;
    return true;
}

// Checks the tables against the spec; returns the number of failures.
std::size_t validate_tables(const Elf32& elf, const sp::SpTables& t, bool verbose) {
    std::size_t failures = 0;
    auto fail = [&](const char* fmt, auto... args) {
        std::printf("  FAIL ");
        std::printf(fmt, args...);
        std::printf("\n");
        ++failures;
    };
    for (int i = 0; i < sp::kDtypeCount; ++i) {
        const auto& e = t.dtype[std::size_t(i)];
        const ExpectedType x = i <= 30 ? kExpectedTypes[i] : ExpectedType{i + 165, 0};
        if (e.initial_state != x.initial || e.alt_state != x.alt)
            fail("DTYPE %d: initial/alt %d/%d, spec %d/%d", i, e.initial_state, e.alt_state, x.initial, x.alt);
    }
    auto sym = [&](const char* name) {
        const auto s = elf.symbol(name);
        return s ? s->value : 0u;
    };
    for (int i = 0; i < sp::kDtypeCount; ++i) {
        std::uint32_t init = 0, ctl = 0;
        if (i == 2) init = sym("NDrone2_initDTYPE_Sniper__FP9Drone_tag");
        if (i == 25) init = sym("NDrone2_initDTYPE_SniperAlert__FP9Drone_tag");
        if (i == 30) init = sym("NDrone2_initDTYPE_BotInit__FP9Drone_tag");
        if (i == 13) ctl = sym("NDrone2_ControlDTYPE_Ninja__FP10DCVars_tag");
        if (i == 17) ctl = sym("NDrone2_ControlDTYPE_Zoe__FP10DCVars_tag");
        if (i == 29) ctl = sym("NDrone2_ControlDTYPE_Astronaut__FP10DCVars_tag");
        if (t.dtype[std::size_t(i)].init_fn != init || t.dtype[std::size_t(i)].control_fn != ctl)
            fail("DTYPE %d: init/control fn 0x%x/0x%x, expected 0x%x/0x%x", i, t.dtype[std::size_t(i)].init_fn,
                 t.dtype[std::size_t(i)].control_fn, init, ctl);
    }
    for (int i = 0; i < sp::kDmodeCount; ++i) {
        const auto& e = t.dmode[std::size_t(i)];
        const ExpectedMode& x = kExpectedModes[i];
        if (e.dtype != x.dtype || e.side != x.side || e.alertness != x.alertness)
            fail("DMODE %d: dtype/side/alertness %d/%d/%g, spec %d/%d/%g", i, e.dtype, e.side, double(e.alertness), x.dtype,
                 x.side, double(x.alertness));
        // init function symbol NDrone2_init_DMODE_<name>
        const std::string name = std::string("NDrone2_init_DMODE_") + kModeNames[i] + "__FP9Drone_tag";
        const auto s = elf.symbol(name);
        const std::uint32_t want = s ? s->value : 0;
        if (e.init_fn != want) fail("DMODE %d (%s): init fn 0x%x, symbol 0x%x", i, kModeNames[i], e.init_fn, want);
    }
    if (const int bad = sp::check_behaviour_layout(t)) fail("behaviour layout differs from bitDescs/bitMasks in %d ids", bad);
    const std::uint32_t widths[8] = {8, 3, 8, 8, 5, 8, 8, 1};
    for (std::size_t i = 0; i < 8; ++i)
        if (t.stat_widths[i] != widths[i]) fail("stat field width %zu = %u", i, t.stat_widths[i]);
    if (!same_ops(t.dmode_defaults, expected_defaults()))
        fail("DMODE_Defaults ops: decoded %zu, expected %zu", t.dmode_defaults.size(), expected_defaults().size());
    std::size_t checked = 0;
    for (const ExpectedInit& e : expected_inits()) {
        ++checked;
        if (!same_ops(t.dmode_init[std::size_t(e.dmode)], e.ops))
            fail("DMODE %d (%s) init program: decoded %zu ops, pseudocode %zu", e.dmode, kModeNames[e.dmode],
                 t.dmode_init[std::size_t(e.dmode)].size(), e.ops.size());
    }
    if (verbose)
        std::printf("  tables: %d DTYPE rows, %d DMODE rows, %zu init functions cross-checked with the pseudocode, "
                    "behaviour layout of %d ids verified, %zu failures\n",
                    sp::kDtypeCount, sp::kDmodeCount, checked, drone::Behaviour::kCount, failures);
    return failures;
}

struct LevelSummary {
    std::string name;
    std::uint32_t id = 0;
    sp::SpLevel data;
};

sp::ResolveEnv make_env(const sp::SpTables& t, std::uint32_t level, int difficulty) {
    sp::ResolveEnv env;
    env.level_id = level;
    env.difficulty = difficulty;
    env.tables = &t;
    env.frand = [](float range) { return range * 0.5f; };   // deterministic
    return env;
}

const char* dtype_name(int t) {
    static const char* names[] = {"Normal", "Guard", "Sniper", "Assassin", "HostageKiller", "Hostage", "Attacker",
                                  "RunToPoint", "JustStand", "Civilian", "CivilianScared", "MissionFailer", "Mayhew",
                                  "Ninja", "AlarmRaiser", "SearchLight", "Ambush", "Zoe", "PartyGirl", "CivilianGuard",
                                  "Interogator", "DeleteMe", "CivDoorGuard", "TruckDriver", "CastleChatGuard",
                                  "SniperAlert", "(26)", "Abseil1b", "Abseil1c", "Astronaut", "Bot"};
    return t >= 0 && t <= 30 ? names[t] : "?";
}

bool is_sp_bin(const GameFile& f) {
    if (!f.name.ends_with(".bin") || f.name.size() != 12) return false;
    return sp::is_sp_level(sp::level_id_from_bin(f.name));
}

}  // namespace

std::size_t validate_sp(GameFiles& files, const std::filesystem::path& gamedir) {
    std::size_t failures = 0;
    std::printf("single-player enemies:\n");
    Elf32 elf(read_file(gamedir / "ACTION.ELF"));
    sp::SpTables tables;
    try {
        tables = sp::SpTables::load(elf);
    } catch (const std::exception& e) {
        std::printf("  FAIL tables: %s\n", e.what());
        return 1;
    }
    failures += validate_tables(elf, tables, true);

    std::size_t levels = 0, npcs = 0, npcs_by_diff[4] = {0, 0, 0, 0}, cover = 0, points = 0, volumes = 0, spawners = 0,
                groups_orphan = 0, captains[4] = {0, 0, 0, 0}, resolved = 0, skins_checked = 0, skin_warnings = 0;
    std::set<std::uint32_t> all_skins;
    for (const GameFile& f : files.files()) {
        if (!is_sp_bin(f)) continue;
        try {
            const std::uint32_t id = sp::level_id_from_bin(f.name);
            Level level(files.read(f));
            const sp::SpLevel data = sp::parse_sp_level(level, id);
            auto bank = open_character_bank(files, f.name);
            ++levels;
            std::size_t level_npcs[4] = {0, 0, 0, 0}, cap[4] = {0, 0, 0, 0};
            std::set<std::uint32_t> group_ids;
            for (const auto& s : data.spawners) group_ids.insert(s.group);
            std::size_t template_npcs = 0;
            for (const sp::PlacedNpc& n : data.npcs) {
                all_skins.insert(n.spec.skin);
                // Missing skins are warnings, not failures: the shipped data contains placeholder-skinned
                // placements (0x5000090 polySurface1 / 0x50000b1 polySurface2, present in sibling levels'
                // banks but absent from 0700000c/07000014) and the engine spawns those drones skinless with
                // the AI fully active (DroneSystem::spawn leaves character null; locomotion falls back to the
                // AnimInfo forward step). Failing here would demand assets the disc never shipped.
                if (bank && !bank->skin(n.spec.skin)) {
                    std::printf("  WARN %s: NPC skin 0x%x does not resolve in the level's character bank\n", f.name.c_str(),
                                n.spec.skin);
                    ++skin_warnings;
                }
                ++skins_checked;
                if (n.spec.mode >= 0x24 && n.spec.mode != 0x64) {
                    std::printf("  FAIL %s: NPC mode 0x%x is neither a DMODE nor the shipped behaviour-blob mode 0x64\n",
                                f.name.c_str(), n.spec.mode);
                    ++failures;
                }
                if (n.group() != 0 && !group_ids.count(n.group())) ++groups_orphan;
                if (n.group() != 0 && group_ids.count(n.group())) ++template_npcs;
            }
            // every NPC must resolve at every difficulty (placement order: drone_stats rows persist over the level)
            for (int diff = 1; diff <= 3; ++diff) {
                sp::DroneStats stats;
                for (const sp::PlacedNpc& n : data.npcs) {
                    if (!data.placed_at(n, diff)) continue;
                    ++level_npcs[diff];
                    const sp::NpcResolved r = sp::resolve_npc(n.spec, make_env(tables, id, diff), stats);
                    ++resolved;
                    if (r.captain) ++cap[diff];
                    const bool bad = r.initial_state <= 0 || r.initial_state >= 250 || r.health <= 0 ||
                                     r.dtype >= sp::kDtypeCount || r.side < 1 || r.side > 3;
                    if (bad) {
                        std::printf("  FAIL %s: NPC static %u (skin 0x%x mode 0x%x) at difficulty %d resolves to state %d "
                                    "health %g dtype %d side %d skin 0x%x\n",
                                    f.name.c_str(), n.static_index, n.spec.skin, n.spec.mode, diff, r.initial_state,
                                    double(r.health), r.dtype, r.side, r.skin);
                        ++failures;
                    }
                }
            }
            npcs += data.npcs.size();
            for (int d = 1; d <= 3; ++d) npcs_by_diff[d] += level_npcs[d], captains[d] += cap[d];
            cover += data.cover_nodes.size();
            points += data.ai_points.size();
            volumes += data.volumes.size();
            spawners += data.spawners.size();
            std::printf("  %s (0x%x): %3zu NPCs (on easy/normal/hard %zu/%zu/%zu, captains %zu/%zu/%zu), %zu spawner-template NPCs, "
                        "%3zu cover nodes, %2zu AI points, %2zu spawners, %3zu AI volumes\n",
                        f.name.c_str(), id, data.npcs.size(), level_npcs[1], level_npcs[2], level_npcs[3], cap[1], cap[2],
                        cap[3], template_npcs, data.cover_nodes.size(), data.ai_points.size(), data.spawners.size(),
                        data.volumes.size());
        } catch (const std::exception& e) {
            std::printf("  FAIL %s: %s\n", f.name.c_str(), e.what());
            ++failures;
        }
    }
    for (std::uint32_t skin : all_skins) {
        const auto& known = sp::known_skins();
        if (!std::binary_search(known.begin(), known.end(), skin)) {
            std::printf("  FAIL placed skin 0x%x is not in the DefaultInit class switch\n", skin);
            ++failures;
        }
    }
    std::printf("  %zu SP levels: %zu NPCs (skins checked %zu with %zu missing-skin warnings, resolved %zu times at easy/normal/hard), %zu cover nodes, "
                "%zu AI points, %zu spawners, %zu AI volumes; %zu distinct skins; %zu NPCs in spawner-less groups; "
                "%zu failures\n",
                levels, npcs, skins_checked, skin_warnings, resolved, cover, points, spawners, volumes, all_skins.size(), groups_orphan,
                failures);
    return failures;
}

int dump_sp(GameFiles& files, const std::filesystem::path& gamedir, const std::string& level_bin) {
    Elf32 elf(read_file(gamedir / "ACTION.ELF"));
    const sp::SpTables tables = sp::SpTables::load(elf);
    for (const GameFile& f : files.files()) {
        if (!is_sp_bin(f) || (!level_bin.empty() && f.name != level_bin)) continue;
        const std::uint32_t id = sp::level_id_from_bin(f.name);
        Level level(files.read(f));
        const sp::SpLevel data = sp::parse_sp_level(level, id);
        std::printf("%s (0x%x): %zu NPCs\n", f.name.c_str(), id, data.npcs.size());
        sp::DroneStats stats;
        for (const sp::PlacedNpc& n : data.npcs) {
            const sp::NpcResolved r = sp::resolve_npc(n.spec, make_env(tables, id, 2), stats);
            std::printf("  #%-4u %6.1f %6.1f %6.1f  skin %08x class %02x/%02x  min-diff %u group %u  dtype %2d %-14s state %3d alt %3d  "
                        "hp %5.1f acc %u  sight %.0f/%.1f  script %08x%s\n",
                        n.static_index, double(n.spec.pos[0]), double(n.spec.pos[1]), double(n.spec.pos[2]), n.spec.skin,
                        r.char_class, r.sub_class, n.spec.min_difficulty, n.group(), r.dtype, dtype_name(r.dtype),
                        r.initial_state, r.alt_state, double(r.health), r.accuracy, double(r.sight_range),
                        double(r.sight_cone), n.spec.script, r.captain ? " CAPTAIN" : "");
        }
        for (const auto& s : data.spawners)
            std::printf("  spawner mode %u group %u max %u budget %u  ch act/stop/done/kill %u/%u/%u/%u  min-dist %.1f at %.1f %.1f %.1f\n",
                        s.mode, s.group, s.max_alive, s.budget, s.activate_channel, s.stop_channel, s.complete_channel,
                        s.kill_channel, double(s.min_distance), double(s.pos[0]), double(s.pos[1]), double(s.pos[2]));
    }
    return 0;
}

}  // namespace nf

// Host-side differentials against the EeInterp truth tables (see nfdump_diff.hpp).
#include "tools/nfdump_diff.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "assets/bin_archive.hpp"
#include "assets/character.hpp"
#include "assets/cutscene.hpp"
#include "assets/elf.hpp"
#include "assets/game_files.hpp"
#include "assets/level.hpp"
#include "assets/mission_data.hpp"
#include "game/actions.hpp"
#include "game/drone_cli.hpp"
#include "game/drone_system.hpp"
#include "game/drone_weap.hpp"
#include "game/mission.hpp"
#include "game/sp_common.hpp"
#include "game/sp_idle.hpp"
#include "game/weapons.hpp"
#include "game/world.hpp"

namespace {

std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == delim) {
            out.push_back(cur);
            cur.clear();
        } else cur += c;
    }
    out.push_back(cur);
    return out;
}

float bits_to_f32(std::uint32_t b) {
    float v;
    std::memcpy(&v, &b, 4);
    return v;
}

struct Harness {
    std::vector<std::uint8_t> bin;
    std::vector<std::uint8_t> elf_bytes;
    std::unique_ptr<nf::Level> level;
    std::unique_ptr<nf::CharacterBank> bank;
    std::unique_ptr<nf::WeaponSystem> owned_weapons;
    nf::WeaponSystem* weapons = nullptr;   // owned by the World after add_system
    std::unique_ptr<nf::World> world;
    std::string tuning_text;
};

std::unique_ptr<Harness> make_harness(nf::GameFiles& gf, const std::string& gamedir,
                                      const std::string& level_bin, bool need_bank) {
    auto h = std::make_unique<Harness>();
    std::string name = level_bin;
    h->bin = nf::read_level_bin(gf, name);
    if (h->bin.empty()) throw std::runtime_error("no such level .bin: " + level_bin);
    h->level = std::make_unique<nf::Level>(std::move(h->bin));
    if (!h->level->map()) throw std::runtime_error(level_bin + " has no Map entry");
    h->elf_bytes = nf::read_file(std::filesystem::path(gamedir) / "ACTION.ELF");
    const nf::Elf32 elf(h->elf_bytes);
    if (const nf::GameFile* tuning = gf.find("TuningVars.txt")) {
        const auto bytes = gf.read(*tuning);
        h->tuning_text.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
    const std::vector<nf::SpawnPoint> spawns = nf::find_spawn_points(*h->level);
    if (spawns.empty()) throw std::runtime_error(level_bin + " has no player start markers");
    nf::PlayerParams params;
    if (!h->tuning_text.empty()) params = nf::player_params_from_tuning(h->tuning_text, "");
    h->world = std::make_unique<nf::World>(*h->level, nf::InputTables::from_elf(elf), params);
    h->world->spawn_player(0, spawns.front());
    if (need_bank) {
        h->bank = nf::open_character_bank(gf, level_bin);
        auto ws = std::make_unique<nf::WeaponSystem>(nf::WeaponTable::from_elf(elf), params.health.damage,
                                                    1u);
        ws->set_bank(h->bank.get());
        h->weapons = ws.get();
        h->world->add_system(std::move(ws));
    }
    // Warm to frame 100: the EE harnesses poke GameState+52 (frame) = 100.
    nf::PadInputs pads{};
    for (int i = 0; i < 100; ++i) h->world->tick(pads);
    return h;
}

}  // namespace

// ---- DroneWeap_DoBulletAccuracy ----------------------------------------------------------------------
// CSV: acc,dist,moving,first_moved,first_stopped,sub,diff,level,seen,lost,phase,bx,by,bz,draw,
//      hit,ox,oy,oz,oxh,oyh,ozh (+ `#` comment lines and one header line).
int cmd_diff_acc(nf::GameFiles& gf, const std::string& gamedir, const std::string& csv_path,
                 const std::string& level_bin) {
    std::ifstream in(csv_path);
    if (!in) throw std::runtime_error("cannot open " + csv_path);
    auto h = make_harness(gf, gamedir, level_bin, false);
    // The EE harness never runs ReadTuningVars: every row uses the ELF .sdata defaults (see the CSV
    // header: Normal 1, Hard 2, ...), not the per-level TuningVars.txt section the game would load.
    const nf::drone::DroneTuning defaults;
    auto tuning_for = [&](std::uint32_t) -> const nf::drone::DroneTuning& { return defaults; };

    nf::drone::DroneConfig cfg;
    cfg.elf = nullptr;   // no anim data needed: accuracy touches no clips
    nf::drone::DroneSystem sys(*h->world, *nf::open_character_bank(gf, level_bin), cfg);
    nf::drone::Drone d;
    d.sys = &sys;
    d.opponent = nf::drone::TargetRef::player(0);

    long rows = 0, hit_bad = 0, off_bad = 0, off_checked = 0;
    double worst = 0;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        if (line.rfind("acc,", 0) == 0) continue;
        const std::vector<std::string> c = split(line, ',');
        if (c.size() != 22) throw std::runtime_error("bad diff-acc row: " + line);
        const int acc = std::atoi(c[0].c_str());
        const float dist = std::strtof(c[1].c_str(), nullptr);
        const int moving = std::atoi(c[2].c_str()), fm = std::atoi(c[3].c_str()), fs = std::atoi(c[4].c_str());
        const int sub = std::atoi(c[5].c_str()), diff = std::atoi(c[6].c_str());
        const std::uint32_t level = std::uint32_t(std::strtoul(c[7].c_str(), nullptr, 0));
        const int seen = std::atoi(c[8].c_str()), lost = std::atoi(c[9].c_str());
        const std::uint32_t phase = std::uint32_t(std::strtoul(c[10].c_str(), nullptr, 0));
        const float bx = std::strtof(c[11].c_str(), nullptr), by = std::strtof(c[12].c_str(), nullptr),
                    bz = std::strtof(c[13].c_str(), nullptr);
        const float draw = std::strtof(c[14].c_str(), nullptr);
        const int want_hit = std::atoi(c[15].c_str());
        const float ox = std::strtof(c[16].c_str(), nullptr), oy = std::strtof(c[17].c_str(), nullptr),
                    oz = std::strtof(c[18].c_str(), nullptr);

        sys.config().level_id = level;
        sys.config().difficulty = diff;
        sys.config().tuning = tuning_for(level);
        d.accuracy_class = acc;
        d.opp_dist = dist;
        d.opp_moving = moving != 0;
        d.opp_first_moved = fm != 0;
        d.opp_first_stopped = fs != 0;
        d.sub_class = sub;
        d.seen_frames = std::uint32_t(seen);
        d.lost_since_shot = lost != 0;
        d.rand_phase = phase;   // obj+0xec (Rand_Rand(10000)): the wobble phase source
        d.aim_euler = {bx, by, bz};
        const bool hit = nf::drone::weap::do_bullet_accuracy(d, draw);
        ++rows;
        if (hit != (want_hit != 0)) {
            if (hit_bad < 10)
                std::printf("diff-acc MISMATCH hit row %ld: acc=%d dist=%.3g moving=%d fm=%d fs=%d sub=%d diff=%d level=0x%x seen=%d lost=%d phase=%u draw=%.3g want=%d got=%d\n",
                            rows, acc, double(dist), moving, fm, fs, sub, diff, level, seen, lost, phase,
                            double(draw), want_hit, int(hit));
            ++hit_bad;
            continue;
        }
        if (!hit) {
            ++off_checked;
            const double ex = std::fabs(double(d.aim_offset[0]) - double(ox)),
                         ey = std::fabs(double(d.aim_offset[1]) - double(oy)),
                         ez = std::fabs(double(d.aim_offset[2]) - double(oz));
            const double e = std::max(ex, std::max(ey, ez));
            worst = std::max(worst, e);
            if (e > 1e-5) {
                if (off_bad < 10)
                    std::printf("diff-acc OFFSET row %ld err=%.3g: got %.7g %.7g %.7g want %.7g %.7g %.7g\n", rows, e,
                                double(d.aim_offset[0]), double(d.aim_offset[1]), double(d.aim_offset[2]),
                                double(ox), double(oy), double(oz));
                ++off_bad;
            }
        }
    }
    std::printf("diff-acc: %ld rows, hit mismatches %ld, offsets checked %ld over 1e-5 %ld (worst %.3g)\n", rows,
                hit_bad, off_checked, off_bad, worst);
    return (hit_bad == 0 && off_bad == 0) ? 0 : 1;
}

// ---- NDrone2_ReFindMissionPath -------------------------------------------------------------------------
// CSV: count,flags,d8,movetest,nodes,fc,mg,lk,dp,ret,ndx,cel,goal,dist,rate,move,ang,aipoint,sm,mtg,dest,nav.
// Decision-level comparison against sp::choose_mission_node: ret==1 iff a reachable node exists (found),
// ndx (+0x9f0) iff the selected array position, AIPoint words 8-10/5 iff goal pos/radius. The +0x9d0 goal /
// dist / rate / move / ang machinery (SetupGoalPosition for the follow-up MoveToGoal walk) has no port
// counterpart by design: sm/mtg only check the branch direction (move path = mtg logged, never SetState;
// setup path with verdict 0-3 = SetState(99,0)).
int cmd_diff_refind(const std::string& csv_path) {
    std::ifstream in(csv_path);
    if (!in) throw std::runtime_error("cannot open " + csv_path);
    const float line[4][3] = {{10, 0, 10}, {20, 0, 20}, {30, 0, 30}, {40, 0, 40}};
    const float scat[4][3] = {{10, 0, 40}, {35, 0, 12}, {8, 0, 30}, {50, 0, 50}};

    long rows = 0, dir_bad = 0, pos_bad = 0, ndx_bad = 0, rad_bad = 0, calls_bad = 0;
    std::string raw;
    while (std::getline(in, raw)) {
        if (raw.empty() || raw[0] == '#') continue;
        if (raw.rfind("count,", 0) == 0) continue;
        const std::vector<std::string> c = split(raw, ',');
        if (c.size() < 22) throw std::runtime_error("bad diff-refind row: " + raw);
        // sm=setstate(99,0) embeds a comma: rejoin a possible split, index mtg/dest/nav from the back.
        const std::string nav = c.back(), dest = c[c.size() - 2], mtg = c[c.size() - 3];
        std::string sm = c[18];
        for (std::size_t k = 19; k + 3 < c.size(); ++k) sm += "," + c[k];
        const int count = std::atoi(c[0].c_str());
        const std::uint32_t flags = std::uint32_t(std::strtoul(c[1].c_str(), nullptr, 0));
        const std::string& mt = c[3];
        const int ni = std::atoi(c[4].c_str());
        const int mg = std::atoi(split(c[6], '=').at(1).c_str());
        const int ret = std::atoi(split(c[9], '=').at(1).c_str());
        const std::uint32_t ndx = std::uint32_t(std::strtoul(split(c[10], '=').at(1).c_str(), nullptr, 0));
        const std::vector<std::string> ap = split(c[17], ' ');
        if (ap.size() != 16) throw std::runtime_error("bad aipoint: " + raw);
        auto w = [&](int i) { return std::uint32_t(std::strtoul(ap[std::size_t(i)].c_str(), nullptr, 16)); };
        const float want_r = bits_to_f32(w(5));
        const float want_x = bits_to_f32(w(8)), want_z = bits_to_f32(w(10));
        (void)dest;
        (void)nav;
        ++rows;

        std::vector<nf::Vec3> nodes;
        for (int k = 0; k < count; ++k) {
            const float* p = (ni == 0 ? line : scat)[k];
            nodes.push_back({p[0], p[1], p[2]});
        }
        const nf::Vec3 feet{0, 0, 0};
        const bool stationary = (flags & 0x10u) != 0;
        const nf::Vec3 opp{10, 20, 30};   // EE player CelPos for the count-0 / fallthrough fallback
        int calls = 0;
        nf::sp::RefindChoice ch = nf::sp::choose_mission_node(
            nodes, feet, stationary, true, opp, [](const nf::Vec3&) { return 7; },
            [&](const nf::CelPos&, const nf::CelPos&) {
                ++calls;
                if (mt == "all1") return 1;
                if (mt == "all0") return 0;
                return (calls % 2 == 1) ? 1 : 0;   // alt: 1,0,1,0... (counter reset per row)
            },
            [&](const nf::Vec3&, float) { return mg; });   // scripted MoveToGoal verdict

        const bool want_found = ret == 1;
        const bool want_mtg = count > 0;
        const bool want_selected = count > 0 && (want_found || (mg >= 0 && mg <= 3));
        const bool want_goto = !want_found && count > 0 && mg >= 0 && mg <= 3;
        if (ch.found != want_found) {
            if (dir_bad < 10) std::printf("diff-refind DIRECTION row %ld: %s want %s (count=%d flags=0x%x %s ni=%d mg=%d sm=%s mtg=%s)\n", rows, ch.found ? "found" : "setup", want_found ? "move" : "setup", count, flags, mt.c_str(), ni, mg, sm.c_str(), mtg.c_str());
            ++dir_bad;
            continue;
        }
        if (ch.selected != want_selected) {
            if (ndx_bad < 10) std::printf("diff-refind SELECT row %ld: %s want %s (ndx=%u mg=%d)\n", rows, ch.selected ? "selected" : "none", want_selected ? "selected" : "none", ndx, mg);
            ++ndx_bad;
        }
        if (ch.goto_goal_state != want_goto) {
            if (dir_bad < 10) std::printf("diff-refind GOTO row %ld: %s want %s (sm=%s)\n", rows, ch.goto_goal_state ? "goto" : "none", want_goto ? "goto" : "none", sm.c_str());
            ++dir_bad;
        }
        if (ch.move_goal_called != want_mtg) {
            if (calls_bad < 5) std::printf("diff-refind MTGCALL row %ld: %s want %s\n", rows, ch.move_goal_called ? "called" : "none", want_mtg ? "called" : "none");
            ++calls_bad;
        }
        if (stationary || count == 0) {
            if (calls != 0) {
                if (calls_bad < 5) std::printf("diff-refind CALLS row %ld: want 0 move_test calls, got %d\n", rows, calls);
                ++calls_bad;
            }
        } else if (calls != count) {
            if (calls_bad < 5) std::printf("diff-refind CALLS row %ld: want %d move_test calls, got %d\n", rows, count, calls);
            ++calls_bad;
        }
        if (!ch.has_choice) {
            if (dir_bad < 5) std::printf("diff-refind NOCHOICE row %ld (count=%d mg=%d)\n", rows, count, mg);
            ++dir_bad;
            continue;
        }
        if (ch.selected && ch.position != ndx) {
            if (ndx_bad < 10) std::printf("diff-refind NDX row %ld: want %u got %zu (count=%d flags=0x%x %s ni=%d mg=%d)\n", rows, ndx, ch.position, count, flags, mt.c_str(), ni, mg);
            ++ndx_bad;
        }
        if (ch.pos[0] != want_x || ch.pos[1] != bits_to_f32(w(9)) || ch.pos[2] != want_z) {
            if (pos_bad < 10) std::printf("diff-refind POS row %ld: want %.3g/%.3g/%.3g got %.3g/%.3g/%.3g\n", rows, double(want_x), double(bits_to_f32(w(9))), double(want_z), double(ch.pos[0]), double(ch.pos[1]), double(ch.pos[2]));
            ++pos_bad;
        }
        if (ch.radius != want_r) {
            if (rad_bad < 10) std::printf("diff-refind RADIUS row %ld: want %.3g got %.3g\n", rows, double(want_r), double(ch.radius));
            ++rad_bad;
        }
    }
    std::printf("diff-refind: %ld rows, direction %ld, ndx %ld, pos %ld, radius %ld, calls %ld\n", rows, dir_bad,
                ndx_bad, pos_bad, rad_bad, calls_bad);
    return (dir_bad == 0 && pos_bad == 0 && ndx_bad == 0 && rad_bad == 0 && calls_bad == 0) ? 0 : 1;
}

// ---- scripted (coder) spawns ----------------------------------------------------------------------------
// Plays a cutscene script through the real MissionSystem, drains take_spawns into
// SpSystem::spawn_scripted via DroneCli::drain_coder_spawns after every tick, and reports new drones.
int cmd_coder_spawn(nf::GameFiles& gf, const std::string& gamedir, const std::string& level_bin,
                    std::uint32_t script_hash, long frames) {
    auto h = make_harness(gf, gamedir, level_bin, true);
    const nf::Elf32 elf(h->elf_bytes);
    const std::uint32_t level_id = nf::level_id_from_bin_name(level_bin);
    nf::drone::DroneCli cli;
    char a0[] = "nfdump", a1[] = "--sp";
    char* av[] = {a0, a1};
    int idx = 1;
    cli.parse(2, av, idx);
    cli.setup(*h->world, *h->level, *h->bank, elf, gf, *h->weapons, level_bin);
    nf::sp::SpSystem* sp = cli.sp_system();
    if (!sp) throw std::runtime_error("no SP layer");
    const std::size_t before = sp->sp_drones().size();

    std::string bin_name = level_bin;
    const std::vector<std::uint8_t> bin = nf::read_level_bin(gf, bin_name);
    std::vector<std::pair<std::uint32_t, nf::CutsceneBin>> scripts;
    for (const nf::BinEntry& e : nf::parse_bin_archive(nf::Bytes(bin))) {
        if (e.type != nf::EntryType::Script) continue;
        scripts.emplace_back(e.hash, nf::parse_cutscene_bin(e.data));
    }
    nf::MissionSystem* mission = nullptr;
    for (const nf::MissionEntry& e : nf::load_mission_data(elf)) {
        if (e.level != level_id) continue;
        auto m = std::make_unique<nf::MissionSystem>(*h->level, level_id, e, *h->weapons, sp);
        mission = m.get();
        mission->add_scripts(std::move(scripts));
        mission->apply_loadout(0);
        h->world->add_system(std::move(m));
        break;
    }
    if (!mission) throw std::runtime_error("no mission row for " + level_bin);
    mission->play_nis(script_hash);
    nf::PadInputs pads{};
    long spawned = 0;
    for (long i = 0; i < frames; ++i) {
        h->world->tick(pads);
        for (int id : cli.drain_coder_spawns(*mission)) {
            std::printf("coder-spawn: drone %d live at frame %ld\n", id, i);
            ++spawned;
        }
    }
    const std::size_t after = sp->sp_drones().size();
    std::printf("coder-spawn: %08x on %s: %ld scripted spawns (%zu -> %zu SP drones)\n", script_hash,
                level_bin.c_str(), spawned, before, after);
    return spawned > 0 ? 0 : 1;
}

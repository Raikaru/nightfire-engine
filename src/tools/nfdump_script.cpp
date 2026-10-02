#include "tools/nfdump_script.hpp"

#include <cstdio>
#include <functional>
#include <map>
#include <optional>
#include <set>

#include "assets/cutscene.hpp"
#include "assets/elf.hpp"
#include "assets/level.hpp"
#include "assets/mission_data.hpp"
#include "assets/nav_data.hpp"
#include "assets/reader.hpp"
#include "assets/strings.hpp"
#include "game/objects.hpp"
#include "game/sp_common.hpp"

namespace nf {

namespace {

// Every `case` of `parsemap_create_dynamic_objects` (plus 0x8000 world cels and the sky class):
// a static with any other class is genuinely unknown data.
const std::set<std::uint32_t>& known_classes() {
    static const std::set<std::uint32_t> classes = {
        15, 32, 33, 34, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53,
        54, 55, 56, 57, 58, 59, 62, 63, 65, 74, 78, 113, 210, 212, 213, 216, 217, 218, 219, 220, 222,
        223, 224, 225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 235, 236, 237, 238, 239, 240, 241,
        242, 243, 244, 245, 248, 249, 250, 251, 252, 253, 254, 0x2A, 0x8000};
    return classes;
}

StringTable load_strings(GameFiles& files) {
    const GameFile* f = files.find("USATxt.dat");
    if (!f) throw FormatError("FILES.BIN has no USATxt.dat");
    const auto data = files.read(*f);
    return StringTable::parse(Bytes(data), false);
}

std::string bin_for_level(GameFiles& files, std::uint32_t level) {
    char name[16];
    std::snprintf(name, sizeof(name), "%08x.bin", level);
    return files.find(name) ? name : std::string();
}

}  // namespace

std::size_t validate_script(GameFiles& files, const std::filesystem::path& gamedir) {
    std::size_t failures = 0, notes = 0, levels = 0, scripts = 0, script_cmds = 0;
    const Elf32 elf(read_file(gamedir / "ACTION.ELF"));
    const StringTable strings = load_strings(files);
    auto label_ok = [&](std::uint32_t label) { return !strings.label(label).empty(); };
    // ---- mission tables ----
    std::vector<MissionEntry> missions;
    try {
        missions = load_mission_data(elf);
    } catch (const std::exception& e) {
        std::printf("script: MissionData: %s\n", e.what());
        return 1;
    }
    std::printf("script: %zu mission rows\n", missions.size());
    for (const MissionEntry& m : missions) {
        const std::string bin = bin_for_level(files, m.level);
        if (bin.empty()) {  // cut rows (0e/0f/10: not in sp_level, not on disc)
            std::printf("script: mission %08x: no level bin (cut content)\n", m.level);
            ++notes;
            continue;
        }
        if (m.objectives.empty()) {
            std::printf("script: mission %08x: no objectives\n", m.level);
            ++failures;
        }
        for (std::size_t i = 0; i < m.objectives.size(); ++i) {
            const MissionObjective& o = m.objectives[i];
            if (!label_ok(o.label)) {
                std::printf("script: mission %08x obj %zu: label %08x does not resolve\n", m.level, i, o.label);
                ++failures;
            }
            if (o.fail_label != 0xFFFFFFFF && !label_ok(o.fail_label)) {
                std::printf("script: mission %08x obj %zu: fail label %08x does not resolve\n", m.level, i,
                            o.fail_label);
                ++failures;
            }
        }
    }
    try {
        const auto order = load_sp_level_order(elf);
        std::printf("script: %zu sp_level rows, first ACTION %08x\n", order.size(), first_action_level(order));
        for (const SpLevelRow& r : order) {
            if ((r.level & 0x0F000000) != 0x07000000) continue;  // DRIVING.ELF owns 0x09 levels
            if (bin_for_level(files, r.level).empty()) {
                std::printf("script: sp_level %08x: no level bin\n", r.level);
                ++failures;
            }
        }
    } catch (const std::exception& e) {
        std::printf("script: sp_level: %s\n", e.what());
        ++failures;
    }
    std::map<std::uint32_t, std::size_t> class_totals;
    std::map<std::uint32_t, std::size_t> ignored_classes;
    for (const GameFile& f : files.files()) {
        std::vector<std::uint8_t> bin;
        try {
            bin = files.read(f);
        } catch (...) {
            continue;
        }
        std::vector<BinEntry> entries;
        try {
            entries = parse_bin_archive(Bytes(bin));
        } catch (...) {
            continue;
        }
        std::set<std::uint32_t> script_hashes;
        const std::function<bool(std::uint32_t)> label_fn = label_ok;
        for (const auto& e : entries) {
            if (e.type != EntryType::Script) continue;
            ++scripts;
            script_hashes.insert(e.hash);
            const auto issues = check_cutscene_bin(e.data, &label_fn);
            for (const auto& is : issues) {
                std::printf("script: %s %s: %s\n", f.name.c_str(), e.name.c_str(), is.c_str());
                ++failures;
            }
            try {
                const CutsceneBin b = parse_cutscene_bin(e.data);
                for (const CutsceneScript& s : b.scripts) {
                    const std::vector<ScriptCommand> cmds = decode_stream(s);
                    script_cmds += cmds.size();
                    for (const ScriptCommand& c : cmds) {
                        // CutsceneOp::Event(18) with event id 8 = Drone_CoderCreate (scripted drone spawn).
                        if (c.op == 18 && c.payload.size() > 1 && c.payload[1] == 8)
                            std::printf("script: %s %s: coder-spawn event at t=%.1f\n", f.name.c_str(),
                                        e.name.c_str(), double(c.time));
                    }
                }
            } catch (const std::exception& e2) {
                std::printf("script: %s %s: decode: %s\n", f.name.c_str(), e.name.c_str(), e2.what());
                ++failures;
            }
        }
        std::optional<Level> level;
        try {
            level.emplace(std::move(bin));
        } catch (const std::exception& e) {
            std::printf("script: %s: level: %s\n", f.name.c_str(), e.what());
            ++failures;
            continue;
        }
        if (!level->map()) continue;
        ++levels;
        const MapChunk& map = level->map()->chunk;
        // Census; classes outside the parsemap switch are ignored by the original's default
        // case (dead editor data like the origin-placed 0x2031 statics), aggregated below.
        for (std::size_t i = 0; i < level->placements().size(); ++i) {
            const Placement& p = level->placements()[i];
            if (p.instance >= map.statics.size()) continue;
            const StaticInstance& s = map.statics[p.instance];
            const std::uint32_t cls = s.object_class();
            if (cls < 256) class_totals[cls]++;
            if ((s.flags & 0x8000) != 0) continue;
            if (!known_classes().count(cls) && cls != 0x2A) ignored_classes[cls]++;
        }
        // Dynamic objects build cleanly; SP/movie refs resolve or are noted.
        sp::SwitchChannels channels;
        SpObjects objects(*level, level_id_from_bin_name(f.name), channels);
        try {
            objects.build();
        } catch (const std::exception& e) {
            std::printf("script: %s: objects: %s\n", f.name.c_str(), e.what());
            ++failures;
            continue;
        }
        for (const auto& a : objects.script_players()) {
            if (a.hash != 0 && !script_hashes.count(a.hash)) {
                std::printf("script: %s: SP script %08x not in the level bin (assumed streaming)\n", f.name.c_str(),
                            a.hash);
                ++notes;
            }
            if (a.hash != 0)
                std::printf("script: %s: anchor %08x auto=%d trigger_ch=%u placement=%zu\n", f.name.c_str(), a.hash,
                            int(a.auto_play), a.trigger_channel, a.placement);
        }
        // LoadLevel destinations exist; channel params fit in u8.
        SpObjects::Census census = objects.census();
        for (const auto& t : objects.script_players()) (void)t;
        for (std::size_t i = 0; i < level->placements().size(); ++i) {
            const Placement& p = level->placements()[i];
            if (p.instance >= map.statics.size()) continue;
            const StaticInstance& s = map.statics[p.instance];
            if ((s.flags & 0x8000) != 0) continue;
            const std::uint32_t cls = s.object_class();
            if (cls == 232) {
                // Destinations with 0x300000 set are end-of-mission exits (results screen for the
                // base id, like `Mission_Update`'s fail LevelToEndTo); the rest load the bin.
                const std::uint32_t dest = s.param(0);
                char name[16];
                std::snprintf(name, sizeof(name), "%08x.bin", dest & ~std::uint32_t(0x00300000));
                if (!files.find(name)) {
                    std::printf("script: %s placement %zu: LoadLevel destination %s missing\n", f.name.c_str(), i,
                                name);
                    ++failures;
                }
            }
            // Channel params are u8s; checked per class with the decoded param maps.
            auto channel_bad = [&](std::initializer_list<std::int32_t> keys) {
                for (std::int32_t k : keys) {
                    if (s.param(k, 0) > 255) {
                        std::printf("script: %s placement %zu: channel param %d = %u out of range\n",
                                    f.name.c_str(), i, k, s.param(k, 0));
                        ++failures;
                    }
                }
            };
            switch (cls) {
            case 219:
                channel_bad({2, 3});
                break;  // door unlock/lock
            case 220:
                channel_bad({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10});
                break;  // trigger type/out/inputs/gate
            case 234:
            case 235:
                channel_bad({0});
                break;  // touch out
            case 236:
            case 237:
            case 238:
            case 239:
                channel_bad({0, 1, 2, 3, 4, 5, 6, 7, 8});
                break;  // multiplex out/inputs
            case 41:
                channel_bad({0, 5});
                break;  // switch channel/gate (p1 = lever script)
            case 226:
                channel_bad({0, 3});
                break;  // SS out/value
            case 40:
                channel_bad({3, 4});
                break;  // sensor alarm/gate
            case 222:
                channel_bad({1});
                break;  // searchlight alarm
            case 46:
            case 48:
                channel_bad({0, 1});
                break;  // lock/monitor gate/out
            case 49:
                channel_bad({1, 2, 3, 4});
                break;  // fusebox channels (p0 = spark script)
            case 51:
                channel_bad({3});
                break;  // hint gate
            case 249:
                channel_bad({0, 4});
                break;  // sound trigger channels
            case 251:
                channel_bad({0, 1});
                break;  // music trigger channels
            case 225:
                channel_bad({1});
                break;  // destroy gate
            default:
                break;
            }
            // Lever/spark scripts resolve like SP scripts.
            if (cls == 41 || cls == 49) {
                const std::uint32_t h = s.param(cls == 41 ? 1 : 0, 0);
                if ((h & 0xFF000000) == 0x06000000 && !script_hashes.count(h)) {
                    std::printf("script: %s placement %zu: object script %08x not in the level bin\n",
                                f.name.c_str(), i, h);
                    ++notes;
                }
            }
        }
        // Door path refs land on real tracks.
        const std::vector<PathTrack> tracks = parse_path_data(map);
        for (const StaticPathRef& r : static_path_refs(map)) {
            if (r.count != 0 && r.path_index >= tracks.size()) {
                std::printf("script: %s: path ref %u out of range (%zu tracks)\n", f.name.c_str(), r.path_index,
                            tracks.size());
                ++failures;
            }
        }
    }
    std::printf("script: %zu levels, %zu script entries, %zu commands, class census:", levels, scripts, script_cmds);
    for (const auto& [cls, n] : class_totals) std::printf(" %u:%zu", cls, n);
    std::printf("\nscript: ignored classes (original default-case):");
    for (const auto& [cls, n] : ignored_classes) {
        std::printf(" %u:%zu", cls, n);
        notes += n;
    }
    std::printf("\nscript: %zu failures, %zu notes\n", failures, notes);
    return failures;
}

int dump_script(GameFiles& files, const std::filesystem::path& gamedir, const std::string& level_bin) {
    const Elf32 elf(read_file(gamedir / "ACTION.ELF"));
    const StringTable strings = load_strings(files);
    const std::uint32_t level = level_id_from_bin_name(level_bin.empty() ? "07000005.bin" : level_bin);
    for (const MissionEntry& m : load_mission_data(elf)) {
        if (m.level != level) continue;
        std::printf("mission %08x base %08x order %u profile %04x unlock %u\n", m.level, m.base, m.order, m.profile,
                    m.unlock);
        for (std::size_t i = 0; i < m.objectives.size(); ++i) {
            const MissionObjective& o = m.objectives[i];
            std::printf("  obj %zu label %08x '%s' fail %08x ch %u init %u ch2 %u flags %u\n", i, o.label,
                        std::string(strings.label(o.label)).c_str(), o.fail_label, o.channel, o.init, o.channel2,
                        o.flags);
        }
    }
    std::string name = level_bin.empty() ? "07000005.bin" : level_bin;
    const GameFile* f = files.find(name);
    if (!f) {
        std::printf("no such bin\n");
        return 1;
    }
    std::vector<std::uint8_t> bin = files.read(*f);
    Level lvl(std::move(bin));
    sp::SwitchChannels channels;
    SpObjects objects(lvl, level, channels);
    objects.build();
    SpObjects::Census census = objects.census();
    for (std::size_t c = 0; c < 256; ++c)
        if (census.per_class[c] != 0) std::printf("class %zu: %zu placements\n", c, census.per_class[c]);
    for (const DoorObject& d : objects.doors()) {
        const Placement& p = lvl.placements()[d.placement];
        std::printf("door placement %zu unlock %u lock %u auto %d open_sfx %u close_sfx %u pos %.1f,%.1f,%.1f\n",
                    d.placement, d.unlock_channel, d.lock_channel, int(d.auto_door), d.open_sound, d.close_sound,
                    double(p.transform[12]), double(p.transform[13]), double(p.transform[14]));
    }
    std::vector<std::uint8_t> raw = files.read(*f);
    const auto entries = parse_bin_archive(Bytes(raw));
    for (const auto& e : entries) {
        if (e.type != EntryType::Script) continue;
        const CutsceneBin b = parse_cutscene_bin(e.data);
        std::printf("script %s: %zu streams, %zu keys\n", e.name.c_str(), b.scripts.size(), b.keys.size() / 48);
    }
    return 0;
}

}  // namespace nf

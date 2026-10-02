#include "driving/mission_data.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>

namespace nf::driving {
namespace {

float f32(Bytes b, std::size_t o) {
    float f;
    std::memcpy(&f, &b[o], 4);
    return f;
}
std::uint32_t u32(Bytes b, std::size_t o) {
    std::uint32_t v;
    std::memcpy(&v, &b[o], 4);
    return v;
}
std::uint16_t u16(Bytes b, std::size_t o) {
    std::uint16_t v;
    std::memcpy(&v, &b[o], 2);
    return v;
}

// Yaw facing from orthonormal basis rows (X = right, Z = forward): forward = (m8, m10).
float yaw_of(Bytes b, std::size_t o) {
    const float fx = f32(b, o + 8), fz = f32(b, o + 10);
    if (fx == 0 && fz == 0) return 0;
    return std::atan2(fx, fz);
}

std::string printable_run(Bytes b, std::size_t o, std::size_t n) {
    std::string s;
    for (std::size_t k = 0; k < n; ++k) {
        const char ch = static_cast<char>(b[o + k]);
        if (ch == 0) break;
        if (ch < 32 || ch >= 127) return "";
        s += ch;
    }
    return s;
}

const char* power_kind(const std::string& article) {
    // Smackable articles (attrib.dir): PowerUpMissiles, PowerUpOilSlick, PowerUpSmokescreen,
    // PowerUpEMP, PowerUpBooster, PowerUpShield, PowerUpHealth, PowerUpRockets, PowerUpTankCannon...
    if (article.find("Missile") != std::string::npos) return "missiles";
    if (article.find("Rocket") != std::string::npos) return "rockets";
    if (article.find("Oil") != std::string::npos) return "oil";
    if (article.find("Smoke") != std::string::npos) return "smoke";
    if (article.find("EMP") != std::string::npos) return "emp";
    if (article.find("Boost") != std::string::npos) return "boost";
    if (article.find("Shield") != std::string::npos) return "shield";
    if (article.find("Health") != std::string::npos) return "health";
    if (article.find("Cannon") != std::string::npos) return "cannon";
    if (article.find("007") != std::string::npos) return "bonus";
    return "bonus";
}

}  // namespace

MissionData MissionData::load(const CarpFile& carp) {
    MissionData md;
    const auto& entries = carp.entries();
    // Mission group = the head whose members hold Rule/MSet (group 495 in Paris; found dynamically).
    int mission_group = -1;
    for (const CarpEntry& e : entries)
        if (!e.is_head && (e.tag == "Rule" || e.tag == "MSet")) mission_group = e.group;
    // Namespace strings for sr-link resolution (group 495 only).
    std::map<int, std::string> sr;
    for (const CarpEntry& e : entries)
        if (e.tag == "sr" && e.group == mission_group && e.index >= 0 && !e.is_head)
            sr[e.index] = std::string(load_cstr(carp.payload(e), 0));
    auto sr_name = [&](std::uint16_t idx) -> std::string {
        const auto it = sr.find(idx);
        return it == sr.end() ? "" : it->second;
    };

    for (const CarpEntry* x : carp.find("MSet")) {
        const Bytes b = carp.payload(*x, x->size ? x->size : 760);
        if (b.size() < 32) throw FormatError("MSet too short");
        md.mset_name = std::string(load_cstr(b, 0));
        for (std::size_t k = 32; k + 4 <= b.size(); k += 4) md.mset_words.push_back(u32(b, k));
    }
    for (const CarpEntry* x : carp.find("AICo")) md.ai_controllers += x->count ? 0 : x->size / 160;
    for (const CarpEntry* x : carp.find("AIEl")) {
        const Bytes b = carp.payload(*x, x->size);
        for (std::size_t k = 0; k + 256 <= b.size(); k += 256) {
            // Only kind==1 slots with a car name are live roster entries; kind==0 slots are
            // dormant/empty (verified: all named MIS3/13A entries have kind 1, all empty kind 0).
            if (static_cast<std::int32_t>(u32(b, k + 0x44)) != 1) continue;
            AiSpawn s;
            s.pos = {f32(b, k + 0x30), f32(b, k + 0x34), f32(b, k + 0x38)};
            s.yaw = yaw_of(b, k);
            s.id = static_cast<std::int32_t>(u32(b, k + 0x40));
            s.kind = 1;
            s.wake = f32(b, k + 0x4C);
            s.car = printable_run(b, k + 0x54, 32);
            if (s.car.empty()) continue;
            s.health = static_cast<std::int32_t>(u32(b, k + 0x90));
            // +0xF8 {u16, 'sr'}: tag bytes are stored reversed ('r','s' on disk = "sr").
            if (b[k + 0xFA] == 'r' && b[k + 0xFB] == 's') s.link = sr_name(u16(b, k + 0xF8));
            md.ai.push_back(std::move(s));
        }
    }
    for (const CarpEntry* x : carp.find("AICo"))
        if (x->size % 160 == 0) md.ai_controllers += x->size / 160;
    for (const CarpEntry* x : carp.find("AISp")) {
        const Bytes b = carp.payload(*x, x->size);
        for (std::size_t k = 0; k + 112 <= b.size(); k += 112) {
            AiSpawnPoint p;
            p.pos = {f32(b, k + 0x30), f32(b, k + 0x34), f32(b, k + 0x38)};
            p.yaw = yaw_of(b, k);
            // Inline path name: longest printable run in the tail half of the stride.
            std::string best;
            for (std::size_t j = 64; j < 112; ++j) {
                if (b[k + j] == 0) continue;
                std::string s = printable_run(b, k + j, 112 - j);
                if (s.size() > best.size()) best = std::move(s);
            }
            p.path = best;
            md.spawns.push_back(std::move(p));
        }
    }
    for (const CarpEntry* x : carp.find("Rule")) {
        const Bytes b = carp.payload(*x, x->size);
        for (std::size_t k = 0; k + 32 <= b.size(); k += 32) {
            MissionRule r;
            r.type = u32(b, k);
            r.a = u32(b, k + 4);
            r.b = u32(b, k + 8);
            if (b[k + 0x0E] == 'l' && b[k + 0x0F] == 'e') r.element = u16(b, k + 0x0C);
            md.rules.push_back(r);
        }
    }
    for (const CarpEntry* x : carp.find("Trgr")) {
        const Bytes b = carp.payload(*x, x->size);
        for (std::size_t k = 0; k + 64 <= b.size(); k += 64) {
            Trigger t;
            t.pos = {f32(b, k), f32(b, k + 4), f32(b, k + 8)};
            t.radius = f32(b, k + 12);
            t.flags = u32(b, k + 16);
            if (b[k + 0x1A] == 'l' && b[k + 0x1B] == 'e') t.element = u16(b, k + 0x18);
            md.triggers.push_back(t);
        }
    }
    md.cams = 0;
    for (const CarpEntry* x : carp.find("Cams")) md.cams += x->size / 64;
    md.windows = carp.find("wn").size();
    md.paths = carp.find("pt").size();
    for (const CarpEntry* x : carp.find("el"))
        if (x->group == mission_group) md.elements += 1;

    for (const CarpEntry* x : carp.find("rs")) {
        const Bytes b = carp.payload(*x, x->size ? x->size : 100);
        if (b.size() < 36) continue;
        RoadSeg s;
        s.a = {f32(b, 0), f32(b, 4), f32(b, 8)};
        s.b = {f32(b, 16), f32(b, 20), f32(b, 24)};
        s.flags = u32(b, 28);
        s.width = f32(b, 32);
        md.road.push_back(s);
    }
    // Routable route: `rn` positions in index order. (`rn` = {vec3 pos, u32 (self|road<<16),
    // u32 (prev|self<<16), ...}; the road id marks segment types, not routes — filtering by it
    // fragments the route at every junction, so all nodes are kept in index order. The walk
    // bridges the remaining gaps.)
    {
        struct Rn { int idx; Vec3 pos; };
        std::vector<Rn> all;
        for (const CarpEntry* x : carp.find("rn")) {
            const Bytes b = carp.payload(*x, x->size ? x->size : 32);
            if (b.size() < 12) continue;
            all.push_back({x->index, {f32(b, 0), f32(b, 4), f32(b, 8)}});
        }
        std::sort(all.begin(), all.end(), [](const Rn& p, const Rn& q) { return p.idx < q.idx; });
        for (const Rn& n : all) md.route.push_back(n.pos);
    }

    // PowerUp pickups: Map-group instances whose sr article name holds "PowerUp". Instance rows
    // are 64 bytes (track_model.cpp): u16 sr ref at +28, translation at +48..+59.
    std::map<int, std::string> world_ref;
    for (const CarpEntry& e : entries)
        if (e.tag == "sr" && !e.is_head && e.index >= 0) {
            const auto it = world_ref.find(e.index);
            if (it == world_ref.end()) world_ref[e.index] = std::string(load_cstr(carp.payload(e), 0));
        }
    for (const CarpEntry& e : entries) {
        if (e.tag != "in" || e.is_head || e.count < 2) continue;
        const Bytes inst = carp.payload(e);
        for (std::size_t i = 0, n = e.count; i < n; ++i) {
            const std::size_t base = i * 64;
            if (base + 64 > inst.size()) break;
            const auto it = world_ref.find(u16(inst, base + 28));
            if (it == world_ref.end()) continue;
            const std::string& name = it->second;
            if (name.find("PowerUp") == std::string::npos) continue;
            PowerUpSpot p;
            p.pos = {f32(inst, base + 48), f32(inst, base + 52), f32(inst, base + 56)};
            const auto open = name.rfind("::{as  ");
            const std::string article =
                open == std::string::npos ? name : name.substr(0, open) + name.substr(open + 8);
            p.kind = power_kind(article);
            md.powerups.push_back(std::move(p));
        }
    }
    return md;
}

std::string player_car_for(std::string_view viv, const MissionData& md) {
    // MSet corroboration + seat/type data (docs/driving-missions.md). Comparisons are lowercase.
    std::string m = md.mset_name;
    for (char& ch : m) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (viv == "MIS01") return "vanquish";
    if (viv == "MIS3") return "supersnow";
    if (viv == "MIS4") return "vanquishalps";
    if (viv == "MIS11") return "vanquishsub";
    if (viv == "MIS13A") return "jungle_truck";
    if (viv == "MIS13B") return "ultralight";
    if (viv == "MIS13C") return "ultralightbig";
    if (viv == "RACE") return "cobra_player";
    if (!m.empty()) return md.mset_name;
    return "vanquish";
}

const std::vector<DrivingMissionId>& driving_mission_ids() {
    // ACTION.ELF Menu_IsDrivingLevel (0x9000001-3,5,6) + HT_Level_Driving_*_ST tokens
    // (Paris/SnowMobile/Alps/Underwater/JungleA); the JungleA slot chains 13A->13B->13C via
    // CHAIN_NEXT_MISSION, and RACE is the standalone mini-mission (Menu_RunMiniMission).
    static const std::vector<DrivingMissionId> ids = {
        {0x9000001, "MIS01", "paris"},
        {0x9000002, "MIS3", "alps"},
        {0x9000003, "MIS4", "alps2"},
        {0x9000005, "MIS11", "underwater"},
        {0x9000006, "MIS13A", "jungle1"},
    };
    return ids;
}

const DrivingMissionId* driving_mission_for_id(std::uint32_t level_id) {
    for (const auto& m : driving_mission_ids())
        if (m.level_id == level_id) return &m;
    return nullptr;
}

const DrivingMissionId* driving_mission_for_viv(std::string_view viv) {
    for (const auto& m : driving_mission_ids())
        if (m.viv == viv) return &m;
    return nullptr;
}

std::size_t validate_mission(const CarpFile& carp, const std::string& label) {
    std::size_t failures = 0;
    auto fail = [&](const std::string& what) {
        std::printf("  FAIL %s: %s\n", label.c_str(), what.c_str());
        ++failures;
    };
    MissionData md;
    try {
        md = MissionData::load(carp);
    } catch (const std::exception& e) {
        fail(std::string("parse: ") + e.what());
        return failures;
    }
    if (md.mset_name.empty()) fail("empty MSet name");
    if (md.mset_words.size() != 182) fail("MSet words != 182");
    for (const MissionRule& r : md.rules)
        if (r.type > 14) fail("rule type out of range");
    for (const Trigger& t : md.triggers) {
        if (!(t.radius > 0 && t.radius < 1e4)) fail("trigger radius insane");
        if (t.element >= 0 && std::size_t(t.element) > md.elements + 32) fail("trigger el ref far out");
    }
    for (const AiSpawn& s : md.ai) {
        if (s.car.empty() || s.car.size() > 32) fail("AI car name bad");
        if (!(s.pos[1] > -2000 && s.pos[1] < 2000)) fail("AI spawn height insane");
    }
    if (md.road.empty())
        std::printf("  note %s: no road segments (roadless mission, e.g. jungle3)\n", label.c_str());
    // `ps` is opaque ped-spawn data (not all zeros); only its stride shape is validated.
    for (const CarpEntry* x : carp.find("ps")) {
        if (x->size % 32 != 0) fail("ps record stride not 32");
    }
    std::printf("  mission %s: MSet=%s ai=%zu spawns=%zu triggers=%zu rules=%zu powerups=%zu road=%zu\n",
                label.c_str(), md.mset_name.c_str(), md.ai.size(), md.spawns.size(), md.triggers.size(),
                md.rules.size(), md.powerups.size(), md.road.size());
    return failures;
}

}  // namespace nf::driving

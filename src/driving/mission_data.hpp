#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "assets/carp_file.hpp"
#include "core/math.hpp"

namespace nf::driving {

// The mission/scripting records of a driving track `.crp` (the `<<Map>>` sub-TAR, group 495 in
// paris_mis01.crp; loaded by sub_230730/sub_22FAF8, driven at runtime by SMissionManager 0x205220+,
// AIElementController and the SRule*/E* rule/event classes). Layouts below are verified against the
// data of all 8 tracks; per-word semantics that the spec does not pin down are kept raw with the
// offsets noted so a future differential test (EeInterp nfmips) can settle them.
//
//   MSet  one record: char name[32] + 182 u32 words. The name is the mission's signature vehicle
//         (MIS3 'supersnow', MIS4 'vanquishalps', MIS11 'vanquishsub', RACE 'cobra_player'; Paris
//         names the scripted 'helicopter' hunter, MIS13A/B the 'jungle_truck'/'ultralight' player
//         vehicles, MIS13C the 'fodbase' assault target [INFERENCE: role per mission, see
//         player_car_for()]). Words are small AI-budget-style ints (50/30/10/100/24...); kept raw.
//   AICo  stride 160: AI controller slots (AIElementController_Construct).
//   AIEl  stride 256: AI vehicle roster. +0x00 float basis[12] (3 orthonormal rows + padding, the
//         spawn orientation), +0x30 float pos[3], +0x40 u32 id, +0x44 u32 active(1), +0x48 u32 ?,
//         +0x4C float (wake range?), +0x54 char car[32] (pvehicle/*.atr stem), +0x80 AI params,
//         +0x90 u32 health?, +0xF8 {u16, 'sr'} link into the AICo/AISp/AIEl namespace strings
//         (`CARP::<<Map>>::{AICo}::<byte offset>` etc.).
//   AISp  stride 112: AI spawn points. +0x00 float basis[12], +0x30 float pos[3], +0x40 inline
//         NUL-terminated path name (`rspath_Helicopter02`, `rspath_Heli_Hover`, ...).
//   Rule  stride 32: mission rules. +0x00 u32 type (0..14 indexes the SRule* factory jump table at
//         ELF 0x209390 via 0x3979A0: Always/Ammo/Collision/Damage/Death/Difficulty/Health/PIP/
//         PlayerDir/ProgCounter/Prog/Property/Range/Speed/Timer [INFERENCE: exact id order]),
//         +0x04/+0x08 params, +0x0C {u16, 'el'} target mission element.
//   Trgr  stride 64: trigger volumes. +0x00 float pos[3], +0x0C float radius, +0x10 u32 flags,
//         +0x14 float ?, +0x18 {u16, 'el'} target element.
//   ps    stride 32: opaque ped-spawn data (MIS01's first slots are zeros; other tracks carry
//         small nonzero headers like `01 ff`: kept structurally, see validate_mission).
//   pt    count=2 records of waypoints: ped/patrol paths (decoded as float triples when they fall
//         inside the track bounds; otherwise kept raw).
//   wn    per-window records (AICharacterEnemyWindow sniper spots); counted only.
//   el    (group 495) mission elements: {u16 idx, 'el'} addressable event instances holding
//         {u16, 'sr'} links to game objects. The E* event factory mapping (event id -> class) is
//         not reversed; elements are exposed raw with their sr links resolved to names.
//   sr    (group 495) namespace strings resolving every {u16,'sr'} link.

struct AiSpawn {
    std::string car;     // pvehicle stem, e.g. "paris_henchbigbox"
    Vec3 pos{};          // world (Y up, metres)
    float yaw = 0;       // spawn facing, radians (from the basis rows)
    std::int32_t id = 0;       // +0x40
    std::int32_t kind = 0;     // +0x44 (1 = active in all shipped data)
    float wake = 0;            // +0x4C
    std::int32_t health = 0;   // +0x90
    std::string link;    // resolved sr target, e.g. "CARP::<<Map>>::{AICo}::8960"
};

struct AiSpawnPoint {
    Vec3 pos{};
    float yaw = 0;
    std::string path;    // "rspath_..." name, "" when none
};

struct Trigger {
    Vec3 pos{};
    float radius = 0;
    std::uint32_t flags = 0;
    std::int32_t element = -1;  // 'el' idx, -1 when the slot holds no link
};

struct MissionRule {
    std::uint32_t type = 0;     // 0..14, SRule* factory id
    std::uint32_t a = 0, b = 0;
    std::int32_t element = -1;
};

struct PowerUpSpot {
    Vec3 pos{};
    std::string kind;    // "missiles", "oil", "smoke", "emp", "boost", "shield", "health", ...
};

// Road-network lane piece (`rs` record, RNgp group; sub_220C28): a start point `+0x00` plus
// lane parameters. Only Paris stores chainable world-space starts; elsewhere the `+0x00`
// x/y are small lane ids and only z carries route distance, so `rs` is not routable there.
struct RoadSeg {
    Vec3 a{}, b{};
    std::uint32_t flags = 0;  // word +0x1C
    float width = 0;          // word +0x20 as float, when sane
};

// Routable road points: the main-road `rn` records (most populous road id of the RNgp group)
// in index order. `rn` = {vec3 pos, u32 (self | road<<16), u32 (prev | self<<16), ...} and the
// positions chain continuously along the mission route on every track with a network.
struct MissionData {
    std::string mset_name;
    std::vector<std::uint32_t> mset_words;  // 182 words after the name
    std::size_t ai_controllers = 0;         // AICo stride count (stride 160)
    std::vector<AiSpawn> ai;
    std::vector<AiSpawnPoint> spawns;
    std::vector<Trigger> triggers;
    std::vector<MissionRule> rules;
    std::vector<PowerUpSpot> powerups;      // Map instances of PowerUp* smackable articles
    std::vector<RoadSeg> road;              // all `rs` segments, index order
    std::vector<Vec3> route;               // all `rn` positions, index order (the route)
    std::size_t cams = 0, windows = 0, elements = 0, paths = 0;

    static MissionData load(const CarpFile& carp);
};

// The player-driven vehicle per mission: the MSet name when it is a Bond-drivable pvehicle
// (AIC_BOND_POS seat or a subsnow/flying player type), else the mission's Bond car. Grounded in
// MSet + AIC_BOND_POS + SECONDARY_TYPE target data; MIS13C's 'fodbase' MSet names the assault
// target, and the only seat-fitted car there is the ultralightbig [INFERENCE, see docs].
std::string player_car_for(std::string_view viv, const MissionData& md);

// ACTION.ELF story level id (Menu_IsDrivingLevel: 0x9000001-3,5,6) <-> driving mission.
struct DrivingMissionId {
    std::uint32_t level_id;   // 0x9000001 ...
    std::string_view viv;     // "MIS01" ...
    std::string_view name;    // nfdrive name ...
};
const std::vector<DrivingMissionId>& driving_mission_ids();
const DrivingMissionId* driving_mission_for_id(std::uint32_t level_id);
const DrivingMissionId* driving_mission_for_viv(std::string_view viv);

// Structural validation for `nfdump validate`: counts, stride sizes, sr-link resolution,
// trigger radii, all-zero ps. Returns failure count (0 = clean), printing details.
std::size_t validate_mission(const CarpFile& carp, const std::string& label);

}  // namespace nf::driving

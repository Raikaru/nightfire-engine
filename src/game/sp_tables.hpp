#pragma once

// Single-player drone data tables (docs/spec-arena-ai.md Part 3 §2, docs/ai-sp.md): everything that decides what
// a placed NPC *is* before its state machine starts.
//   - DroneTypeSettings @0x29aee8 (85 x 12 B) and DroneModeSettings @0x29ad40 (35 x 12 B), read from ACTION.ELF;
//   - the behaviour bit-set layout (`bitDescs` @0x2c7250, `bitMasks` @0x2c7308) and the per-DMODE init functions
//     (NDrone2_init_DMODE_*), decoded from the ELF's MIPS code (straight-line `behaviour_util_setProperty` calls);
//   - the DIVars (level_tag) parameter layout of a placed NPC (`SpNpcSpec`) and the resolution the original does in
//     NDrone2_DefaultInit (0x14b300) / NDrone2_DoModeSettingsOLD (0x14cfa0) / ...NEW (0x14c600) /
//     NDrone2_GetDroneTypeAttackTypeFriend (0x14cbd8) / NDrone2_GetDTYPENEW (0x14c938): `resolve_npc`.

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "assets/elf.hpp"
#include "assets/map_file.hpp"
#include "game/drone.hpp"

namespace nf::sp {

using nf::drone::Behaviour;

// ---- ELF tables -------------------------------------------------------------------------------------------
constexpr std::uint32_t kDroneTypeSettingsAddr = 0x29aee8, kDroneModeSettingsAddr = 0x29ad40;
constexpr std::uint32_t kBitDescsAddr = 0x2c7250, kBitMasksAddr = 0x2c7308, kStatWidthsAddr = 0x303330;
constexpr int kDtypeCount = 85, kDmodeCount = 35;

struct DroneTypeEntry {            // DroneTypeSettings row
    std::int16_t initial_state;    // -1 = computed (NDrone2_DoTypeSettingsOLD / GetDroneTypeAttackTypeFriend)
    std::int16_t alt_state;        // 0 = none
    std::uint32_t init_fn;         // ELF address, 0 = none (initDTYPE_Sniper, SniperAlert, BotInit)
    std::uint32_t control_fn;      // ELF address, 0 = NDrone2_ControlSTANDARD (ControlDTYPE_Zoe/Ninja/Astronaut)
};
struct DroneModeEntry {            // DroneModeSettings row (DMODE < 0x24)
    std::uint8_t dtype;
    std::uint8_t side;             // 1 enemy, 2 friend, 3 neutral
    float alertness;               // initial alertness (+0x50c/+0x500)
    std::uint32_t init_fn;         // NDrone2_init_DMODE_* address, 0 = none
};
// One `behaviour_util_setProperty(id, behaviour, value)` executed by an init function, in call order.
// `id == kCallDefaults` marks the call of NDrone2_init_DMODE_Defaults (0x14d528).
struct BehaviourOp {
    int id;
    unsigned value;
};
constexpr int kCallDefaults = -1;

class SpTables {
public:
    static SpTables load(const nf::Elf32& action_elf);

    std::array<DroneTypeEntry, kDtypeCount> dtype{};
    std::array<DroneModeEntry, kDmodeCount> dmode{};
    std::array<std::uint8_t, Behaviour::kCount * 2> bit_descs{};   // bitDescs: {word|shift<<3, mask index}
    std::array<std::uint32_t, 35> bit_masks{};
    std::array<std::uint32_t, 8> stat_widths{};                    // 8,3,8,8,5,8,8,1 (behaviour_util_get)
    // Program of each DMODE init function (ordered ops); Defaults is expanded by apply_dmode_init.
    std::array<std::vector<BehaviourOp>, kDmodeCount> dmode_init{};
    std::vector<BehaviourOp> dmode_defaults;

    // Runs NDrone2_init_DMODE_<dmode> (Defaults expanded) on `b`.
    void apply_dmode_init(int dmode_index, Behaviour& b) const;
};

// The behaviour layout the core implements (drone.hpp Behaviour::get/set) must equal bitDescs/bitMasks: returns
// the number of ids whose position/width differs from the ELF (0 = identical).
int check_behaviour_layout(const SpTables& t);

// Decodes a straight-line MIPS function made of `li a0,id / li a2,value / jal behaviour_util_setProperty` (and
// calls of `defaults_addr`) into ops. Returns false if it finds an instruction it does not understand.
bool decode_init_function(const nf::Elf32& elf, std::uint32_t addr, std::uint32_t set_property_addr,
                          std::uint32_t defaults_addr, std::vector<BehaviourOp>& out);

// ---- placed NPC (DIVars) ----------------------------------------------------------------------------------
// Level-tag param key k <-> DIVars offset 0x34 + 4k <-> level_tag offset 0x2c + 4k (spec §1.2).
struct SpNpcSpec {
    Vec3 pos{};                      // _TARG23_PLACEMENT position (obj+0x30, above the feet)
    float yaw = 0;                   // euler.y
    std::uint32_t skin = 0;          // key 0: 0x5000004.. model/skin id
    std::uint32_t script = 0x6000000;   // key 1: script/action id
    std::uint32_t min_difficulty = 0;   // key 2
    std::uint8_t start_channel = 0;     // key 3 (byte)
    std::uint32_t key4 = 0;             // key 4 -> Drone+0x144
    std::uint32_t mode = 0x64;          // key 5 (u16): DMODE index; >= 0x24 = behaviour-blob path
    std::uint8_t alt_channel = 0;       // key 6 (byte)
    std::uint32_t key7 = 0;             // key 7 -> Drone+0x148
    std::uint32_t alt_mode = 0x65;      // key 8 (u16)
    std::uint32_t voice_set = 0;        // key 9 (u16): weapon/ammo set (+0xbbc/+0xbbe, +0xda group, +0x100)
    std::uint32_t variant = 0;          // key 10 -> Drone+0x45
    std::uint32_t kit = 0;              // key 11: head/armour kit (>>4 = kit, low nibble = colour)
    std::uint32_t key12 = 0;            // key 12 -> Drone+0x13c
    std::uint32_t item = 0;             // key 13: carried special item (0x600021f = key card)
    std::uint32_t sight_profile = 0;    // key 14 (0..5)
    std::array<std::uint32_t, 18> blob{};   // keys 15..32: behaviour blob (behaviour_util_get)
    std::uint32_t param_count = 0;      // number of params present in the map (26 or 33 in the shipped data)
};

// Parses class-0x0f statics' params (missing keys read as 0 except the documented defaults).
SpNpcSpec npc_spec_from_static(const nf::StaticInstance& s);

// behaviour_util_get: the parsed behaviour blob (the two bitsets, roles and, when present, the three stat rows).
struct BehaviourBlob {
    bool valid = false;
    std::uint32_t type = 0, mode = 0;        // roles (Drone+0xc7 / +0xc8)
    std::uint32_t type2 = 0;                 // Drone+0xc9
    Behaviour first, second;                 // Drone+0x4dc / +0x4e8
    bool has_stats = false;
    std::array<std::array<std::uint32_t, 8>, 3> stats{};   // drone_stats[type] difficulty rows
};
BehaviourBlob parse_behaviour_blob(const SpTables& t, const std::array<std::uint32_t, 18>& blob);

// `drone_stats` @0x31aa30 (17 types x 3 rows x 8 u32, .bss = zero at load). behaviour_util_get overwrites the
// rows of a type with each placed NPC's blob; DoModeSettingsNEW reads row 1 of the NPC's type.
struct DroneStats {
    std::array<std::array<std::array<std::uint32_t, 8>, 3>, 17> rows{};
};

// ---- level facts ------------------------------------------------------------------------------------------
enum class LevelGroup { None, Estate, Castle, Tower1, PowerStation, Tower2, EvilBase, SpaceStation, Multiplayer };
LevelGroup level_group(std::uint32_t level_id);
constexpr std::uint32_t kLevelCastleC = 0x7000007, kLevelEvilBase = 0x7000014, kLevelTower2A = 0x7000011,
                        kLevelEvilBaseC = 0x7000016;
bool is_sp_level(std::uint32_t level_id);
// Level id from a level bin name ("07000004.bin" -> 0x7000004); 0 if the name does not parse.
std::uint32_t level_id_from_bin(const std::string& name);

// ---- resolution --------------------------------------------------------------------------------------------
struct ResolveEnv {
    std::uint32_t level_id = 0;
    int difficulty = 2;                                   // GameState+0x28
    bool multiplayer = false;                             // MPSettings+0x184
    float captain_health = 2.0f, captain_bullet_damage = 2.0f, captain_bullet_accuracy = 2.0f;   // DroneCaptain_Mod_*
    float astronaut_hits = 1.0f;                          // weapon_data[0x33] damage (DroneInit_TakeXHits)
    std::function<float(float)> frand;                    // Rand_FRand(range) uniform in [0, range)
    const SpTables* tables = nullptr;
};

// Everything NDrone2_DefaultInit + the mode/type settings decide for a placed NPC (Drone+offsets in comments).
struct NpcResolved {
    // identity
    std::uint32_t skin = 0;              // +0xe0 (level swap, captain swap applied)
    std::uint32_t base_skin = 0;         // the placed skin
    std::uint16_t char_class = 0;        // +0xd8
    std::uint16_t sub_class = 0;         // +0xda
    bool captain = false;                // +0x19
    bool captain_flag18 = false;         // +0x18 (class 2 captain)
    bool flag14 = true;                  // +0x14
    bool flag15 = false;                 // +0x15
    bool invulnerable_anim = false;      // +0x1b
    bool free_move = false;              // +0x23 (abseil / astronaut zero-g route)
    bool flag1f = true;                  // +0x1f (SP default 1; ninja/abseil/astronaut 0)
    bool fire_locked = false;            // +0x3e
    std::uint8_t variant = 0;            // +0x45
    // weapon set
    std::uint16_t ammo = 0, ammo_max = 0;   // +0xbbc / +0xbbe
    float bullet_damage_mod = 1.0f;      // +0x100
    std::uint16_t kit = 0, kit_colour = 0;   // +0xbcc / +0xbce
    std::uint32_t item = 0;              // +0xbd4
    // ranges (sight profile)
    float sight_cone = 1.5707964f, sight_range = 24.0f, range_ec = 4.0f, engage_dist = 12.0f, range_f4 = 4.0f,
          min_cover_dist = 2.0f, max_combat_dist = 13.0f;
    int d0 = 15;                         // +0xd0
    float walk_scale = 0.075f;           // +0x50 (0 -> 0.075)
    float route_radius = 2.0f;           // +0x8ec/+0x8f0 (4.0 / 8.0 for free-move drones and ninjas)
    // roles
    std::uint8_t dtype_base = 0, dtype = 0, dtype_alt = 0;   // +0xc4 / +0xc5 / +0xc6
    std::uint8_t side = 1;                                   // +0x44
    std::uint8_t role_type = 0, role_mode = 0, role_type2 = 0;   // +0xc7 / +0xc8 / +0xc9
    int initial_state = 4;               // +0x5a2
    int alt_state = 0;                   // +0x5a4
    std::uint8_t state_arg_13d = 0;      // +0x13d
    // behaviour and alertness
    std::array<Behaviour, 2> behaviour{};   // +0x4dc / +0x4e8
    float alertness = 0, alertness_floor = 0, alertness_floor2 = 0;   // +0x50c/+0x500/+0x504
    // stats (drone_stats row 1)
    float health = 10.0f;                // +0xac
    std::uint8_t accuracy = 5, aggression = 0;   // +0xb4 / +0xb5
    std::uint8_t stat_b6 = 0, stat_b7 = 0, stat_b8 = 0, stat_b9 = 0, stat_ba = 0;
    // script / switches
    std::uint32_t script = 0x6000000;    // +0x554
    std::uint8_t start_channel = 0, alt_channel = 0;   // +0x134 / +0x135
    int dmode = 0, alt_dmode = 0;        // +0x138 / +0x13a
    std::uint32_t key4 = 0, key7 = 0;    // +0x144 / +0x148
    std::uint8_t key12 = 0;              // +0x13c
    std::uint32_t flags_or = 0;          // bits OR-ed into Drone+0x4f8 (0x801e2 base, 0x10 stationary...)
    std::uint32_t alert_flags_or = 0;    // +0x4fc
    bool starts_attacking = false;       // initial state Attack/AlertToPosition: DroneFunc_SetAsAttacking
    bool alt_mode_none = true;           // +0x13a==0 after GetDroneTypeAttackTypeFriend
    std::uint16_t impact_mask = 0x80;    // +0xba0
    std::uint32_t ba4 = 0x1c00;          // +0xba4
    int captain_grenade = 0;             // (derived) unused
};

// (Class, sub-class, flags) of a skin id: the big switch in NDrone2_DefaultInit, transcribed.
struct SkinClass {
    std::uint16_t char_class, sub_class;
    bool flag15 = false, flag14_off = false, flag1b = false, free_move = false;
    std::uint8_t force_dtype = 0;        // 0x5000035 truck driver: +0xc5 = 0x17
    std::uint8_t variant_override = 0;   // Mayhew: DIVars+0x5c = 6
    bool no_voice_default = false;       // 0x500001e: voice-set byte starts at 0
};
SkinClass skin_class(std::uint32_t skin);
// All skin ids the table names (for validation).
const std::vector<std::uint32_t>& known_skins();

// NDrone2_DefaultInit + DoModeSettingsOLD/NEW + GetDroneTypeAttackTypeFriend for one placed NPC. `stats` is the
// running `drone_stats` table of the level (updated by the NPC's blob, in placement order).
NpcResolved resolve_npc(const SpNpcSpec& spec, const ResolveEnv& env, DroneStats& stats);

// Captain skin swap (class -> skin), 0 if the class has none.
std::uint32_t captain_skin(int char_class);

// DroneFunc_DropGrenade weapon id for a captain's death (spec §9); 0 = none. Levels 1-8: 0x35 (x2), others 0x34.
int captain_drop_weapon(std::uint32_t level_id);

}  // namespace nf::sp

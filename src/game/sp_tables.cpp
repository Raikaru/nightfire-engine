#include "game/sp_tables.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "assets/map_file.hpp"
#include "assets/reader.hpp"

namespace nf::sp {

using nf::drone::Behaviour;

namespace {

// ---- MIPS decoding of the NDrone2_init_DMODE_* functions ------------------------------------------------
struct Regs {
    std::uint32_t r[32]{};
};

// Executes one straight-line instruction for the registers we care about; false = unknown control flow.
bool step(Regs& g, std::uint32_t w) {
    const unsigned op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31, rd = (w >> 11) & 31;
    const std::int32_t imm = std::int16_t(w & 0xffff);
    switch (op) {
    case 0x09:  // addiu
    case 0x19:  // daddiu
        if (rs == 0) g.r[rt] = std::uint32_t(imm);
        else if (rt != 29) g.r[rt] = g.r[rs] + std::uint32_t(imm);
        return true;
    case 0x0d:  // ori
        g.r[rt] = (rs == 0 ? 0 : g.r[rs]) | (w & 0xffff);
        return true;
    case 0x00: {
        const unsigned funct = w & 63;
        if (funct == 0x2d || funct == 0x21 || funct == 0x25) {  // daddu / addu / or
            g.r[rd] = (rs == 0 ? 0 : g.r[rs]) + (rt == 0 ? 0 : g.r[rt]);
            return true;
        }
        return w == 0 || funct == 0x08;   // nop / jr
    }
    case 0x1f:  // sq
    case 0x1e:  // lq
    case 0x23:  // lw
    case 0x2b:  // sw
    case 0x1b:  // ld? (unused) -- harmless
        return true;
    default:
        return false;
    }
}

}  // namespace

bool decode_init_function(const nf::Elf32& elf, std::uint32_t addr, std::uint32_t set_property_addr,
                          std::uint32_t defaults_addr, std::vector<BehaviourOp>& out) {
    out.clear();
    Regs g;
    for (std::uint32_t pc = addr; pc < addr + 0x400; pc += 4) {
        const nf::Bytes b = elf.at(pc, 8);
        const std::uint32_t w = nf::load<std::uint32_t>(b, 0);
        if (w == 0x03e00008) return true;   // jr ra
        if ((w >> 26) == 3) {               // jal: the delay slot runs before the callee sees a0..a2
            const std::uint32_t target = (w & 0x3ffffff) << 2;
            if (!step(g, nf::load<std::uint32_t>(b, 4))) return false;
            if (target == set_property_addr) out.push_back({int(g.r[4]), g.r[6]});
            else if (target == defaults_addr) out.push_back({kCallDefaults, 0});
            else return false;
            pc += 4;
            continue;
        }
        if (!step(g, w)) return false;
    }
    return false;
}

SpTables SpTables::load(const nf::Elf32& elf) {
    SpTables t;
    {
        const nf::Bytes b = elf.at(kDroneTypeSettingsAddr, kDtypeCount * 12);
        for (int i = 0; i < kDtypeCount; ++i) {
            t.dtype[std::size_t(i)] = {nf::load<std::int16_t>(b, std::size_t(i) * 12),
                                       nf::load<std::int16_t>(b, std::size_t(i) * 12 + 2),
                                       nf::load<std::uint32_t>(b, std::size_t(i) * 12 + 4),
                                       nf::load<std::uint32_t>(b, std::size_t(i) * 12 + 8)};
        }
    }
    {
        const nf::Bytes b = elf.at(kDroneModeSettingsAddr, kDmodeCount * 12);
        for (int i = 0; i < kDmodeCount; ++i) {
            const std::size_t o = std::size_t(i) * 12;
            t.dmode[std::size_t(i)] = {nf::load<std::uint8_t>(b, o), nf::load<std::uint8_t>(b, o + 1),
                                       nf::load<float>(b, o + 4), nf::load<std::uint32_t>(b, o + 8)};
        }
    }
    {
        const nf::Bytes d = elf.at(kBitDescsAddr, Behaviour::kCount * 2);
        std::copy(d.begin(), d.end(), t.bit_descs.begin());
        const nf::Bytes m = elf.at(kBitMasksAddr, 35 * 4);
        for (std::size_t i = 0; i < 35; ++i) t.bit_masks[i] = nf::load<std::uint32_t>(m, i * 4);
        const nf::Bytes w = elf.at(kStatWidthsAddr, 32);
        for (std::size_t i = 0; i < 8; ++i) t.stat_widths[i] = nf::load<std::uint32_t>(w, i * 4);
    }
    const auto set_prop = elf.symbol("behaviour_util_setProperty__FiPUiUi");
    const auto defaults = elf.symbol("NDrone2_init_DMODE_Defaults__FP9Drone_tag");
    if (!set_prop || !defaults) throw nf::FormatError("ACTION.ELF has no behaviour_util_setProperty / init_DMODE_Defaults");
    if (!decode_init_function(elf, defaults->value, set_prop->value, 0, t.dmode_defaults))
        throw nf::FormatError("cannot decode NDrone2_init_DMODE_Defaults");
    for (int i = 0; i < kDmodeCount; ++i) {
        const std::uint32_t fn = t.dmode[std::size_t(i)].init_fn;
        if (fn == 0) continue;
        // NDrone2_init_DMODE_Ninja/_Bot/_DeleteMe live in another translation unit and are not plain property
        // lists (Bot); a function that does not decode is left empty and reported by nfdump.
        if (!decode_init_function(elf, fn, set_prop->value, defaults->value, t.dmode_init[std::size_t(i)]))
            t.dmode_init[std::size_t(i)].clear();
    }
    return t;
}

void SpTables::apply_dmode_init(int dmode_index, Behaviour& b) const {
    if (dmode_index < 0 || dmode_index >= kDmodeCount) return;
    for (const BehaviourOp& op : dmode_init[std::size_t(dmode_index)]) {
        if (op.id == kCallDefaults) {
            for (const BehaviourOp& d : dmode_defaults) b.set(d.id, d.value);
        } else {
            b.set(op.id, op.value);
        }
    }
}

int check_behaviour_layout(const SpTables& t) {
    int bad = 0;
    for (int id = 0; id < Behaviour::kCount; ++id) {
        const unsigned desc = t.bit_descs[std::size_t(id) * 2], mask_index = t.bit_descs[std::size_t(id) * 2 + 1];
        if (mask_index >= t.bit_masks.size()) { ++bad; continue; }
        const unsigned word = desc & 7, shift = desc >> 3;
        const std::uint32_t mask = t.bit_masks[mask_index];
        Behaviour b;
        b.set(id, mask);   // all field bits on
        Behaviour expect;
        if (word < 3) expect.word[word] = mask << shift;
        if (b.word != expect.word) ++bad;
    }
    return bad;
}

// ---- level facts -----------------------------------------------------------------------------------------
LevelGroup level_group(std::uint32_t id) {
    if (id >= 0x7000001 && id <= 0x7000004) return LevelGroup::Estate;
    if (id >= 0x7000005 && id <= 0x7000008) return LevelGroup::Castle;
    if (id >= 0x7000009 && id <= 0x700000b) return LevelGroup::Tower1;
    if (id == 0x700000c || id == 0x700000d) return LevelGroup::PowerStation;
    if ((id >= 0x7000011 && id <= 0x7000013) || id == 0x700004a) return LevelGroup::Tower2;
    if (id >= 0x7000014 && id <= 0x7000016) return LevelGroup::EvilBase;
    if (id == 0x700001b) return LevelGroup::SpaceStation;
    if ((id >= 0x7000021 && id <= 0x7000029) || id == 0x700004b || id == 0x700004c) return LevelGroup::Multiplayer;
    return LevelGroup::None;
}

bool is_sp_level(std::uint32_t id) {
    const LevelGroup g = level_group(id);
    return g != LevelGroup::None && g != LevelGroup::Multiplayer;
}

std::uint32_t level_id_from_bin(const std::string& name) {
    if (name.size() < 8) return 0;
    char* end = nullptr;
    const std::string stem = name.substr(0, 8);
    const unsigned long v = std::strtoul(stem.c_str(), &end, 16);
    if (end != stem.c_str() + 8) return 0;
    return std::uint32_t(v);
}

// ---- behaviour blob (behaviour_util_get 0x1c7a98) --------------------------------------------------------
BehaviourBlob parse_behaviour_blob(const SpTables& t, const std::array<std::uint32_t, 18>& b) {
    BehaviourBlob r;
    auto fail = [&]() {
        r = BehaviourBlob{};   // behaviour_util_get zeroes both sets and reports type/mode 0
        return r;
    };
    const std::uint32_t type = b[0];
    if (type >= 0x11) return fail();
    const std::uint32_t mode = b[1], count = b[2] & 0xffff, bits = b[2] >> 16;
    if (mode >= 6 || count - 1 >= 3 || bits - 1 > 0x5a) return fail();
    std::size_t p = 3;
    for (std::uint32_t i = 0; i < count; ++i) r.first.word[i] = b[p + i];
    p += count;
    const std::uint32_t type2 = b[p], w2 = b[p + 1], count2 = w2 & 0xffff, bits2 = w2 >> 16;
    p += 2;
    if (p + count2 > b.size()) return fail();
    if (count2 - 1 >= 3 || bits2 - 1 >= 0x5b || type2 >= 6) return fail();
    for (std::uint32_t i = 0; i < count2; ++i) r.second.word[i] = b[p + i];
    r.type = type;
    r.mode = mode;
    r.type2 = type2;
    r.valid = true;
    if (p + count2 >= b.size()) return r;
    const std::uint32_t tag = b[p + count2];
    if (std::int32_t(tag) >= 0) return r;   // no stats block
    p += count2 + 1;
    if ((tag & 0xffff0000) != 0x80010000) return r;
    const std::uint32_t stride = (tag & 0xffff) / 3;
    for (int blk = 0; blk < 3; ++blk) {
        std::uint32_t pos = 0;
        for (int f = 0; f < 8; ++f) {
            const std::size_t idx = p + (pos >> 5);
            const std::uint32_t word = idx < b.size() ? b[idx] : 0;
            r.stats[std::size_t(blk)][std::size_t(f)] =
                (word >> (pos & 31)) & ((1u << (t.stat_widths[std::size_t(f)] & 31)) - 1u) & 0xff;
            pos += t.stat_widths[std::size_t(f)];
        }
        p += stride;
    }
    r.has_stats = true;
    return r;
}

// ---- skins ----------------------------------------------------------------------------------------------
namespace {

struct SkinRow {
    std::uint32_t skin;
    SkinClass c;
};

// NDrone2_DefaultInit's switch on the skin id (0x14b300), {skin, {class(+0xd8), sub-class(+0xda), flags...}}.
// Fields: char_class, sub_class, flag15, flag14_off, flag1b, free_move, force_dtype, variant_override.
const std::vector<SkinRow>& skin_rows() {
    static const std::vector<SkinRow> rows = [] {
        std::vector<SkinRow> v;
        auto add = [&](std::initializer_list<std::uint32_t> ids, SkinClass c) {
            for (std::uint32_t id : ids) v.push_back({id, c});
        };
        add({0x5000004, 0x5000006, 0x5000008, 0x500004c}, {3, 0xc});
        add({0x500000e, 0x500000f}, {0x15, 2, true});
        add({0x5000010, 0x5000011, 0x5000052}, {0x16, 2, true});
        add({0x5000012, 0x5000013, 0x5000014}, {0x11, 10});
        add({0x5000015, 0x5000020, 0x5000022, 0x5000023, 0x5000024}, {0xd, 0xe, true});
        add({0x5000016, 0x500008f, 0x50000ac}, {0x14, 0xc});
        add({0x5000017, 0x5000018}, {0x12, 0xe, true});
        add({0x5000019, 0x5000021}, {0xe, 0xc});
        add({0x500001b, 0x500004f}, {9, 3});
        add({0x500001e}, {0xb, 7});
        add({0x5000025}, {5, 0xc});
        add({0x5000026, 0x5000066}, {0x10, 0xc, false, false, false, false, 0, 6});
        add({0x500002b, 0x500002d, 0x5000034}, {8, 1, true});
        add({0x500002e, 0x500002f, 0x5000030}, {2, 0xc});
        add({0x5000035}, {0xf, 0xc, false, false, false, false, 0x17});
        add({0x5000036, 0x5000037, 0x5000038, 0x5000039, 0x500003a}, {1, 0xc});
        add({0x500003b, 0x500003c}, {0xc, 0xe, true, true});
        add({0x5000053}, {0x17, 0xb, false, true, true});
        add({0x5000027, 0x5000028, 0x5000029, 0x500002a, 0x5000031, 0x5000032, 0x5000033}, {0x17, 0xb});
        add({0x5000054, 0x500006d, 0x500006e, 0x500007e}, {4, 0xc});
        add({0x500005e}, {0x13, 0xd, true});
        add({0x5000078, 0x5000094, 0x50000ba}, {0x18, 0x10, false, false, false, true});
        add({0x5000087, 0x5000090, 0x5000093}, {6, 0xc});
        add({0x500008c, 0x50000b1, 0x50000b2}, {7, 0xc});
        return v;
    }();
    return rows;
}

}  // namespace

SkinClass skin_class(std::uint32_t skin) {
    for (const SkinRow& r : skin_rows())
        if (r.skin == skin) return r.c;
    return {0, 0xc};   // `default:` of the switch
}

const std::vector<std::uint32_t>& known_skins() {
    static const std::vector<std::uint32_t> ids = [] {
        std::vector<std::uint32_t> v;
        for (const SkinRow& r : skin_rows()) v.push_back(r.skin);
        std::sort(v.begin(), v.end());
        return v;
    }();
    return ids;
}

std::uint32_t captain_skin(int char_class) {
    switch (char_class) {
    case 1: return 0x5000038;
    case 2: return 0x500002f;
    case 3: return 0x5000006;
    case 6: return 0x5000093;
    case 7: return 0x50000b2;
    default: return 0;
    }
}

int captain_drop_weapon(std::uint32_t level_id) {
    if (level_id >= 0x7000001 && level_id <= 0x7000008) return 0x35;
    if (level_id >= 0x7000009) return 0x34;
    return 0;
}

// ---- placed NPC ----------------------------------------------------------------------------------------
SpNpcSpec npc_spec_from_static(const nf::StaticInstance& s) {
    SpNpcSpec n;
    n.pos = {s.position[0], s.position[1], s.position[2]};
    n.yaw = s.euler[1];
    n.skin = s.param(0);
    n.script = s.param(1, 0x6000000);
    n.min_difficulty = s.param(2);
    n.start_channel = std::uint8_t(s.param(3));
    n.key4 = s.param(4);
    n.mode = s.param(5);
    n.alt_channel = std::uint8_t(s.param(6));
    n.key7 = s.param(7);
    n.alt_mode = s.param(8);
    n.voice_set = s.param(9);
    n.variant = s.param(10);
    n.kit = s.param(11);
    n.key12 = s.param(12);
    n.item = s.param(13);
    n.sight_profile = s.param(14);
    for (std::size_t i = 0; i < n.blob.size(); ++i) n.blob[i] = s.param(std::int32_t(15 + i));
    n.param_count = std::uint32_t(s.params.size());
    return n;
}

// ---- NDrone2_DefaultInit + mode/type settings ---------------------------------------------------------
namespace {

int patrol_or_idle(const Behaviour& b) {
    return (b.has(nf::drone::beh::kPatrolStart) || b.has(nf::drone::beh::kPatrolAlert)) ? 6 : 4;
}

// NDrone2_GetDTYPENEW 0x14c938 (`mode` = param_3; the roles are the blob's type/mode bytes).
int get_dtype_new(NpcResolved& r, const ResolveEnv& env, int type, int mode) {
    Behaviour& b = r.behaviour[0];
    if (mode == 0 && b.has(0x10)) return 0x10;
    switch (type) {
    case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7: case 8: case 9:
        if (env.level_id == kLevelCastleC && mode == 0) return b.has(0x4b) ? 0x16 : 0x13;
        break;
    case 10: case 11:
        if (mode == 0) {
            if (env.level_id == kLevelCastleC) {
                b.set(0x1f, 0);
                b.set(0x54, 1);
            }
            if (r.alertness < 1.0f) return r.flag15 ? 0x12 : 9;
            return 10;
        }
        break;
    case 12:
        if (mode == 0) return r.char_class == 0xc ? 0x11 : 0xc;
        break;
    case 13:
        if (mode == 0) {
            if (r.char_class == 0xf) return 0x17;
            if (env.level_id == kLevelCastleC) b.set(0x1f, 0);
            return r.alertness < 1.0f ? 9 : 10;
        }
        break;
    case 14:
        if (mode == 0) {
            if (b.has(0x4e)) return 5;
            return r.alertness >= 1.0f ? 10 : 9;
        }
        break;
    case 15:
        if (mode == 0) return 0xd;
        break;
    case 16:
        return 0x1e;
    }
    switch (mode) {
    case 2: return r.alertness >= 1.0f ? 0x19 : 2;
    case 3: return b.has(0x56) ? 0xe : 7;
    case 4: return 4;
    case 5: return 0x15;
    default: return 0;
    }
}

// The state list of "already alerted at spawn" remaps (GetDroneTypeAttackTypeFriend 0x14cbd8).
int alerted_initial_state(int state) {
    switch (state) {
    case 4: case 5: case 6: case 0x30: case 0x31: case 0x36: case 0x56: return 0x56;
    case 0x10: case 0x12: case 0x2e: case 0x32: case 0xb5: return 0x12;
    case 0x29: case 0x2a: return 0x2a;
    case 0xa6: case 0xa7: return 0xa7;
    default: return state;
    }
}

int alt_state_from_initial(int state) {
    switch (state) {
    case 4: case 5: case 6: case 8: case 0x30: case 0x31: case 0x36: case 0x37: case 0x85: case 0x89: return 0x56;
    case 0x10: case 0x2e: case 0x32: return 0x12;
    case 0x29: return 0x2a;
    case 0xa6: return 0xa7;
    default: return 0;
    }
}

// NDrone2_GetDroneTypeAttackTypeFriend (0x14cbd8).
void get_dtype_attack_type_friend(NpcResolved& r, const ResolveEnv& env) {
    const SpTables& t = *env.tables;
    if (r.role_mode == r.role_type2 || r.alt_dmode == 0x65) r.alt_dmode = 0;
    r.dtype_base = std::uint8_t(get_dtype_new(r, env, int(r.role_type), 0));
    r.dtype = std::uint8_t(get_dtype_new(r, env, int(r.role_type), int(r.role_mode)));
    if (r.alt_dmode == 0 || r.alt_dmode == 0x65) {
        r.dtype_alt = 0;
    } else if (r.alt_channel == 0 && r.alertness_floor >= 1.0f) {
        r.dtype_alt = 0;
    } else {
        r.dtype_alt = std::uint8_t(get_dtype_new(r, env, int(r.role_type), int(r.role_type2)));
    }
    switch (r.role_type) {
    case 10: case 11: case 13: case 14: r.side = 3; break;
    case 12: r.side = 2; break;
    default: r.side = 1;
    }
    const DroneTypeEntry& te = t.dtype[std::min<std::size_t>(r.dtype, kDtypeCount - 1)];
    if (te.initial_state < 0) {
        r.initial_state = 4;
        switch (r.dtype) {
        case 2: r.initial_state = r.alertness < 1.0f ? 0x29 : 0x2a; break;
        case 4: r.initial_state = 8; break;
        case 7: r.initial_state = 0x85; break;
        case 0x10: r.initial_state = 0x37; break;
        case 0x13: r.initial_state = 0x30; break;
        case 0x16: r.initial_state = 0x31; break;
        case 0x18: r.initial_state = 0x36; break;
        default: r.initial_state = patrol_or_idle(r.behaviour[0]);
        }
    } else {
        r.initial_state = te.initial_state;
    }
    if (r.dtype_alt == 0) {
        r.alt_state = 0;
    } else {
        r.alt_state = t.dtype[std::min<std::size_t>(r.dtype_alt, kDtypeCount - 1)].alt_state;
        if (r.alt_state == 0) r.alt_state = te.alt_state;
    }
    if (te.init_fn != 0 && (r.dtype == nf::drone::kDtypeSniper || r.dtype == nf::drone::kDtypeSniperAlert))
        r.flags_or |= nf::drone::flag::kStationary;   // NDrone2_initDTYPE_Sniper / SniperAlert
    if (r.alertness >= 1.0f) {
        if (r.char_class == 0xb) {
            r.starts_attacking = true;   // NDrone2_ChangeToAttackMode
            r.health = 100.0f;
            return;
        }
        r.initial_state = alerted_initial_state(r.initial_state);
    }
    if (r.dtype_alt == 0) {
        if (r.alt_state != 0) {
            if (r.alt_dmode == 0) r.alt_dmode = 100;
            return;
        }
        if (r.alt_channel == 0 && r.alertness_floor2 < 1.0f) return;
        r.alt_state = alt_state_from_initial(r.initial_state);
    }
    if (r.alt_state != 0 && r.alt_dmode == 0) r.alt_dmode = 100;
}

// NDrone2_DoTypeSettingsOLD (0x14d2f8) initial/alt state part.
void do_type_settings_old(NpcResolved& r, const ResolveEnv& env, int dtype) {
    const SpTables& t = *env.tables;
    r.dtype = std::uint8_t(dtype);
    const DroneTypeEntry& te = t.dtype[std::min<std::size_t>(dtype, kDtypeCount - 1)];
    if (te.initial_state < 0) {
        if (dtype == 4) r.initial_state = 8;
        else if (dtype == 2) r.initial_state = r.alertness < 1.0f ? 0x29 : 0x2a;
        else if (dtype == 7) r.initial_state = 0x85;
        else r.initial_state = patrol_or_idle(r.behaviour[0]);
    } else {
        r.initial_state = te.initial_state;
    }
    if (r.dtype_alt == 0) {
        r.alt_state = 0;
    } else {
        r.alt_state = t.dtype[std::min<std::size_t>(r.dtype_alt, kDtypeCount - 1)].alt_state;
        if (r.alt_state == 0) r.alt_state = te.alt_state;
    }
    if (te.init_fn != 0 && (dtype == nf::drone::kDtypeSniper || dtype == nf::drone::kDtypeSniperAlert))
        r.flags_or |= nf::drone::flag::kStationary;
    if (r.alertness < 1.0f) return;
    if (r.char_class == 0xb) {
        r.starts_attacking = true;
        r.health = 100.0f;
        return;
    }
    if (dtype == 5) return;
    if (r.alt_state != 0) {
        r.initial_state = r.alt_state;
        return;
    }
    const int s = r.initial_state;
    int keep = -1;
    if (s == 0x85) return;
    if (s < 0x86) {
        if (s == 10) return;
        keep = 8;
        if (s > 10) {
            keep = 0x2a;
            if (s == 0x1c) return;
        }
    } else {
        if (s == 0xb4) return;
        keep = 0x89;
        if (s > 0xb4) {
            if (s < 0xb8 && s > 0xb5) return;
            r.initial_state = 0x56;
            return;
        }
    }
    if (s == keep) return;
    r.initial_state = 0x56;
}

// NDrone2_DoModeSettingsOLD (0x14cfa0), DMODE < 0x24.
void do_mode_settings_old(NpcResolved& r, const SpNpcSpec& spec, const ResolveEnv& env, int& anim62) {
    const SpTables& t = *env.tables;
    const int mode = int(spec.mode);
    const DroneModeEntry& me = t.dmode[std::size_t(mode)];
    r.alertness = r.alertness_floor = me.alertness;
    r.behaviour = {};
    r.dtype = me.dtype;
    const std::uint32_t alt = r.alt_dmode;
    if (alt == 0 || alt == 0x65) r.dtype_alt = 0;
    else if (r.alt_channel == 0) r.dtype_alt = t.dmode[std::min<std::size_t>(alt, kDmodeCount - 1)].dtype;
    else r.dtype_alt = 0;
    r.side = me.side;
    t.apply_dmode_init(mode, r.behaviour[0]);
    Behaviour& b = r.behaviour[0];
    switch (r.char_class) {
    case 0x10: b.set(0x51, 1); break;
    case 0xd: b.set(0x4d, 1); b.set(0x4c, 1); b.set(0x52, 1); break;
    case 0x11: b.set(0x4c, 1); break;
    case 0x13: b.set(0x4c, 1); b.set(0x52, 1); b.set(0x55, 1); break;
    default: break;
    }
    for (int id : {0x18, 0x26, 0x27, 0x28, 0x29, 0x31, 0x32, 0x33, 0x4f, 0x1f, 0x3c, 0x30, 0x0a, 0x0c, 0x37, 0x38})
        b.set(id, 1);
    if (r.dtype == 0x11) {
        b.set(0x40, 1);
        b.set(0x53, 1);
        b.set(0x54, 1);
    }
    if (mode > 8 && (mode < 0xb || mode == 0x13)) {
        r.sub_class = mode < 0xb ? 1 : 0xc;
        anim62 = 0;
    }
}

// NDrone2_DoModeSettingsNEW (0x14c600).
void do_mode_settings_new(NpcResolved& r, const SpNpcSpec& spec, const ResolveEnv& env, DroneStats& stats) {
    static const float kAlertTable[4] = {0.0f, 0.0f, 0.66f, 1.0f};   // 0x3f28f5c3 = 0.66, 0x3f800000 = 1.0
    const SpTables& t = *env.tables;
    BehaviourBlob blob = parse_behaviour_blob(t, spec.blob);
    if (blob.has_stats) stats.rows[blob.type] = blob.stats;
    r.behaviour[0] = blob.first;
    r.behaviour[1] = blob.second;
    r.alertness = r.alertness_floor = kAlertTable[r.behaviour[0].get(0x20) & 3];
    r.alertness_floor2 = kAlertTable[r.behaviour[1].get(0x20) & 3];
    r.role_type = std::uint8_t(blob.type);
    r.role_mode = std::uint8_t(blob.mode);
    r.role_type2 = std::uint8_t(blob.type2);
    get_dtype_attack_type_friend(r, env);
    const auto& row = stats.rows[std::min<std::size_t>(blob.type, 16)][1];
    if (row[2] == 0) {
        r.accuracy = 5;
        r.health = 10.0f;
        r.aggression = r.stat_b6 = r.stat_b7 = r.stat_b8 = r.stat_b9 = r.stat_ba = 0;
    } else {
        r.health = float(row[2]);
        r.accuracy = std::uint8_t(row[0]);
        r.aggression = std::uint8_t(row[1]);
        r.stat_b6 = std::uint8_t(row[3]);
        r.stat_b7 = std::uint8_t(row[4]);
        r.stat_b8 = std::uint8_t(row[5]);
        r.stat_b9 = std::uint8_t(row[6]);
        r.stat_ba = std::uint8_t(row[7]);
    }
    if (r.char_class == 0x10) r.health = 100.0f;
    Behaviour& b = r.behaviour[0];
    if (env.level_id != kLevelCastleC) {
        if (env.level_id > kLevelCastleC && env.level_id < 0x700000b && env.level_id > 0x7000008) {
            b.set(0x1a, 1);
            b.set(0x56, 1);
        } else {
            b.set(0x1a, 0);
        }
    }
    switch (spec.script) {
    case 0x600011d: b.set(0x1d, 1); r.dtype = 0x17; break;
    case 0x600011f: r.dtype = 0x18; r.state_arg_13d = 0x19; break;
    case 0x6000124:
    case 0x6000126: r.dtype = 0x18; r.state_arg_13d = 0x11; break;
    case 0x600021f: b.set(0x38, 0); r.flags_or |= nf::drone::flag::kStationary; break;
    default: break;
    }
}

}  // namespace

NpcResolved resolve_npc(const SpNpcSpec& spec, const ResolveEnv& env, DroneStats& stats) {
    using nf::drone::flag::kStationary;
    NpcResolved r;
    const auto frand = [&](float range) { return env.frand ? env.frand(range) : 0.0f; };
    r.base_skin = spec.skin;
    std::uint32_t skin = spec.skin;
    if (env.level_id == kLevelEvilBase && skin == 0x50000b1) skin = 0x500008c;
    r.skin = skin;
    r.flags_or = 0x801e2;   // *(+0x4f8) & 0xffea05e2 | 0x801e2, |= 0x1a0 at the end
    r.script = spec.script;
    r.start_channel = spec.start_channel;
    r.alt_channel = spec.alt_channel;
    r.dmode = int(spec.mode & 0xffff);
    r.alt_dmode = int(spec.alt_mode & 0xffff);
    r.key4 = spec.key4;
    r.key7 = spec.key7;
    r.key12 = std::uint8_t(spec.key12);
    r.item = spec.item;
    if (spec.item != 0 && spec.item != 0x600021f) r.flags_or |= kStationary;

    // -- class / sub-class from the skin (NDrone2_DefaultInit switch) --
    const SkinClass sc = skin_class(skin);
    r.char_class = sc.char_class;
    r.sub_class = sc.sub_class;
    r.flag15 = sc.flag15;
    r.flag14 = !sc.flag14_off;
    r.invulnerable_anim = sc.flag1b;
    r.free_move = sc.free_move;
    r.variant = std::uint8_t(sc.variant_override ? sc.variant_override : spec.variant);
    if (sc.force_dtype) r.dtype = sc.force_dtype;

    // -- weapon / voice set (DIVars+0x58) --
    int anim62 = 1;
    if (spec.voice_set != 0) anim62 = int(spec.voice_set & 0xffff);
    switch (anim62) {
    case 2: case 6: case 10: case 0xb: case 0xe: case 0x10: case 0x3b: case 0x3c: case 0x3d: case 0x3e: case 0x3f:
    case 0x40:
        r.sub_class = r.char_class == 0xc ? 8 : 4;
        r.ammo = r.ammo_max = 0xc;
        r.bullet_damage_mod = 2.0f;
        break;
    case 0x12: case 0x14: case 0x15:
        if (r.char_class == 0xb) anim62 = 0x14;
        else r.sub_class = 6;
        r.ammo = r.ammo_max = 0x40;
        break;
    case 0x16: case 0x18: case 0x19: case 0x1a: case 0x1b:
        r.sub_class = 5;
        r.ammo = r.ammo_max = 0x40;
        break;
    case 0x1c: case 0x1d: case 0x2a: case 0x2b: case 0x2c: case 0x2d:
        r.sub_class = 9;
        r.ammo = r.ammo_max = 8;
        break;
    case 0x1e: case 0x1f: case 0x24: case 0x25:
        r.sub_class = 0x13;
        r.ammo = r.ammo_max = 5;
        break;
    case 0x30: case 0x32: case 0x33: case 0x6a: case 0x6e:
        if (r.char_class != 0x18) r.sub_class = 6;
        r.ammo = r.ammo_max = 10000;
        break;
    default:
        anim62 = 1;
        r.ammo = r.ammo_max = 0;
        break;
    }

    // -- head / armour kit (DIVars+0x60) --
    {
        const std::uint32_t hi = (spec.kit >> 4) & 0xffff, lo = spec.kit & 0xf;
        r.kit = std::uint16_t(hi);
        r.kit_colour = std::uint16_t(lo);
        switch (hi) {
        case 1: r.kit = 0x36; break;
        case 2: r.kit = 0x35; break;
        case 4: r.kit = 0x34; break;
        case 5: r.kit = std::uint16_t(0x3a + lo); r.kit_colour = 1; break;
        case 6: r.kit = 0x6c; r.kit_colour = 1; break;
        default: r.kit = r.kit_colour = 0;
        }
        if (env.level_id == kLevelEvilBase && r.kit < 0x37 && r.kit > 0x34) r.kit = 0x34;
    }

    // -- sight profile (DIVars+0x6c) --
    r.range_ec = 4.0f;
    r.min_cover_dist = 2.0f;
    r.sight_cone = 1.5707964f;
    r.d0 = 15;
    switch (spec.sight_profile) {
    case 1: {
        const float f0 = 12.0f + frand(3), f1 = frand(3);
        r.engage_dist = f0;
        r.sight_range = 12.0f;
        r.max_combat_dist = f0 + 1.0f + f1;
        break;
    }
    case 2: {
        const float f0 = 12.0f + frand(3), f1 = frand(3);
        r.engage_dist = f0;
        r.sight_range = 16.0f;
        r.max_combat_dist = f0 + 1.0f + f1;
        break;
    }
    case 3:
        r.engage_dist = 25.0f + frand(6);
        r.sight_range = 40.0f;
        r.max_combat_dist = 100.0f + frand(6);
        break;
    case 4:
        r.engage_dist = 75.0f + frand(8);
        r.sight_range = 1000.0f;
        r.max_combat_dist = 1000.0f;
        break;
    case 5: {
        const float f0 = 12.0f + frand(4), f1 = frand(4);
        r.engage_dist = f0;
        r.sight_range = 0.0f;
        r.max_combat_dist = f0 + 1.0f + f1;
        break;
    }
    default: {
        const float f0 = 12.0f + frand(4), f1 = frand(4);
        r.engage_dist = f0;
        r.sight_range = 24.0f;
        r.max_combat_dist = f0 + 1.0f + f1;
    }
    }
    if (env.level_id == kLevelTower2A) {
        if (r.sub_class == 9) { r.engage_dist = 5.0f; r.max_combat_dist = 8.0f; }
        else { r.engage_dist = 6.0f; r.max_combat_dist = 9.0f; }
    } else if (env.level_id == kLevelEvilBase) {
        if (r.sub_class != 5) {
            if (r.sub_class < 6 || r.sub_class != 9) r.engage_dist = 10.0f;
            else r.engage_dist = 7.0f;
        }
    }
    r.range_f4 = r.range_ec + frand(0);
    r.route_radius = r.free_move ? 4.0f : 2.0f;

    // -- mode / type --
    int anim62_mode = anim62;
    if (spec.mode < 0x24) do_mode_settings_old(r, spec, env, anim62_mode);
    else do_mode_settings_new(r, spec, env, stats);
    if (spec.mode < 0x24) do_type_settings_old(r, env, r.dtype);   // Drone+0x32 != 0: NDrone2_DoTypeSettingsOLD
    anim62 = anim62_mode;

    // -- captains (behaviour bits 9 / 5 / 8 by difficulty) --
    r.captain = false;
    Behaviour& b = r.behaviour[0];
    if (b.has(nf::drone::beh::kCaptainEasy) && env.difficulty == 1) r.captain = true;
    if (b.has(nf::drone::beh::kCaptainNormal) && env.difficulty == 2) r.captain = true;
    if (b.has(nf::drone::beh::kCaptainHard) && env.difficulty == 3) r.captain = true;
    if (r.captain) {
        if (const std::uint32_t cs = captain_skin(r.char_class)) {
            r.skin = cs;
            if (r.char_class == 2) r.captain_flag18 = true;
            r.health *= env.captain_health;
            r.bullet_damage_mod = env.captain_bullet_damage;
            r.accuracy = std::uint8_t(int(float(r.accuracy) * (1.0f / env.captain_bullet_accuracy)));
        } else {
            r.captain = false;
        }
    }
    if (r.role_type == 0xc) {   // DefaultInit: `Drone+0xc7 == 0xc` (ally role) drops the door interactions
        b.set(0x14, 0);
        b.set(0x15, 0);
        b.set(0x16, 0);
        if (r.char_class == 9) b.set(0x4c, 1);
    }

    // -- script id overrides of the type (abseilers): +0xc5 = 0x1c / 0x1b, sub-class 0xf, script cleared --
    if (spec.script == 0x6000892 || spec.script == 0x600089c) {
        r.dtype = spec.script == 0x6000892 ? 0x1c : 0x1b;
        r.sub_class = 0xf;
        r.script = 0x6000000;
    }

    // -- sniper animation class --
    if (r.dtype == nf::drone::kDtypeSniper) {
        if (r.sub_class == 5) r.sub_class = 0x12;
    } else if (r.dtype == nf::drone::kDtypeSniperAlert && r.sub_class == 5) {
        r.sub_class = 0x12;
    }

    r.starts_attacking = r.starts_attacking || r.initial_state == 0x56 || r.initial_state == 0x12;
    if (r.initial_state == 0x56 || r.initial_state == 0x12) {
        r.flags_or |= 0x44000000;
        r.alert_flags_or |= 0x10;
    }

    // -- class specific tail of DefaultInit --
    if (r.char_class == 0x13) {
        if (env.level_id == kLevelEvilBase) {
            r.d0 = 0x3c;
            r.sight_cone = 0.7853982f;
        }
        if (env.level_id == kLevelEvilBaseC) r.invulnerable_anim = true;
    } else if (r.char_class == 0x10) {
        if (env.level_id == kLevelEvilBase) r.health *= 3.0f;
    } else if (r.char_class == 0x18) {
        r.dtype = r.dtype_base = 0x1d;
        r.initial_state = 0xbd;
        r.dtype_alt = 0;
        r.alt_state = 0;
        r.health = env.astronaut_hits * (env.difficulty == 1 ? 1.0f : 2.0f);
        if (r.skin == 0x50000ba) r.health *= 3.0f;
    }
    if (r.dtype == 0xd) {
        b.set(0x3c, 1);
        b.set(0x36, 1);
        b.set(0x35, 1);
        r.health = 100.0f;
        r.route_radius = 8.0f;
        r.range_ec = 2.0f;
        r.accuracy = 0;
    } else if (r.dtype == 0x1b || r.dtype == 0x1c) {
        r.free_move = true;
        r.initial_state = 0x8e;
    }
    r.walk_scale = 0.075f;

    // -- single-player tail (MPSettings+388 == 0) --
    if (!env.multiplayer) {
        b.set(0x17, 0);
        r.flag1f = true;
        switch (r.dtype) {
        case 0xc:
            b.set(0x34, 0);
            b.set(0x36, 0);
            b.set(0x3b, 0);
            b.set(0x3a, 0);
            break;
        case 0xd: case 0x1b: case 0x1c: case 0x1d:
            r.flag1f = false;
            break;
        case 0x11:
            b.set(0x3b, 0);
            b.set(0x3a, 0);
            break;
        default: break;
        }
        r.fire_locked = anim62 >= 0x1c && anim62 < 0x1e;
        if ((env.level_id == 0x7000005 || env.level_id == 0x7000006) && spec.voice_set == 0x16) {
            b.set(0, 1);
            b.set(1, 1);
            b.set(6, 1);
            b.set(0x13, 0);
            b.set(0x41, 1);
            b.set(0x42, 1);
            r.engage_dist = 10.0f;
        }
    }
    r.impact_mask = 0x80;
    if (b.has(0x34) && !b.has(0x36)) r.impact_mask |= 0x200;
    if (b.has(0x3b) || b.has(0x3a)) r.impact_mask |= 0x100;
    return r;
}

}  // namespace nf::sp

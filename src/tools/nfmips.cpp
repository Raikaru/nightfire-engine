// nfmips: headless R5900 harness for ACTION.ELF (see docs/ee.md).
//
//   nfmips <elf> call <sym|addr> [i:<int> ...] [f:<float> ...] [--state p2s|--ram dump]
//                                  [--dump addr[:len] ...] [--steps N] [--no-hooks] [--trace-first N]
//                                  [--reg s0=0x.. ...] [--log-mem]
//   nfmips <elf> init [--steps N] [--dump-out file] [--check]
//   nfmips <elf> trace <sym|addr> [args...] [--steps N] [--state p2s|--ram dump]
//   nfmips <elf> disasm <addr> [count]
//   nfmips <elf> symbols [substring]
//   nfmips <elf> diff [--count N] [--seed N] [--state p2s]
//   nfmips <elf> float-selftest
//
// Integer args (decimal/0xhex, or i:..) fill a0-a3 then the o32 stack area;
// f:.. args fill f12, f13, ... . Results print as v0/v1 (hex+dec) and f0.
#include <array>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <zlib.h>

#include "assets/elf.hpp"
#include "assets/weapon_data.hpp"
#include "ee/machine.hpp"
#include "ee/ps2float_test.hpp"
#include "ee/disasm.hpp"
#include "game/actions.hpp"

namespace {

using nf::ee::CallArgs;
using nf::ee::Machine;
using nf::ee::s16;
using nf::ee::s32;
using nf::ee::s64;
using nf::ee::u16;
using nf::ee::u32;
using nf::ee::u64;
using nf::ee::u8;
using i16 = std::int16_t;

u32 parse_u32(const std::string& s) {
    size_t k = 0;
    unsigned long v = std::stoul(s, &k, 0);
    if (k != s.size()) throw std::runtime_error("bad number: " + s);
    return u32(v);
}

struct GlobalOpts {
    std::string state, ram, sym_file, fs_root;
    u64 steps = 0;  // 0 = default
    bool no_hooks = false;
};

Machine open_machine(const std::string& elf, const GlobalOpts& g) {
    Machine m(elf);
    if (!g.sym_file.empty()) m.load_sym_file(g.sym_file);
    if (!g.state.empty()) m.load_p2s(g.state);
    if (!g.ram.empty()) m.load_ram_dump(g.ram);
    if (!g.no_hooks) m.install_libc_hooks();
    if (!g.fs_root.empty()) m.set_fs_root(g.fs_root);
    return m;
}
bool vu0_macro_self_test() {
    nf::ee::Vu0 vu;
    vu.vf[2].w[0] = 0x40000000u;  // 2.0
    vu.vf[4].w[0] = 0x40400000u;  // 3.0
    vu.acc.w[0] = 0x41200000u;    // 10.0, must not participate in VMULA
    vu.macro(0x4bc221bcu);        // VMULAx.xyz ACC, vf4, vf2x
    const bool ok = vu.acc.w[0] == 0x40c00000u;
    std::printf("VU0 VMULAx ACC (3 * 2, replaces old ACC): %s\n", ok ? "PASS" : "FAIL");
    return ok;
}


u32 resolve(Machine& m, const std::string& s) {
    if (auto sym = m.symbol(s)) return sym->value;
    return parse_u32(s);
}

// Splits leading [i:|f:]INT/FLOAT positional args from --flags.
void parse_call_args(const std::vector<std::string>& argv, size_t& k, CallArgs& out,
                     std::vector<std::string>& rest) {
    for (; k < argv.size(); k++) {
        const std::string& a = argv[k];
        if (a == "--" || (!a.empty() && a[0] == '-' && !(a.size() > 1 && a[1] == ':'))) {
            rest.push_back(a);
            while (++k < argv.size()) rest.push_back(argv[k]);
            return;
        }
        if (a.starts_with("f:")) {
            const std::string v = a.substr(2);
            if (v.starts_with("0x") || v.starts_with("0X")) out.fbits(parse_u32(v));
            else out.f(std::stof(v));
        } else if (a.starts_with("i:")) {
            out.i(parse_u32(a.substr(2)));
        } else if (a.find_first_not_of("0123456789xXabcdefABCDEF") == std::string::npos ||
                   (a[0] == '-' && a.find_first_not_of("0123456789xXabcdefABCDEF", 1) == std::string::npos)) {
            // Bare number: float when it has '.'/'e', else integer.
            if (a.find_first_of(".eEnN") != std::string::npos &&
                a.find_first_of("xX") == std::string::npos) {
                if (a == "nan" || a == "inf" || a == "-inf") {
                    std::fprintf(stderr, "nfmips: use f:0x<bits> for non-finite floats\n");
                    std::exit(2);
                }
                out.f(std::stof(a));
            } else {
                out.i(std::stoul(a, nullptr, 0));
            }
        } else {
            rest.push_back(a);
            while (++k < argv.size()) rest.push_back(argv[k]);
            return;
        }
    }
}

// Writes applied to RAM after open_machine (lets call/trace take pointer args).
struct PokeSet {
    std::vector<std::pair<u32, std::vector<u8>>> pokes;
    void hex(const std::string& s) {
        // addr:hexbytes (e.g. 0x1e00000:0000803f)
        const auto p = s.find(':');
        if (p == std::string::npos) throw std::runtime_error("bad --poke: " + s);
        const std::string h = s.substr(p + 1);
        if (h.empty() || h.size() % 2) throw std::runtime_error("bad --poke hex: " + s);
        std::vector<u8> bytes;
        for (size_t q = 0; q < h.size(); q += 2)
            bytes.push_back(u8(std::stoul(h.substr(q, 2), nullptr, 16)));
        pokes.emplace_back(parse_u32(s.substr(0, p)), std::move(bytes));
    }
    void floats(const std::string& s) {
        // addr:v0,v1,... (decimal floats or 0x bit patterns)
        const auto p = s.find(':');
        if (p == std::string::npos) throw std::runtime_error("bad --vf32: " + s);
        std::vector<u8> bytes;
        size_t q = p + 1;
        while (q <= s.size()) {
            const auto e = s.find(',', q);
            const std::string t = s.substr(q, e == std::string::npos ? e : e - q);
            u32 w = 0;
            if (t.starts_with("0x") || t.starts_with("0X")) w = parse_u32(t);
            else {
                float v = std::stof(t);
                std::memcpy(&w, &v, 4);
            }
            bytes.push_back(u8(w));
            bytes.push_back(u8(w >> 8));
            bytes.push_back(u8(w >> 16));
            bytes.push_back(u8(w >> 24));
            if (e == std::string::npos) break;
            q = e + 1;
        }
        pokes.emplace_back(parse_u32(s.substr(0, p)), std::move(bytes));
    }
    void apply(Machine& m) const {
        for (const auto& [a, bytes] : pokes) m.mem.write_block(a, bytes.data(), bytes.size());
    }
};

// Initial GPR preset: --reg s0=0x1E00000 (names v0,v1,a0-a3,t0-t9,s0-s7,gp,sp,fp,ra or rN).
inline void parse_reg_preset(CallArgs& args, const std::string& s) {
    const auto p = s.find('=');
    if (p == std::string::npos) throw std::runtime_error("bad --reg: " + s);
    const std::string name = s.substr(0, p);
    const u64 v = std::stoull(s.substr(p + 1), nullptr, 0);
    static const std::map<std::string, int> kNames = {{"v0", 2}, {"v1", 3}, {"a0", 4}, {"a1", 5},
        {"a2", 6}, {"a3", 7}, {"t0", 8}, {"t1", 9}, {"t2", 10}, {"t3", 11}, {"t4", 12}, {"t5", 13},
        {"t6", 14}, {"t7", 15}, {"s0", 16}, {"s1", 17}, {"s2", 18}, {"s3", 19}, {"s4", 20}, {"s5", 21},
        {"s6", 22}, {"s7", 23}, {"t8", 24}, {"t9", 25}, {"gp", 28}, {"sp", 29}, {"fp", 30}, {"ra", 31}};
    auto it = kNames.find(name);
    int r = -1;
    if (it != kNames.end()) r = it->second;
    else if (name.size() > 1 && name[0] == 'r') r = std::stoi(name.substr(1));
    if (r < 0 || r >= 32) throw std::runtime_error("bad --reg: " + s);
    args.reg(r, v);
}

// Skips a function with v0 = 0 (stubbing HW-bound calls like audio).
void hook_noop(Machine& m, const std::string& sym) {
    if (!m.hook(sym, [](nf::ee::Cpu& c) {
            c.r[2].d[0] = 0;
            c.r[2].d[1] = 0;
            return true;
        }))
        throw std::runtime_error("no symbol to --noop: " + sym);
}
// Scratchpad staging (SPR_TO/SPR_FROM DMA programmed at 0x1000D000+; spins on
// the channel STR bit, so it traps without hardware). Observed semantics:
//   psiCopyToSP(a0 = RAM src, a1 = scratch offset, a2 = bytes): stage to
//     0x70000000 + (a1 & 0x3FFF), return 0x70000000 + a1 (callers read the
//     staged data through the return, e.g. Intersect_CylGeom).
//   psiCopyFromSP(a0 = scratch offset, a1 = RAM dst, a2 = bytes): copy back,
//     return ignored by the only caller (UpdateDrops), stubbed as 0.
void hook_sp_copy(Machine& m) {
    m.hook("psiCopyToSP__FPvUiUi", [](nf::ee::Cpu& c) {
        const u32 src = c.r[4].w[0], off = c.r[5].w[0], n = c.r[6].w[0];
        std::vector<u8> tmp(n);
        if (n) {
            c.mem.read_block(src, tmp.data(), n);
            c.mem.write_block(0x70000000u + (off & 0x3FFFu), tmp.data(), n);
        }
        c.r[2].d[0] = off + 0x70000000u;  // jr ra / move v0, a1 with a1 += base
        c.r[2].d[1] = 0;
        return true;
    });
    m.hook("psiCopyFromSP__FUiPvUi", [](nf::ee::Cpu& c) {
        const u32 off = c.r[4].w[0], dst = c.r[5].w[0], n = c.r[6].w[0];
        std::vector<u8> tmp(n);
        if (n) {
            c.mem.read_block(0x70000000u + (off & 0x3FFFu), tmp.data(), n);
            c.mem.write_block(dst, tmp.data(), n);
        }
        c.r[2].d[0] = 0;
        c.r[2].d[1] = 0;
        return true;
    });
}

// (Global flags are parsed inline per subcommand.)
void print_result(const nf::ee::CallResult& r) {
    float f;
    std::memcpy(&f, &r.f0, 4);
    std::printf("v0 = 0x%016llx (%lld)\n", (unsigned long long)r.v0, (long long)r.v0);
    std::printf("v1 = 0x%016llx (%lld)\n", (unsigned long long)r.v1, (long long)r.v1);
    std::printf("f0 = 0x%08x (%g)\n", r.f0, (double)f);
}

int cmd_call(Machine& m, u32 entry, const CallArgs& args, u64 steps, const std::vector<std::string>& dumps,
             u64 trace_first) {
    if (trace_first) {
        m.trace(entry, args, trace_first, stdout);
        return 0;
    }
    // RAM is kept (call_keep) so --dump observes the results.
    try {
        const auto r = m.call_keep(entry, args, steps ? steps : 10'000'000);
        print_result(r);
    } catch (const nf::ee::Trap& t) {
        std::fprintf(stderr, "TRAP %s at %08x (inst %08x): %s\n", nf::ee::trap_kind_name(t.kind), t.pc,
                     t.inst, t.what());
        return 1;
    }
    for (const auto& d : dumps) {
        const auto c = d.find(':');
        const u32 a = parse_u32(c == std::string::npos ? d : d.substr(0, c));
        const u32 n = c == std::string::npos ? 64 : parse_u32(d.substr(c + 1));
        const auto bytes = m.dump(a, n);
        std::printf("; dump %08x +%u:\n", a, n);
        for (u32 k = 0; k < n; k += 16) {
            std::printf("%08x:", a + k);
            for (u32 j = k; j < k + 16 && j < n; j++) std::printf(" %02x", bytes[j]);
            std::printf("\n");
        }
    }
    return 0;
}

// ---- init -----------------------------------------------------------------
// weapon_data rows parsed per docs/spec-weapons.md 2.1 / assets/weapon_data.hpp.
struct WeaponRow {
    u16 id, base;
    u8 sel, alt, cat;
    float dmg, blast, range;
    u16 cls;
    float autoaim;
    u8 pellets;
    float speed;
    u32 interval;
    u16 delay;
    float zoom;
    u8 ammo, rps;
    i16 clip;
    u8 rumble;
};

WeaponRow parse_row(const u8* b) {
    WeaponRow r{};
    auto rd16 = [&](int o) -> u16 { return u16(b[o] | (b[o + 1] << 8)); };
    auto rd32 = [&](int o) -> u32 {
        return u32(b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24));
    };
    auto rdfl = [&](int o) {
        float v;
        const u32 w = rd32(o);
        std::memcpy(&v, &w, 4);
        return v;
    };
    r.id = rd16(0);
    r.base = rd16(2);
    r.sel = b[4];
    r.alt = b[5];
    r.cat = b[6];
    r.blast = rdfl(8);
    r.dmg = rdfl(12);
    r.cls = rd16(16);
    r.autoaim = rdfl(20);
    r.pellets = b[24];
    r.range = rdfl(28);
    r.speed = rdfl(32);
    r.interval = rd32(64);
    r.delay = rd16(68);
    r.zoom = rdfl(136);
    r.ammo = b[144];
    r.rps = b[145];
    r.clip = i16(rd16(146));
    r.rumble = b[148];
    return r;
}

int cmd_init(const std::string& elf_path, u64 steps, const std::string& dump_out, bool check) {
    Machine m(elf_path);
    m.install_libc_hooks();
    try {
        m.run_static_init(steps ? steps : 500'000'000);
    } catch (const nf::ee::Trap& t) {
        std::fprintf(stderr, "TRAP %s at %08x (inst %08x): %s\n", nf::ee::trap_kind_name(t.kind), t.pc,
                     t.inst, t.what());
        std::fprintf(stderr, "%s", m.disasm_range(t.pc > 16 ? t.pc - 16 : 0, 8).c_str());
        return 1;
    }
    const u32 table = m.addr("weapon_data");
    auto sym = m.symbol("weapon_data");
    std::printf("weapon_data at 0x%08x, size %u\n", table, sym ? sym->size : 0);
    const auto bytes = m.dump(table, 115 * 268);
    int bad_ids = 0;
    for (int i = 0; i < 115; i++) {
        const WeaponRow r = parse_row(bytes.data() + size_t(i) * 268);
        if (r.id != i) {
            std::printf("row %d: BAD id %u\n", i, r.id);
            bad_ids++;
        }
    }
    std::printf("row ids: %s\n", bad_ids ? "MISMATCH" : "all 115 match index");
    for (int i : {1, 2, 6}) {
        const WeaponRow r = parse_row(bytes.data() + size_t(i) * 268);
        std::printf("row %d: base=%u sel=%u alt=%d cat=%u dmg=%g blast=%g cls=0x%x autoaim=%g pellets=%u "
                    "range=%g speed=%g interval=%u delay=%u zoom=%g ammo=%u rps=%u clip=%d rumble=%u\n",
                    i, r.base, r.sel, (int)(int8_t)r.alt, r.cat, (double)r.dmg, (double)r.blast, r.cls,
                    (double)r.autoaim, r.pellets, (double)r.range, (double)r.speed, r.interval, r.delay,
                    (double)r.zoom, r.ammo, r.rps, (int)r.clip, r.rumble);
    }
    if (!dump_out.empty()) {
        FILE* f = std::fopen(dump_out.c_str(), "wb");
        if (!f) throw std::runtime_error("cannot write " + dump_out);
        std::fwrite(bytes.data(), 1, bytes.size(), f);
        std::fclose(f);
        std::printf("wrote %zu bytes to %s\n", bytes.size(), dump_out.c_str());
    }
    if (check) {
        // Independent cross-check: WeaponTable::from_elf runs only the single
        // _GLOBAL_$I$weapon_data constructor on its own small interpreter.
        FILE* f = std::fopen(elf_path.c_str(), "rb");
        if (!f) throw std::runtime_error("cannot reopen " + elf_path);
        std::vector<std::uint8_t> img;
        char buf[65536];
        size_t n = 0;
        while ((n = std::fread(buf, 1, sizeof buf, f)) != 0) img.insert(img.end(), buf, buf + n);
        std::fclose(f);
        nf::WeaponTable t = nf::WeaponTable::from_elf(nf::Elf32(std::move(img)));
        int mism = 0;
        for (int i = 0; i < 115; i++) {
            const WeaponRow r = parse_row(bytes.data() + size_t(i) * 268);
            const nf::WeaponDef& w = t.weapon(i);
            bool ok = r.id == w.id && r.base == w.base && r.sel == w.selectable &&
                      (int8_t)r.alt == w.alt && r.cat == w.category && r.dmg == w.damage &&
                      r.blast == w.blast_radius && r.cls == w.class_flags && r.autoaim == w.autoaim &&
                      r.pellets == w.pellets && r.range == w.range && r.speed == w.speed &&
                      r.interval == w.fire_interval && r.delay == w.fire_delay && r.zoom == w.zoom_max &&
                      r.ammo == w.ammo_type && r.rps == w.rounds_per_shot && r.clip == w.clip_size &&
                      r.rumble == w.rumble;
            if (!ok) {
                std::printf("row %d differs from WeaponTable::from_elf\n", i);
                mism++;
            }
        }
        std::printf("WeaponTable cross-check: %s (%d mismatches)\n", mism ? "MISMATCH" : "all 115 rows agree",
                    mism);
        // Spot values from docs/spec-weapons.md 5.1.
        const nf::WeaponDef& w2 = t.weapon(2);
        const bool spec = w2.damage == 3.5f && w2.clip_size == 7 && w2.ammo_type == 1 && w2.range == 50.0f;
        std::printf("spec-weapons.md 5.1 row 2 (PP7 dmg=3.5 clip=7 ammo=1 range=50): %s\n",
                    spec ? "match" : "MISMATCH");
        if (mism || !spec || bad_ids) return 1;
    }
    return 0;
}

// ---- differential tests ----------------------------------------------------
// Reference transcriptions of the engine ports (kept next to the EE calls so
// the comparison is exact; cites the canonical source for each).
namespace ref {
// src/game/player.cpp accel_ramp (AccelFunc0__FfPffffff, 0x1A9218).
float accel_ramp(bool boosted, float stick, float speed, float mul, float steps, float centred, float state) {
    float target = speed * mul;
    const float step = (target - speed) / steps;
    if (!boosted) target = speed;
    if (state < target) {
        state += step;
        if (target < state) state = target;
    } else if (target < state) {
        state += step;
        if (state < target) state = target;
    }
    if (std::fabs(stick) <= centred) state = speed;
    const float limit = speed + speed * mul;
    if (state >= 0.0f) {
        if (limit < state) state = limit;
    } else if (state < -limit) {
        state = -limit;
    }
    return state;
}
// src/game/collision_world.cpp ray_box (Intersect_RayBox, 0x1E9008).
constexpr float kSlabFar = std::bit_cast<float>(0x7F7FC99Eu);
constexpr float kSlabNear = std::bit_cast<float>(0xFDCCA14Bu);
bool ray_box(const float o[3], const float dir[3], float radius, const float bmin[3], const float bmax[3],
             float& t) {
    if (bmin[0] <= o[0] && o[0] <= bmax[0] && bmin[1] <= o[1] && o[1] <= bmax[1] && bmin[2] <= o[2] &&
        o[2] <= bmax[2]) {
        t = 0;
        return true;
    }
    float near_[3], far_[3];
    for (int axis : {0, 2, 1}) {
        if (dir[axis] == 0.0f) {
            if (o[axis] < bmin[axis] || bmax[axis] < o[axis]) return false;
            near_[axis] = kSlabNear;
            far_[axis] = kSlabFar;
        } else {
            const float t0 = (bmin[axis] - o[axis]) / dir[axis];
            const float t1 = (bmax[axis] - o[axis]) / dir[axis];
            far_[axis] = std::max(t0, t1);
            near_[axis] = std::min(t0, t1);
            if (far_[axis] < 0.0f) return false;
        }
    }
    const float tnear = std::max({near_[0], near_[2], near_[1]});
    const float tfar = std::min({far_[0], far_[2], far_[1]});
    if (tfar < tnear) return false;
    t = tnear;
    return 0.0f <= tnear && tnear <= radius;
}
}  // namespace ref

int cmd_diff(const std::string& elf_path, int count, unsigned seed, const std::string& state) {
    Machine m(elf_path);
    m.install_libc_hooks();
    if (!state.empty()) m.load_p2s(state);
    std::mt19937 rng(seed);
    int failures = 0;

    // 1. AccelFunc0 (FPU add/sub/mul/div/compare path) vs player.cpp accel_ramp.
    {
        const u32 entry = m.addr("AccelFunc0__FfPffffff");
        const u32 state_ptr = m.alloc(4);
        int exact = 0;
        double max_abs = 0;
        std::uniform_real_distribution<float> stick(-2, 2), speed(0.05f, 8), mul(0.2f, 3), steps(1, 30),
            centred(0, 0.3f), full(0.5f, 1), st(-30, 30);
        for (int k = 0; k < count; k++) {
            const float a_stick = stick(rng), a_speed = speed(rng), a_mul = mul(rng),
                        a_steps = steps(rng), a_centred = centred(rng), a_full = full(rng),
                        a_state = st(rng);
            m.mem.write<u32>(state_ptr, Machine::fbits(a_state));
            CallArgs a;
            a.i(state_ptr).f(a_stick).f(a_speed).f(a_mul).f(a_steps).f(a_centred).f(a_full);
            const float got = m.call(entry, a).f0f();
            const float want =
                ref::accel_ramp(a_full <= std::fabs(a_stick), a_stick, a_speed, a_mul, a_steps, a_centred, a_state);
            u32 gb, wb;
            std::memcpy(&gb, &got, 4);
            std::memcpy(&wb, &want, 4);
            if (gb == wb) exact++;
            max_abs = std::max(max_abs, (double)std::fabs(got - want));
        }
        const bool pass = max_abs < 1e-5;
        std::printf("AccelFunc0 x%d: exact-bit %d/%d (%.2f%%), max|diff| %g -> %s\n", count, exact, count,
                    100.0 * exact / count, max_abs, pass ? "PASS" : "FAIL");
        if (!pass) failures++;
    }
    // 2. Intersect_RayBox (slab/max/min/div path) vs collision_world.cpp ray_box.
    {
        const u32 entry = m.addr("Intersect_RayBox__FPC11HITTEST_tagP7_VECTORT1Rf");
        const u32 ht = m.alloc(256), bmin = m.alloc(16), bmax = m.alloc(16), tout = m.alloc(4);
        int agree = 0, exact_t = 0;
        double max_t_abs = 0, max_t_rel = 0;
        std::uniform_real_distribution<float> c(-50, 50), d(-5, 5), r(0, 3);
        for (int k = 0; k < count; k++) {
            float o[3] = {c(rng), c(rng), c(rng)}, dir[3] = {d(rng), d(rng), d(rng)};
            if (k % 7 == 0) dir[k % 3] = 0;  // parallel-slab cases
            float mn[3] = {c(rng), c(rng), c(rng)}, size[3] = {std::fabs(c(rng)) + 0.01f,
                                                               std::fabs(c(rng)) + 0.01f,
                                                               std::fabs(c(rng)) + 0.01f};
            if (k % 2 == 0) {
                // Near-box origins: exercise the box faces and the inside-box (t = 0) path.
                std::uniform_real_distribution<float> n(-2, 2);
                o[0] = mn[0] + n(rng);
                o[1] = mn[1] + n(rng);
                o[2] = mn[2] + n(rng);
            }
            float mx[3] = {mn[0] + size[0], mn[1] + size[1], mn[2] + size[2]};
            const float dl = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
            const float radius = (k % 2) ? std::max(dl, 1.0f) : r(rng);
            for (int j = 0; j < 64; j++) m.mem.write<u32>(ht + 4 * j, 0);
            m.write_vec3(ht + 0x20, o[0], o[1], o[2]);
            m.write_vec3(ht + 0x40, dir[0], dir[1], dir[2]);
            m.write_f32(ht + 0x8C, radius);
            m.write_vec3(bmin, mn[0], mn[1], mn[2]);
            m.write_vec3(bmax, mx[0], mx[1], mx[2]);
            m.mem.write<u32>(tout, 0xDEADBEEFu);
            CallArgs a;
            a.i(ht).i(bmin).i(bmax).i(tout);
            const auto res = m.call_keep(entry, a);  // keep RAM: tout is read back below
            const bool got_hit = (res.v0 & 0xFFFFFFFF) != 0;
            u32 got_t = 0;
            std::memcpy(&got_t, m.mem.host(tout, 4), 4);
            float want_t = 0;
            const bool want_hit = ref::ray_box(o, dir, radius, mn, mx, want_t);
            u32 want_b = 0;
            std::memcpy(&want_b, &want_t, 4);
            if (got_hit == want_hit) {
                agree++;
                if (!got_hit || got_t == want_b) {
                    exact_t++;
                } else {
                    float got_tf = 0;
                    std::memcpy(&got_tf, &got_t, 4);
                    max_t_abs = std::max(max_t_abs, (double)std::fabs(got_tf - want_t));
                    max_t_rel = std::max(max_t_rel, (double)std::fabs(got_tf - want_t) /
                                                         std::max<double>(std::fabs(want_t), 1e-30));
                }
            } else if (failures == 0 && agree < 3) {
                std::printf("  mismatch o=(%g,%g,%g) d=(%g,%g,%g) r=%g: ee=%d ref=%d\n", (double)o[0],
                            (double)o[1], (double)o[2], (double)dir[0], (double)dir[1], (double)dir[2],
                            (double)radius, got_hit, want_hit);
            }
        }
        const bool pass = agree == count && max_t_abs < 1e-4;
        std::printf("Intersect_RayBox x%d: hit-agree %d/%d, t-exact %d/%d, max|t-diff| %g, max rel %g -> %s\n",
                    count, agree, count, exact_t, count, max_t_abs, max_t_rel, pass ? "PASS" : "REPORT");
        if (!pass) failures++;
    }
    // 3. Vec_Dist3D (VU0 VSUB/VMUL + FPU add/sqrt path) vs fp64 reference.
    {
        const u32 entry = m.addr("Vec_Dist3D__FPC7_VECTORT0");
        const u32 pa = m.alloc(16), pb = m.alloc(16);
        int exact = 0;
        double max_rel = 0;
        std::uniform_real_distribution<float> c(-100, 100);
        for (int k = 0; k < count; k++) {
            const double ax = c(rng), ay = c(rng), az = c(rng), bx = c(rng), by = c(rng), bz = c(rng);
            m.write_vec3(pa, (float)ax, (float)ay, (float)az);
            m.write_vec3(pb, (float)bx, (float)by, (float)bz);
            CallArgs a;
            a.i(pa).i(pb);
            const float got = m.call(entry, a).f0f();
            const double want =
                std::sqrt((ax - bx) * (ax - bx) + (ay - by) * (ay - by) + (az - bz) * (az - bz));
            float wf = (float)want;
            u32 gb, wb;
            std::memcpy(&gb, &got, 4);
            std::memcpy(&wb, &wf, 4);
            if (gb == wb) exact++;
            max_rel = std::max(max_rel, std::fabs(got - want) / std::max(want, 1e-30));
        }
        const bool pass = max_rel < 1e-6;
        std::printf("Vec_Dist3D x%d: exact-bit %d/%d (%.2f%%), max rel err %g -> %s\n", count, exact, count,
                    100.0 * exact / count, max_rel, pass ? "PASS" : "FAIL");
        if (!pass) failures++;
    }
    return failures ? 1 : 0;
}
// ---- DroneWeap_DoBulletAccuracy sweep ------------------------------------------------------------
// EE truth table for the Bots slice diff: per (inputs, scripted Rand draw) row prints hit + aim
// offset. Rand_FRand is hooked to return the scripted draw, so the port side replays the same values
// via weap::do_bullet_accuracy(Drone&, float). Hit is inferred as (offset == 0); the original returns
// void and zeroes obj+0x200 on a hit.
int cmd_diff_acc(const std::string& elf_path) {
    Machine m(elf_path);
    m.install_libc_hooks();
    const u32 entry = m.addr("DroneWeap_DoBulletAccuracy__FP10DCVars_tag");
    const u32 game_state = m.addr("GameState");
    const u32 dc = m.alloc(0x100), obj = m.alloc(0x280);
    float scripted_draw = 0;
    m.hook("Rand_FRand__Ff", [&](nf::ee::Cpu& c) {
        c.f[0] = Machine::fbits(scripted_draw);
        return true;
    });
    auto rd_f32 = [&](u32 va) {
        u32 w = 0;
        m.mem.read_block(va, &w, 4);
        float v;
        std::memcpy(&v, &w, 4);
        return std::make_pair(v, w);
    };
    std::printf("# DroneWeap_DoBulletAccuracy truth table (hit = offset xyz all zero)\n");
    std::printf("# GameState=0x%x DCVars=0x%x obj=0x%x\n", game_state, dc, obj);
    for (const char* g : {"DroneFiring_TooClose_Distance", "DroneFiring_TooClose_Accuracy",
                           "DroneFiring_TargetFirstMoved_Accuracy", "DroneFiring_TargetMoving_Accuracy",
                           "DroneFiring_TargetFirstStopped_Accuracy", "DroneFiring_Accuracy_Easy",
                           "DroneFiring_Accuracy_Normal", "DroneFiring_Accuracy_Hard"}) {
        const u32 a = m.addr(g);
        std::printf("# %s @0x%x = %g\n", g, a, (double)rd_f32(a).first);
    }
    std::printf("acc,dist,moving,first_moved,first_stopped,sub,diff,level,seen,lost,phase,bx,by,bz,draw,"
                "hit,ox,oy,oz,oxh,oyh,ozh\n");
    const int accs[] = {0, 5, 10, 20};
    const float dists[] = {1.0f, 5.0f, 30.0f};
    const float draws[] = {0.0f, 33.333f, 66.666f, 99.999f};
    const u32 levels[] = {0, 0x700000cu, 0x700000du, 0x700000eu};
    const float bearings[][3] = {{0, 0, 1}, {0.70710678f, 0, 0.70710678f}};
    std::vector<u8> clean_dc(0x100, 0), clean_obj(0x280, 0);
    for (int acc : accs)
        for (float dist : dists)
            for (int moving : {0, 1})
                for (int fm : {0, 1})
                    for (int fs : {0, 1})
                        for (u32 sub : {0u, 0x13u})
                            for (u32 diff : {0u, 1u, 2u, 3u, 4u})
                                for (u32 level : levels)
                                    for (u32 seen : {0u, 600u})
                                        for (int lost : {0, 1})
                                            for (u32 phase : {0u, 7777u})
                                                for (const auto& b : bearings)
                                                    for (float draw : draws) {
                                                        m.mem.write_block(dc, clean_dc.data(), 0x100);
                                                        m.mem.write_block(obj, clean_obj.data(), 0x280);
                                                        m.mem.write<u32>(dc + 4, obj);
                                                        // DCVars[0] is an obj_tag* (passed to FeetPos /
                                                        // control_link_object_to_cel); the wobble phase is
                                                        // read as *(DCVars[0] + 236), so point it at the
                                                        // drone blob and poke the phase there.
                                                        m.mem.write<u32>(dc, obj);
                                                        m.mem.write<u32>(obj + 0xec, phase);
                                                        m.mem.write<u32>(obj + 0x170, 0x12345678u);
                                                        m.mem.write<u8>(obj + 0x174, 0);
                                                        m.mem.write<u8>(obj + 0xb4, u8(acc));
                                                        m.write_f32(obj + 0x1a0, dist);
                                                        m.mem.write<u8>(obj + 0x1dd, u8(fm));
                                                        m.mem.write<u8>(obj + 0x1df, u8(fs));
                                                        m.mem.write<u8>(obj + 0x1dc, u8(moving));
                                                        m.mem.write<u16>(obj + 0xda, u16(sub));
                                                        m.mem.write<u32>(obj + 0x270, seen);
                                                        m.mem.write<u8>(obj + 0x41, u8(lost));
                                                        m.write_vec3(obj + 0x1c0, b[0], b[1], b[2]);
                                                        m.mem.write<u32>(game_state + 12, level);
                                                        // Ghidra ._NN_4_ fields are decimal: difficulty @
                                                        // +40 dec (0x28), frame @ +52 dec (0x34, aligned).
                                                        m.mem.write<u32>(game_state + 40, diff);
                                                        m.mem.write<u32>(game_state + 52, 100);
                                                        scripted_draw = draw;
                                                        CallArgs a;
                                                        a.i(dc);
                                                        // call_keep: the verdict lives in RAM (obj+0x200),
                                                        // which call() would roll back.
                                                        m.call_keep(entry, a);
                                                        const auto [ox, oxh] = rd_f32(obj + 0x200);
                                                        const auto [oy, oyh] = rd_f32(obj + 0x204);
                                                        const auto [oz, ozh] = rd_f32(obj + 0x208);
                                                        const int hit = (oxh == 0 && oyh == 0 && ozh == 0) ? 1 : 0;
                                                        std::printf("%d,%.9g,%d,%d,%d,%u,%u,0x%x,%u,%d,%u,"
                                                                    "%.9g,%.9g,%.9g,%.9g,%d,%.9g,%.9g,%.9g,0x%08x,0x%08x,0x%08x\n",
                                                                    acc, (double)dist, moving, fm, fs, sub, diff,
                                                                    level, seen, lost, phase, (double)b[0],
                                                                    (double)b[1], (double)b[2], (double)draw,
                                                                    hit, (double)ox, (double)oy, (double)oz,
                                                                    oxh, oyh, ozh);
                                                    }
    return 0;
}
// ---- psiInput_MapInputs, all eight controller styles --------------------------------------------------
// Runs the original on pad states (every button word with random sticks, plus stick extremes) for each style and
// compares the 40 action floats bit for bit against nf::map_inputs. Two players are mapped per call so the
// player-0-only start+cross action is covered too. Prints the mismatch count per style.
int cmd_diff_input(const std::string& elf_path) {
    Machine m(elf_path);
    m.install_libc_hooks();
    const u32 entry = m.addr("psiInput_MapInputs__FP15PlayerInput_tagUs");
    const u32 tslot = m.addr("tSlot");
    m.hook("MapPlayerToSlot__Fi", [](nf::ee::Cpu& c) {   // player i reads slot i
        c.r[2].d[0] = c.r[4].d[0];
        return true;
    });
    m.hook("sceMtapGetConnection", [](nf::ee::Cpu& c) {
        c.r[2].d[0] = 0;
        return true;
    });
    constexpr u32 kSlot = 0x180, kRecord = 0x158;
    const u32 records = m.alloc(2 * kRecord);
    std::mt19937 rng(0x1F2E3D4C);
    std::vector<std::pair<nf::PadState, nf::PadState>> pads;
    auto random_pad = [&](std::uint16_t buttons) {
        nf::PadState p;
        p.buttons = buttons;
        p.rx = u8(rng()), p.ry = u8(rng()), p.lx = u8(rng()), p.ly = u8(rng());
        return p;
    };
    for (u32 b = 0; b < 0x10000; ++b) pads.emplace_back(random_pad(u16(b)), random_pad(u16(rng())));
    static constexpr u8 kStick[] = {0x00, 0x01, 0x19, 0x40, 0x7E, 0x7F, 0x80, 0x81, 0xBF, 0xE6, 0xFE, 0xFF};
    for (u8 a : kStick)
        for (u8 b : kStick) {
            nf::PadState p = random_pad(u16(rng()));
            p.rx = a, p.ly = a, p.lx = b, p.ry = b;
            pads.emplace_back(p, random_pad(u16(rng())));
        }
    int total = 0;
    for (int style = 0; style < 8; ++style) {
        int mismatches = 0;
        for (const auto& [p0, p1] : pads) {
            const nf::PadState* player_pads[2] = {&p0, &p1};
            for (u32 i = 0; i < 2; ++i) {
                const nf::PadState& p = *player_pads[i];
                const u32 slot = tslot + i * kSlot;
                m.mem.write<u16>(slot + 0x122, nf::sony_pad_word(p.buttons));
                for (u32 copy : {0x128u, 0x148u}) {
                    m.mem.write<u8>(slot + copy + 0, p.rx);
                    m.mem.write<u8>(slot + copy + 1, p.ry);
                    m.mem.write<u8>(slot + copy + 2, p.lx);
                    m.mem.write<u8>(slot + copy + 3, p.ly);
                }
                const u32 rec = records + i * kRecord;
                for (u32 o = 0; o < kRecord; o += 4) m.mem.write<u32>(rec + o, 0);   // Input_Update's memset
                m.mem.write<u16>(rec + 0xE, u16(style));
            }
            CallArgs a;
            a.i(records).i(2);
            m.call_keep(entry, a);
            for (u32 i = 0; i < 2; ++i) {
                const auto want = nf::map_inputs(*player_pads[i], style, int(i));
                for (u32 k = 0; k < u32(nf::kActionCount); ++k) {
                    const u32 got = m.mem.read<u32>(records + i * kRecord + 0x14 + 4 * k);
                    if (got != Machine::fbits(want[k])) {
                        if (mismatches < 8)
                            std::printf("style %d player %u buttons %04x sticks %02x %02x %02x %02x: action %u ee %08x port %08x\n",
                                        style, i, player_pads[i]->buttons, player_pads[i]->rx, player_pads[i]->ry,
                                        player_pads[i]->lx, player_pads[i]->ly, k, got, Machine::fbits(want[k]));
                        ++mismatches;
                    }
                }
            }
        }
        std::printf("style %d: %zu pad pairs, %d mismatching action values\n", style, pads.size(), mismatches);
        total += mismatches;
    }
    std::printf("diff-input: %d mismatches -> %s\n", total, total == 0 ? "PASS" : "FAIL");
    return total == 0 ? 0 : 1;
}

// ---- PS2Sinf__Ff sweep --------------------------------------------------------------------------
int cmd_diff_sin(const std::string& elf_path) {
    Machine m(elf_path);
    m.install_libc_hooks();
    const u32 entry = m.addr("PS2Sinf__Ff");
    std::printf("# PS2Sinf__Ff truth table (in/out at %%.9g plus out bits)\n");
    std::printf("x,y,yh\n");
    constexpr double kPi = 3.141592653589793;
    auto emit = [&](float x) {
        CallArgs a;
        a.f(x);
        const u32 yb = m.call(entry, a).f0;
        float y;
        std::memcpy(&y, &yb, 4);
        std::printf("%.9g,%.9g,0x%08x\n", (double)x, (double)y, yb);
    };
    for (int k = 0; k <= 2000; k++) emit(float(-kPi / 2 + k * (kPi / 2000)));
    for (float x : {78.77f, -78.77f, 157.54f, -157.54f, 1000.0f, -1000.0f}) emit(x);
    return 0;
}
// ---- DroneFunc_CombatState sweep ------------------------------------------------------------------
// EE truth table for the Bots combat-state diff. One-factor-plus-combo rows over a base case that
// reaches the grenade leg; world-dependent services scripted + logged (CoverAvailable, Rand_Rand coin,
// CanDoAnimState, MoveToObject verdict, ChooseCombatMove if it traps, CallAnim/SetCombatMoveAnim
// record-only), everything else real (InTransition, getProperty, SetAngleToObj, fptoui, Vec fns).
// Outputs: return state id + branch evidence (anim/call logs, +0x3b fire byte, +0x9f0 untouched check).
int cmd_diff_combat(const std::string& elf_path) {
    Machine m(elf_path);
    m.install_libc_hooks();
    const u32 entry = m.addr("DroneFunc_CombatState__FP10DCVars_tag");
    const u32 game_state = m.addr("GameState");
    const u32 dc = m.alloc(0x100), drone = m.alloc(0xC00), obj = m.alloc(0x300);
    const u32 opp = m.alloc(0x300), params = m.alloc(0x100);
    const u32 rng_state[4] = {0x30D0A0u, 0x30D0A4u, 0x30D0A8u, 0x30D0ACu};
    int coin_script = 0, cover_script = 0, animok_script = 1, moveverdict = 0, choosemove_ret = 0;
    bool hook_choose = false;
    std::vector<std::string> call_log, rand_log;
    auto log_call = [&](const char* fmt, auto v) {
        char b[64];
        std::snprintf(b, sizeof b, fmt, v);
        call_log.emplace_back(b);
    };
    m.hook("Rand_Rand__FUi", [&](nf::ee::Cpu& c) {
        char b[64];
        std::snprintf(b, sizeof b, "rand(%u)->%d", c.r[4].w[0], coin_script);
        rand_log.emplace_back(b);
        c.r[2].d[0] = u64(coin_script);
        c.r[2].d[1] = 0;
        return true;
    });
    m.hook("NDrone2_CoverAvailable__FP10DCVars_tagSc", [&](nf::ee::Cpu& c) {
        log_call("cover(%u)", c.r[5].w[0]);
        c.r[2].d[0] = u64(cover_script);
        c.r[2].d[1] = 0;
        return true;
    });
    m.hook("DroneAnim_CanDoAnimState__FP10DCVars_tagss", [&](nf::ee::Cpu& c) {
        log_call("animok(0x%x)", c.r[5].w[0]);
        c.r[2].d[0] = u64(animok_script);
        c.r[2].d[1] = 0;
        return true;
    });
    m.hook("NDrone2_MoveToObject__FP10DCVars_tagP7obj_tagfSc", [&](nf::ee::Cpu& c) {
        float f;
        u32 fb = c.f[12];
        std::memcpy(&f, &fb, 4);
        char b[64];
        std::snprintf(b, sizeof b, "moveto(%g)", (double)f);
        call_log.emplace_back(b);
        c.r[2].d[0] = u64(moveverdict);
        c.r[2].d[1] = 0;
        return true;
    });
    m.hook("NDrone2_ChooseCombatMove__FP10DCVars_tag", [&](nf::ee::Cpu& c) {
        (void)c;
        if (!hook_choose) return false;
        call_log.emplace_back("choosemove*");
        c.r[2].d[0] = u64(choosemove_ret);
        c.r[2].d[1] = 0;
        return true;
    });
    m.hook("DroneAnim_CallAnim__FUiUisUifScP10DCVars_tag", [&](nf::ee::Cpu& c) {
        log_call("callanim(%u)", c.r[5].w[0]);
        c.r[2].d[0] = 0;
        c.r[2].d[1] = 0;
        return true;
    });
    m.hook("DroneAnim_SetCombatMoveAnim__FP10DCVars_tagf", [&](nf::ee::Cpu& c) {
        float f;
        u32 fb = c.f[12];
        std::memcpy(&f, &fb, 4);
        char b[64];
        std::snprintf(b, sizeof b, "setmoveanim(%g)", (double)f);
        call_log.emplace_back(b);
        c.r[2].d[0] = 0;
        c.r[2].d[1] = 0;
        return true;
    });
    auto rd_u32 = [&](u32 va) {
        u32 w = 0;
        m.mem.read_block(va, &w, 4);
        return w;
    };
    (void)rd_u32;
    std::printf("# DroneFunc_CombatState truth table\n");
    std::printf("# dc=0x%x drone=0x%x obj=0x%x opp=0x%x params=0x%x frame=10000\n", dc, drone, obj, opp,
                params);
    std::printf("case,opp,dist,engage,idle,flags,sight,lost,bnd,ammo,coin,animok,cover,stamp,level,anim,"
                "p88,d8,move,lastseen,ret,fire3b,calls,rand\n");
    struct Row {
        const char* name;
        int use_opp;
        float dist;
        float engage;
        int idle;
        u32 flags;
        int sight;
        int lost;
        int bnd;
        int ammo;
        int coin;
        int animok;
        int cover;
        u32 stamp;
        u32 level;
        u32 anim;
        float p88;
        int d8;
        int move;
        u32 lastseen;
    };
    // engage gates the case-0/2 legs (engage <= dist returns early); lastseen gates the no-opponent
    // proceed path (+0x238 + 30 >= frame 10000). Grenade legs need engage > dist with dist in [8,20).
    const Row rows[] = {
        {"base", 1, 10.0f, 8.0f, 0x58, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0x02000000u, 0.0f, 0, 0, 0},
        {"noopp0", 0, 10.0f, 8.0f, 0x58, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0x02000000u, 0.0f, 0, 0, 0},
        {"noopp9990", 0, 10.0f, 8.0f, 0x58, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0x02000000u, 0.0f, 0, 0, 9990},
        {"idle68", 1, 10.0f, 8.0f, 0x68, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0x02000000u, 0.0f, 0, 0, 0},
        {"idle70", 1, 10.0f, 8.0f, 0x70, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0x02000000u, 0.0f, 0, 0, 0},
        {"idle00", 1, 10.0f, 8.0f, 0x00, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0x02000000u, 0.0f, 0, 0, 0},
        {"idle5a", 1, 10.0f, 8.0f, 0x5a, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0x02000000u, 0.0f, 0, 0, 0},
        {"dist5", 1, 5.0f, 8.0f, 0x58, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0x02000000u, 0.0f, 0, 0, 0},
        {"dist25", 1, 25.0f, 8.0f, 0x58, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0x02000000u, 0.0f, 0, 0, 0},
        {"stat", 1, 10.0f, 8.0f, 0x58, 0x10, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0x02000000u, 0.0f, 0, 0, 0},
        {"sight4", 1, 10.0f, 25.0f, 0x58, 0, 4, 0, 0, 1, 0, 1, 0, 0, 0, 0x02000000u, 0.0f, 0, 0, 0},
        {"lost16", 1, 10.0f, 25.0f, 0x58, 0, 0, 16, 0, 1, 0, 1, 0, 0, 0, 0x02000000u, 0.0f, 0, 0, 0},
        {"bnd2", 1, 10.0f, 25.0f, 0x58, 0, 0, 0, 2, 1, 0, 1, 0, 0, 0, 0x02000000u, 0.0f, 0, 0, 0},
        {"g_noammo", 1, 10.0f, 25.0f, 0x70, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0x02000000u, 0.0f, 0, 0, 0},
        {"g_coin1", 1, 10.0f, 25.0f, 0x70, 0, 0, 0, 0, 1, 1, 1, 0, 0, 0, 0x02000000u, 0.0f, 0, 0, 0},
        {"g_animok0", 1, 10.0f, 25.0f, 0x70, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0x02000000u, 0.0f, 0, 0, 0},
        {"g_base", 1, 10.0f, 25.0f, 0x70, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0x02000000u, 0.0f, 0, 0, 0},
        {"g_cover1", 1, 10.0f, 25.0f, 0x70, 0, 0, 0, 0, 1, 0, 1, 1, 0, 0, 0x02000000u, 0.0f, 0, 0, 0},
        {"g_stampmax", 1, 10.0f, 25.0f, 0x70, 0, 0, 0, 0, 1, 0, 1, 0, 0xFFFFFFFFu, 0, 0x02000000u, 0.0f,
         0, 0, 0},
        {"g_d8", 1, 10.0f, 25.0f, 0x70, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0x02000000u, 0.0f, 9, 0, 0},
        {"c2lost0", 1, 10.0f, 8.0f, 0x5a, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0x02000000u, 0.0f, 0, 0, 0},
        {"c2lost16", 1, 10.0f, 8.0f, 0x5a, 0, 0, 16, 0, 1, 0, 1, 0, 0, 0, 0x02000000u, 0.0f, 0, 0, 0},
        {"c2mv2", 1, 10.0f, 8.0f, 0x5a, 0, 0, 16, 0, 1, 0, 1, 0, 0, 0, 0x02000000u, 0.0f, 0, 2, 0},
        {"c2mv3", 1, 10.0f, 8.0f, 0x5a, 0, 0, 16, 0, 1, 0, 1, 0, 0, 0, 0x02000000u, 0.0f, 0, 3, 0},
        {"c2mv5", 1, 10.0f, 8.0f, 0x5a, 0, 0, 16, 0, 1, 0, 1, 0, 0, 0, 0x02000000u, 0.0f, 0, 5, 0},
        {"c2mv9", 1, 10.0f, 8.0f, 0x5a, 0, 0, 16, 0, 1, 0, 1, 0, 0, 0, 0x02000000u, 0.0f, 0, 9, 0},
        {"c2mv10", 1, 10.0f, 8.0f, 0x5a, 0, 0, 16, 0, 1, 0, 1, 0, 0, 0, 0x02000000u, 0.0f, 0, 10, 0},
        {"c2mvb", 1, 10.0f, 8.0f, 0x5a, 0, 0, 16, 0, 1, 0, 1, 0, 0, 0, 0x02000000u, 0.0f, 0, 0xb, 0},
        {"c2mvc", 1, 10.0f, 8.0f, 0x5a, 0, 0, 16, 0, 1, 0, 1, 0, 0, 0, 0x02000000u, 0.0f, 0, 0xc, 0},
        {"c2mv99", 1, 10.0f, 8.0f, 0x5a, 0, 0, 16, 0, 1, 0, 1, 0, 0, 0, 0x02000000u, 0.0f, 0, 99, 0},
        {"c2lvl", 1, 10.0f, 8.0f, 0x5a, 0, 0, 16, 0, 1, 0, 1, 0, 0, 0x7000005u, 0x02000000u, 0.0f, 0, 0,
         0},
        {"c2lvl3", 1, 10.0f, 8.0f, 0x5a, 0, 0, 16, 0, 1, 0, 1, 0, 0, 0x7000005u, 0x02000000u, 3.0f, 0, 0,
         0},
        {"c2mv2p3", 1, 10.0f, 8.0f, 0x5a, 0, 0, 16, 0, 1, 0, 1, 0, 0, 0x7000005u, 0x02000000u, 3.0f, 0,
         2, 0},
        {"animblk", 1, 10.0f, 25.0f, 0x70, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0x02000100u, 0.0f, 0, 0, 0},
        {"mask68pass", 1, 10.0f, 25.0f, 0x68, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0x000A0002u, 0.0f, 0, 0, 0},
        {"mask68fail", 1, 10.0f, 25.0f, 0x68, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0x02000000u, 0.0f, 0, 0, 0},
        {"mask68noammo", 1, 10.0f, 8.0f, 0x68, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0x000A0002u, 0.0f, 0, 0, 0},
        {"mask70pass", 1, 10.0f, 25.0f, 0x70, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0x000A0002u, 0.0f, 0, 0, 0},
        {"d8-9", 1, 10.0f, 25.0f, 0x58, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0x02000000u, 0.0f, 9, 0, 0},
        {"level5", 1, 10.0f, 25.0f, 0x58, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0x7000005u, 0x02000000u, 0.0f, 0, 0,
         0},
        {"p88-3", 1, 10.0f, 25.0f, 0x58, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0x02000000u, 3.0f, 0, 0, 0},
    };
    std::vector<u8> clean_drone(0xC00, 0), clean_small(0x300, 0), clean_dc(0x100, 0);
    for (const Row& r : rows) {
        m.mem.write_block(drone, clean_drone.data(), 0xC00);
        m.mem.write_block(obj, clean_small.data(), 0x300);
        m.mem.write_block(opp, clean_small.data(), 0x300);
        m.mem.write_block(dc, clean_dc.data(), 0x100);
        m.mem.write<u32>(dc, obj);
        m.mem.write<u32>(dc + 4, drone);
        m.mem.write<u32>(dc + 12, 0xEEEEEEEEu);
        m.mem.write<u32>(drone + 0x170, r.use_opp ? opp : 0);
        m.mem.write<u32>(drone + 0x174, 0);
        m.mem.write<u16>(drone + 0x10c, u16(r.idle));
        m.write_f32(drone + 0xf0, r.engage);
        m.mem.write<u32>(drone + 0x4f8, r.flags);
        m.mem.write<u32>(drone + 0x4d8, 0);
        m.mem.write<u8>(drone + 0x3b, 0);
        m.mem.write<u32>(drone + 0x238, r.lastseen);
        m.mem.write<u32>(drone + 0x274, u32(r.lost));
        m.mem.write<u32>(drone + 0x228, u32(r.sight));
        m.mem.write<u32>(drone + 0x31c, u32(r.bnd));
        m.mem.write<u32>(drone + 0x568, r.anim);
        m.mem.write<u32>(drone + 0x854, params);
        m.mem.write<u32>(drone + 0x11c, r.stamp);
        m.mem.write<u16>(drone + 0xd8, u16(r.d8));
        m.write_f32(drone + 0x1a0, r.dist);
        m.mem.write<u16>(drone + 0xbcc, u16(r.ammo));
        m.mem.write<u16>(drone + 0xbce, u16(r.ammo));
        // +0xBBC gates the 0x68 path (bgtz): SpExt.ammo low half in the port tree.
        m.mem.write<u16>(drone + 0xbbc, u16(r.ammo));
        m.write_f32(params + 0x88, r.p88);
        m.write_vec3(obj + 0x30, 0, 0, 0);
        m.write_vec3(opp + 0x30, 5, 0, 5);
        m.mem.write<u32>(game_state + 12, r.level);
        m.mem.write<u32>(game_state + 52, 10000);
        m.mem.write<u32>(rng_state[0], 1);
        m.mem.write<u32>(rng_state[1], 2);
        m.mem.write<u32>(rng_state[2], 3);
        m.mem.write<u32>(rng_state[3], 4);
        coin_script = r.coin;
        cover_script = r.cover;
        animok_script = r.animok;
        moveverdict = r.move;
        call_log.clear();
        rand_log.clear();
        try {
            CallArgs a;
            a.i(dc);
            const auto res = m.call_keep(entry, a, 50'000'000);
            std::string calls, rands;
            for (const auto& s : call_log) calls += (calls.empty() ? "" : " ") + s;
            for (const auto& s : rand_log) rands += (rands.empty() ? "" : " ") + s;
            if (calls.empty()) calls = "-";
            if (rands.empty()) rands = "-";
            std::printf("%s,%d,%g,%g,0x%x,0x%x,%d,%d,0x%x,%d,%d,%d,%d,0x%x,0x%x,0x%08x,%g,%d,%d,%u,"
                        "ret=0x%x,fire=0x%x,calls=%s,rand=%s\n",
                        r.name, r.use_opp, (double)r.dist, (double)r.engage, r.idle, r.flags, r.sight,
                        r.lost, r.bnd, r.ammo, r.coin, r.animok, r.cover, r.stamp, r.level, r.anim,
                        (double)r.p88, r.d8, r.move, r.lastseen, int(res.v0 & 0xFFFFFFFF),
                        m.mem.read<u8>(drone + 0x3b), calls.c_str(), rands.c_str());
        } catch (const nf::ee::Trap& t) {
            std::printf("%s,TRAP %s at %08x\n", r.name, nf::ee::trap_kind_name(t.kind), t.pc);
        }
    }
    return 0;
}
// ---- MP combat differential (MpCombat) ---------------------------------------------------------------
// EE truth tables for multiplayer combat accuracy:
//  1. spread draws: per-shot draw triples in Bullet_init order (Rand_FRand_MVar2(2A,A) then
//     Rand_FRand(2pi), Rand_FRand(pi)) from the boot seed, plus Vec_Spherical_2_Cartesian(r,theta,phi)
//     output vectors for fixed inputs (pins the polar-axis convention the port must match).
//  2. Player_HandlePain matrix over (damage, part, type, armour, health, MP/SP flags, difficulty,
//     location-damage, rapid): the damage path of every MP weapon (docs/spec-weapons.md 8.1).
// Sound/rumble/RNG side effects are hooked and logged so the host replica (nfdump diff-mpweap) can
// compare them exactly; GameFlow_GetState is stubbed to 2 (match in play).
int cmd_diff_mpweap(const std::string& elf_path) {
    Machine m(elf_path);
    m.install_libc_hooks();
    auto as_float = [](u32 w) {
        float v;
        std::memcpy(&v, &w, 4);
        return v;
    };
    // ---- section 1: spread draws + spherical convention -------------------------------------------
    std::printf("# section=spread order=MVar2(2A,A),FRand(2pi),FRand(pi) seed=boot\n");
    std::printf("shot,A,mvar2,mvar2h,theta,thetah,phi,phih\n");
    {
        const u32 mvar2 = m.addr("Rand_FRand_MVar2__Fff");
        const u32 frand = m.addr("Rand_FRand__Ff");
        const float bases[] = {0.0f, 3.0f, 10.0f, 32.0f, 40.0f, 100.0f};
        int shot = 0;
        for (float A : bases) {
            for (int rep = 0; rep < 4; rep++) {
                CallArgs a;
                a.f(2.0f * A).f(A);
                const u32 rb = m.call_keep(mvar2, a).f0;
                CallArgs b;
                b.f(6.283185307f);
                const u32 tb = m.call_keep(frand, b).f0;
                CallArgs c;
                c.f(3.141592654f);
                const u32 pb = m.call_keep(frand, c).f0;
                std::printf("%d,%.9g,%.9g,0x%08x,%.9g,0x%08x,%.9g,0x%08x\n", shot++, (double)A,
                            (double)as_float(rb), rb, (double)as_float(tb), tb, (double)as_float(pb),
                            pb);
            }
        }
    }
    std::printf("# section=spherical in=(r,theta,phi) out=vec3+bith\n");
    std::printf("r,theta,phi,x,y,z,xh,yh,zh\n");
    {
        const u32 entry = m.addr("Vec_Spherical_2_Cartesian__FP7_VECTORfff");
        const u32 out = m.alloc(16);
        const float rs[] = {0.14f, 0.5f};
        const float ths[] = {0.0f, 1.0f, 3.141592654f};
        const float phs[] = {0.0f, 1.0f, 3.141592654f};
        for (float r : rs)
            for (float th : ths)
                for (float ph : phs) {
                    CallArgs a;
                    a.i(out).f(r).f(th).f(ph);
                    m.call_keep(entry, a);
                    u32 w[4] = {0, 0, 0, 0};
                    m.mem.read_block(out, w, 16);
                    std::printf("%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,0x%08x,0x%08x,0x%08x\n", (double)r,
                                (double)th, (double)ph, (double)as_float(w[0]),
                                (double)as_float(w[1]), (double)as_float(w[2]), w[0], w[1], w[2]);
                }
    }
    // ---- section 2: Player_HandlePain --------------------------------------------------------------
    const u32 entry = m.addr("Player_HandlePain__FP7obj_tagP6BLDatafUss");
    const u32 obj = m.alloc(0x300), bldat = m.alloc(0x1000), obj220 = m.alloc(0x100);
    auto sym_addr = [&](const char* name, u32 fallback) {
        if (auto s = m.symbol(name)) return s->value;
        return fallback;
    };
    const u32 mp_flag = sym_addr("dword_2A4920", 0x2A4920u);
    const u32 team_flag = sym_addr("dword_2A4924", 0x2A4924u);
    const u32 difficulty = sym_addr("dword_2A3790", 0x2A3790u);
    const u32 loc_flag = sym_addr("dword_2A4968", 0x2A4968u);
    const u32 rapid_flag = sym_addr("dword_2A495C", 0x2A495Cu);
    const u32 cheat_b = sym_addr("byte_26FCEF", 0x26FCEFu);
    const u32 cheat_info = sym_addr("CheatInfo", 0u);
    std::printf("# section=pain obj=0x%x bldat=0x%x obj220=0x%x\n", obj, bldat, obj220);
    for (const char* g : {"Plr_DMod_Multi", "Plr_DMod_Head", "Plr_DMod_LowerLimb", "Plr_DMod_UpperLimb",
                           "Plr_DMod_Easy", "Plr_DMod_Normal", "Plr_DMod_Hard"}) {
        if (auto s = m.symbol(g)) {
            u32 w = 0;
            m.mem.read_block(s->value, &w, 4);
            std::printf("# tuning %s @0x%x = %.9g (0x%08x)\n", g, s->value, (double)as_float(w), w);
        }
    }
    int coin_script = 0;
    std::vector<std::string> snd_log, rumble_log;
    m.hook("GameFlow_GetState__Fv", [](nf::ee::Cpu& c) {
        c.r[2].d[0] = 2;
        c.r[2].d[1] = 0;
        return true;
    });
    m.hook("Sound_Play__FUifsUi", [&](nf::ee::Cpu& c) {
        char b[64];
        std::snprintf(b, sizeof b, "play(%u)", c.r[4].w[0]);
        snd_log.emplace_back(b);
        c.r[2].d[0] = 0;
        c.r[2].d[1] = 0;
        return true;
    });
    m.hook("Sound_Play3D__FUiP7_VECTORfffsUii", [&](nf::ee::Cpu& c) {
        char b[64];
        std::snprintf(b, sizeof b, "play3d(%u)", c.r[4].w[0]);
        snd_log.emplace_back(b);
        c.r[2].d[0] = 0;
        c.r[2].d[1] = 0;
        return true;
    });
    m.hook("Input_RumbleStart__FUsii", [&](nf::ee::Cpu& c) {
        char b[64];
        std::snprintf(b, sizeof b, "rumble(pad%u,%u,%d)", c.r[4].w[0], c.r[5].w[0],
                      int(c.r[6].w[0]));
        rumble_log.emplace_back(b);
        c.r[2].d[0] = 0;
        c.r[2].d[1] = 0;
        return true;
    });
    m.hook("Rand_Rand__FUi", [&](nf::ee::Cpu& c) {
        c.r[2].d[0] = u64(coin_script);
        c.r[2].d[1] = 0;
        return true;
    });
    // PlrStat_OkToUpdate gates the health write on Mission_Status (0 with no mission loaded); stub it to 1
    // to replicate in-mission behaviour (the health write + PlrStat_LogHealth then run for real).
    m.hook("PlrStat_OkToUpdate__Fv", [](nf::ee::Cpu& c) {
        c.r[2].d[0] = 1;
        c.r[2].d[1] = 0;
        return true;
    });
    std::printf("dmg,part,type,armour0,health0,alpha0,mp,team,diff,loc,rapid,coin,health1,armour1,flash,"
                "paindir,paalpha,sounds,rumble\n");
    struct Row {
        float dmg;
        int part, type;
        float armour0, health0;
        u32 mp, team, diff, loc, rapid;
        int coin;
        unsigned alpha0;
    };
    std::vector<Row> rows;
    for (int part : {-1, 2, 5, 20, 49})
        for (int type : {0, 1, 5, 6, 7})
            for (float armour : {0.0f, 50.0f})
                rows.push_back({10.0f, part, type, armour, 100.0f, 1, 0, 2, 1, 0, 0, 0});
    for (int part : {-1, 2, 5, 20, 49}) rows.push_back({10.0f, part, 0, 50.0f, 100.0f, 1, 0, 2, 1, 1, 0, 0});
    for (int part : {-1, 2, 5, 20, 49}) rows.push_back({10.0f, part, 0, 50.0f, 100.0f, 1, 0, 2, 0, 0, 0, 0});
    rows.push_back({100.0f, 5, 0, 50.0f, 30.0f, 1, 0, 2, 1, 0, 0, 0});
    rows.push_back({100.0f, 5, 0, 50.0f, 30.0f, 1, 0, 2, 1, 0, 1, 0});
    rows.push_back({3.5f, -1, 0, 0.0f, 3.0f, 1, 0, 2, 1, 0, 0, 0});
    rows.push_back({0.5f, 2, 0, 0.0f, 0.6f, 1, 0, 2, 1, 0, 0, 0});
    // Sub-1 remainder ("a remainder under 1 point kills") and overlay accumulation from a preset alpha.
    rows.push_back({9.5f, -1, 0, 0.0f, 10.0f, 1, 0, 2, 1, 0, 0, 0});
    rows.push_back({9.0f, -1, 0, 0.0f, 10.0f, 1, 0, 2, 1, 0, 0, 0});
    rows.push_back({9.5f, -1, 0, 0.0f, 10.0f, 0, 0, 2, 0, 0, 0, 0});
    rows.push_back({10.0f, -1, 0, 0.0f, 100.0f, 1, 0, 2, 1, 0, 0, 100});
    rows.push_back({10.0f, -1, 0, 0.0f, 100.0f, 1, 0, 2, 1, 0, 0, 200});
    rows.push_back({0.5f, -1, 0, 0.0f, 100.0f, 1, 0, 2, 1, 0, 0, 100});
    for (u32 diff : {1u, 2u, 3u, 4u}) {
        rows.push_back({10.0f, -1, 0, 0.0f, 100.0f, 0, 0, diff, 0, 0, 0, 0});
        rows.push_back({10.0f, -1, 0, 50.0f, 100.0f, 0, 0, diff, 0, 0, 0, 0});
        rows.push_back({10.0f, -1, 6, 50.0f, 100.0f, 0, 0, diff, 0, 0, 0, 0});
    }
    rows.push_back({10.0f, 2, 0, 50.0f, 100.0f, 1, 1, 2, 1, 0, 0, 0});
    std::vector<u8> clean_obj(0x300, 0), clean_bldat(0x1000, 0), clean_220(0x100, 0);
    for (const Row& r : rows) {
        m.mem.write_block(obj, clean_obj.data(), 0x300);
        m.mem.write_block(bldat, clean_bldat.data(), 0x1000);
        m.mem.write_block(obj220, clean_220.data(), 0x100);
        m.mem.write<u32>(obj + 224, bldat);
        m.mem.write<u32>(obj + 220, obj220);
        m.write_vec3(obj + 48, 0.0f, 0.0f, 0.0f);
        m.mem.write<u8>(obj220 + 98, 6);
        m.write_f32(bldat + 2196, r.health0);
        m.write_f32(bldat + 2224, r.armour0);
        m.mem.write<u8>(bldat + 2408, u8(r.alpha0));
        m.mem.write<u8>(bldat + 2382, 0);
        m.mem.write<u32>(mp_flag, r.mp);
        m.mem.write<u32>(team_flag, r.team);
        m.mem.write<u32>(difficulty, r.diff);
        m.mem.write<u32>(loc_flag, r.loc);
        m.mem.write<u32>(rapid_flag, r.rapid);
        m.mem.write<u8>(cheat_b, 0);
        if (cheat_info) m.mem.write<u32>(cheat_info, 0);
        coin_script = r.coin;
        snd_log.clear();
        rumble_log.clear();
        CallArgs a;
        a.i(obj).i(bldat).i(u32(r.type)).i(u32(r.part)).f(r.dmg);
        bool trapped = false;
        std::string trap_name;
        u32 trap_pc = 0;
        try {
            m.call_keep(entry, a, 10'000'000);
        } catch (const nf::ee::Trap& t) {
            trapped = true;
            trap_name = nf::ee::trap_kind_name(t.kind);
            trap_pc = t.pc;
        }
        u32 hw = 0, aw = 0, fw = 0;
        m.mem.read_block(bldat + 2196, &hw, 4);
        m.mem.read_block(bldat + 2224, &aw, 4);
        m.mem.read_block(bldat + 2236, &fw, 4);
        const u8 pdir = m.mem.read<u8>(bldat + 2407);
        const u8 palpha = m.mem.read<u8>(bldat + 2408);
        std::string snd, rum;
        for (const auto& s : snd_log) snd += (snd.empty() ? "" : " ") + s;
        for (const auto& s : rumble_log) rum += (rum.empty() ? "" : " ") + s;
        if (snd.empty()) snd = "-";
        if (rum.empty()) rum = "-";
        if (trapped) {
            std::printf("%.9g,%d,%d,%.9g,%.9g,%u,%u,%u,%u,%u,%u,%d,TRAP %s at %08x\n", (double)r.dmg,
                        r.part, r.type, (double)r.armour0, (double)r.health0, r.alpha0, r.mp, r.team,
                        r.diff, r.loc, r.rapid, r.coin, trap_name.c_str(), trap_pc);
        } else {
            std::printf("%.9g,%d,%d,%.9g,%.9g,%u,%u,%u,%u,%u,%u,%d,%.9g,%.9g,%.9g,%u,%u,%s,%s\n",
                        (double)r.dmg, r.part, r.type, (double)r.armour0, (double)r.health0, r.alpha0,
                        r.mp, r.team, r.diff, r.loc, r.rapid, r.coin, (double)as_float(hw),
                        (double)as_float(aw), (double)as_float(fw), unsigned(pdir), unsigned(palpha),
                        snd.c_str(), rum.c_str());
        }
    }
    return 0;
}
// ---- NDrone2_ReFindMissionPath sweep -------------------------------------------------------------
// EE truth table for the Bots mission-route diff. Per case builds a drone blob (>= 0xAA0) with a
// mission u16 array (+0xa60), a node table (+0xa64 -> +0x30 entries, stride 0x40, +0x10 vec4, +0xc cel),
// and an obj blob; hooks script NDrone2_MoveTest reachability, record Drone_SM_SetState, and provide
// NDrone2_Player. Everything else (LinkCreep, FindCel, SetupGoalPosition, MoveToGoalPosition,
// GetAnglesToMoveTarget, FeetPos, Vec fns) runs real. Traps are recorded per row, not fatal.
int cmd_diff_refind(const std::string& elf_path) {
    Machine m(elf_path);
    m.install_libc_hooks();
    const u32 entry = m.addr("NDrone2_ReFindMissionPath__FP10DCVars_tag");
    const u32 dc = m.alloc(0x100), drone = m.alloc(0xC00), obj = m.alloc(0x300);
    const u32 player = m.alloc(0x100), table = m.alloc(0x100), entries = m.alloc(4 * 0x40);
    const u32 mission = m.alloc(16);
    int movetest_mode = 0, movetest_calls = 0;  // 0: always 1, 1: always 0, 2: alternating
    u32 findcel_script = 0x22220000u;
    std::vector<std::string> sm_log;
    m.hook("NDrone2_MoveTest__FP10CelPos_tagT0P7obj_tagT2P7_VECTORScSc", [&](nf::ee::Cpu& c) {
        int r = 1;
        if (movetest_mode == 1) r = 0;
        else if (movetest_mode == 2) r = (movetest_calls++ % 2 == 0) ? 1 : 0;
        c.r[2].d[0] = u64(r);
        c.r[2].d[1] = 0;
        return true;
    });
    m.hook("Drone_SM_SetState__FP20StateMachineInfo_tagUiUi", [&](nf::ee::Cpu& c) {
        char b[64];
        std::snprintf(b, sizeof b, "setstate(%u,%u)", c.r[5].w[0], c.r[6].w[0]);
        sm_log.emplace_back(b);
        c.r[2].d[0] = 0;
        c.r[2].d[1] = 0;
        return true;
    });
    m.hook("NDrone2_Player__Fv", [&](nf::ee::Cpu& c) {
        c.r[2].d[0] = player;
        c.r[2].d[1] = 0;
        return true;
    });
    // CalcRouteToPosition loops forever without nav data; script the verdict
    // (route planning gets its own table) and record the call for the replica.
    int mtg_ret = 0;
    std::vector<std::string> mtg_log;
    m.hook("NDrone2_MoveToGoalPosition__FP10DCVars_tagP10CelPos_tagfUs", [&](nf::ee::Cpu& c) {
        char b[64];
        std::snprintf(b, sizeof b, "mtg(f12=0x%08x)", c.f[12]);
        mtg_log.emplace_back(b);
        c.r[2].d[0] = u64(mtg_ret);
        c.r[2].d[1] = 0;
        return true;
    });
    // build_FindCel loops forever on an empty world, so script it: the search
    // cel comes from the row input and is recorded, like MoveTest outcomes.
    m.hook("NDrone2_FindCel__FP7_VECTOR", [&](nf::ee::Cpu& c) {
        c.r[2].d[0] = findcel_script;
        c.r[2].d[1] = 0;
        return true;
    });
    // LinkCreep_ForNodes loops forever on empty routes, and Dest/NavPath need
    // nav data; script all three (route planning gets its own table) and log them.
    int linkcalc_ret = 0;
    float dest_pos[3] = {12, 0, 12};
    u32 dest_cel = 0x33330000u;
    std::vector<std::string> dest_log, nav_log;
    m.hook("LinkCreep_Calc__FP11AIRoute_tagSc", [&](nf::ee::Cpu& c) {
        c.r[2].d[0] = u64(linkcalc_ret);
        c.r[2].d[1] = 0;
        return true;
    });
    m.hook("LinkCreep_Dest__FP11AIRoute_tagsP10CelPos_tag", [&](nf::ee::Cpu& c) {
        const u32 out = c.r[6].w[0];
        char b[64];
        std::snprintf(b, sizeof b, "dest(%u)", c.r[5].w[0]);
        dest_log.emplace_back(b);
        m.write_vec3(out, dest_pos[0], dest_pos[1], dest_pos[2]);
        m.mem.write<u32>(out + 0x10, dest_cel);
        c.r[2].d[0] = 0;
        c.r[2].d[1] = 0;
        return true;
    });
    m.hook("AINetwork_NavPathForPosition__FP7_VECTORP7cel_tagSc", [&](nf::ee::Cpu& c) {
        nav_log.emplace_back("navpath");
        c.r[2].d[0] = 0x44440000u;
        c.r[2].d[1] = 0;
        return true;
    });
    auto rd_u32 = [&](u32 va) {
        u32 w = 0;
        m.mem.read_block(va, &w, 4);
        return w;
    };
    auto rd_f32 = [&](u32 va) {
        u32 w = 0;
        m.mem.read_block(va, &w, 4);
        float v;
        std::memcpy(&v, &w, 4);
        return v;
    };
    auto rd_vec = [&](u32 va) {
        float v[4];
        m.mem.read_block(va, v, 16);
        return std::array<float, 4>{v[0], v[1], v[2], v[3]};
    };
    std::printf("# NDrone2_ReFindMissionPath truth table\n");
    std::printf("# dc=0x%x drone=0x%x obj=0x%x player=0x%x\n", dc, drone, obj, player);
    std::printf("count,flags,d8,movetest,nodes,fc,mg,lk,dp,ret,ndx,cel,goal,dist,rate,move,ang,aipoint,sm,mtg,dest,nav\n");
    const float line[4][3] = {{10, 0, 10}, {20, 0, 20}, {30, 0, 30}, {40, 0, 40}};
    const float scat[4][3] = {{10, 0, 40}, {35, 0, 12}, {8, 0, 30}, {50, 0, 50}};
    const char* modes[] = {"all1", "all0", "alt"};
    std::vector<u8> clean(0xC00, 0);
    for (int count : {0, 1, 4})
        for (u32 flags : {0u, 0x10u})
            for (u32 d8 : {0u, 9u})
                for (int mt = 0; mt < 3; mt++)
                    for (int ni = 0; ni < 2; ni++)
                        for (u32 fc : {0x22220000u, 0u})
                            for (int mg : {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14})
                                for (int lk : {0, 1})
                                    for (int dp = 0; dp < 2; dp++) {
                            findcel_script = fc;
                            mtg_ret = mg;
                            mtg_log.clear();
                            linkcalc_ret = lk;
                            dest_log.clear();
                            nav_log.clear();
                            if (dp == 0) {
                                dest_pos[0] = 12;
                                dest_pos[1] = 0;
                                dest_pos[2] = 12;
                                dest_cel = 0x33330000u;
                            } else {
                                dest_pos[0] = 100;
                                dest_pos[1] = 0;
                                dest_pos[2] = 100;
                                dest_cel = 0x33330001u;
                            }
                            m.mem.write_block(drone, clean.data(), 0xC00);
                            m.mem.write<u32>(dc, obj);
                            m.mem.write<u32>(dc + 4, drone);
                            m.mem.write<u32>(dc + 12, 0xEEEEEEEEu);  // SetState hooked, never touched
                            m.mem.write<u32>(drone + 0x4f8, flags);
                            m.mem.write<u16>(drone + 0x9f2, u16(count));
                            m.mem.write<u16>(drone + 0x9f0, 0);  // output; initial value never read
                            m.mem.write<u16>(drone + 0xd8, u16(d8));
                            m.mem.write<u32>(drone + 0xa60, mission);
                            m.mem.write<u32>(drone + 0xa64, table);
                            m.mem.write<u32>(drone + 0x704, 0);
                            m.mem.write<u32>(table + 0x30, entries);
                            for (int k = 0; k < 4; k++) {
                                m.mem.write<u16>(mission + 2 * k, u16(k));
                                const float* p = (ni == 0 ? line : scat)[k];
                                m.write_vec3(entries + 64 * k + 0x10, p[0], p[1], p[2]);
                                m.mem.write<u32>(entries + 64 * k + 0xc, 0x11110000u + u32(k));
                            }
                            m.write_vec3(obj + 0x20, 0, 0, 0);
                            m.write_vec3(obj + 0x30, 0, 0, 0);
                            // Player CelPos assembles as [P+0x30..+0x3c, P+0x20-as-cel]; keep cel 0 so
                            // FindCel (scripted) resolves it and the goal carries a real position.
                            m.write_vec3(player + 0x30, 10, 20, 30);
                            m.mem.write<u32>(player + 0x20, 0);
                            movetest_mode = mt;
                            movetest_calls = 0;
                            sm_log.clear();
                            try {
                                CallArgs a;
                                a.i(dc);
                                // call_keep: verdicts live in RAM (drone blob), which call() rolls back.
                                const auto r = m.call_keep(entry, a, 50'000'000);
                                const std::string sm = sm_log.empty() ? "-" : sm_log.back();
                                const auto gv = rd_vec(drone + 0x9d0);
                                const auto mv = rd_vec(drone + 0x670);
                                const auto av = rd_vec(drone + 0x690);
                                std::string goal;
                                for (int q = 0; q < 64; q += 4) {
                                    char hb[12];
                                    std::snprintf(hb, sizeof hb, "%08x", rd_u32(drone + 0x6f0 + q));
                                    goal += (q ? " " : "") + std::string(hb);
                                }
                                std::printf("%d,0x%x,%u,%s,%d,fc=0x%x,mg=%d,lk=%d,dp=%d,ret=%d,ndx=0x%x,"
                                            "cel=0x%x,goal=%g/%g/%g/%g/0x%x,dist=%g,rate=%g,"
                                            "move=%g/%g/%g,ang=%g/%g/%g,aipoint=%s,sm=%s,mtg=%s,"
                                            "dest=%s,nav=%s\n",
                                            count, flags, d8, modes[mt], ni, fc, mg, lk, dp,
                                            int(r.v0 & 0xFFFFFFFF), rd_u32(drone + 0x9f0) & 0xFFFFu,
                                            rd_u32(drone + 0x9e0), gv[0], gv[1], gv[2], gv[3],
                                            rd_u32(drone + 0x9d0 + 0x10), rd_f32(drone + 0x660),
                                            rd_f32(drone + 0x664), mv[0], mv[1], mv[2], av[0], av[1],
                                            av[2], goal.c_str(), sm.c_str(),
                                            mtg_log.empty() ? "-" : mtg_log.back().c_str(),
                                            dest_log.empty() ? "-" : dest_log.back().c_str(),
                                            nav_log.empty() ? "-" : nav_log.back().c_str());
                            } catch (const nf::ee::Trap& t) {
                                std::printf("%d,0x%x,%u,%s,%d,fc=0x%x,mg=%d,lk=%d,dp=%d,TRAP %s at %08x\n",
                                            count, flags, d8, modes[mt], ni, fc, mg, lk, dp,
                                            nf::ee::trap_kind_name(t.kind), t.pc);
                            }
                        }
    return 0;
}
// ---- MP bot brain sweep -------------------------------------------------------------------------
// EE truth tables for the MP bot-brain differential (MpBots slice). Sections print CSV to stdout:
//   B  BOTSTATE_getStateType over 0xc0..0xfd (pure table; nfdump validate cross-checks it too)
//   S  BOT_getMovementSpeedMul over speed bytes (drone+182)
//   G  BOT_getAggressionMul over (acc, mp-live, opp?, dist)
//   M  BOT_getMovePossibility over (n, acc, speed, seed): result plus draw evidence
//   C  BOTSTATE_checkAttackMove over (state, can, scenario, slot, in-hill, hill-test)
//   E  BOTSTATE_EvasiveMove / BOTSTATE_chooseCombatMove over (evasive, acc, speed, rand script)
//   D  BOTSTATE_increaseDistraction / isDistracted over (bits, goal, state, distr, limit, d)
//   P  BOT_handlePain over (dmg, type, loc, armour, health, flags)
//   T  BOTSTATE_gotoGoal over (slot, kind, teams, personality): commitment budget
// Sections C/E/G/P/T script callees and log draws; the port side replays the same scripts with a
// throwaway checker against the real BotBrain (not committed). Blob bases print in `#` headers.
int cmd_diff_bot(const std::string& elf_path) {
    Machine m(elf_path);
    m.install_libc_hooks();
    auto as_float = [](u32 w) {
        float f = 0;
        std::memcpy(&f, &w, 4);
        return f;
    };
    auto zero = [&](u32 va, u32 len) {
        for (u32 o = 0; o < len; o += 4) m.mem.write<u32>(va + o, 0);
    };
    const u32 dc = m.alloc(0x100), drone = m.alloc(0x1000), obj = m.alloc(0x300);
    const u32 objx = m.alloc(0x600), botvars = m.alloc(0x800), smi = m.alloc(0x20);
    std::printf("# dc=0x%x drone=0x%x obj=0x%x objx=0x%x botvars=0x%x smi=0x%x\n", dc, drone, obj,
                objx, botvars, smi);

    {  // B: state types (pure table, 0xc3..0xf9 plus guards).
        std::printf("B,state,type\n");
        for (int s = 0xc0; s <= 0xfd; ++s) {
            CallArgs a;
            a.i(u32(s));
            int type = -1;
            std::string trap;
            try {
                type = int(m.call("BOTSTATE_getStateType__FUi", a).v0);
            } catch (const std::exception& e) { trap = e.what(); }
            std::printf("B,0x%x,%d%s\n", s, type, trap.empty() ? "" : (" TRAP " + trap).c_str());
        }
    }
    {  // S: movement speed multiplier (drone+182).
        std::printf("S,speed,f0,f0hex\n");
        for (int b : {0, 1, 2, 3, 4, 255}) {
            zero(drone, 0x1000);
            m.mem.write<u8>(drone + 182, u8(b));
            CallArgs a;
            a.i(drone);
            float f = 0;
            u32 w = 0;
            std::string trap;
            try {
                auto r = m.call("BOT_getMovementSpeedMul__FP9Drone_tag", a);
                w = r.f0;
                f = as_float(w);
            } catch (const std::exception& e) { trap = e.what(); }
            std::printf("S,%d,%.9g,0x%08x%s\n", b, (double)f, w,
                        trap.empty() ? "" : (" TRAP " + trap).c_str());
        }
    }
    {  // G: aggression multiplier (drone+181, MPSettings+0x184 live, drone+368 opp, +416 dist).
        std::printf("G,acc,live,opp,dist,f0,f0hex\n");
        for (int acc : {0, 1, 2, 3, 4, 5, 255}) {
            for (int live : {0, 1}) {
                for (int opp : {0, 1}) {
                    for (float dist : {0.0f, 1.0f, 2.49f, 2.5f, 10.0f}) {
                        zero(drone, 0x1000);
                        m.mem.write<u8>(drone + 181, u8(acc));
                        m.mem.write<u32>(0x2A4924u, u32(live));
                        m.mem.write<u32>(drone + 368, opp ? drone : 0u);
                        u32 db = 0;
                        std::memcpy(&db, &dist, 4);
                        m.mem.write<u32>(drone + 416, db);
                        CallArgs a;
                        a.i(drone);
                        float f = 0;
                        u32 w = 0;
                        std::string trap;
                        try {
                            auto r = m.call("BOT_getAggressionMul__FP9Drone_tag", a);
                            w = r.f0;
                            f = as_float(w);
                        } catch (const std::exception& e) { trap = e.what(); }
                        std::printf("G,%d,%d,%d,%.9g,%.9g,0x%08x%s\n", acc, live, opp, (double)dist,
                                    (double)f, w, trap.empty() ? "" : (" TRAP " + trap).c_str());
                    }
                }
            }
        }
        m.mem.write<u32>(0x2A4924u, 0u);
    }
    {  // M: move possibility over (n, acc, speed, seed) with draw evidence (kept RNG advances).
        // m.call rolls RAM back, which would hide the draw count; call_keep preserves the RNG advance
        // so `next` proves exactly how many draws the function consumed.
        std::printf("M,n,acc,speed,m,seed0,draw0,ret,next,draw1\n");
        const u32 poss_entry = m.addr("BOT_getMovePossibility__FP9Drone_tagUi");
        const u32 rand_entry = m.addr("Rand_Rand__FUi");
        const u32 seeds[4][4] = {{0x1F123BB5u, 0x159A55E5u, 0x87654321u, 0x9ABCDEF0u},
                                 {1u, 2u, 3u, 4u},
                                 {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu},
                                 {12345u, 67890u, 11111u, 22222u}};
        for (int n : {2, 3}) {
            for (int acc : {1, 3, 5, 8}) {
                for (int speed : {0, 1, 2}) {
                    const int mm = n + acc + 2 - speed;
                    for (int si = 0; si < 4; ++si) {
                        auto set_rng = [&] {
                            for (int k = 0; k < 4; ++k) m.mem.write<u32>(0x30D0A0u + 4u * u32(k), seeds[si][k]);
                        };
                        auto keep_rand = [&](u32 arg) {
                            CallArgs a;
                            a.i(arg);
                            return u32(m.call_keep(rand_entry, a, 10'000'000).v0);
                        };
                        zero(drone, 0x1000);
                        m.mem.write<u8>(drone + 180, u8(acc));
                        m.mem.write<u8>(drone + 182, u8(speed));
                        set_rng();
                        CallArgs a;
                        a.i(drone);
                        a.i(u32(n));
                        int ret = -1;
                        std::string trap;
                        u32 next = 0xFFFFFFFFu, draw0 = 0xFFFFFFFFu, draw1 = 0xFFFFFFFFu;
                        try {
                            ret = int(m.call_keep(poss_entry, a, 10'000'000).v0);
                            next = keep_rand(1000);   // draw right after the function: single-draw iff == draw1
                            set_rng();
                            draw0 = keep_rand(u32(mm));
                            draw1 = keep_rand(1000);
                        } catch (const std::exception& e) { trap = e.what(); }
                        std::printf("M,%d,%d,%d,%d,%u,%u,%d,%u,%u%s\n", n, acc, speed, mm,
                                    seeds[si][0], draw0, ret, next, draw1,
                                    trap.empty() ? "" : (" TRAP " + trap).c_str());
                    }
                }
            }
        }
    }
    {  // C: checkAttackMove over (state, can, scenario, slot, in-hill, hill-test).
        std::printf("C,state,can,scen,slot,inhill,hill,ret,calls\n");
        int can_script = 0, plr_script = -1, hill_script = 0;
        std::string calls;
        auto hook_can = [&](const char* name) {
            m.hook(name, [&, name](nf::ee::Cpu& c) {
                calls += calls.empty() ? name : std::string("+") + name;
                c.r[2].d[0] = u64(can_script);
                c.r[2].d[1] = 0;
                return true;
            });
        };
        hook_can("NDrone2_CanStrafeLeft__FP10DCVars_tag");
        hook_can("NDrone2_CanStrafeRight__FP10DCVars_tag");
        hook_can("NDrone2_CanBackoff__FP10DCVars_tag");
        hook_can("NDrone2_CanRollLeft__FP10DCVars_tag");
        hook_can("NDrone2_CanRollRight__FP10DCVars_tag");
        hook_can("NDrone2_CanStepLeft__FP10DCVars_tag");
        hook_can("NDrone2_CanStepRight__FP10DCVars_tag");
        m.hook("Control_Plr2Ind__FP7obj_tag", [&](nf::ee::Cpu& c) {
            calls += calls.empty() ? "plr" : "+plr";
            c.r[2].d[0] = u64(std::int64_t(plr_script));   // sign-extended like the real li/mf
            c.r[2].d[1] = 0;
            return true;
        });
        m.hook("MP_isPosOnHill__FP7_VECTOR", [&](nf::ee::Cpu& c) {
            (void)c;
            calls += calls.empty() ? "hill" : "+hill";
            c.r[2].d[0] = u64(hill_script);
            c.r[2].d[1] = 0;
            return true;
        });
        const int states[] = {212, 213, 215, 217, 218, 219, 220, 214, 216, 207, 0xCF, 0};
        for (int st : states) {
            for (int can : {0, 1}) {
                for (u32 scen : {1u, 0x40000800u, 0x60001000u}) {
                    // (slot, inhill, hill) combos only matter when the KOTH veto can run.
                    const int combos[][3] = {{-1, 0, 0}, {4, 0, 0}, {4, 1, 0}, {4, 1, 1}};
                    for (const auto& [slot, inhill, hill] : combos) {
                        if (scen == 1u && (slot != -1 || inhill || hill)) continue;
                        if (can == 0 && (slot != -1 || inhill || hill)) continue;
                        can_script = can;
                        plr_script = slot;
                        hill_script = hill;
                        zero(dc, 0x100);
                        zero(drone, 0x1000);
                        m.mem.write<u32>(dc, obj);
                        m.mem.write<u32>(dc + 4, drone);
                        m.mem.write<u32>(0x2A4944u, scen);
                        m.mem.write<u16>(0x2A4980u + 4u * 0x30u + 0x26u,
                                         u16(inhill ? 0x10 : 0));
                        calls.clear();
                        CallArgs a;
                        a.i(dc);
                        a.i(u32(u16(st)));
                        int ret = -999;
                        std::string trap;
                        try {
                            ret = int(m.call("BOTSTATE_checkAttackMove__FP10DCVars_tags", a).v0);
                        } catch (const std::exception& e) { trap = e.what(); }
                        if (calls.empty()) calls = "-";
                        std::printf("C,%d,%d,0x%x,%d,%d,%d,%d,%s%s\n", st, can, scen, slot, inhill,
                                    hill, ret, calls.c_str(),
                                    trap.empty() ? "" : (" TRAP " + trap).c_str());
                    }
                }
            }
        }
        m.mem.write<u32>(0x2A4944u, 0u);
        m.mem.write<u16>(0x2A4980u + 4u * 0x30u + 0x26u, 0u);
    }
    {  // E: EvasiveMove / chooseCombatMove over (evasive, acc, speed, rand script).
        std::printf("E,fn,evasive,acc,speed,script,ret,draws\n");
        int evasive_script = 0;
        std::vector<u32> rscript;
        std::size_t ridx = 0;
        std::string draws;
        m.hook("NDrone2_EvasiveMove__FP10DCVars_tag", [&](nf::ee::Cpu& c) {
            (void)c;
            c.r[2].d[0] = u64(evasive_script);
            c.r[2].d[1] = 0;
            return true;
        });
        m.hook("Rand_Rand__FUi", [&](nf::ee::Cpu& c) {
            const u32 arg = c.r[4].w[0];
            const u32 ret = ridx < rscript.size() ? rscript[ridx++] : 0u;
            char b[64];
            std::snprintf(b, sizeof b, "%u->%u", arg, ret);
            draws += draws.empty() ? b : std::string(" ") + b;
            c.r[2].d[0] = u64(ret);
            c.r[2].d[1] = 0;
            return true;
        });
        int can_script = 1;
        auto hook_can = [&](const char* name) {
            m.hook(name, [&](nf::ee::Cpu& c) {
                (void)c;
                c.r[2].d[0] = u64(can_script);
                c.r[2].d[1] = 0;
                return true;
            });
        };
        hook_can("NDrone2_CanStrafeLeft__FP10DCVars_tag");
        hook_can("NDrone2_CanStrafeRight__FP10DCVars_tag");
        hook_can("NDrone2_CanBackoff__FP10DCVars_tag");
        hook_can("NDrone2_CanRollLeft__FP10DCVars_tag");
        hook_can("NDrone2_CanRollRight__FP10DCVars_tag");
        hook_can("NDrone2_CanStepLeft__FP10DCVars_tag");
        hook_can("NDrone2_CanStepRight__FP10DCVars_tag");
        m.mem.write<u32>(0x2A4944u, 1u);
        struct ERow {
            const char* fn;
            int evasive;
            int acc;
            int speed;
            std::vector<u32> script;
        };
        const ERow rows[] = {
            {"ev", 0, 1, 1, {}},   {"ev", 117, 1, 1, {}}, {"ev", 118, 1, 1, {}},
            {"ev", 121, 1, 1, {4}}, {"ev", 121, 1, 1, {0}}, {"ev", 122, 8, 0, {12}},
            {"ev", 122, 8, 0, {0}}, {"ev", 200, 1, 1, {}},
            {"ch", 0, 1, 1, {0}},  {"ch", 0, 1, 1, {3}},  {"ch", 0, 1, 1, {3, 0}},
            {"ch", 0, 1, 1, {3, 1}}, {"ch", 0, 1, 1, {3, 2}}, {"ch", 0, 1, 1, {3, 3}},
            {"ch", 117, 1, 1, {}}, {"ch", 118, 8, 0, {}},
        };
        for (const ERow& r : rows) {
            evasive_script = r.evasive;
            rscript = r.script;
            ridx = 0;
            draws.clear();
            zero(dc, 0x100);
            zero(drone, 0x1000);
            m.mem.write<u32>(dc, obj);
            m.mem.write<u32>(dc + 4, drone);
            m.mem.write<u8>(drone + 180, u8(r.acc));
            m.mem.write<u8>(drone + 182, u8(r.speed));
            CallArgs a;
            a.i(dc);
            int ret = -999;
            std::string trap, script_txt;
            for (u32 s : r.script) script_txt += (script_txt.empty() ? "" : "/") + std::to_string(s);
            try {
                if (r.fn[0] == 'e' && r.fn[1] == 'v')
                    ret = int(m.call("BOTSTATE_EvasiveMove__FP10DCVars_tag", a).v0);
                else
                    ret = int(m.call("BOTSTATE_chooseCombatMove__FP10DCVars_tag", a).v0);
            } catch (const std::exception& e) { trap = e.what(); }
            if (draws.empty()) draws = "-";
            if (script_txt.empty()) script_txt = "-";
            std::printf("E,%s,%d,%d,%d,%s,%d,%s%s\n", r.fn, r.evasive, r.acc, r.speed,
                        script_txt.c_str(), ret, draws.c_str(),
                        trap.empty() ? "" : (" TRAP " + trap).c_str());
        }
        m.mem.write<u32>(0x2A4944u, 0u);
    }
    {  // D: increaseDistraction / isDistracted over (bits, goal, state, distr, limit, d).
        std::printf("D,fn,bits,goal,state,distr0,limit,d,ret,distr1\n");
        struct DRow {
            const char* fn;
            u32 bits;
            int goal;
            u32 state;
            float distr0;
            float limit;
            float d;
        };
        const DRow rows[] = {
            {"inc", 0, 255, 0xEB, 0.0f, 0.0f, 1.7f}, {"inc", 0, 0, 0xEB, 0.0f, 10.0f, 1.7f},
            {"inc", 0, 0, 0xEB, 9.0f, 10.0f, 1.7f},  {"inc", 0, 0, 0xEB, 0.0f, 10.0f, -5.0f},
            {"inc", 0, 0, 0xCF, 0.0f, 10.0f, 1.7f},  {"inc", 4, 0, 0xEB, 0.0f, 10.0f, 1.7f},
            {"inc", 4, 0, 0xEB, 9.0f, 10.0f, 5.0f},  {"inc", 0, 1, 0xEB, 3.0f, 10.0f, 1.0f},
            {"is", 0, 255, 0xEB, 0.0f, 0.0f, 0.0f},  {"is", 4, 0, 0xEB, 5.0f, 10.0f, 0.0f},
            {"is", 0, 0, 0xCF, 0.0f, 10.0f, 0.0f},   {"is", 0, 0, 0xEB, 5.0f, 10.0f, 0.0f},
            {"is", 0, 0, 0xEB, 0.0f, 10.0f, 0.0f},   {"is", 0, 0, 0xEB, 0.0f, 0.0f, 0.0f},
        };
        for (const DRow& r : rows) {
            zero(drone, 0x1000);
            zero(botvars, 0x800);
            m.mem.write<u32>(drone + 3356, botvars);
            m.mem.write<u32>(drone + 268, r.state);
            m.mem.write<u32>(botvars + 1844, r.bits);
            m.mem.write<u8>(botvars + 1893, u8(r.goal));
            u32 dw = 0, lw = 0;
            std::memcpy(&dw, &r.distr0, 4);
            std::memcpy(&lw, &r.limit, 4);
            m.mem.write<u32>(botvars + 1832, dw);
            if (r.goal >= 0 && r.goal < 2)
                m.mem.write<u32>(botvars + u32(r.goal) * 80u + 32u, lw);
            int ret = -999;
            std::string trap;
            try {
                if (r.fn[0] == 'i' && r.fn[1] == 'n' && r.fn[2] == 'c') {
                    CallArgs a;
                    a.i(drone);
                    a.f(r.d);
                    const u32 entry = m.addr("BOTSTATE_increaseDistraction__FP9Drone_tagf");
                    ret = int(m.call_keep(entry, a, 10'000'000).v0);
                } else {
                    CallArgs a;
                    a.i(drone);
                    const u32 entry = m.addr("BOTSTATE_isDistracted__FP9Drone_tag");
                    ret = int(m.call_keep(entry, a, 10'000'000).v0);
                }
            } catch (const std::exception& e) { trap = e.what(); }
            u32 d1w = 0;
            m.mem.read_block(botvars + 1832, &d1w, 4);
            std::printf("D,%s,%u,%d,0x%x,%.9g,%.9g,%.9g,%d,%.9g%s\n", r.fn, r.bits, r.goal,
                        r.state, (double)r.distr0, (double)r.limit, (double)r.d, ret,
                        (double)as_float(d1w), trap.empty() ? "" : (" TRAP " + trap).c_str());
        }
    }
    {  // P: handlePain over (dmg, type, loc, armour, health, locflag, profflag, sfxbusy, guards).
        std::printf("P,dmg,type,loc,armour,health,locflag,profflag,sfxbusy,extra,ret,h1,a1,last,sfx,draws,fe,cflags,otype\n");
        int sfxbusy_script = 0;
        std::vector<u32> rscript;
        std::size_t ridx = 0;
        std::string draws;
        int sfx_id = -1;
        if (!m.hook("GameFlow_GetState__Fv", [&](nf::ee::Cpu& c) {
                c.r[2].d[0] = 2u;
                c.r[2].d[1] = 0;
                return true;
            }))
            std::fprintf(stderr, "diff-bot: GameFlow hook MISSING\n");
        m.hook("Sound_Playing_Ref__FUiUi", [&](nf::ee::Cpu& c) {
            (void)c;
            c.r[2].d[0] = u64(sfxbusy_script);
            c.r[2].d[1] = 0;
            return true;
        });
        m.hook("NDrone2_PlaySFX__FP10DCVars_tagUiP7_VECTORScf", [&](nf::ee::Cpu& c) {
            sfx_id = int(c.r[5].w[0]);
            c.r[2].d[0] = 0u;
            c.r[2].d[1] = 0;
            return true;
        });
        m.hook("PlrStat_LogHealth__FfUi", [&](nf::ee::Cpu& c) {
            (void)c;
            c.r[2].d[0] = 0u;
            c.r[2].d[1] = 0;
            return true;
        });
        m.hook("Rand_Rand__FUi", [&](nf::ee::Cpu& c) {
            const u32 arg = c.r[4].w[0];
            const u32 ret = ridx < rscript.size() ? rscript[ridx++] : 0u;
            char b[64];
            std::snprintf(b, sizeof b, "%u->%u", arg, ret);
            draws += draws.empty() ? b : std::string(" ") + b;
            c.r[2].d[0] = u64(ret);
            c.r[2].d[1] = 0;
            return true;
        });
        struct PRow {
            float dmg;
            int type;
            int loc;
            int armour;
            float health;
            int locflag;
            int profflag;
            int sfxbusy;
            int extra;
            std::vector<u32> script;
            int fe = 0;          // obj+0xfe value (1 = pending-delete skip)
            u32 cflags = 0x100u; // guard-struct +1272 flags (needs 0x100, not 0x600)
            int otype = 2;       // obj+0xff (17 = eliminated skip)
        };
        const PRow rows[] = {
            {10.0f, 0, -1, 0, 100.0f, 1, 0, 0, 0, {2}}, {10.0f, 0, 5, 0, 100.0f, 1, 0, 0, 0, {2}},
            {10.0f, 0, 20, 0, 100.0f, 1, 0, 0, 0, {2}}, {10.0f, 0, 50, 0, 100.0f, 1, 0, 0, 0, {2}},
            {10.0f, 0, 7, 0, 100.0f, 1, 0, 0, 0, {2}},  {10.0f, 3, 5, 20, 100.0f, 1, 0, 0, 0, {2}},
            {10.0f, 0, 5, 20, 100.0f, 1, 0, 0, 0, {2}}, {10.0f, 8, 5, 20, 100.0f, 1, 0, 0, 0, {2}},
            {10.0f, 0, 5, 0, 100.0f, 0, 0, 0, 0, {2}},  {10.0f, 0, 5, 0, 100.0f, 1, 1, 0, 0, {2}},
            {10.0f, 0, 5, 0, 0.5f, 1, 0, 0, 0, {1}},    {200.0f, 0, 5, 0, 100.0f, 1, 0, 0, 0, {1}},
            {10.0f, 0, 5, 0, 100.0f, 1, 0, 1, 0, {}},   {0.0f, 0, 5, 0, 100.0f, 1, 0, 0, 0, {}},
            {10.0f, 0, 5, 0, 0.0f, 1, 0, 0, 0, {}},     {10.0f, 0, 5, 0, 100.0f, 1, 0, 0, 7, {2}},
            {10.0f, 0, 5, 0, 100.0f, 1, 0, 0, 0, {}, 1},          // 0xfe set: skip, ret 1
            {10.0f, 0, 5, 0, 100.0f, 1, 0, 0, 0, {}, 0, 0x0},     // 0x100 clear: skip, ret 1
            {10.0f, 0, 5, 0, 100.0f, 1, 0, 0, 0, {}, 0, 0x100u, 17},  // type 17: skip, ret 1
        };
        for (const PRow& r : rows) {
            sfxbusy_script = r.sfxbusy;
            rscript = r.script;
            ridx = 0;
            draws.clear();
            sfx_id = -1;
            zero(dc, 0x100);
            zero(drone, 0x1000);
            zero(obj, 0x300);
            zero(objx, 0x600);
            zero(botvars, 0x800);
            m.mem.write<u32>(dc, obj);
            m.mem.write<u32>(dc + 4, drone);
            m.mem.write<u32>(obj + 224, drone);   // guard struct aliases the drone blob headless
            m.mem.write<u8>(obj + 254, u8(r.fe));
            m.mem.write<u8>(obj + 255, u8(r.otype));
            m.mem.write<u32>(drone + 1272, r.cflags);
            u32 hw = 0;
            std::memcpy(&hw, &r.health, 4);
            m.mem.write<u32>(drone + 172, hw);
            m.mem.write<u32>(drone + 3356, botvars);
            m.mem.write<u8>(botvars + 1897, u8(r.armour));
            m.mem.write<u32>(0x2A4968u, u32(r.locflag));
            m.mem.write<u32>(0x2A495Cu, u32(r.profflag));
            CallArgs a;
            a.i(dc);
            a.i(u32(r.type));
            a.i(u32(std::int16_t(r.loc)));
            a.i(u32(r.extra));
            a.f(r.dmg);
            int ret = -999;
            std::string trap;
            try {
                const u32 entry = m.addr("BOT_handlePain__FP10DCVars_tagfUss");
                ret = int(m.call_keep(entry, a, 10'000'000).v0);
            } catch (const std::exception& e) { trap = e.what(); }
            u32 h1w = 0, lastw = 0;
            m.mem.read_block(drone + 172, &h1w, 4);
            m.mem.read_block(drone + 336, &lastw, 4);
            const u8 a1 = m.mem.read<u8>(botvars + 1897);
            if (draws.empty()) draws = "-";
            std::printf("P,%.9g,%d,%d,%d,%.9g,%d,%d,%d,%d,%d,%.9g,%u,%.9g,%d,%s,%d,%u,%d%s\n", (double)r.dmg,
                        r.type, r.loc, r.armour, (double)r.health, r.locflag, r.profflag, r.sfxbusy,
                        r.extra, ret, (double)as_float(h1w), unsigned(a1), (double)as_float(lastw),
                        sfx_id, draws.c_str(), r.fe, r.cflags, r.otype,
                        trap.empty() ? "" : (" TRAP " + trap).c_str());
        }
        m.mem.write<u32>(0x2A4968u, 0u);
        m.mem.write<u32>(0x2A495Cu, 0u);
    }
    {  // T: gotoGoal over (slot, type, kind, target, teams, personality, prev): commitment budget.
        std::printf("T,slot,type,kind,teams,pers,prev,script,ret,agoal,commit,limit,setup,draws\n");
        std::vector<u32> rscript;
        std::size_t ridx = 0;
        std::string draws, setup;
        m.hook("Rand_Rand__FUi", [&](nf::ee::Cpu& c) {
            const u32 arg = c.r[4].w[0];
            const u32 ret = ridx < rscript.size() ? rscript[ridx++] : 0u;
            char b[64];
            std::snprintf(b, sizeof b, "%u->%u", arg, ret);
            draws += draws.empty() ? b : std::string(" ") + b;
            c.r[2].d[0] = u64(ret);
            c.r[2].d[1] = 0;
            return true;
        });
        m.hook("AINetwork_SetupGoalPosition__FP12AITarget_tagP11AIPoint_tagP10CelPos_tagfSc",
               [&](nf::ee::Cpu& c) {
                   (void)c;
                   setup = "pos";
                   c.r[2].d[0] = 0u;
                   c.r[2].d[1] = 0;
                   return true;
               });
        m.hook("AINetwork_SetupGoalPositionToObj__FP12AITarget_tagP11AIPoint_tagP7obj_tagfSc",
               [&](nf::ee::Cpu& c) {
                   (void)c;
                   setup = "obj";
                   c.r[2].d[0] = 0u;
                   c.r[2].d[1] = 0;
                   return true;
               });
        struct TRow {
            int slot;
            int type;
            int kind;
            int teams;
            int pers;
            int prev;
            std::vector<u32> script;
        };
        const TRow rows[] = {
            {0, 1, 0, 0, 0, 255, {}}, {1, 1, 0, 0, 0, 255, {}}, {1, 1, 1, 0, 0, 255, {111}},
            {1, 1, 2, 0, 0, 255, {}}, {1, 1, 3, 0, 0, 255, {222}}, {1, 1, 4, 0, 0, 255, {333}},
            {1, 1, 5, 0, 0, 255, {}}, {1, 1, 6, 0, 0, 255, {444}}, {1, 1, 7, 0, 0, 255, {555}},
            {1, 1, 8, 0, 0, 255, {123}}, {1, 3, 9, 0, 0, 255, {234}},
            {1, 1, 1, 0, 5, 255, {111}}, {1, 1, 1, 1, 5, 255, {111}},
            {1, 1, 1, 1, 3, 255, {111}}, {1, 1, 1, 1, 0, 255, {111}},
            {1, 1, 1, 0, 0, 0, {111}}, {1, 2, 4, 1, 0, 255, {333}},
        };
        for (const TRow& r : rows) {
            rscript = r.script;
            ridx = 0;
            draws.clear();
            setup.clear();
            zero(dc, 0x100);
            zero(drone, 0x1000);
            zero(botvars, 0x800);
            zero(smi, 0x20);
            m.mem.write<u32>(dc, obj);
            m.mem.write<u32>(dc + 4, drone);
            m.mem.write<u32>(dc + 12, smi);
            m.mem.write<u32>(smi + 4, 0xCFu);
            m.mem.write<u32>(drone + 3356, botvars);
            m.mem.write<u8>(drone + 182, 1u);
            m.mem.write<u32>(0x2A492Cu, u32(r.teams));
            m.mem.write<u8>(botvars + 171, u8(r.pers));
            m.mem.write<u32>(botvars + 1844, 0u);
            m.mem.write<u8>(botvars + 1893, u8(r.prev));
            m.mem.write<u8>(botvars + u32(r.slot) * 80u + 69u, u8(r.type));
            m.mem.write<u8>(botvars + u32(r.slot) * 80u + 74u, u8(r.kind));
            m.mem.write<u32>(botvars + u32(r.slot) * 80u + 60u, 3u);
            CallArgs a;
            a.i(dc);
            a.i(u32(r.slot));
            a.i(0xF9u);
            int ret = -999;
            std::string trap;
            try {
                const u32 entry = m.addr("BOTSTATE_gotoGoal__FP10DCVars_tagii");
                ret = int(m.call_keep(entry, a, 10'000'000).v0);
            } catch (const std::exception& e) { trap = e.what(); }
            const u8 agoal = m.mem.read<u8>(botvars + 1893);
            u32 bits = 0, limw = 0;
            m.mem.read_block(botvars + 1844, &bits, 4);
            m.mem.read_block(botvars + u32(r.slot) * 80u + 32u, &limw, 4);
            std::string script_txt;
            for (u32 s : r.script) script_txt += (script_txt.empty() ? "" : "/") + std::to_string(s);
            if (script_txt.empty()) script_txt = "-";
            if (setup.empty()) setup = "-";
            if (draws.empty()) draws = "-";
            std::printf("T,%d,%d,%d,%d,%d,%d,%s,%d,%u,%u,%.9g,%s,%s%s\n", r.slot, r.type, r.kind,
                        r.teams, r.pers, r.prev, script_txt.c_str(), ret, unsigned(agoal),
                        (bits & 4u) != 0, (double)as_float(limw), setup.c_str(), draws.c_str(),
                        trap.empty() ? "" : (" TRAP " + trap).c_str());
        }
        m.mem.write<u32>(0x2A492Cu, 0u);
    }
    return 0;
}
// Run the original death-drop path against real bot object/animation state in a P2S match.
int cmd_diff_botdrop(const std::string& elf_path, const std::string& state, int wanted_slot) {
    Machine m(elf_path);
    m.install_libc_hooks();
    m.load_p2s(state);
    hook_sp_copy(m);
    // The saved live frame has its rendered palette cached. Skip its VU0 rebuild; AnimGetBoneWorldTrans still
    // executes against the cached per-bot world matrices, which are the data this seeded probe needs.
    if (!m.hook("psiBuildMatrixPalette__FP7obj_tagP15sAnimObject_tagSc", [](nf::ee::Cpu&) { return true; }))
        throw std::runtime_error("ACTION.ELF lacks psiBuildMatrixPalette symbol");
    struct Capture {
        u32 item = 0, rounds = 0;
        float pos[3]{};
    };
    std::vector<Capture> captures;
    const u32 fake_pickup = m.alloc(0x200);
    m.hook("Pickup_CreateSimple__FP6MATRIXsss", [&](nf::ee::Cpu& cpu) {
        Capture c;
        const u32 matrix = cpu.r[4].w[0];
        c.item = cpu.r[5].w[0];
        c.rounds = cpu.r[6].w[0];
        for (u32 i = 0; i < 3; ++i) c.pos[i] = m.mem.read<float>(matrix + 48 + 4 * i);
        captures.push_back(c);
        cpu.r[2].d[0] = fake_pickup;
        cpu.r[2].d[1] = 0;
        return true;
    });

    std::printf("slot,weapon,type,item,rounds,x,y,z\n");
    for (int slot = 4; slot < 8; ++slot) {
        if (wanted_slot >= 0 && slot != wanted_slot) continue;
        const u32 obj = m.mem.read<u32>(0x2A4980u + u32(slot) * 0x30u + 0x1Cu);
        if (!obj) continue;
        const u8 type = m.mem.read<u8>(obj + 0xFFu);
        if (type != 2u && type != 0x11u) continue;
        const u32 drone = m.mem.read<u32>(obj + 0xE0u);
        const u32 weapon = m.mem.read<u32>(drone + 0xC58u);
        const u32 dc = m.alloc(0x100);
        CallArgs init;
        init.i(obj);
        init.i(dc);
        const auto initialized = m.call_keep("Drone_DCVfromOBJ__FP7obj_tagP10DCVars_tag", init);
        if (!initialized.v0) {
            std::printf("# slot %d: Drone_DCVfromOBJ rejected object 0x%x\n", slot, obj);
            continue;
        }
        captures.clear();
        CallArgs drop;
        drop.i(dc);
        try {
            m.call_keep("DroneWeap_DropWeapon__FP10DCVars_tag", drop, 50'000'000);
        } catch (const nf::ee::Trap& e) {
            const auto source = m.symbol_at(e.pc);
            std::printf("# slot %d weapon %u: TRAP pc=0x%08x inst=0x%08x addr=0x%08x %s%s\n",
                        slot, weapon, e.pc, e.inst, e.addr, e.what(),
                        source ? (" in " + source->name).c_str() : "");
            continue;
        }
        if (captures.empty()) {
            std::printf("# slot %d weapon %u: no Pickup_CreateSimple call\n", slot, weapon);
            continue;
        }
        for (const Capture& c : captures)
            std::printf("%d,%u,0x%x,%u,%u,%.9g,%.9g,%.9g\n", slot, weapon, unsigned(type), c.item, c.rounds,
                        double(c.pos[0]), double(c.pos[1]), double(c.pos[2]));
    }
    return 0;
}

// ---- Seeded MP-brain sweep --------------------------------------------------------------------------
// EE truth tables that need a live match image (weapon_data, nav, pickups, participants): run with
// `--state <match.p2s>` (slot 02 = Skyrail arena). Sections print CSV to stdout:
//   WV/WU/WN/WL/WF  BOTWEAP_isValidWeapon / punchIsBetterIfClose / getWeaponAnimSet / reloadAnimForWeapon /
//                   BOTSTATE_isPreferredWeapon(pref x id) — pure tables over seeded weapon_data
//   WK              weapon_data fields per id (class/minrange/clip/ammotype/flags) for checker defs
//   F  NDrone2_FindOpponent over poked perception cache + moved human (seeded world for rays/sight globals)
//   G  BOTSTATE_pickGoal/processGoals on seeded MP slots (DistanceToEmitter stubbed to 1m; nav traps reported)
//   X  held-state weapon fns over seeded loadouts (list/tooClose/explosive/combatChoice)
//   PV BOTSTATE_setPickupVisitTime over synthetic pickup/clock/index cases
int cmd_diff_botmp(const std::string& elf_path, const std::string& state) {
    Machine m(elf_path);
    m.install_libc_hooks();
    m.load_p2s(state);
    auto as_float = [](u32 w) {
        float f = 0;
        std::memcpy(&f, &w, 4);
        return f;
    };
    auto zero = [&](u32 va, u32 len) {
        for (u32 o = 0; o < len; o += 4) m.mem.write<u32>(va + o, 0);
    };
    const u32 dc = m.alloc(0x100), drone = m.alloc(0x1000), obj = m.alloc(0x300);
    const u32 botvars = m.alloc(0x800), smi = m.alloc(0x20);
    std::printf("# seeded: %s\n", state.c_str());
    std::printf("# dc=0x%x drone=0x%x obj=0x%x botvars=0x%x smi=0x%x\n", dc, drone, obj, botvars,
                smi);
    const int ids[] = {-1, 0, 1, 2, 3, 6, 7, 0x14, 0x16, 0x17, 0x18, 0x1a, 0x1b, 0x1c, 0x1e, 0x24,
                       0x25, 0x2e, 0x2f, 0x30, 0x33, 0x3a, 0x42, 0x43, 0x45, 0x52, 0x53, 0x60};
    {  // WV/WU/WN/WL: pure id tables.
        std::printf("W,fn,id,ret\n");
        const char* fns[] = {"V", "U", "N", "L"};
        const char* syms[] = {
            "BOTWEAP_isValidWeapon__Fs",
            "BOTWEAP_punchIsBetterIfClose__Fs",
            "BOTWEAP_getWeaponAnimSet__Fs",
            "BOTWEAP_reloadAnimForWeapon__Fs",
        };
        for (int f = 0; f < 4; ++f) {
            for (int id : ids) {
                CallArgs a;
                a.i(u32(std::int16_t(id)));
                int ret = -999;
                std::string trap;
                try {
                    ret = int(m.call(syms[f], a).v0);
                } catch (const std::exception& e) { trap = e.what(); }
                std::printf("W,%s,%d,%d%s\n", fns[f], id, ret,
                            trap.empty() ? "" : (" TRAP " + trap).c_str());
            }
        }
    }
    {  // WF: isPreferredWeapon over (pref, id).
        std::printf("WF,pref,id,ret\n");
        for (int pref : {0, 1, 2, 3, 4, 5}) {
            for (int id : ids) {
                zero(botvars, 0x800);
                m.mem.write<u8>(botvars + 171, u8(pref));
                CallArgs a;
                a.i(botvars);
                a.i(u32(std::int16_t(id)));
                int ret = -999;
                std::string trap;
                try {
                    ret = int(m.call("BOTSTATE_isPreferredWeapon__FP10BOT_vars_tUi", a).v0);
                } catch (const std::exception& e) { trap = e.what(); }
                std::printf("WF,%d,%d,%d%s\n", pref, id, ret,
                            trap.empty() ? "" : (" TRAP " + trap).c_str());
            }
        }
    }
    {  // WK: weapon_data fields per id (checker builds identical WeaponDefs).
        std::printf("WK,id,class,minrange,clip,ammotype,flags\n");
        for (int id : ids) {
            if (id < 0 || id >= 115) {
                std::printf("WK,%d,-,-,-,-,-\n", id);
                continue;
            }
            const u32 base = 0x2BF150u + u32(id) * 0x10Cu;
            const u8 cls = m.mem.read<u8>(base + 6);
            u32 mrw = 0, flw = 0;
            m.mem.read_block(base + 8, &mrw, 4);
            m.mem.read_block(base + 0x68, &flw, 4);
            const u16 clip = m.mem.read<u16>(base + 0x92);
            const u8 at = m.mem.read<u8>(base + 0x90);
            std::printf("WK,%d,%u,%.9g,%u,%u,0x%x\n", id, unsigned(cls), (double)as_float(mrw),
                        unsigned(clip), unsigned(at), flw);
        }
    }
    {  // R/S: real goal selection + processing against the seeded MP pickup/objective state.
        std::printf("G,slot,phase,goal_slot,ret,type,flags,max_range,target,kind,route\n");
        m.hook("NDrone2_DistanceToEmitter__FP10CelPos_tagP10AIPath_tagPUsP13AIEmitter_tagP7obj_tagPf",
               [&](nf::ee::Cpu& c) {
                   const u32 out = c.r[29].w[0] + 20u;
                   const u32 ptr = m.mem.read<u32>(out);
                   m.mem.write<u32>(ptr, 0x3F800000u);
                   c.r[2].d[0] = 1u;
                   c.r[2].d[1] = 0;
                   return true;
               });
        const u32 vars_base = 0x26D660u;
        for (int slot = 4; slot < 8; ++slot) {
            const u32 vars = vars_base + u32(slot - 4) * 0x780u;
            const u32 obj = m.mem.read<u32>(0x2A4980u + u32(slot) * 0x30u + 0x1Cu);
            const u32 drone = m.mem.read<u32>(vars + 0x754u);
            if (!obj || !drone) continue;
            const u32 ai_path = m.mem.read<u32>(drone + 2388u);
            if ((ai_path & 3u) != 0 || ai_path >= 0x02000000u) {
                std::printf("G,%d,skip,bad-ai-path,0,0,0,0,0,0,0\n", slot);
                continue;
            }
            const u32 dc = m.alloc(0x100);
            zero(dc, 0x100);
            CallArgs dc_args;
            dc_args.i(obj);
            dc_args.i(dc);
            if (m.call("Drone_DCVfromOBJ__FP7obj_tagP10DCVars_tag", dc_args).v0 == 0) {
                std::printf("G,%d,skip,dcv-init-failed,0,0,0,0,0,0,0\n", slot);
                continue;
            }
            for (int goal_slot = 0; goal_slot < 2; ++goal_slot) {
                CallArgs a;
                a.i(dc);
                a.i(u32(goal_slot));
                int ret = -999;
                std::string trap;
                try {
                    ret = int(m.call_keep("BOTSTATE_pickGoal__FP10DCVars_tagUi", a, 10'000'000).v0);
                } catch (const std::exception& e) {
                    trap = e.what();
                }
                const u32 g = vars + u32(goal_slot) * 0x50u;
                std::printf("G,%d,pick,%d,%d,%u,%u,%u,%u,%u,%u%s\n", slot, goal_slot, ret,
                            unsigned(m.mem.read<u8>(g + 0x45)), unsigned(m.mem.read<u8>(g + 0x46)),
                            unsigned(m.mem.read<u8>(g + 0x47)), unsigned(m.mem.read<u32>(g + 0x3C)),
                            unsigned(m.mem.read<u8>(g + 0x4A)), unsigned(m.mem.read<u32>(g + 0x48)),
                            trap.empty() ? "" : (" TRAP " + trap).c_str());
            }
            CallArgs a;
            a.i(dc);
            std::string trap;
            try {
                m.call_keep("BOTSTATE_processGoals__FP10DCVars_tag", a, 10'000'000);
            } catch (const std::exception& e) {
                trap = e.what();
            }
            for (int goal_slot = 0; goal_slot < 2; ++goal_slot) {
                const u32 g = vars + u32(goal_slot) * 0x50u;
                std::printf("G,%d,process,%d,0,%u,%u,%u,%u,%u,%u%s\n", slot, goal_slot,
                            unsigned(m.mem.read<u8>(g + 0x45)), unsigned(m.mem.read<u8>(g + 0x46)),
                            unsigned(m.mem.read<u8>(g + 0x47)), unsigned(m.mem.read<u32>(g + 0x3C)),
                            unsigned(m.mem.read<u8>(g + 0x4A)), unsigned(m.mem.read<u32>(g + 0x48)),
                            trap.empty() ? "" : (" TRAP " + trap).c_str());
            }
        }
    }
    {  // X: live held-weapon selection over its three one-byte policy flags.
        std::printf("X,slot,arg1,arg2,arg3,opponent,ret\n");
        const u32 vars_base = 0x26D660u;
        for (int slot = 4; slot < 8; ++slot) {
            const u32 vars = vars_base + u32(slot - 4) * 0x780u;
            const u32 obj = m.mem.read<u32>(0x2A4980u + u32(slot) * 0x30u + 0x1Cu);
            const u32 drone = m.mem.read<u32>(vars + 0x754u);
            if (!obj || !drone) continue;
            const u32 opponent = m.mem.read<u32>(drone + 368u);
            const u32 target = opponent ? opponent : m.mem.read<u32>(0x2A4980u + 0x1Cu);
            for (int check_same = 0; check_same < 2; ++check_same) {
                for (int silent = 0; silent < 2; ++silent) {
                    for (int arg3 = 0; arg3 < 2; ++arg3) {
                        CallArgs a;
                        a.i(drone);
                        a.i(target);
                        a.i(u32(check_same));
                        a.i(u32(silent));
                        a.i(u32(arg3));
                        int ret = -999;
                        std::string trap;
                        try {
                            ret = int(m.call("BOTSTATE_combatWeaponChangeChoice__FP9Drone_tagP7obj_tagScScSc",
                                             a).v0);
                        } catch (const std::exception& e) {
                            trap = e.what();
                        }
                        std::printf("X,%d,%d,%d,%d,%u,%d%s\n", slot, check_same, silent, arg3, target, ret,
                                    trap.empty() ? "" : (" TRAP " + trap).c_str());
                    }
                }
            }
        }
    }
    {  // PV: real pickup visit lockout write (clock + 45 seconds, per BOT_vars index).
        std::printf("PV,obj_type,bot_index,clock,slot_value\n");
        const u32 drone = m.alloc(0x1000), obj = m.alloc(0x300), botvars = m.alloc(0x800);
        const u32 pickup = m.alloc(0x100);
        m.mem.write<u32>(drone + 12u, obj);
        m.mem.write<u32>(drone + 3356u, botvars);
        for (int type : {2, 17}) {
            for (float clock : {0.0f, 100.0f}) {
                for (int bot_index = 0; bot_index < 4; ++bot_index) {
                    zero(obj, 0x300);
                    zero(botvars, 0x800);
                    zero(pickup, 0x100);
                    m.mem.write<u8>(obj + 255u, u8(type));
                    m.mem.write<u16>(botvars + 1886u, u16(bot_index));
                    u32 cw = 0, sentinel = 0xBF800000u;
                    std::memcpy(&cw, &clock, 4);
                    m.mem.write<u32>(0x2A4B1Cu, cw);
                    for (int i = 0; i < 4; ++i) m.mem.write<u32>(pickup + 128u + 4u * u32(i), sentinel);
                    CallArgs a;
                    a.i(drone);
                    a.i(pickup);
                    try {
                        m.call_keep("BOTSTATE_setPickupVisitTime__FP9Drone_tagP9MP_PICKUP", a, 10'000'000);
                    } catch (const std::exception& e) {
                        std::printf("PV,%d,%d,%.9g,TRAP %s\n", type, bot_index, double(clock), e.what());
                        continue;
                    }
                    const u32 stored = m.mem.read<u32>(pickup + 128u + 4u * u32(bot_index));
                    std::printf("PV,%d,%d,%.9g,%.9g\n", type, bot_index, double(clock),
                                double(as_float(stored)));
                }
            }
        }
    }
    {  // F: NDrone2_FindOpponent over poked perception cache (seeded globals, synth structs).
        // MPGame slots: 0 = human obj (synth, poked pos/team/status), 4 = self obj (synth, skipped),
        // 5 = spare bot obj (synth, for bot-candidate + pile-on rows, with synth BOT_vars).
        std::printf("F,row,ret,opp,distr,alerted,trait,hist,calls,seen,lost\n");
        int setopp_log[8];
        int setopp_n = 0;
        m.hook("NDrone2_SetOpponent__FP9Drone_tagP7obj_tag", [&](nf::ee::Cpu& c) {
            // Observe-only (return false): log the call, then execute the real thing so +368/history
            // writes are faithful for multi-call rows (drop then re-acquire).
            u32 o = c.r[5].w[0];
            int slot = -1;
            for (int s = 0; s < 8; ++s) {
                u32 so = 0;
                m.mem.read_block(0x2A4980u + u32(s) * 0x30u + 0x1Cu, &so, 4);
                if (so == o && o != 0) slot = s;
            }
            if (setopp_n < 8) setopp_log[setopp_n++] = slot;
            return false;
        });
        // (no mid-function observe hooks; SetOpponent observe above is the only hook)
        const u32 selfobj = m.alloc(0x300), humanobj = m.alloc(0x300), bot5obj = m.alloc(0x300);
        const u32 selfcell = m.alloc(0x600);   // self obj+224 struct (alive-test gate: +1272/+172)
        const u32 bot5cell = m.alloc(0x600);   // bot5's obj+224 struct (+368 opp, +1272 flags, +172 health)
        const u32 bot5vars = m.alloc(0x800);
        auto wvec = [&](u32 va, float x, float y, float z) {
            u32 b[4] = {0, 0, 0, 0};
            std::memcpy(b, &x, 4);
            std::memcpy(b + 1, &y, 4);
            std::memcpy(b + 2, &z, 4);
            for (int k = 0; k < 4; ++k) m.mem.write<u32>(va + 4u * u32(k), b[k]);
        };
        auto wother = [&](u32 bv, int j, float stamp, float sq, float facing, u32 flags) {
            const u32 o = bv + 176u + u32(j) * 16u;
            u32 sw = 0, qw = 0, fw = 0;
            std::memcpy(&sw, &stamp, 4);
            std::memcpy(&qw, &sq, 4);
            std::memcpy(&fw, &facing, 4);
            m.mem.write<u32>(o, sw);
            m.mem.write<u32>(o + 4, qw);
            m.mem.write<u32>(o + 8, fw);
            m.mem.write<u32>(o + 12, flags);
        };
        struct FRow {
            const char* name;
            int pers;
            int aware;
            int alerted;
            int trait;
            int curropp;      // -1 none, else slot with obj
            float oppdist;
            int lost;
            int aggr;         // Drone+0xb5 byte value 0..4
            int teams;        // MPSettings+0x18c teams flag
            int mteam;        // my team (slot 4)
            int hteam;        // human team
            int hstatus;      // human MPGame+0x26
            float hx, hy, hz; // human pos
            float hyaw;
            float mx, my, mz; // me pos
            float myaw;
            float hsq;        // other[0].sq_dist
            float hface;      // other[0].facing (cache; scoring only)
            u32 hflags;       // other[0].flags
            int bot5;         // 0 absent, 1 present
            float b5sq;
            float b5face;     // my-cache facing for bot5
            u32 b5flags;
            float b5x, b5y, b5z;
            float b5yaw;
            int b5opp;        // bot5's drone+368: 0 none, 1 human, 2 self
            u32 b5targ;       // bot5 BOT_vars+0x770 targeted flag (pile-on input)
            float b5mface;    // bot5-cache other[5].facing (cone test for bot candidates)
            int histn;        // history preload count (objs: 0 human, 4 self, 5 bot5)
            int hist[4];
            int htype;        // human obj+0xff (3 human, 1 removed, 17/18 eliminated)
            int b5fe;         // bot5 obj+0xfe (1 = skip damage/targeting window)
            u32 b5cflags;     // bot5 cell +1272 state flags (needs 0x100, not 0x600)
            float visang = 0;    // drone+228 sight half-angle rad (0 = game default pi/2)
            float visrange = 0;  // drone+232 sight range m (0 = game default 24)
            int f186 = 0;        // BOT_vars+1886 u16 (cached-facing index for bot candidates)
            int kaw = 0;         // extra Drone+0x4f8 kAware bit16 (history runs without aware-bit)
            int b4opp = 0;       // selfcell+368 b4 gate: 0 auto-mirror curropp, 1 human obj, 2 self obj, -1 none
        };
        const FRow rows[] = {
            // name, pers,aware,alerted,trait,curropp,oppdist,lost,aggr, teams,mteam,hteam,hstatus,
            //   hx,hy,hz,hyaw, mx,my,mz,myaw, hsq,hface,hflags,
            //   bot5, b5sq,b5face,b5flags, b5x,b5y,b5z,b5yaw, b5opp,b5targ, b5mface, histn,hist, htype,b5fe,b5cflags
            {"empty", 0,0,0,-1,-1,1e9f,0,2, 0,2,2,0, 0,0,0,0, 0,0,0,0, 1e9f,0,0, 0, 0,0,0, 0,0,0,0, 0,0, 0, 0,{0,0,0,0}, 3,0,0x100u},
            {"ahead", 0,0,0,-1,-1,1e9f,0,2, 0,2,2,0, 0,0,10,0, 0,0,0,0, 100,0,6, 0, 0,0,0, 0,0,0,0, 0,0, 0, 0,{0,0,0,0}, 3,0,0x100u},
            {"behind", 0,0,0,-1,-1,1e9f,0,2, 0,2,2,0, 0,0,-10,0, 0,0,0,0, 100,180,6, 0, 0,0,0, 0,0,0,0, 0,0, 0, 0,{0,0,0,0}, 3,0,0x100u},
            {"diag45", 0,0,0,-1,-1,1e9f,0,2, 0,2,2,0, 10,0,10,0, 0,0,0,0, 200,0,6, 0, 0,0,0, 0,0,0,0, 0,0, 0, 0,{0,0,0,0}, 3,0,0x100u},
            {"diag135", 0,0,0,-1,-1,1e9f,0,2, 0,2,2,0, 10,0,-10,0, 0,0,0,0, 200,0,6, 0, 0,0,0, 0,0,0,0, 0,0, 0, 0,{0,0,0,0}, 3,0,0x100u},
            {"far", 0,0,0,-1,-1,1e9f,0,2, 0,2,2,0, 0,0,10,0, 0,0,0,0, 100000,0,6, 0, 0,0,0, 0,0,0,0, 0,0, 0, 0,{0,0,0,0}, 3,0,0x100u},
            {"edge576", 0,0,0,-1,-1,1e9f,0,2, 0,2,2,0, 0,0,10,0, 0,0,0,0, 576,0,6, 0, 0,0,0, 0,0,0,0, 0,0, 0, 0,{0,0,0,0}, 3,0,0x100u},
            {"edge575", 0,0,0,-1,-1,1e9f,0,2, 0,2,2,0, 0,0,10,0, 0,0,0,0, 575,0,6, 0, 0,0,0, 0,0,0,0, 0,0, 0, 0,{0,0,0,0}, 3,0,0x100u},
            {"concealed", 0,0,0,-1,-1,1e9f,0,2, 0,2,2,0, 0,0,10,0, 0,0,0,0, 100,0,7, 0, 0,0,0, 0,0,0,0, 0,0, 0, 0,{0,0,0,0}, 3,0,0x100u},
            {"conceal-side", 0,0,0,-1,-1,1e9f,0,2, 0,2,2,0, 0,0,10,0, 0,0,0,0, 100,45,7, 0, 0,0,0, 0,0,0,0, 0,0, 0, 0,{0,0,0,0}, 3,0,0x100u},
            {"carrier", 0,0,0,-1,-1,1e9f,0,2, 0,2,2,1, 0,0,20,0, 0,0,0,0, 400,0,6, 0, 0,0,0, 0,0,0,0, 0,0, 0, 0,{0,0,0,0}, 3,0,0x100u},
            {"sameteam", 0,0,0,-1,-1,1e9f,0,2, 1,0,0,0, 0,0,10,0, 0,0,0,0, 100,0,14, 0, 0,0,0, 0,0,0,0, 0,0, 0, 0,{0,0,0,0}, 3,0,0x100u},
            {"guardian", 2,0,0,-1,-1,1e9f,0,2, 1,0,0,0, 0,0,10,0, 0,0,0,0, 100,0,14, 0, 0,0,0, 0,0,0,0, 0,0, 0, 0,{0,0,0,0}, 3,0,0x100u},
            {"aware-far", 0,1,0,-1,-1,1e9f,0,2, 0,2,2,0, 0,0,200,0, 0,0,0,0, 40000,0,2, 0, 0,0,0, 0,0,0,0, 0,0, 0, 0,{0,0,0,0}, 3,0,0x100u},
            {"keep", 0,0,0,-1,0,10,0,2, 0,2,2,0, 0,0,10,0, 0,0,0,0, 100,0,6, 0, 0,0,0, 0,0,0,0, 0,0, 0, 0,{0,0,0,0}, 3,0,0x100u},
            {"timeout", 0,0,0,-1,0,10,99999,2, 0,2,2,0, 0,0,10,0, 0,0,0,0, 100,0,6, 0, 0,0,0, 0,0,0,0, 0,0, 0, 0,{0,0,0,0}, 3,0,0x100u},
            {"notimeout", 0,0,0,-1,0,10,0,2, 0,2,2,0, 0,0,10,0, 0,0,0,0, 100,0,6, 0, 0,0,0, 0,0,0,0, 0,0, 0, 0,{0,0,0,0}, 3,0,0x100u},
            {"deadtype", 0,0,0,-1,0,10,0,2, 0,2,2,0, 0,0,10,0, 0,0,0,0, 100,0,6, 0, 0,0,0, 0,0,0,0, 0,0, 0, 0,{0,0,0,0}, 1,0,0x100u},
            {"alerted-bypass", 0,0,1,-1,-1,1e9f,0,2, 0,2,2,0, 0,0,-10,0, 0,0,0,0, 100,180,6, 0, 0,0,0, 0,0,0,0, 0,0, 0, 0,{0,0,0,0}, 3,0,0x100u},
            {"concealed-novis", 0,0,0,-1,-1,1e9f,0,2, 0,2,2,0, 0,0,10,0, 0,0,0,0, 100,0,3, 0, 0,0,0, 0,0,0,0, 0,0, 0, 0,{0,0,0,0}, 3,0,0x100u},
            {"trait-far", 0,0,0,0,-1,1e9f,0,2, 0,2,2,0, 0,0,20,0, 0,0,0,0, 400,0,6, 1, 100,0,6, 0,0,10,0, 0,0, 0, 0,{0,0,0,0}, 3,0,0x100u},
            {"notrait", 0,0,0,-1,-1,1e9f,0,2, 0,2,2,0, 0,0,20,0, 0,0,0,0, 400,0,6, 1, 100,0,6, 0,0,10,0, 0,0, 0, 0,{0,0,0,0}, 3,0,0x100u},
            {"pile-berserk", 5,0,0,-1,-1,1e9f,0,2, 0,2,2,0, 0,0,20,0, 0,0,0,0, 400,0,6, 1, 100,0,6, 0,0,10,0, 0,1, 0, 0,{0,0,0,0}, 3,0,0x100u, 0,0,0,1},
            {"nopile", 5,0,0,-1,-1,1e9f,0,2, 0,2,2,0, 0,0,20,0, 0,0,0,0, 400,0,6, 1, 100,0,6, 0,0,10,0, 0,0, 0, 0,{0,0,0,0}, 3,0,0x100u, 0,0,0,1},
            {"pile-human", 5,0,0,-1,-1,1e9f,0,2, 0,2,2,0, 0,0,10,0, 0,0,0,0, 100,0,6, 1, 400,0,6, 0,0,20,0, 1,0, 0, 0,{0,0,0,0}, 3,0,0x100u, 0,0,0,1},
            {"nopile-human", 5,0,0,-1,-1,1e9f,0,2, 0,2,2,0, 0,0,10,0, 0,0,0,0, 100,0,6, 1, 400,0,6, 0,0,20,0, 0,0, 0, 0,{0,0,0,0}, 3,0,0x100u, 0,0,0,1},
            {"b5-fe", 0,0,0,-1,-1,1e9f,0,2, 0,2,2,0, 0,0,20,0, 0,0,0,0, 400,0,6, 1, 100,0,6, 0,0,10,0, 0,0, 0, 0,{0,0,0,0}, 3,1,0x100u},
            {"b5-noactive", 0,0,0,-1,-1,1e9f,0,2, 0,2,2,0, 0,0,20,0, 0,0,0,0, 400,0,6, 1, 100,0,6, 0,0,10,0, 0,0, 0, 0,{0,0,0,0}, 3,0,0x0},
            {"b5-ahead", 0,0,0,-1,-1,1e9f,0,2, 0,2,2,0, 0,0,0,0, 0,0,0,0, 1e9f,0,0, 1, 100,0,6, 0,0,10,0, 0,0, 0, 0,{0,0,0,0}, 3,0,0x100u},
            {"b5-behind", 0,0,0,-1,-1,1e9f,0,2, 0,2,2,0, 0,0,0,0, 0,0,0,0, 1e9f,0,0, 1, 100,0,6, 0,0,-10,0, 0,0, 180, 0,{0,0,0,0}, 3,0,0x100u},
            {"hist-stick", 0,1,0,-1,0,10,0,2, 0,2,2,0, 0,0,10,0, 0,0,0,0, 100,0,6, 0, 0,0,0, 0,0,0,0, 0,0, 0, 4,{0,0,0,0}, 3,0,0x100u},
            {"hist-switch", 0,1,0,-1,0,10,0,2, 0,2,2,0, 0,0,10,0, 0,0,0,0, 100,0,6, 1, 0,0,0, 0,0,0,0, 0,0, 0, 4,{5,5,5,5}, 3,0,0x100u},
            {"hist-allzero", 0,1,0,-1,0,10,0,2, 0,2,2,0, 0,0,10,0, 0,0,0,0, 100,0,6, 0, 0,0,0, 0,0,0,0, 0,0, 0, 0,{0,0,0,0}, 3,0,0x100u},
            {"lost13", 0,0,0,-1,0,10,13,2, 0,2,2,0, 0,0,10,0, 0,0,0,0, 100,0,6, 0, 0,0,0, 0,0,0,0, 0,0, 0, 0,{0,0,0,0}, 3,0,0x100u, 0,0,0,0},
            {"lost14", 0,0,0,-1,0,10,14,2, 0,2,2,0, 0,0,10,0, 0,0,0,0, 100,0,6, 0, 0,0,0, 0,0,0,0, 0,0, 0, 0,{0,0,0,0}, 3,0,0x100u, 0,0,0,0},
            {"lost15", 0,0,0,-1,0,10,15,2, 0,2,2,0, 0,0,10,0, 0,0,0,0, 100,0,6, 0, 0,0,0, 0,0,0,0, 0,0, 0, 0,{0,0,0,0}, 3,0,0x100u, 0,0,0,0},
            {"lost16", 0,0,0,-1,0,10,16,2, 0,2,2,0, 0,0,10,0, 0,0,0,0, 100,0,6, 0, 0,0,0, 0,0,0,0, 0,0, 0, 0,{0,0,0,0}, 3,0,0x100u, 0,0,0,0},
            {"lost20", 0,0,0,-1,0,10,20,2, 0,2,2,0, 0,0,10,0, 0,0,0,0, 100,0,6, 0, 0,0,0, 0,0,0,0, 0,0, 0, 0,{0,0,0,0}, 3,0,0x100u, 0,0,0,0},
            {"lost24", 0,0,0,-1,0,10,24,2, 0,2,2,0, 0,0,10,0, 0,0,0,0, 100,0,6, 0, 0,0,0, 0,0,0,0, 0,0, 0, 0,{0,0,0,0}, 3,0,0x100u, 0,0,0,0},
            {"lost30", 0,0,0,-1,0,10,30,2, 0,2,2,0, 0,0,10,0, 0,0,0,0, 100,0,6, 0, 0,0,0, 0,0,0,0, 0,0, 0, 0,{0,0,0,0}, 3,0,0x100u, 0,0,0,0},
            {"kaw-behind", 0,0,0,-1,-1,1e9f,0,2, 0,2,2,0, 0,0,-10,0, 0,0,0,0, 100,180,6, 0, 0,0,0, 0,0,0,0, 0,0, 0, 0,{0,0,0,0}, 3,0,0x100u, 0,0,0,1},
            {"kaw-ahead", 0,0,0,-1,-1,1e9f,0,2, 0,2,2,0, 0,0,10,0, 0,0,0,0, 100,0,6, 0, 0,0,0, 0,0,0,0, 0,0, 0, 0,{0,0,0,0}, 3,0,0x100u, 0,0,0,1},
            {"flag1-behind", 0,0,0,-1,-1,1e9f,0,2, 0,2,2,0, 0,0,-10,0, 0,0,0,0, 100,180,7, 0, 0,0,0, 0,0,0,0, 0,0, 0, 0,{0,0,0,0}, 3,0,0x100u, 0,0,0,1},
            {"lost839", 0,0,0,-1,0,10,839,2, 0,2,2,0, 0,0,10,0, 0,0,0,0, 100,0,6, 0, 0,0,0, 0,0,0,0, 0,0, 0, 0,{0,0,0,0}, 3,0,0x100u, 0,0,0,0},
            {"lost840", 0,0,0,-1,0,10,840,2, 0,2,2,0, 0,0,10,0, 0,0,0,0, 100,0,6, 0, 0,0,0, 0,0,0,0, 0,0, 0, 0,{0,0,0,0}, 3,0,0x100u, 0,0,0,0},
            {"lost841", 0,0,0,-1,0,10,841,2, 0,2,2,0, 0,0,10,0, 0,0,0,0, 100,0,6, 0, 0,0,0, 0,0,0,0, 0,0, 0, 0,{0,0,0,0}, 3,0,0x100u, 0,0,0,0},
            {"keep-aware", 0,1,0,-1,0,10,0,2, 0,2,2,0, 0,0,10,0, 0,0,0,0, 100,0,6, 0, 0,0,0, 0,0,0,0, 0,0, 0, 0,{0,0,0,0}, 3,0,0x100u, 0,0,0,0},
            {"aware-behind", 0,1,0,-1,-1,1e9f,0,2, 0,2,2,0, 0,0,-10,0, 0,0,0,0, 100,180,6, 0, 0,0,0, 0,0,0,0, 0,0, 0, 0,{0,0,0,0}, 3,0,0x100u, 0,0,0,0},
            {"aware-alerted", 0,1,1,-1,-1,1e9f,0,2, 0,2,2,0, 0,0,10,0, 0,0,0,0, 100,0,6, 0, 0,0,0, 0,0,0,0, 0,0, 0, 0,{0,0,0,0}, 3,0,0x100u, 0,0,0,0},
            {"b5cach-acc", 0,0,0,-1,-1,1e9f,0,2, 0,2,2,0, 0,0,0,0, 0,0,0,0, 1e9f,0,0, 1, 100,0,6, 0,0,10,0, 0,0, 0, 0,{0,0,0,0}, 3,0,0x100u, 0,0,0,1},
            {"b5cach-rej", 0,0,0,-1,-1,1e9f,0,2, 0,2,2,0, 0,0,0,0, 0,0,0,0, 1e9f,0,0, 1, 100,0,6, 0,0,10,0, 0,0, 180, 0,{0,0,0,0}, 3,0,0x100u, 0,0,0,1},
            {"b5fe-kaw", 0,0,0,-1,-1,1e9f,0,2, 0,2,2,0, 0,0,0,0, 0,0,0,0, 1e9f,0,0, 1, 100,0,6, 0,0,10,0, 0,0, 0, 0,{0,0,0,0}, 3,1,0x100u, 0,0,0,1},
            {"pile-b4", 5,0,0,-1,-1,1e9f,0,2, 0,2,2,0, 0,0,10,0, 0,0,0,0, 100,0,6, 1, 400,0,6, 0,0,20,0, 0,0, 0, 0,{0,0,0,0}, 3,0,0x100u, 0,0,0,1,1},
            {"narrow-flag1", 0,0,0,-1,-1,1e9f,0,2, 0,2,2,0, 0,0,-10,0, 0,0,0,0, 100,8,7, 0, 0,0,0, 0,0,0,0, 0,0, 0, 0,{0,0,0,0}, 3,0,0x100u, 0.1f,0,0,1},
            {"narrow-plain", 0,0,0,-1,-1,1e9f,0,2, 0,2,2,0, 0,0,-10,0, 0,0,0,0, 100,8,6, 0, 0,0,0, 0,0,0,0, 0,0, 0, 0,{0,0,0,0}, 3,0,0x100u, 0.1f,0,0,1},
            {"ag4-1199", 0,0,0,-1,0,10,1199,4, 0,2,2,0, 0,0,10,0, 0,0,0,0, 100,0,6, 0, 0,0,0, 0,0,0,0, 0,0, 0, 0,{0,0,0,0}, 3,0,0x100u, 0,0,0,0},
            {"ag4-1200", 0,0,0,-1,0,10,1200,4, 0,2,2,0, 0,0,10,0, 0,0,0,0, 100,0,6, 0, 0,0,0, 0,0,0,0, 0,0, 0, 0,{0,0,0,0}, 3,0,0x100u, 0,0,0,0},
            {"ag4-1201", 0,0,0,-1,0,10,1201,4, 0,2,2,0, 0,0,10,0, 0,0,0,0, 100,0,6, 0, 0,0,0, 0,0,0,0, 0,0, 0, 0,{0,0,0,0}, 3,0,0x100u, 0,0,0,0},
            {"ag0-359", 0,0,0,-1,0,10,359,0, 0,2,2,0, 0,0,10,0, 0,0,0,0, 100,0,6, 0, 0,0,0, 0,0,0,0, 0,0, 0, 0,{0,0,0,0}, 3,0,0x100u, 0,0,0,0},
            {"ag0-360", 0,0,0,-1,0,10,360,0, 0,2,2,0, 0,0,10,0, 0,0,0,0, 100,0,6, 0, 0,0,0, 0,0,0,0, 0,0, 0, 0,{0,0,0,0}, 3,0,0x100u, 0,0,0,0},
            {"ag0-361", 0,0,0,-1,0,10,361,0, 0,2,2,0, 0,0,10,0, 0,0,0,0, 100,0,6, 0, 0,0,0, 0,0,0,0, 0,0, 0, 0,{0,0,0,0}, 3,0,0x100u, 0,0,0,0},
        };
        for (const FRow& r : rows) {
            zero(dc, 0x100);
            zero(drone, 0x1000);
            zero(botvars, 0x800);
            zero(selfobj, 0x300);
            zero(selfcell, 0x600);
            zero(humanobj, 0x300);
            zero(bot5obj, 0x300);
            zero(bot5cell, 0x600);
            zero(bot5vars, 0x800);
            setopp_n = 0;
            // Synth structs. Drone+12 = own obj (entry alive-test gate); obj+224 = guard struct.
            m.mem.write<u32>(dc, selfobj);
            m.mem.write<u32>(dc + 4, drone);
            m.mem.write<u32>(drone + 12, selfobj);
            m.mem.write<u32>(selfobj + 224, selfcell);
            m.mem.write<u32>(selfcell + 1272, 0x100u);
            u32 b4o = 0u; // pile-on b4 gate (MPGame[4].obj+224+368): auto-mirror curropp unless overridden
            if (r.b4opp == 1) b4o = humanobj;
            else if (r.b4opp == 2) b4o = selfobj;
            else if (r.b4opp == 0 && r.curropp == 0) b4o = humanobj;
            if (b4o) m.mem.write<u32>(selfcell + 368, b4o);
            float glim = 300.0f; // goal[0] distraction limit so increaseDistraction writes are visible
            u32 glimw = 0;
            std::memcpy(&glimw, &glim, 4);
            m.mem.write<u32>(botvars + 32, glimw);
            m.mem.write<u32>(drone + 268, 235u); // GotoGoal state: distraction path gate
            u32 shw = 0;
            float sh100 = 100.0f;
            std::memcpy(&shw, &sh100, 4);
            m.mem.write<u32>(selfcell + 172, shw);
            m.mem.write<u32>(drone + 3356, botvars);
            m.mem.write<u8>(drone + 181, u8(r.aggr));
            m.mem.write<u8>(drone + 180, 1u);
            m.mem.write<u8>(drone + 182, 1u);
            u32 sr = 0, cn = 0;
            float rr = r.visrange != 0 ? r.visrange : 24.0f, cc = r.visang != 0 ? r.visang : 1.5707964f;
            std::memcpy(&sr, &rr, 4);
            std::memcpy(&cn, &cc, 4);
            m.mem.write<u32>(drone + 0xe8, sr);
            m.mem.write<u32>(drone + 0xe4, cn);
            m.mem.write<u32>(drone + 1272, 0x100u);
            u32 hw = 0;
            float h100 = 100.0f;
            std::memcpy(&hw, &h100, 4);
            m.mem.write<u32>(drone + 172, hw);
            m.mem.write<u16>(botvars + 1886, u16(r.f186));
            m.mem.write<u32>(drone + 368, 0u);
            u32 odw = 0;
            std::memcpy(&odw, &r.oppdist, 4);
            m.mem.write<u32>(drone + 416, odw);
            m.mem.write<u32>(drone + 628, u32(r.lost));
            if (r.aware) m.mem.write<u32>(drone + 1272, 0x10100u);
            if (r.kaw) m.mem.write<u32>(drone + 1272, 0x10100u); // gate+kAware, no aware-bit
            m.mem.write<u8>(selfobj + 255, 2u);
            wvec(selfobj + 0x30, r.mx, r.my, r.mz);
            u32 myw = 0;
            std::memcpy(&myw, &r.myaw, 4);
            m.mem.write<u32>(selfobj + 0x54, myw);
            m.mem.write<u8>(humanobj + 255, u8(r.htype));
            m.mem.write<u8>(humanobj + 254, 0u);
            wvec(humanobj + 0x30, r.hx, r.hy, r.hz);
            u32 hyw = 0;
            std::memcpy(&hyw, &r.hyaw, 4);
            m.mem.write<u32>(humanobj + 0x54, hyw);
            m.mem.write<u8>(botvars + 171, u8(r.pers));
            if (r.aware) m.mem.write<u8>(botvars + 172, 8u);
            m.mem.write<u8>(botvars + 1899, u8(r.trait < 0 ? 255 : r.trait));
            m.mem.write<u8>(botvars + 1903, u8(r.alerted));
            wother(botvars, 0, 0, r.hsq, r.hface, r.hflags);
            if (r.bot5) {
                m.mem.write<u8>(bot5obj + 255, 2u);
                m.mem.write<u8>(bot5obj + 254, u8(r.b5fe));
                m.mem.write<u32>(bot5obj + 224, bot5cell);
                wvec(bot5obj + 0x30, r.b5x, r.b5y, r.b5z);
                u32 b5yw = 0;
                std::memcpy(&b5yw, &r.b5yaw, 4);
                m.mem.write<u32>(bot5obj + 0x54, b5yw);
                u32 b5o = 0;
                if (r.b5opp == 1) b5o = humanobj;
                else if (r.b5opp == 2) b5o = selfobj;
                m.mem.write<u32>(bot5cell + 368, b5o);
                m.mem.write<u32>(bot5cell + 1272, r.b5cflags);
                m.mem.write<u32>(bot5cell + 172, hw);
                m.mem.write<u32>(bot5cell + 3356, bot5vars);
                wother(botvars, 5, 0, r.b5sq, r.b5face, r.b5flags);
                wother(bot5vars, r.f186, 0, r.b5sq, r.b5mface, 6u);
                m.mem.write<u8>(bot5vars + 1904, u8(r.b5targ));
            }
            // History preload (objs; head 0).
            for (int k = 0; k < 16; ++k) {
                u32 w = 0;
                if (k < r.histn && k < 4) {
                    const int sl = r.hist[k];
                    w = sl == 0 ? humanobj : (sl == 4 ? selfobj : (sl == 5 ? bot5obj : 0u));
                }
                m.mem.write<u32>(botvars + 1756u + 4u * u32(k), w);
            }
            m.mem.write<u8>(botvars + 1900, 0u);
            // MPGame/MPSettings pokes.
            m.mem.write<u32>(0x2A4980u + 0 * 0x30u + 0x1Cu, humanobj);
            m.mem.write<u32>(0x2A4980u + 4 * 0x30u + 0x1Cu, selfobj);
            m.mem.write<u32>(0x2A4980u + 5 * 0x30u + 0x1Cu, r.bot5 ? bot5obj : 0u);
            for (int s : {1, 2, 3, 6, 7}) m.mem.write<u32>(0x2A4980u + u32(s) * 0x30u + 0x1Cu, 0u);
            if (r.curropp >= 0) {
                u32 oo = r.curropp == 0 ? humanobj : selfobj;
                m.mem.write<u32>(drone + 368, oo);
            }
            (void)0; // (per-row pre-state diag removed; CSV below is the record)
            int ret = -999;
            std::string trap;
            try {
                CallArgs a;
                a.i(drone);
                ret = int(m.call_keep(m.addr("NDrone2_FindOpponent__FP9Drone_tag"), a, 10'000'000).v0);
            } catch (const std::exception& e) { trap = e.what(); }
            u32 oppafter = 0, distr = 0, seen = 0, lost = 0;
            m.mem.read_block(drone + 368, &oppafter, 4);
            m.mem.read_block(botvars + 1832, &distr, 4);
            m.mem.read_block(drone + 624, &seen, 4);
            m.mem.read_block(drone + 628, &lost, 4);
            u8 al = m.mem.read<u8>(botvars + 1903), tr = m.mem.read<u8>(botvars + 1893);
            std::string hist = "";
            for (int k = 0; k < 16; ++k) {
                u32 w = 0;
                m.mem.read_block(botvars + 1756u + 4u * u32(k), &w, 4);
                char b[16];
                std::snprintf(b, sizeof b, "%s%x", k ? "/" : "",
                              w == 0 ? 99 : (w == humanobj ? 0 : (w == selfobj ? 4 : (w == bot5obj ? 5 : 8))));
                hist += b;
            }
            std::string calls = "";
            for (int k = 0; k < setopp_n; ++k) {
                char b[16];
                std::snprintf(b, sizeof b, "%s%d", k ? "+" : "", setopp_log[k]);
                calls += b;
            }
            if (calls.empty()) calls = "-";
            const int oppslot =
                oppafter == 0 ? -1 : (oppafter == humanobj ? 0 : (oppafter == selfobj ? 4 : (oppafter == bot5obj ? 5 : 8)));
            std::printf("F,%s,%d,%d,%.9g,%u,%u,%s,%s,%u,%u%s\n", r.name, ret, oppslot,
                        (double)as_float(distr), unsigned(al), unsigned(tr), hist.c_str(), calls.c_str(),
                        seen, lost, trap.empty() ? "" : (" TRAP " + trap).c_str());
        }
    }
    return 0;
}




struct OraclePad {
    u32 port = 0;
    u16 buttons = 0;
    u8 sticks[4] = {};
};

int cmd_mp_oracle(const std::string& elf, const std::string& state, int rows,
                  const std::string& pad_script, int watch_human_hp_slot,
                  u32 trace_frame, int trace_slot, bool trace_rng,
                  const std::vector<int>& give_weapons,
                  int watch_drone_anim_slot, u32 watch_drone_frame,
                  bool game_flow, bool sample_at_game_run_hook, u32 watch_addr = 0) {
    if (state.empty() || rows < 1)
        throw std::runtime_error("mp-oracle needs --state <p2s> and --rows N");
    if (sample_at_game_run_hook && !game_flow)
        throw std::runtime_error("--sample-at-game-run-hook requires GameFlow_Main");
    if (watch_addr != 0 && watch_human_hp_slot >= 0)
        throw std::runtime_error("--watch-addr excludes --watch-human-hp (single watch slot)");
    if (watch_addr != 0 && watch_drone_anim_slot >= 0)
        throw std::runtime_error("--watch-addr excludes --watch-drone-anim (single watch slot)");
    if (sample_at_game_run_hook && trace_frame != 0)
        throw std::runtime_error("--sample-at-game-run-hook excludes --trace-frame");
    if ((trace_frame == 0) != (trace_slot == -1)
        || (trace_frame != 0 && (trace_slot < 4 || trace_slot > 7))
        || (trace_frame != 0 && (watch_human_hp_slot >= 0 || trace_rng)))
        throw std::runtime_error(
            "--trace-frame excludes --watch-human-hp and --trace-rng");
    std::map<u32, std::vector<OraclePad>> pads;
    if (!pad_script.empty()) {
        std::ifstream input(pad_script);
        if (!input) throw std::runtime_error("cannot open pad script: " + pad_script);
        u32 frame = 0, port = 0, buttons = 0;
        int x = 0, y = 0, rx = 0, ry = 0;
        while (input >> frame >> port >> buttons >> x >> y >> rx >> ry) {
            if (port >= 4 || buttons > 0xFFFF || x < 0 || x > 255 || y < 0 || y > 255
                || rx < 0 || rx > 255 || ry < 0 || ry > 255)
                throw std::runtime_error("invalid pad script row");
            pads[frame].push_back({port, u16(buttons), {u8(x), u8(y), u8(rx), u8(ry)}});
        }
        if (!input.eof()) throw std::runtime_error("malformed pad script: " + pad_script);
    }

    GlobalOpts opts;
    opts.state = state;
    Machine m = open_machine(elf, opts);
    // PCSX2 savestates preserve the MpOracle pnach call-site jumps. Restore
    // every EE code patch to its ACTION.ELF word before replaying a checkpoint.
    const std::filesystem::path patch_path =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path()
        / "tools/oracle/pcsx2/SLUS-20579_5B86BB62.pnach";
    std::ifstream patch_file(patch_path);
    if (!patch_file)
        throw std::runtime_error("cannot open MP oracle pnach: " + patch_path.string());
    std::vector<u8> elf_image;
    FILE* elf_file = std::fopen(elf.c_str(), "rb");
    if (!elf_file) throw std::runtime_error("cannot reopen " + elf);
    char elf_buffer[65536];
    size_t elf_bytes = 0;
    while ((elf_bytes = std::fread(elf_buffer, 1, sizeof(elf_buffer), elf_file)) != 0) {
        const auto* bytes = reinterpret_cast<const u8*>(elf_buffer);
        elf_image.insert(elf_image.end(), bytes, bytes + elf_bytes);
    }
    if (std::ferror(elf_file)) {
        std::fclose(elf_file);
        throw std::runtime_error("failed reading " + elf);
    }
    std::fclose(elf_file);
    const nf::Elf32 original_elf(std::move(elf_image));
    size_t restored_words = 0;
    std::string patch_line;
    while (std::getline(patch_file, patch_line)) {
        constexpr std::string_view kGameCodePatch = "patch=2,EE,";
        if (!patch_line.starts_with(kGameCodePatch)) continue;
        const size_t address_end = patch_line.find(',', kGameCodePatch.size());
        if (address_end == std::string::npos
            || patch_line.compare(address_end + 1, 5, "word,") != 0)
            continue;
        const std::string patch_address = patch_line.substr(
            kGameCodePatch.size(), address_end - kGameCodePatch.size());
        const u32 address = parse_u32("0x" + patch_address);
        const nf::Bytes original = original_elf.at(address, sizeof(u32));
        m.mem.write_block(address, original.data(), u32(original.size()));
        ++restored_words;
    }
    if (!patch_file.eof())
        throw std::runtime_error("failed reading MP oracle pnach: " + patch_path.string());
    std::fprintf(stderr, "MP_ORACLE restored %zu pnach-patched words from ACTION.ELF\n",
                 restored_words);
    // Game_Run polls EE peripheral registers; model the inert register bank as
    // last-value storage while scratchpad DMA is handled by hook_sp_copy().
    m.mem.stub_hw_window(0x10000000u, 0x00100000u);
    hook_sp_copy(m);
    m.cpu.on_syscall = [](nf::ee::Cpu& cpu, u32 code) {
        const u32 service = cpu.r[3].w[0];
        // Game_Run's semaphore/event-flag, cache-flush, SIF-DMA, and DECI2
        // services have no asynchronous workers in this interpreter.
        if (code != 0 || !((service >= 64 && service <= 79) ||
                          service == 100 || (service >= 118 && service <= 119) ||
                          service == 124)) return false;
        cpu.r[2].w[0] = 0;
        return true;
    };
    // Game_Run's sceTtyWrite only emits DECI2 debug-console output and can
    // otherwise spin waiting for an IOP-side completion.
    if (m.symbol("sceTtyWrite")) hook_noop(m, "sceTtyWrite");
    // Preserve CPU-side scene/animation traversal while bypassing object
    // matrix submission leaves that send geometry to GS/VIF.
    for (const char* draw : {"psiDrawObjectMatrix__FP12celglist_tagP6MATRIXUi",
                             "psiDrawSkinObjectMatrix__FP12celglist_tagP6MATRIXP7obj_tagT1"})
        hook_noop(m, draw);
    if (!m.hook("PS2StartCalcPacket__Fv", [&m](nf::ee::Cpu&) {
            constexpr u32 kDmaBufferStride = 0x4104;
            const u32 buffer = m.mem.read<u32>(m.addr("CalcPacket"));
            if (buffer >= 2)
                throw std::runtime_error("PS2 CalcPacket index is out of range");
            // This buffer-completion flag is normally advanced by the PS2 DMA
            // engine. The interpreter has no DMA worker, so release the wait.
            m.mem.write<u32>(m.addr("DBufData") + buffer * kDmaBufferStride, 3);
            return false;
        }))
        throw std::runtime_error("ACTION.ELF has no PS2StartCalcPacket");
    for (const char* sound : {"Sound_Play__FUifsUi", "Sound_Play3D__FUiP7_VECTORfffsUii"})
        if (m.symbol(sound)) hook_noop(m, sound);
    // Preserve scripted tSlot data while running the game's real input mapper.
    if (m.symbol("psiInput_PollDevices__Fv"))
        hook_noop(m, "psiInput_PollDevices__Fv");

    constexpr u32 kGameState = 0x002A3768;
    constexpr u32 kDone = kGameState + 0x30;
    constexpr u32 kFrame = kGameState + 0x34;
    constexpr u32 kFrameStart = kGameState + 0x3C;
    constexpr u32 kFrameAccumulator = kGameState + 0x38;
    constexpr u32 kTslot0 = 0x00245680;
    constexpr u32 kTslotStride = 0x180;
    constexpr u32 kPadInput = 0x120;
    constexpr u32 kRamSize = nf::ee::Memory::kRamSize;
    for (const int weapon : give_weapons) {
        if (weapon < 0 || weapon >= 115)
            throw std::runtime_error("--give-weapon id must be in 0..114");
        const u32 object = m.mem.read<u32>(0x002A499Cu);
        if (!object) throw std::runtime_error("--give-weapon requires a human in MP slot 0");
        const u32 bl_data = m.mem.read<u32>(object + 0xE0);
        if (!bl_data) throw std::runtime_error("--give-weapon human has no BLData");
        if (weapon != 1) {
            nf::ee::CallArgs args;
            args.i(bl_data).i(u32(weapon)).i(999);
            const auto result = m.call_keep("Player_EquipWeapon__FP6BLDatass", args);
            if (result.v0 == 0)
                throw std::runtime_error("Player_EquipWeapon rejected weapon " + std::to_string(weapon));
        }
        // Fists are the source's empty-inventory fallback; Player_EquipWeapon rejects them.
        const u32 weapon_owner = m.mem.read<u32>(object + 0xDC);
        if (!weapon_owner) throw std::runtime_error("--give-weapon human has no weapon owner");
        const u8 current_before = m.mem.read<u8>(weapon_owner + 0x62);
        const u8 selected_before = m.mem.read<u8>(weapon_owner + 0x63);
        m.mem.write<u8>(weapon_owner + 0x63, u8(weapon));
        std::fprintf(stderr,
                     "SELECT_WEAPON slot=0 id=%d before_current=%u before_selected=%u\n",
                     weapon, unsigned(current_before), unsigned(selected_before));
        if (current_before != u8(weapon)) {
            nf::ee::CallArgs select_args;
            select_args.i(object);
            m.call_keep("Player_WeaponSelect__FP7obj_tag", select_args);
        }
        u8 current_after = m.mem.read<u8>(weapon_owner + 0x62);
        const u8 selected_after = m.mem.read<u8>(weapon_owner + 0x63);
        std::fprintf(stderr,
                     "PLAYER_WEAPON_SELECT slot=0 id=%d owner_current=%u owner_selected=%u\n",
                     weapon, unsigned(current_after), unsigned(selected_after));
        if (selected_after != u8(weapon))
            throw std::runtime_error("Player_WeaponSelect changed selected weapon unexpectedly");
        if (current_after != u8(weapon)) {
            const u32 entry = m.addr(game_flow ? "GameFlow_Main__Fv" : "Game_Run__Fv");
            constexpr u32 kTslot0 = 0x00245680;
            constexpr u32 kPadInput = 0x120;
            m.mem.write<u16>(kTslot0 + kPadInput + 2, 0);
            const u32 frame_before = m.mem.read<u32>(kFrameStart);
            const u32 timer_before = m.mem.read<u32>(kFrame);
            constexpr u32 kSelectionFrameLimit = 180;
            for (u32 tick = 0; current_after != u8(weapon) && tick < kSelectionFrameLimit; ++tick) {
                if (!game_flow) {
                    const u32 frame_rate_int = m.mem.read<u32>(m.addr("FRAME_RATE_INT"));
                    if (!frame_rate_int)
                        throw std::runtime_error("FRAME_RATE_INT is zero during weapon selection");
                    const u32 video_frame_rate = m.mem.read<u32>(m.addr("VIDEO_FRAME_RATE"));
                    m.mem.write<u32>(kFrameStart, m.mem.read<u32>(kFrameStart) + 1);
                    m.mem.write<u32>(kFrame, m.mem.read<u32>(kFrame) + 1);
                    m.mem.write<u32>(kFrameAccumulator,
                                     m.mem.read<u32>(kFrameAccumulator)
                                         + video_frame_rate / frame_rate_int);
                }
                m.call_keep(entry, {}, 200'000'000);
                current_after = m.mem.read<u8>(weapon_owner + 0x62);
            }
            const u32 frame_after = m.mem.read<u32>(kFrameStart);
            const u32 timer_after = m.mem.read<u32>(kFrame);
            if (current_after != u8(weapon))
                throw std::runtime_error("--give-weapon selection did not finish within 180 frames");
            if (frame_after == frame_before || timer_after == timer_before)
                throw std::runtime_error("--give-weapon selection transition did not advance game time");
        }
        std::fprintf(stderr,
                     "GAVE_WEAPON slot=0 id=%d bl=%08x owner_current=%u owner_selected=%u\n",
                     weapon, bl_data, unsigned(current_after),
                     unsigned(m.mem.read<u8>(weapon_owner + 0x63)));
    }
    if ((watch_drone_anim_slot == -1) != (watch_drone_frame == 0)
        || (watch_drone_anim_slot != -1
            && (watch_drone_anim_slot < 4 || watch_drone_anim_slot > 7))
        || (watch_drone_anim_slot != -1 && watch_human_hp_slot >= 0))
        throw std::runtime_error(
            "--watch-drone-anim requires --watch-drone-frame and excludes --watch-human-hp");
    if (watch_drone_anim_slot != -1) {
        constexpr u32 kMpGame = 0x002A4980;
        constexpr u32 kMpSlotStride = 0x30;
        const u32 object = m.mem.read<u32>(
            kMpGame + u32(watch_drone_anim_slot) * kMpSlotStride + 0x1C);
        if (!object) throw std::runtime_error("--watch-drone-anim slot has no MP object");
        const u32 drone = m.mem.read<u32>(object + 0xE0);
        const u32 owner = m.mem.read<u32>(object + 0xDC);
        if (!drone || !owner)
            throw std::runtime_error("--watch-drone-anim slot has no Drone or animation owner");
        const u32 anim = owner + 0x70;
        struct WatchRange { u32 address, size; const char* label; };
        std::vector<WatchRange> ranges;
        const auto add_range = [&](u32 address, u32 size, const char* label) {
            if (!size || m.mem.ram_offset(address) == ~u32(0)
                || m.mem.ram_offset(address + size - 1) == ~u32(0))
                throw std::runtime_error(std::string("--watch-drone-anim has invalid ") + label);
            ranges.push_back({address, size, label});
        };
        add_range(object + 0x30, 12, "object position");
        add_range(object + 0xFC, 1, "object animation flag");
        add_range(drone + 0x58C, 4, "Drone animation step");
        add_range(anim + 0x2C, 4, "animation layer head");
        add_range(anim + 0x5C, 4, "animation root height");
        add_range(anim + 0x64, 4, "animation distance step");
        add_range(anim + 0x6C, 4, "animation distance accumulator");
        add_range(anim + 0xCC, 4, "animation foot height");
        u32 layer = m.mem.read<u32>(anim + 0x2C);
        for (u32 count = 0; layer && count < 64; ++count) {
            if (m.mem.ram_offset(layer + 0xC0) == ~u32(0))
                throw std::runtime_error("--watch-drone-anim has invalid layer chain");
            add_range(layer + 0x48, 4, "animation layer next");
            add_range(layer + 0x50, 4, "animation sequence pointer");
            add_range(layer + 0x74, 4, "animation layer script id");
            add_range(layer + 0x80, 4, "animation layer id");
            add_range(layer + 0x90, 4, "animation layer frame");
            add_range(layer + 0x94, 4, "animation layer previous frame");
            add_range(layer + 0x98, 4, "animation layer speed");
            add_range(layer + 0x9C, 4, "animation layer blend progress");
            const u32 sequence = m.mem.read<u32>(layer + 0x50);
            if (sequence) {
                add_range(sequence, 16, "animation sequence root");
                add_range(sequence + 0x10, 16, "animation sequence root delta");
            }
            layer = m.mem.read<u32>(layer + 0x48);
        }
        if (layer) throw std::runtime_error("--watch-drone-anim layer chain exceeds 64 entries");
        std::fprintf(stderr,
                     "watching slot %d Drone animation writes at frame %u (obj=%08x drone=%08x anim=%08x)\n",
                     watch_drone_anim_slot, watch_drone_frame, object, drone, anim);
        m.mem.set_watch([&m, watch_drone_anim_slot, watch_drone_frame,
                         ranges = std::move(ranges), kFrameStart](
                            u32 address, u32 size, bool is_write) {
            if (!is_write || m.mem.read<u32>(kFrameStart) != watch_drone_frame) return;
            for (const WatchRange& range : ranges) {
                if (u64(address) >= u64(range.address) + range.size
                    || u64(address) + size <= range.address)
                    continue;
                std::fprintf(stderr, "ANIM_WRITE frame=%u slot=%d field=%s addr=%08x size=%u old=",
                             watch_drone_frame, watch_drone_anim_slot, range.label, address, size);
                for (u32 i = 0; i < size && i < 16; ++i)
                    std::fprintf(stderr, "%02x", m.mem.read<u8>(address + i));
                const u32 pc = m.cpu.cur_pc, ra = u32(m.cpu.r[31].d[0]);
                std::fprintf(stderr, " pc=%08x ra=%08x", pc, ra);
                if (auto symbol = m.symbol_at(pc))
                    std::fprintf(stderr, " <%s+0x%x>", symbol->name.c_str(), pc - symbol->value);
                if (ra >= 8) {
                    if (auto symbol = m.symbol_at(ra - 8))
                        std::fprintf(stderr, " caller=%s+0x%x", symbol->name.c_str(),
                                     ra - symbol->value - 8);
                }
                std::fputc('\n', stderr);
            }
        });
    }
    if (watch_human_hp_slot < -1 || watch_human_hp_slot >= 4)
        throw std::runtime_error("--watch-human-hp slot must be in 0..3");
    if (watch_human_hp_slot >= 0) {
        constexpr u32 kMpGame = 0x002A4980;
        constexpr u32 kMpSlotStride = 0x30;
        constexpr u32 kMpObject = 0x1C;
        constexpr u32 kObjectBlData = 0xE0;
        constexpr u32 kBlHealth = 0x894;
        const u32 object = m.mem.read<u32>(
            kMpGame + u32(watch_human_hp_slot) * kMpSlotStride + kMpObject);
        if (!object) throw std::runtime_error("HP watch slot has no MP object");
        const u32 bl_data = m.mem.read<u32>(object + kObjectBlData);
        const u32 hp_address = bl_data + kBlHealth;
        const u32 hp_offset = m.mem.ram_offset(hp_address);
        if (!bl_data || hp_offset == ~u32(0))
            throw std::runtime_error("HP watch slot has no valid BLData health address");
        std::fprintf(stderr, "watching slot %d human HP at %08x\n",
                     watch_human_hp_slot, hp_address);
        m.mem.set_watch([&m, watch_human_hp_slot, hp_address, hp_offset](
                            u32 address, u32 size, bool is_write) {
            if (!is_write) return;
            const u32 offset = m.mem.ram_offset(address);
            if (offset == ~u32(0) || offset >= hp_offset + 4
                || u64(offset) + size <= hp_offset)
                return;
            const u32 frame = m.mem.read<u32>(0x002A37A4);
            const u32 sp = u32(m.cpu.r[29].d[0]);
            const u32 stack_saved_ra_10 = m.mem.ram_offset(sp + 0x10) != ~u32(0)
                ? m.mem.read<u32>(sp + 0x10) : 0;
            const u32 stack_saved_ra_90 = m.mem.ram_offset(sp + 0x90) != ~u32(0)
                ? m.mem.read<u32>(sp + 0x90) : 0;
            const u32 stack_saved_ra_a0 = m.mem.ram_offset(sp + 0xa0) != ~u32(0)
                ? m.mem.read<u32>(sp + 0xa0) : 0;
            const u32 hitdata_pointer = m.mem.ram_offset(sp + 0xd0) != ~u32(0)
                ? m.mem.read<u32>(sp + 0xd0) : 0;
            const bool hitdata_valid = hitdata_pointer
                && u64(hitdata_pointer) + 0x5c <= 0xffffffffu
                && m.mem.ram_offset(hitdata_pointer + 0x5c) != ~u32(0);
            const u16 hit_type = hitdata_valid ? m.mem.read<u16>(hitdata_pointer + 0x50) : 0;
            const u16 hit_attacker = hitdata_valid ? m.mem.read<u16>(hitdata_pointer + 0x52) : 0;
            const u32 hit_source = hitdata_valid ? m.mem.read<u32>(hitdata_pointer + 0x58) : 0;
            const u32 hit_damage = hitdata_valid ? m.mem.read<u32>(hitdata_pointer + 0x08) : 0;
            const u32 pc = m.cpu.cur_pc;
            const u32 ra = u32(m.cpu.r[31].d[0]);
            const auto pc_symbol = m.symbol_at(pc);
            const auto ra_symbol = ra >= 8 ? m.symbol_at(ra - 8) : std::nullopt;
            const auto stack_symbol_10 = stack_saved_ra_10 >= 8
                ? m.symbol_at(stack_saved_ra_10 - 8) : std::nullopt;
            const auto stack_symbol_90 = stack_saved_ra_90 >= 8
                ? m.symbol_at(stack_saved_ra_90 - 8) : std::nullopt;
            const auto stack_symbol_a0 = stack_saved_ra_a0 >= 8
                ? m.symbol_at(stack_saved_ra_a0 - 8) : std::nullopt;
            std::fprintf(stderr, "HP write frame=%u slot=%d addr=%08x size=%u old=%08x "
                                 "pc=%08x inst=%08x sp=%08x sp+10=%08x sp+90=%08x sp+a0=%08x sp+d0=%08x",
                         frame, watch_human_hp_slot, address, size,
                         m.mem.read<u32>(hp_address), pc, m.cpu.cur_inst, sp,
                         stack_saved_ra_10, stack_saved_ra_90, stack_saved_ra_a0,
                         hitdata_pointer);
            if (hitdata_valid)
                std::fprintf(stderr, " hit[type=%04x attacker=%04x source=%08x damage=%08x]",
                             hit_type, hit_attacker, hit_source, hit_damage);
            else
                std::fprintf(stderr, " hitdata=invalid");
            if (pc_symbol)
                std::fprintf(stderr, " %s+0x%x", pc_symbol->name.c_str(),
                             pc - pc_symbol->value);
            std::fprintf(stderr, " ra=%08x", ra);
            if (ra_symbol)
                std::fprintf(stderr, " %s+0x%x", ra_symbol->name.c_str(),
                             ra - ra_symbol->value - 8);
            if (stack_symbol_10)
                std::fprintf(stderr, " caller=%s+0x%x", stack_symbol_10->name.c_str(),
                             stack_saved_ra_10 - stack_symbol_10->value - 8);
            if (stack_symbol_90 && stack_symbol_90->name != "_end")
                std::fprintf(stderr, " caller+90=%s+0x%x", stack_symbol_90->name.c_str(),
                             stack_saved_ra_90 - stack_symbol_90->value - 8);
            if (stack_symbol_a0)
                std::fprintf(stderr, " caller+a0=%s+0x%x", stack_symbol_a0->name.c_str(),
                             stack_saved_ra_a0 - stack_symbol_a0->value - 8);
            std::fputc('\n', stderr);
        });
    }
    if (watch_addr != 0) {
        const u32 watched = watch_addr;
        const u32 watched_off = m.mem.ram_offset(watched);
        if (watched_off == ~u32(0)) throw std::runtime_error("--watch-addr is outside EE RAM");
        std::fprintf(stderr, "watching writes at %08x\n", watched);
        m.mem.set_watch([&m, watched, watched_off](u32 address, u32 size, bool is_write) {
            if (!is_write) return;
            const u32 offset = m.mem.ram_offset(address);
            if (offset == ~u32(0) || offset >= watched_off + 4 || u64(offset) + size <= watched_off)
                return;
            const u32 frame = m.mem.read<u32>(0x002A37A4);
            const u32 pc = m.cpu.cur_pc, ra = u32(m.cpu.r[31].d[0]);
            std::fprintf(stderr, "ADDR write frame=%u addr=%08x size=%u pc=%08x", frame, address,
                         size, pc);
            if (auto sym = m.symbol_at(pc)) std::fprintf(stderr, " <%s+0x%x>", sym->name.c_str(), pc - sym->value);
            std::fprintf(stderr, " a0=%08x a1=%08x a2=%08x a3=%08x ra=%08x", u32(m.cpu.r[4].d[0]),
                         u32(m.cpu.r[5].d[0]), u32(m.cpu.r[6].d[0]), u32(m.cpu.r[7].d[0]), ra);
            if (ra >= 8) {
                if (auto sym = m.symbol_at(ra - 8))
                    std::fprintf(stderr, " caller=%s+0x%x", sym->name.c_str(), ra - sym->value - 8);
            }
            std::fputc('\n', stderr);
        });
    }
    auto write_u32 = [](u32 value) {
        const u8 bytes[] = {u8(value), u8(value >> 8), u8(value >> 16), u8(value >> 24)};
        if (std::fwrite(bytes, 1, sizeof(bytes), stdout) != sizeof(bytes))
            throw std::runtime_error("writing oracle stream failed");
    };
    std::vector<u8> compressed(compressBound(kRamSize));
    auto snapshot = [&]() {
        const u32 frame = m.mem.read<u32>(kFrameStart);
        const u32 done = m.mem.read<u32>(kDone);
        const u32 timer = m.mem.read<u32>(kFrame);
        uLongf compressed_size = compressed.size();
        const int status = compress2(compressed.data(), &compressed_size, m.mem.ram(),
                                     kRamSize, Z_BEST_SPEED);
        if (status != Z_OK) throw std::runtime_error("compressing EE RAM snapshot failed");
        write_u32(frame);
        write_u32(done);
        write_u32(timer);
        write_u32(u32(compressed_size));
        if (std::fwrite(compressed.data(), 1, compressed_size, stdout) != compressed_size)
            throw std::runtime_error("writing oracle snapshot failed");
    };

    const u8 magic[] = {'N', 'F', 'O', 'R'};
    if (std::fwrite(magic, 1, sizeof(magic), stdout) != sizeof(magic))
        throw std::runtime_error("writing oracle stream header failed");
    write_u32(1); // stream version
    write_u32(u32(rows));
    write_u32(kRamSize);
    snapshot();   // P2S state is the first emitted source frame.

    const u32 entry = m.addr(game_flow ? "GameFlow_Main__Fv" : "Game_Run__Fv");
    struct RngFunction {
        u32 address;
        const char* name;
        bool float_result;
    };
    struct PendingRandom {
        u32 caller_return;
        u32 caller;
        u32 stack;
        u32 frame;
        const RngFunction* function;
    };
    bool sample_hook_seen = false;
    std::vector<RngFunction> rng_functions;
    std::vector<PendingRandom> pending;
    if (trace_rng) {
        rng_functions = {
            {m.addr("Rand_Random__Fv"), "Rand_Random", false},
            {m.addr("Rand_Rand__FUi"), "Rand_Rand", false},
            {m.addr("Rand_FRand__Ff"), "Rand_FRand", true},
            {m.addr("Rand_FRandHalf__Ff"), "Rand_FRandHalf", true},
            {m.addr("Rand_FRand_MVar2__Fff"), "Rand_FRand_MVar2", true},
        };
        std::fprintf(stderr, "RNG_TRACE enabled functions=%zu\n", rng_functions.size());
        m.set_instruction_observer([&](const nf::ee::Cpu& cpu, u32 pc, u32) {
            if (sample_at_game_run_hook && pc == 0x001C98BCu && !sample_hook_seen) {
                snapshot();
                sample_hook_seen = true;
            }
            const u32 stack = u32(cpu.r[29].d[0]);
            for (auto it = pending.begin(); it != pending.end();) {
                if (pc == it->caller_return && stack == it->stack) {
                    const auto symbol = m.symbol_at(it->caller);
                    const u32 result = it->function->float_result ? cpu.f[0] : cpu.r[2].w[0];
                    std::fprintf(stderr, "RNG_CALL frame=%u fn=%s caller=%08x result=%08x",
                                 it->frame, it->function->name, it->caller, result);
                    if (symbol)
                        std::fprintf(stderr, " <%s+0x%x>", symbol->name.c_str(),
                                     it->caller - symbol->value);
                    std::fputc('\n', stderr);
                    it = pending.erase(it);
                } else {
                    ++it;
                }
            }
            for (const RngFunction& function : rng_functions) {
                if (pc != function.address) continue;
                const u32 caller_return = u32(cpu.r[31].d[0]);
                pending.push_back({caller_return, caller_return - 8, stack,
                                   m.mem.read<u32>(kFrameStart), &function});
                break;
            }
        });
    }
    auto sample_hook_observer = [&](const nf::ee::Cpu&, u32 pc, u32) {
        if (pc == 0x001C98BCu && !sample_hook_seen) {
            snapshot();
            sample_hook_seen = true;
        }
    };
    for (int row = 1; row < rows; ++row) {
        const u32 before = m.mem.read<u32>(kFrameStart);
        const u32 timer_before = m.mem.read<u32>(kFrame);
        const u32 next_frame = before + 1;
        const u32 next_timer_frame = timer_before + 1;
        if (!game_flow) {
            const u32 frame_rate_int = m.mem.read<u32>(m.addr("FRAME_RATE_INT"));
            if (!frame_rate_int)
                throw std::runtime_error("FRAME_RATE_INT is zero");
            const u32 video_frame_rate = m.mem.read<u32>(m.addr("VIDEO_FRAME_RATE"));
            m.mem.write<u32>(kFrameStart, next_frame);
            m.mem.write<u32>(kFrame, next_timer_frame);
            m.mem.write<u32>(kFrameAccumulator,
                             m.mem.read<u32>(kFrameAccumulator)
                                 + video_frame_rate / frame_rate_int);
        }
        const auto events = pads.find(before + 1);
        if (events != pads.end()) {
            for (const OraclePad& event : events->second) {
                const u32 address = kTslot0 + event.port * kTslotStride + kPadInput;
                m.mem.write<u16>(address + 2, event.buttons);
                m.mem.write_block(address + 8, event.sticks, sizeof(event.sticks));
            }
        }
        bool trace_done = false;
        if (next_frame == trace_frame) {
            constexpr u32 kMpGame = 0x002A4980;
            constexpr u32 kMpSlotStride = 0x30;
            constexpr u32 kMpObject = 0x1C;
            constexpr u32 kObjectPosition = 0x30;
            const u32 object = m.mem.read<u32>(
                kMpGame + u32(trace_slot) * kMpSlotStride + kMpObject);
            if (!object)
                throw std::runtime_error("trace slot has no MP object");
            const u32 position = m.mem.ram_offset(object + kObjectPosition);
            const u32 control = m.addr("Drone_Control__FP7obj_tag");
            u32 return_pc = 0;
            bool trace_active = false;
            m.mem.set_watch([&](u32 address, u32 size, bool write) {
                if (!write) return;
                const u32 offset = m.mem.ram_offset(address);
                const u32 matrix = m.mem.ram_offset(object + 0x90);
                const bool position_write = offset != ~u32(0) && offset < position + 12
                    && u64(offset) + size > position;
                const bool matrix_write = offset != ~u32(0) && offset < matrix + 0x30
                    && u64(offset) + size > matrix;
                if (!position_write && !matrix_write) return;
                const unsigned vt = (m.cpu.cur_inst >> 16) & 31;
                const auto& source = m.cpu.vu0.vf[vt];
                std::fprintf(stderr,
                    "OBJ_STORE %s frame=%u slot=%d pc=%08x inst=%08x addr=%08x size=%u "
                    "pre=%08x,%08x,%08x src-vf%u=%08x,%08x,%08x,%08x sp=%08x ra=%08x\n",
                    position_write ? "position" : "matrix",
                    next_frame, trace_slot, m.cpu.cur_pc, m.cpu.cur_inst, address, size,
                    m.mem.read<u32>(object + (position_write ? kObjectPosition : 0x90)),
                    m.mem.read<u32>(object + (position_write ? kObjectPosition + 4 : 0x94)),
                    m.mem.read<u32>(object + (position_write ? kObjectPosition + 8 : 0x98)),
                    vt, source.w[0], source.w[1], source.w[2], source.w[3],
                    u32(m.cpu.r[29].d[0]), u32(m.cpu.r[31].d[0]));
            });
            if (next_frame == trace_frame) {
                m.set_instruction_observer(
                [&](const nf::ee::Cpu& cpu, u32 pc, u32 inst) {
                    if (!trace_active) {
                        if (pc != control || cpu.r[4].w[0] != object) return;
                        trace_active = true;
                        return_pc = u32(cpu.r[31].d[0]);
                        std::fprintf(stderr,
                            "EE_TRACE_BEGIN frame=%u slot=%d object=%08x return=%08x\n",
                            next_frame, trace_slot, object, return_pc);
                    }
                    if (pc == return_pc) {
                        trace_active = false;
                        trace_done = true;
                        std::fprintf(stderr, "EE_TRACE_END frame=%u slot=%d\n",
                                     next_frame, trace_slot);
                        return;
                    }
                    const auto symbol = m.symbol_at(pc);
                    std::fprintf(stderr,
                        "EE_TRACE frame=%u slot=%d pc=%08x inst=%08x %-28s "
                        "a0=%08x a1=%08x a2=%08x a3=%08x ra=%08x sp=%08x "
                        "f0=%08x f1=%08x f2=%08x f3=%08x fcr31=%08x",
                        next_frame, trace_slot, pc, inst,
                        nf::ee::disassemble(inst, pc).c_str(),
                        cpu.r[4].w[0], cpu.r[5].w[0], cpu.r[6].w[0], cpu.r[7].w[0],
                        u32(cpu.r[31].d[0]), u32(cpu.r[29].d[0]),
                        cpu.f[0], cpu.f[1], cpu.f[2], cpu.f[3], cpu.fcr31);
                    if (symbol)
                        std::fprintf(stderr, " <%s+0x%x>", symbol->name.c_str(),
                                     pc - symbol->value);
                    if ((inst >> 26) == 0x12 && (inst & 0x02000000)) {
                        const unsigned fd = (inst >> 6) & 31;
                        const unsigned fs = (inst >> 11) & 31;
                        const unsigned ft = (inst >> 16) & 31;
                        const auto& vd = cpu.vu0.vf[fd];
                        const auto& vs = cpu.vu0.vf[fs];
                        const auto& vt = cpu.vu0.vf[ft];
                        std::fprintf(stderr,
                            " vu-fs%u=%08x,%08x,%08x,%08x"
                            " ft%u=%08x,%08x,%08x,%08x"
                            " fd%u=%08x,%08x,%08x,%08x mac=%08x clip=%08x",
                            fs, vs.w[0], vs.w[1], vs.w[2], vs.w[3],
                            ft, vt.w[0], vt.w[1], vt.w[2], vt.w[3],
                            fd, vd.w[0], vd.w[1], vd.w[2], vd.w[3],
                            cpu.vu0.vi[nf::ee::Vu0::kMac],
                            cpu.vu0.vi[nf::ee::Vu0::kClip]);
                    }
                    std::fputc('\n', stderr);
                });
            }
        }
        sample_hook_seen = false;
        if (sample_at_game_run_hook && !trace_rng)
            m.set_instruction_observer(sample_hook_observer);
        try {
            m.call_keep(entry, {}, 200'000'000);
        } catch (const std::exception& e) {
            m.set_instruction_observer({});
            m.mem.set_watch({});
            std::fprintf(stderr, "%s frame=%u trap at %08x: %s\n",
                         game_flow ? "GameFlow_Main" : "Game_Run", next_frame,
                         m.cpu.cur_pc, e.what());
            std::fputs(m.disasm_range(m.cpu.cur_pc - 16, 8).c_str(), stderr);
        }
        if (!trace_rng) m.set_instruction_observer({});
        if (trace_frame != 0 && next_frame == trace_frame) {
            m.mem.set_watch({});
            if (!trace_done)
                throw std::runtime_error("trace slot never entered Drone_Control");
        }
        const u32 after = m.mem.read<u32>(kFrameStart);
        const u32 timer_after = m.mem.read<u32>(kFrame);
        if (after != next_frame || timer_after != next_timer_frame)
            throw std::runtime_error("Game_Run frame counters did not advance exactly once");
        if (sample_at_game_run_hook && !sample_hook_seen)
            throw std::runtime_error("GameFlow_Main did not reach mp_record's Game_Run hook");
        if (!sample_at_game_run_hook)
            snapshot();
    }
    if (std::fflush(stdout) != 0) throw std::runtime_error("flushing oracle stream failed");
    return 0;
}


void usage() {
    std::fprintf(stderr,
                 "usage: nfmips <elf> call <sym|addr> [args] [--state p|--ram d] [--dump a[:n]] [--steps N]\n"
                 "       nfmips <elf> init [--check] [--dump-out f] [--steps N]\n"
                 "       nfmips <elf> trace <sym|addr> [args] [--steps N] [--state p|--ram d]\n"
                 "       nfmips <elf> mp-oracle --state <p2s> --rows N [--pads file] [--give-weapon ID ...]\n"
                 "         [--watch-human-hp 0..3 | --watch-drone-anim 4..7 --watch-drone-frame N]\n"
                 "         [--watch-addr EE_RAM_ADDRESS] [--trace-frame N --trace-slot 4..7] [--trace-rng]\n"
                 "         [--game-flow|--no-game-flow] [--sample-at-game-run-hook]\n"
                 "       nfmips <elf> sinf3-selftest (PS2Sinf3 angle-range loops and cardinal outputs)\n"
                 "       nfmips <elf> float-selftest (EE/VU float model assertions)\n"
                 "       nfmips <elf> symbols [substr]\n"
                 "       nfmips <elf> diff [--count N] [--seed N] [--state p2s]\n"
                 "       nfmips <elf> diff-acc (DroneWeap_DoBulletAccuracy truth table, Bots diff)\n"
                 "       nfmips <elf> diff-sin (PS2Sinf__Ff truth table, Bots poly replica)\n"
                 "       nfmips <elf> diff-input (psiInput_MapInputs, all controller styles vs nf::map_inputs)\n"
                 "       nfmips <elf> diff-refind (NDrone2_ReFindMissionPath truth table, Bots diff)\n"
                 "       nfmips <elf> diff-combat (DroneFunc_CombatState truth table, Bots diff)\n"
                 "       nfmips <elf> diff-mpweap (spread draws + spherical + HandlePain table, Combat diff)\n"
                 "       nfmips <elf> diff-bot (MP bot-brain truth tables: state/move/pain/goals, Bots diff)\n"
                 "       nfmips <elf> diff-botmp --state <match.p2s> (seeded MP-brain tables: weapons/find/goals, Bots diff)\n"
                 "       nfmips <elf> diff-botdrop --state <match.p2s> [--slot 4..7] (real seeded bot death-drop origins)\n"
                 "       nfmips <elf> rand <fn> <i:int|f:float> [--count N] [--seed x,y] [--state p2s]\n"
                 "call/trace writes: --poke addr:hexbytes | --vf32 addr:v0,v1,.. (scratch: 0x1E00000)\n"
                 "call/trace files: --sym-file <linker-.SYM> [--fs-root <extracted-disc-dir>]\n"
                 "stubs: --noop <sym> (repeatable, v0 = 0) | --stub-sound (Sound_Play/Play3D no-ops)\n"
                 "       --hook-copy (psiCopyToSP/FromSP as plain RAM<->scratchpad copies)\n");
}

int cmd_sinf3_selftest(const std::string& elf) {
    Machine m(elf);
    m.install_libc_hooks();
    constexpr u32 output = 0x01E00000u;
    constexpr float pi = 3.1415927f;
    const auto run = [&](float x, float y, float z) {
        m.mem.write<u32>(output, 0);
        m.mem.write<u32>(output + 4, 0);
        m.mem.write<u32>(output + 8, 0);
        CallArgs args;
        args.i(output).i(output + 4).i(output + 8).f(x).f(y).f(z);
        const auto result = m.call_keep("PS2Sinf3__FfffPfN23", args, 1'000'000);
        if (u32(result.v0) != 0x3F800000u)
            throw std::runtime_error("PS2Sinf3 returned an unexpected value");
        return std::array<float, 3>{
            std::bit_cast<float>(m.mem.read<u32>(output)),
            std::bit_cast<float>(m.mem.read<u32>(output + 4)),
            std::bit_cast<float>(m.mem.read<u32>(output + 8)),
        };
    };
    const auto cardinal = run(0.0f, pi * 0.5f, pi);
    if (cardinal[0] != 0.0f || std::fabs(cardinal[1] - 1.0f) > 1e-5f
        || std::fabs(cardinal[2]) > 1e-6f)
        throw std::runtime_error("PS2Sinf3 cardinal-angle check failed");
    const auto reduced = run(-10000.0f, 10000.0f, pi);
    for (float value : reduced)
        if (!std::isfinite(value) || std::fabs(value) > 1.01f)
            throw std::runtime_error("PS2Sinf3 range reduction produced an invalid result");
    if (std::fabs(reduced[0] + reduced[1]) > 1e-6f || std::fabs(reduced[2]) > 1e-6f)
        throw std::runtime_error("PS2Sinf3 positive/negative range reduction disagrees");
    std::printf("PS2Sinf3-selftest passed: cardinal angles and finite +/-10000-radian reductions\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> av(argv + 1, argv + argc);
    if (av.size() < 2) {
        usage();
        return 2;
    }
    const std::string elf = av[0], cmd = av[1];
    try {
        if (cmd == "float-selftest") {
            const bool fp_ok = nf::ee::fp::self_test();
            return vu0_macro_self_test() && fp_ok ? 0 : 1;
        }
        if (cmd == "sinf3-selftest") {
            if (av.size() != 2) throw std::runtime_error("sinf3-selftest takes no additional arguments");
            return cmd_sinf3_selftest(elf);
        }

        if (cmd == "mp-oracle") {
            std::string state, pads;
            int rows = 0, watch_human_hp_slot = -1, trace_slot = -1;
            int watch_drone_anim_slot = -1;
            u32 trace_frame = 0, watch_drone_frame = 0, watch_addr = 0;
            bool trace_rng = false, game_flow = true, sample_at_game_run_hook = false;
            std::vector<int> give_weapons;
            for (size_t j = 2; j < av.size(); j++) {
                if (av[j] == "--state" && j + 1 < av.size()) state = av[++j];
                else if (av[j] == "--rows" && j + 1 < av.size()) rows = std::stoi(av[++j]);
                else if (av[j] == "--pads" && j + 1 < av.size()) pads = av[++j];
                else if (av[j] == "--watch-human-hp" && j + 1 < av.size())
                    watch_human_hp_slot = std::stoi(av[++j]);
                else if (av[j] == "--trace-frame" && j + 1 < av.size())
                    trace_frame = u32(std::stoul(av[++j]));
                else if (av[j] == "--trace-slot" && j + 1 < av.size())
                    trace_slot = std::stoi(av[++j]);
                else if (av[j] == "--watch-drone-anim" && j + 1 < av.size())
                    watch_drone_anim_slot = std::stoi(av[++j]);
                else if (av[j] == "--watch-drone-frame" && j + 1 < av.size())
                    watch_drone_frame = u32(std::stoul(av[++j]));
                else if (av[j] == "--watch-addr" && j + 1 < av.size())
                    watch_addr = u32(std::stoul(av[++j], nullptr, 0));
                else if (av[j] == "--give-weapon" && j + 1 < av.size())
                    give_weapons.push_back(std::stoi(av[++j]));
                else if (av[j] == "--trace-rng") trace_rng = true;
                else if (av[j] == "--game-flow") game_flow = true;
                else if (av[j] == "--no-game-flow") game_flow = false;
                else if (av[j] == "--sample-at-game-run-hook")
                    sample_at_game_run_hook = true;
                else throw std::runtime_error(
                    "mp-oracle wants --state <p2s> --rows N [--pads file] "
                    "[--give-weapon ID ...] [--watch-human-hp 0..3] "
                    "[--watch-drone-anim 4..7 --watch-drone-frame N] "
                    "[--trace-frame N --trace-slot 4..7] [--trace-rng] "
                    "[--no-game-flow] [--sample-at-game-run-hook]");
            }
            return cmd_mp_oracle(elf, state, rows, pads, watch_human_hp_slot,
                                 trace_frame, trace_slot, trace_rng, give_weapons,
                                 watch_drone_anim_slot, watch_drone_frame, game_flow,
                                 sample_at_game_run_hook, watch_addr);
        }
        if (cmd == "call") {
            if (av.size() < 3) {
                usage();
                return 2;
            }
            size_t k = 3;
            CallArgs args;
            std::vector<std::string> rest;
            parse_call_args(av, k, args, rest);
            GlobalOpts g;
            std::vector<std::string> dumps, noops;
            bool stub_sound = false, hook_copy = false, log_mem = false;
            PokeSet pokes;
            u64 trace_first = 0;
            for (size_t j = 0; j < rest.size(); j++) {
                if (rest[j] == "--dump") dumps.push_back(rest.at(++j));
                else if (rest[j] == "--poke") pokes.hex(rest.at(++j));
                else if (rest[j] == "--vf32") pokes.floats(rest.at(++j));
                else if (rest[j] == "--noop") noops.push_back(rest.at(++j));
                else if (rest[j] == "--stub-sound") stub_sound = true;
                else if (rest[j] == "--hook-copy") hook_copy = true;
                else if (rest[j] == "--trace-first") trace_first = std::stoull(rest.at(++j));
                else if (rest[j] == "--state") g.state = rest.at(++j);
                else if (rest[j] == "--ram") g.ram = rest.at(++j);
                else if (rest[j] == "--sym-file") g.sym_file = rest.at(++j);
                else if (rest[j] == "--fs-root") g.fs_root = rest.at(++j);
                else if (rest[j] == "--steps") g.steps = std::stoull(rest.at(++j));
                else if (rest[j] == "--reg") parse_reg_preset(args, rest.at(++j));
                else if (rest[j] == "--log-mem") log_mem = true;
                else if (rest[j] == "--no-hooks") g.no_hooks = true;
                else throw std::runtime_error("unknown flag: " + rest[j]);
            }
            Machine m = open_machine(elf, g);
            pokes.apply(m);
            for (const auto& s : noops) hook_noop(m, s);
            if (stub_sound)
                for (const char* s : {"Sound_Play__FUifsUi", "Sound_Play3D__FUiP7_VECTORfffsUii"})
                    if (m.symbol(s)) hook_noop(m, s);
            if (hook_copy) hook_sp_copy(m);
            if (log_mem)
                m.mem.set_watch([](u32 va, u32 size, bool write) {
                    std::fprintf(stderr, "%c %08x %u\n", write ? 'W' : 'R', va, size);
                });
            return cmd_call(m, resolve(m, av[2]), args, g.steps, dumps, trace_first);
        }
        if (cmd == "init") {
            u64 steps = 0;
            std::string dump_out;
            bool check = false;
            for (size_t j = 2; j < av.size(); j++) {
                if (av[j] == "--steps") steps = std::stoull(av.at(++j));
                else if (av[j] == "--dump-out") dump_out = av.at(++j);
                else if (av[j] == "--check") check = true;
                else throw std::runtime_error("unknown flag: " + av[j]);
            }
            return cmd_init(elf, steps, dump_out, check);
        }
        if (cmd == "trace") {
            if (av.size() < 3) {
                usage();
                return 2;
            }
            size_t k = 3;
            CallArgs args;
            std::vector<std::string> rest;
            parse_call_args(av, k, args, rest);
            GlobalOpts g;
            std::vector<std::string> noops;
            bool stub_sound = false, hook_copy = false;
            PokeSet pokes;
            for (size_t j = 0; j < rest.size(); j++) {
                if (rest[j] == "--poke") pokes.hex(rest.at(++j));
                else if (rest[j] == "--vf32") pokes.floats(rest.at(++j));
                else if (rest[j] == "--noop") noops.push_back(rest.at(++j));
                else if (rest[j] == "--stub-sound") stub_sound = true;
                else if (rest[j] == "--hook-copy") hook_copy = true;
                else if (rest[j] == "--state") g.state = rest.at(++j);
                else if (rest[j] == "--ram") g.ram = rest.at(++j);
                else if (rest[j] == "--sym-file") g.sym_file = rest.at(++j);
                else if (rest[j] == "--fs-root") g.fs_root = rest.at(++j);
                else if (rest[j] == "--steps") g.steps = std::stoull(rest.at(++j));
                else throw std::runtime_error("unknown flag: " + rest[j]);
            }
            Machine m = open_machine(elf, g);
            pokes.apply(m);
            for (const auto& s : noops) hook_noop(m, s);
            if (stub_sound)
                for (const char* s : {"Sound_Play__FUifsUi", "Sound_Play3D__FUiP7_VECTORfffsUii"})
                    if (m.symbol(s)) hook_noop(m, s);
            if (hook_copy) hook_sp_copy(m);
            m.trace(resolve(m, av[2]), args, g.steps ? g.steps : 10000, stdout);
            return 0;
        }
        if (cmd == "disasm") {
            Machine m(elf);
            const u32 a = parse_u32(av[2]);
            const u32 n = av.size() > 3 ? parse_u32(av[3]) : 32;
            std::fputs(m.disasm_range(a, n).c_str(), stdout);
            return 0;
        }
        if (cmd == "symbols") {
            Machine m(elf);
            std::string sub;
            for (size_t j = 2; j < av.size(); j++) {
                if (av[j] == "--sym-file") m.load_sym_file(av.at(++j));
                else if (sub.empty()) sub = av[j];
                else throw std::runtime_error("unknown flag: " + av[j]);
            }
            for (const auto& s : m.symbols())
                if (sub.empty() || s.name.find(sub) != std::string::npos)
                    std::printf("%08x %6u %s\n", s.value, s.size, s.name.c_str());
            return 0;
        }
        if (cmd == "diff") {
            int count = 10000;
            unsigned seed = 12345;
            std::string state;
            for (size_t j = 2; j < av.size(); j++) {
                if (av[j] == "--count") count = std::stoi(av.at(++j));
                else if (av[j] == "--seed") seed = unsigned(std::stoul(av.at(++j)));
                else if (av[j] == "--state") state = av.at(++j);
                else throw std::runtime_error("unknown flag: " + av[j]);
            }
            return cmd_diff(elf, count, seed, state);
        }
        if (cmd == "diff-acc") return cmd_diff_acc(elf);
        if (cmd == "diff-sin") return cmd_diff_sin(elf);
        if (cmd == "diff-input") return cmd_diff_input(elf);
        if (cmd == "diff-refind") return cmd_diff_refind(elf);
        if (cmd == "diff-combat") return cmd_diff_combat(elf);
        if (cmd == "diff-mpweap") return cmd_diff_mpweap(elf);
        if (cmd == "diff-bot") return cmd_diff_bot(elf);
        if (cmd == "diff-botdrop") {
            std::string state;
            int slot = -1;
            for (size_t j = 2; j < av.size(); j++) {
                if (av[j] == "--state" && j + 1 < av.size()) state = av[++j];
                else if (av[j] == "--slot" && j + 1 < av.size()) slot = std::stoi(av[++j]);
                else throw std::runtime_error("diff-botdrop wants --state <match.p2s> [--slot 4..7]");
            }
            if (state.empty() || (slot != -1 && (slot < 4 || slot > 7)))
                throw std::runtime_error("diff-botdrop wants --state <match.p2s> [--slot 4..7]");
            return cmd_diff_botdrop(elf, state, slot);
        }
        if (cmd == "diff-botmp") {
            std::string state;
            for (size_t j = 2; j < av.size(); j++) {
                if (av[j] == "--state" && j + 1 < av.size()) state = av[++j];
                else throw std::runtime_error("diff-botmp wants --state <match.p2s>");
            }
            if (state.empty()) throw std::runtime_error("diff-botmp wants --state <match.p2s>");
            return cmd_diff_botmp(elf, state);
        }
        if (cmd == "rand") {
            // Sequential draws on ONE machine (state advances): the shared-RNG oracle.
            // nfmips <elf> rand <fn> <i:int|f:float> [--count N] [--seed x,y] [--state p2s]
            if (av.size() < 4) {
                usage();
                return 2;
            }
            size_t k = 3;
            CallArgs args;
            std::vector<std::string> rest;
            parse_call_args(av, k, args, rest);
            GlobalOpts g;
            int count = 1;
            bool seeded = false;
            u32 sx = 0, sy = 0;
            for (size_t j = 0; j < rest.size(); j++) {
                if (rest[j] == "--count") count = std::stoi(rest.at(++j));
                else if (rest[j] == "--seed") {
                    const std::string s = rest.at(++j);
                    const auto c = s.find(',');
                    if (c == std::string::npos) throw std::runtime_error("bad --seed (want x,y)");
                    sx = parse_u32(s.substr(0, c));
                    sy = parse_u32(s.substr(c + 1));
                    seeded = true;
                } else if (rest[j] == "--state") g.state = rest.at(++j);
                else throw std::runtime_error("unknown flag: " + rest[j]);
            }
            if (int(args.ints.size()) + int(args.floats.size()) != 1 || count < 1)
                throw std::runtime_error("rand takes exactly one draw arg plus --count N");
            Machine m = open_machine(elf, g);
            m.install_libc_hooks();
            const u32 entry = resolve(m, av[2]);
            if (seeded) {
                m.mem.write<u32>(0x30D0A0u, sx);
                m.mem.write<u32>(0x30D0A4u, sy);
            }
            for (int i = 0; i < count; i++) {
                const auto r = m.call_keep(entry, args, 10'000'000);
                float f;
                std::memcpy(&f, &r.f0, 4);
                std::printf("%d v0=%llu f0=%g f0h=0x%08x\n", i, (unsigned long long)r.v0, (double)f,
                            r.f0);
            }
            return 0;
        }
        usage();
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "nfmips: error: %s\n", e.what());
        return 1;
    }
}

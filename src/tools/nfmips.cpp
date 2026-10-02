// nfmips: headless R5900 harness for ACTION.ELF (see docs/ee.md).
//
//   nfmips <elf> call <sym|addr> [i:<int> ...] [f:<float> ...] [--state p2s|--ram dump]
//                                  [--dump addr[:len] ...] [--steps N] [--no-hooks] [--trace-first N]
//   nfmips <elf> init [--steps N] [--dump-out file] [--check]
//   nfmips <elf> trace <sym|addr> [args...] [--steps N] [--state p2s|--ram dump]
//   nfmips <elf> disasm <addr> [count]
//   nfmips <elf> symbols [substring]
//   nfmips <elf> diff [--count N] [--seed N] [--state p2s]
//
// Integer args (decimal/0xhex, or i:..) fill a0-a3 then the o32 stack area;
// f:.. args fill f12, f13, ... . Results print as v0/v1 (hex+dec) and f0.
#include <bit>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include "assets/elf.hpp"
#include "assets/weapon_data.hpp"
#include "ee/machine.hpp"

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
    std::string state, ram;
    u64 steps = 0;  // 0 = default
    bool no_hooks = false;
};

Machine open_machine(const std::string& elf, const GlobalOpts& g) {
    Machine m(elf);
    if (!g.state.empty()) m.load_p2s(g.state);
    if (!g.ram.empty()) m.load_ram_dump(g.ram);
    if (!g.no_hooks) m.install_libc_hooks();
    return m;
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
    const float bearings[][3] = {{0, 0, 1}, {0.7071f, 0, 0.7071f}};
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
                                                        m.mem.write<u32>(dc + 0xec, phase);
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
                                                        m.mem.write<u32>(game_state + 0x40, diff);
                                                        // +0x52 is 2-aligned; the original does an unaligned
                                                        // 4-byte load, so write it byte-wise here.
                                                        const u32 frame = 100;
                                                        m.mem.write_block(game_state + 0x52, &frame, 4);
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
                                                        std::printf("%d,%g,%d,%d,%d,%u,%u,0x%x,%u,%d,%u,"
                                                                    "%g,%g,%g,%g,%d,%g,%g,%g,0x%08x,0x%08x,0x%08x\n",
                                                                    acc, (double)dist, moving, fm, fs, sub, diff,
                                                                    level, seen, lost, phase, (double)b[0],
                                                                    (double)b[1], (double)b[2], (double)draw,
                                                                    hit, (double)ox, (double)oy, (double)oz,
                                                                    oxh, oyh, ozh);
                                                    }
    return 0;
}

void usage() {
    std::fprintf(stderr,
                 "usage: nfmips <elf> call <sym|addr> [args] [--state p|--ram d] [--dump a[:n]] [--steps N]\n"
                 "       nfmips <elf> init [--check] [--dump-out f] [--steps N]\n"
                 "       nfmips <elf> trace <sym|addr> [args] [--steps N] [--state p|--ram d]\n"
                 "       nfmips <elf> disasm <addr> [count]\n"
                 "       nfmips <elf> symbols [substr]\n"
                 "       nfmips <elf> diff [--count N] [--seed N] [--state p2s]\n"
                 "       nfmips <elf> diff-acc (DroneWeap_DoBulletAccuracy truth table, Bots diff)\n"
                 "args: i:<int> | f:<float> | f:0x<bits> | bare numbers (int, or float with ./e/)\n"
                 "call/trace writes: --poke addr:hexbytes | --vf32 addr:v0,v1,.. (scratch: 0x1E00000)\n"
                 "stubs: --noop <sym> (repeatable, v0 = 0) | --stub-sound (Sound_Play/Play3D no-ops)\n"
                 "       --hook-copy (psiCopyToSP/FromSP as plain RAM<->scratchpad copies)\n");
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
            bool stub_sound = false, hook_copy = false;
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
                else if (rest[j] == "--steps") g.steps = std::stoull(rest.at(++j));
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
            const std::string sub = av.size() > 2 ? av[2] : "";
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
        usage();
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "nfmips: error: %s\n", e.what());
        return 1;
    }
}

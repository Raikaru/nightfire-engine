// R5900 multimedia instructions (opcode 0x1C, "MMI"): 128-bit packed integer ops, MULT1/DIV1 pipeline 1,
// HI/LO moves, PLZCW, QFSRV. Semantics follow the EE Core Instruction Set Manual; the PMULTH/PHMADH family,
// PMFHL and PMTHL follow PCSX2's documented register layouts (see docs/ee.md).
#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstring>
#include <limits>

#include "ee/cpu.hpp"

namespace nf::ee {

namespace {

constexpr u64 sext32(u32 v) { return u64(s64(s32(v))); }

std::string hex(u32 v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%08x", v);
    return buf;
}

template <class T>
T get(const Reg128& r, int i) {
    T v;
    std::memcpy(&v, reinterpret_cast<const u8*>(&r) + i * sizeof(T), sizeof(T));
    return v;
}
template <class T>
void put(Reg128& r, int i, T v) {
    std::memcpy(reinterpret_cast<u8*>(&r) + i * sizeof(T), &v, sizeof(T));
}

template <class T, class F>
Reg128 map2(const Reg128& a, const Reg128& b, F fn) {
    Reg128 out;
    for (int i = 0; i < int(16 / sizeof(T)); ++i) put<T>(out, i, T(fn(get<T>(a, i), get<T>(b, i))));
    return out;
}
template <class T, class F>
Reg128 map1(const Reg128& a, F fn) {
    Reg128 out;
    for (int i = 0; i < int(16 / sizeof(T)); ++i) put<T>(out, i, T(fn(get<T>(a, i))));
    return out;
}

// PEXT{L,U}{W,H,B}: interleave the low (or high) half of the lanes: out = rt0, rs0, rt1, rs1, ...
template <class T>
Reg128 interleave(const Reg128& rs, const Reg128& rt, bool upper) {
    constexpr int n = int(16 / sizeof(T));
    const int base = upper ? n / 2 : 0;
    Reg128 out;
    for (int j = 0; j < n / 2; ++j) {
        put<T>(out, 2 * j, get<T>(rt, base + j));
        put<T>(out, 2 * j + 1, get<T>(rs, base + j));
    }
    return out;
}

// PPAC{W,H,B}: even lanes of rt then even lanes of rs.
template <class T>
Reg128 pack_even(const Reg128& rs, const Reg128& rt) {
    constexpr int n = int(16 / sizeof(T));
    Reg128 out;
    for (int j = 0; j < n / 2; ++j) {
        put<T>(out, j, get<T>(rt, 2 * j));
        put<T>(out, n / 2 + j, get<T>(rs, 2 * j));
    }
    return out;
}

template <class T, class Wide>
T sat_signed(Wide v) {
    constexpr Wide lo = std::numeric_limits<T>::min(), hi = std::numeric_limits<T>::max();
    return T(v < lo ? lo : (v > hi ? hi : v));
}

Reg128 to128(u128 v) {
    Reg128 r;
    r.d[0] = u64(v);
    r.d[1] = u64(v >> 64);
    return r;
}
u128 from128(const Reg128& r) { return u128(r.d[0]) | (u128(r.d[1]) << 64); }

int lead_sign_bits(s32 x) { return x < 0 ? std::countl_zero(~u32(x)) : std::countl_zero(u32(x)); }

}  // namespace

void Cpu::exec_mmi(u32 inst) {
    const u32 rs = (inst >> 21) & 31, rt = (inst >> 16) & 31, rd = (inst >> 11) & 31, shamt = (inst >> 6) & 31;
    const Reg128 &vs = r[rs], &vt = r[rt];
    auto W = [&](const Reg128& v) {
        if (rd) r[rd] = v;
    };
    auto W64 = [&](u64 v) {
        if (rd) r[rd].d[0] = v;
    };
    switch (inst & 63) {
        case 0x00: {  // MADD
            const s64 acc = s64((hi.d[0] << 32) | (lo.d[0] & 0xFFFFFFFFu)) + s64(vs.sw[0]) * s64(vt.sw[0]);
            lo.d[0] = sext32(u32(acc));
            hi.d[0] = sext32(u32(u64(acc) >> 32));
            W64(lo.d[0]);
            return;
        }
        case 0x01: {  // MADDU
            const u64 acc = ((hi.d[0] << 32) | (lo.d[0] & 0xFFFFFFFFu)) + u64(vs.w[0]) * u64(vt.w[0]);
            lo.d[0] = sext32(u32(acc));
            hi.d[0] = sext32(u32(acc >> 32));
            W64(lo.d[0]);
            return;
        }
        case 0x04:  // PLZCW
            if (rd) {
                r[rd].w[0] = u32(lead_sign_bits(vs.sw[0]) - 1);
                r[rd].w[1] = u32(lead_sign_bits(vs.sw[1]) - 1);
            }
            return;
        case 0x08: exec_mmi0(inst); return;
        case 0x09: exec_mmi2(inst); return;
        case 0x10: W64(hi.d[1]); return;                                   // MFHI1
        case 0x11: hi.d[1] = vs.d[0]; return;                              // MTHI1
        case 0x12: W64(lo.d[1]); return;                                   // MFLO1
        case 0x13: lo.d[1] = vs.d[0]; return;                              // MTLO1
        case 0x18: {                                                       // MULT1
            const s64 p = s64(vs.sw[0]) * s64(vt.sw[0]);
            lo.d[1] = sext32(u32(p));
            hi.d[1] = sext32(u32(u64(p) >> 32));
            W64(lo.d[1]);
            return;
        }
        case 0x19: {                                                       // MULTU1
            const u64 p = u64(vs.w[0]) * u64(vt.w[0]);
            lo.d[1] = sext32(u32(p));
            hi.d[1] = sext32(u32(p >> 32));
            W64(lo.d[1]);
            return;
        }
        case 0x1A: {                                                       // DIV1
            const s32 a = vs.sw[0], b = vt.sw[0];
            if (a == INT32_MIN && b == -1) {
                lo.d[1] = sext32(0x80000000u);
                hi.d[1] = 0;
            } else if (b != 0) {
                lo.d[1] = sext32(u32(a / b));
                hi.d[1] = sext32(u32(a % b));
            } else {
                lo.d[1] = a < 0 ? 1 : ~u64(0);
                hi.d[1] = sext32(u32(a));
            }
            return;
        }
        case 0x1B: {                                                       // DIVU1
            const u32 a = vs.w[0], b = vt.w[0];
            if (b != 0) {
                lo.d[1] = sext32(a / b);
                hi.d[1] = sext32(a % b);
            } else {
                lo.d[1] = ~u64(0);
                hi.d[1] = sext32(a);
            }
            return;
        }
        case 0x20: {  // MADD1
            const s64 acc = s64((hi.d[1] << 32) | (lo.d[1] & 0xFFFFFFFFu)) + s64(vs.sw[0]) * s64(vt.sw[0]);
            lo.d[1] = sext32(u32(acc));
            hi.d[1] = sext32(u32(u64(acc) >> 32));
            W64(lo.d[1]);
            return;
        }
        case 0x21: {  // MADDU1
            const u64 acc = ((hi.d[1] << 32) | (lo.d[1] & 0xFFFFFFFFu)) + u64(vs.w[0]) * u64(vt.w[0]);
            lo.d[1] = sext32(u32(acc));
            hi.d[1] = sext32(u32(acc >> 32));
            W64(lo.d[1]);
            return;
        }
        case 0x28: exec_mmi1(inst); return;
        case 0x29: exec_mmi3(inst); return;
        case 0x30: pmfhl(inst); return;                            // PMFHL
        case 0x31:                                                         // PMTHL
            if (shamt != 0) trap(TrapKind::Reserved, "reserved PMTHL format " + hex(inst));
            lo.w[0] = vs.w[0];
            hi.w[0] = vs.w[1];
            lo.w[2] = vs.w[2];
            hi.w[2] = vs.w[3];
            return;
        case 0x34: W(map1<u16>(vt, [&](u16 x) { return u16(x << (shamt & 15)); })); return;           // PSLLH
        case 0x36: W(map1<u16>(vt, [&](u16 x) { return u16(x >> (shamt & 15)); })); return;           // PSRLH
        case 0x37: W(map1<s16>(vt, [&](s16 x) { return s16(x >> (shamt & 15)); })); return;           // PSRAH
        case 0x3C: W(map1<u32>(vt, [&](u32 x) { return x << shamt; })); return;                        // PSLLW
        case 0x3E: W(map1<u32>(vt, [&](u32 x) { return x >> shamt; })); return;                        // PSRLW
        case 0x3F: W(map1<s32>(vt, [&](s32 x) { return x >> shamt; })); return;                        // PSRAW
        default: trap(TrapKind::Reserved, "reserved MMI instruction " + hex(inst));
    }
}

// PMFHL (sa selects the format).
void Cpu::pmfhl(u32 inst) {
    const u32 rd = (inst >> 11) & 31, fmt = (inst >> 6) & 31;
    if (fmt > 4) trap(TrapKind::Reserved, "reserved PMFHL format " + hex(inst));
    if (!rd) return;
    Reg128& d = r[rd];
    switch (fmt) {
        case 0: d.w[0] = lo.w[0]; d.w[1] = hi.w[0]; d.w[2] = lo.w[2]; d.w[3] = hi.w[2]; break;   // LW
        case 1: d.w[0] = lo.w[1]; d.w[1] = hi.w[1]; d.w[2] = lo.w[3]; d.w[3] = hi.w[3]; break;   // UW
        case 2:                                                                                   // SLW
            for (int i = 0; i < 2; ++i) {
                const s64 t = s64((u64(hi.w[2 * i]) << 32) | lo.w[2 * i]);
                d.d[i] = t >= 0x7FFFFFFFLL ? 0x7FFFFFFFull : (t <= -0x80000000LL ? 0xFFFFFFFF80000000ull : sext32(lo.w[2 * i]));
            }
            break;
        case 3:                                                                                   // LH
            d.h[0] = lo.h[0]; d.h[1] = lo.h[2]; d.h[2] = hi.h[0]; d.h[3] = hi.h[2];
            d.h[4] = lo.h[4]; d.h[5] = lo.h[6]; d.h[6] = hi.h[4]; d.h[7] = hi.h[6];
            break;
        default: {                                                                                // SH
            const u32 src[8] = {lo.w[0], lo.w[1], hi.w[0], hi.w[1], lo.w[2], lo.w[3], hi.w[2], hi.w[3]};
            for (int i = 0; i < 8; ++i) d.h[i] = u16(sat_signed<s16, s32>(s32(src[i])));
            break;
        }
    }
}

void Cpu::exec_mmi0(u32 inst) {
    const u32 rs = (inst >> 21) & 31, rt = (inst >> 16) & 31, rd = (inst >> 11) & 31, sub = (inst >> 6) & 31;
    const Reg128 vs = r[rs], vt = r[rt];
    Reg128 out;
    switch (sub) {
        case 0x00: out = map2<u32>(vs, vt, [](u32 a, u32 b) { return a + b; }); break;                                      // PADDW
        case 0x01: out = map2<u32>(vs, vt, [](u32 a, u32 b) { return a - b; }); break;                                      // PSUBW
        case 0x02: out = map2<s32>(vs, vt, [](s32 a, s32 b) { return a > b ? -1 : 0; }); break;                             // PCGTW
        case 0x03: out = map2<s32>(vs, vt, [](s32 a, s32 b) { return std::max(a, b); }); break;                             // PMAXW
        case 0x04: out = map2<u16>(vs, vt, [](u16 a, u16 b) { return a + b; }); break;                                      // PADDH
        case 0x05: out = map2<u16>(vs, vt, [](u16 a, u16 b) { return a - b; }); break;                                      // PSUBH
        case 0x06: out = map2<s16>(vs, vt, [](s16 a, s16 b) { return a > b ? -1 : 0; }); break;                             // PCGTH
        case 0x07: out = map2<s16>(vs, vt, [](s16 a, s16 b) { return std::max(a, b); }); break;                             // PMAXH
        case 0x08: out = map2<u8>(vs, vt, [](u8 a, u8 b) { return a + b; }); break;                                         // PADDB
        case 0x09: out = map2<u8>(vs, vt, [](u8 a, u8 b) { return a - b; }); break;                                         // PSUBB
        case 0x0A: out = map2<s8>(vs, vt, [](s8 a, s8 b) { return a > b ? -1 : 0; }); break;                                // PCGTB
        case 0x10: out = map2<s32>(vs, vt, [](s32 a, s32 b) { return sat_signed<s32, s64>(s64(a) + b); }); break;          // PADDSW
        case 0x11: out = map2<s32>(vs, vt, [](s32 a, s32 b) { return sat_signed<s32, s64>(s64(a) - b); }); break;          // PSUBSW
        case 0x12: out = interleave<u32>(vs, vt, false); break;                                                              // PEXTLW
        case 0x13: out = pack_even<u32>(vs, vt); break;                                                                      // PPACW
        case 0x14: out = map2<s16>(vs, vt, [](s16 a, s16 b) { return sat_signed<s16, s32>(s32(a) + b); }); break;          // PADDSH
        case 0x15: out = map2<s16>(vs, vt, [](s16 a, s16 b) { return sat_signed<s16, s32>(s32(a) - b); }); break;          // PSUBSH
        case 0x16: out = interleave<u16>(vs, vt, false); break;                                                              // PEXTLH
        case 0x17: out = pack_even<u16>(vs, vt); break;                                                                      // PPACH
        case 0x18: out = map2<s8>(vs, vt, [](s8 a, s8 b) { return sat_signed<s8, s32>(s32(a) + b); }); break;              // PADDSB
        case 0x19: out = map2<s8>(vs, vt, [](s8 a, s8 b) { return sat_signed<s8, s32>(s32(a) - b); }); break;              // PSUBSB
        case 0x1A: out = interleave<u8>(vs, vt, false); break;                                                               // PEXTLB
        case 0x1B: out = pack_even<u8>(vs, vt); break;                                                                       // PPACB
        case 0x1E:                                                                                                           // PEXT5
            out = map1<u32>(vt, [](u32 x) {
                return ((x & 0x1F) << 3) | (((x >> 5) & 0x1F) << 11) | (((x >> 10) & 0x1F) << 19) | (((x >> 15) & 1) << 31);
            });
            break;
        case 0x1F:                                                                                                           // PPAC5
            out = map1<u32>(vt, [](u32 x) {
                return ((x >> 3) & 0x1F) | (((x >> 11) & 0x1F) << 5) | (((x >> 19) & 0x1F) << 10) | (((x >> 31) & 1) << 15);
            });
            break;
        default: trap(TrapKind::Reserved, "reserved MMI0 instruction " + hex(inst));
    }
    if (rd) r[rd] = out;
}

void Cpu::exec_mmi1(u32 inst) {
    const u32 rs = (inst >> 21) & 31, rt = (inst >> 16) & 31, rd = (inst >> 11) & 31, sub = (inst >> 6) & 31;
    const Reg128 vs = r[rs], vt = r[rt];
    Reg128 out;
    switch (sub) {
        case 0x01: out = map1<s32>(vt, [](s32 x) { return x == INT32_MIN ? INT32_MAX : (x < 0 ? -x : x); }); break;         // PABSW
        case 0x02: out = map2<u32>(vs, vt, [](u32 a, u32 b) { return a == b ? ~0u : 0u; }); break;                          // PCEQW
        case 0x03: out = map2<s32>(vs, vt, [](s32 a, s32 b) { return std::min(a, b); }); break;                             // PMINW
        case 0x04:                                                                                                           // PADSBH
            for (int i = 0; i < 8; ++i) out.h[i] = i < 4 ? u16(vs.h[i] - vt.h[i]) : u16(vs.h[i] + vt.h[i]);
            break;
        case 0x05: out = map1<s16>(vt, [](s16 x) { return x == INT16_MIN ? INT16_MAX : (x < 0 ? -x : x); }); break;         // PABSH
        case 0x06: out = map2<u16>(vs, vt, [](u16 a, u16 b) { return a == b ? 0xFFFF : 0; }); break;                        // PCEQH
        case 0x07: out = map2<s16>(vs, vt, [](s16 a, s16 b) { return std::min(a, b); }); break;                             // PMINH
        case 0x0A: out = map2<u8>(vs, vt, [](u8 a, u8 b) { return a == b ? 0xFF : 0; }); break;                             // PCEQB
        case 0x10: out = map2<u32>(vs, vt, [](u32 a, u32 b) { return u32(std::min<u64>(u64(a) + b, 0xFFFFFFFFull)); }); break;   // PADDUW
        case 0x11: out = map2<u32>(vs, vt, [](u32 a, u32 b) { return a > b ? a - b : 0u; }); break;                         // PSUBUW
        case 0x12: out = interleave<u32>(vs, vt, true); break;                                                               // PEXTUW
        case 0x14: out = map2<u16>(vs, vt, [](u16 a, u16 b) { return u16(std::min<u32>(u32(a) + b, 0xFFFFu)); }); break;    // PADDUH
        case 0x15: out = map2<u16>(vs, vt, [](u16 a, u16 b) { return a > b ? u16(a - b) : u16(0); }); break;                // PSUBUH
        case 0x16: out = interleave<u16>(vs, vt, true); break;                                                               // PEXTUH
        case 0x18: out = map2<u8>(vs, vt, [](u8 a, u8 b) { return u8(std::min<u32>(u32(a) + b, 0xFFu)); }); break;          // PADDUB
        case 0x19: out = map2<u8>(vs, vt, [](u8 a, u8 b) { return a > b ? u8(a - b) : u8(0); }); break;                     // PSUBUB
        case 0x1A: out = interleave<u8>(vs, vt, true); break;                                                                // PEXTUB
        case 0x1B: {                                                                                                         // QFSRV
            const u32 n = (sa & 0xF) * 8;
            const u128 lo128 = from128(vt), hi128 = from128(vs);
            out = to128(n == 0 ? lo128 : ((lo128 >> n) | (hi128 << (128 - n))));
            break;
        }
        default: trap(TrapKind::Reserved, "reserved MMI1 instruction " + hex(inst));
    }
    if (rd) r[rd] = out;
}

void Cpu::exec_mmi2(u32 inst) {
    const u32 rs = (inst >> 21) & 31, rt = (inst >> 16) & 31, rd = (inst >> 11) & 31, sub = (inst >> 6) & 31;
    const Reg128 vs = r[rs], vt = r[rt];
    auto W = [&](const Reg128& v) {
        if (rd) r[rd] = v;
    };
    // Two-lane 32x32 multiplies: lane dd (0/1) uses words ss = 2*dd of the sources and HI/LO half dd.
    auto madd_signed = [&](int mode /*0 mult, 1 madd, 2 msub*/) {
        for (int dd = 0; dd < 2; ++dd) {
            const int ss = dd * 2;
            const s64 p = s64(vs.sw[ss]) * s64(vt.sw[ss]);
            s64 acc = mode == 0 ? p : s64((hi.d[dd] << 32) | (lo.d[dd] & 0xFFFFFFFFu));
            if (mode == 1) acc += p;
            else if (mode == 2) acc -= p;
            lo.d[dd] = sext32(u32(acc));
            hi.d[dd] = sext32(u32(u64(acc) >> 32));
            if (rd) r[rd].d[dd] = u64(acc);
        }
    };
    auto pdiv = [&](bool is_signed) {
        for (int dd = 0; dd < 2; ++dd) {
            const int ss = dd * 2;
            if (is_signed) {
                const s32 a = vs.sw[ss], b = vt.sw[ss];
                if (a == INT32_MIN && b == -1) {
                    lo.d[dd] = sext32(0x80000000u);
                    hi.d[dd] = 0;
                } else if (b != 0) {
                    lo.d[dd] = sext32(u32(a / b));
                    hi.d[dd] = sext32(u32(a % b));
                } else {
                    lo.d[dd] = a < 0 ? 1 : ~u64(0);
                    hi.d[dd] = sext32(u32(a));
                }
            } else {
                const u32 a = vs.w[ss], b = vt.w[ss];
                if (b != 0) {
                    lo.d[dd] = sext32(a / b);
                    hi.d[dd] = sext32(a % b);
                } else {
                    lo.d[dd] = ~u64(0);
                    hi.d[dd] = sext32(a);
                }
            }
        }
    };
    // Halfword multiplies (PMULTH/PMADDH/PMSUBH): eight 16x16 products into LO/HI words, rd = words {LO0, HI0, LO2, HI2}.
    auto halfword = [&](int mode /*0 mult, 1 madd, 2 msub*/) {
        u32* slots[8] = {&lo.w[0], &lo.w[1], &hi.w[0], &hi.w[1], &lo.w[2], &lo.w[3], &hi.w[2], &hi.w[3]};
        for (int i = 0; i < 8; ++i) {
            const s32 p = s32(vs.sh[i]) * s32(vt.sh[i]);
            *slots[i] = mode == 0 ? u32(p) : (mode == 1 ? *slots[i] + u32(p) : *slots[i] - u32(p));
        }
        if (rd) {
            r[rd].w[0] = lo.w[0]; r[rd].w[1] = hi.w[0]; r[rd].w[2] = lo.w[2]; r[rd].w[3] = hi.w[2];
        }
    };
    // PHMADH/PHMSBH: horizontal pairs. LO/HI word dd gets p(n+1) +/- p(n); word dd+1 keeps p(n+1) (PHMSBH: ~p(n+1)).
    auto horizontal = [&](bool subtract) {
        struct Slot { u32* w; int n; };
        const Slot slots[4] = {{&lo.w[0], 0}, {&hi.w[0], 2}, {&lo.w[2], 4}, {&hi.w[2], 6}};
        for (const Slot& s : slots) {
            const s32 first = s32(vs.sh[s.n + 1]) * s32(vt.sh[s.n + 1]);
            const s32 second = s32(vs.sh[s.n]) * s32(vt.sh[s.n]);
            s.w[0] = u32(subtract ? first - second : first + second);
            s.w[1] = subtract ? ~u32(first) : u32(first);
        }
        if (rd) {
            r[rd].w[0] = lo.w[0]; r[rd].w[1] = hi.w[0]; r[rd].w[2] = lo.w[2]; r[rd].w[3] = hi.w[2];
        }
    };
    switch (sub) {
        case 0x00: madd_signed(1); return;                                                     // PMADDW
        case 0x02:                                                                             // PSLLVW
            if (rd) {
                r[rd].d[0] = sext32(vt.w[0] << (vs.w[0] & 31));
                r[rd].d[1] = sext32(vt.w[2] << (vs.w[2] & 31));
            }
            return;
        case 0x03:                                                                             // PSRLVW
            if (rd) {
                r[rd].d[0] = sext32(vt.w[0] >> (vs.w[0] & 31));
                r[rd].d[1] = sext32(vt.w[2] >> (vs.w[2] & 31));
            }
            return;
        case 0x04: madd_signed(2); return;                                                     // PMSUBW
        case 0x08: W(hi); return;                                                              // PMFHI
        case 0x09: W(lo); return;                                                              // PMFLO
        case 0x0A: {                                                                           // PINTH
            Reg128 out;
            for (int i = 0; i < 4; ++i) {
                out.h[2 * i] = vt.h[i];
                out.h[2 * i + 1] = vs.h[4 + i];
            }
            W(out);
            return;
        }
        case 0x0C: madd_signed(0); return;                                                     // PMULTW
        case 0x0D: pdiv(true); return;                                                         // PDIVW
        case 0x0E: {                                                                           // PCPYLD
            Reg128 out;
            out.d[0] = vt.d[0];
            out.d[1] = vs.d[0];
            W(out);
            return;
        }
        case 0x10: halfword(1); return;                                                        // PMADDH
        case 0x11: horizontal(false); return;                                                  // PHMADH
        case 0x12: { Reg128 o; o.d[0] = vs.d[0] & vt.d[0]; o.d[1] = vs.d[1] & vt.d[1]; W(o); return; }   // PAND
        case 0x13: { Reg128 o; o.d[0] = vs.d[0] ^ vt.d[0]; o.d[1] = vs.d[1] ^ vt.d[1]; W(o); return; }   // PXOR
        case 0x14: halfword(2); return;                                                        // PMSUBH
        case 0x15: horizontal(true); return;                                                   // PHMSBH
        case 0x1A: {                                                                           // PEXEH
            Reg128 o = vt;
            o.h[0] = vt.h[2]; o.h[2] = vt.h[0]; o.h[4] = vt.h[6]; o.h[6] = vt.h[4];
            W(o);
            return;
        }
        case 0x1B: {                                                                           // PREVH
            Reg128 o;
            for (int i = 0; i < 4; ++i) {
                o.h[i] = vt.h[3 - i];
                o.h[4 + i] = vt.h[7 - i];
            }
            W(o);
            return;
        }
        case 0x1C: halfword(0); return;                                                        // PMULTH
        case 0x1D:                                                                             // PDIVBW
            for (int n = 0; n < 4; ++n) {
                const s32 a = vs.sw[n];
                const s16 b = vt.sh[0];
                if (vs.w[n] == 0x80000000u && vt.h[0] == 0xFFFF) {
                    lo.sw[n] = INT32_MIN;
                    hi.sw[n] = 0;
                } else if (b != 0) {
                    lo.sw[n] = a / b;
                    hi.sw[n] = a % b;
                } else {
                    lo.sw[n] = a < 0 ? 1 : -1;
                    hi.sw[n] = a;
                }
            }
            return;
        case 0x1E: {                                                                           // PEXEW
            Reg128 o = vt;
            o.w[0] = vt.w[2]; o.w[2] = vt.w[0];
            W(o);
            return;
        }
        case 0x1F: {                                                                           // PROT3W
            Reg128 o = vt;
            o.w[0] = vt.w[1]; o.w[1] = vt.w[2]; o.w[2] = vt.w[0];
            W(o);
            return;
        }
        default: trap(TrapKind::Reserved, "reserved MMI2 instruction " + hex(inst));
    }
}

void Cpu::exec_mmi3(u32 inst) {
    const u32 rs = (inst >> 21) & 31, rt = (inst >> 16) & 31, rd = (inst >> 11) & 31, sub = (inst >> 6) & 31;
    const Reg128 vs = r[rs], vt = r[rt];
    auto W = [&](const Reg128& v) {
        if (rd) r[rd] = v;
    };
    auto umul = [&](bool accumulate) {
        for (int dd = 0; dd < 2; ++dd) {
            const int ss = dd * 2;
            u64 acc = u64(vs.w[ss]) * u64(vt.w[ss]);
            if (accumulate) acc += (hi.d[dd] << 32) | (lo.d[dd] & 0xFFFFFFFFu);
            lo.d[dd] = sext32(u32(acc));
            hi.d[dd] = sext32(u32(acc >> 32));
            if (rd) r[rd].d[dd] = acc;
        }
    };
    switch (sub) {
        case 0x00: umul(true); return;                                                         // PMADDUW
        case 0x03:                                                                             // PSRAVW
            if (rd) {
                r[rd].d[0] = sext32(u32(vt.sw[0] >> (vs.w[0] & 31)));
                r[rd].d[1] = sext32(u32(vt.sw[2] >> (vs.w[2] & 31)));
            }
            return;
        case 0x08: hi = vs; return;                                                            // PMTHI
        case 0x09: lo = vs; return;                                                            // PMTLO
        case 0x0A: {                                                                           // PINTEH
            Reg128 o;
            for (int i = 0; i < 4; ++i) {
                o.h[2 * i] = vt.h[2 * i];
                o.h[2 * i + 1] = vs.h[2 * i];
            }
            W(o);
            return;
        }
        case 0x0C: umul(false); return;                                                        // PMULTUW
        case 0x0D:                                                                             // PDIVUW
            for (int dd = 0; dd < 2; ++dd) {
                const u32 a = vs.w[dd * 2], b = vt.w[dd * 2];
                if (b != 0) {
                    lo.d[dd] = sext32(a / b);
                    hi.d[dd] = sext32(a % b);
                } else {
                    lo.d[dd] = ~u64(0);
                    hi.d[dd] = sext32(a);
                }
            }
            return;
        case 0x0E: {                                                                           // PCPYUD
            Reg128 o;
            o.d[0] = vs.d[1];
            o.d[1] = vt.d[1];
            W(o);
            return;
        }
        case 0x12: { Reg128 o; o.d[0] = vs.d[0] | vt.d[0]; o.d[1] = vs.d[1] | vt.d[1]; W(o); return; }    // POR
        case 0x13: { Reg128 o; o.d[0] = ~(vs.d[0] | vt.d[0]); o.d[1] = ~(vs.d[1] | vt.d[1]); W(o); return; }  // PNOR
        case 0x1A: {                                                                           // PEXCH
            Reg128 o = vt;
            o.h[1] = vt.h[2]; o.h[2] = vt.h[1]; o.h[5] = vt.h[6]; o.h[6] = vt.h[5];
            W(o);
            return;
        }
        case 0x1B: {                                                                           // PCPYH
            Reg128 o;
            for (int i = 0; i < 4; ++i) {
                o.h[i] = vt.h[0];
                o.h[4 + i] = vt.h[4];
            }
            W(o);
            return;
        }
        case 0x1E: {                                                                           // PEXCW
            Reg128 o = vt;
            o.w[1] = vt.w[2]; o.w[2] = vt.w[1];
            W(o);
            return;
        }
        default: trap(TrapKind::Reserved, "reserved MMI3 instruction " + hex(inst));
    }
}

}  // namespace nf::ee

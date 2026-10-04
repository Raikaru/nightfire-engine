// COP1 (FPU) with PS2 semantics: see ps2float.hpp and docs/ee.md.
#include <cstdio>

#include "ee/cpu.hpp"
#include "ee/ps2float.hpp"

namespace nf::ee {

namespace {
constexpr u32 kC = 0x00800000u, kI = 0x00020000u, kD = 0x00010000u, kO = 0x00008000u, kU = 0x00004000u;
constexpr u32 kSI = 0x40u, kSD = 0x20u, kSO = 0x10u, kSU = 0x08u;
constexpr u64 sext32(u32 v) { return u64(s64(s32(v))); }

std::string hex(u32 v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%08x", v);
    return buf;
}
}  // namespace

void Cpu::fpu_result_flags(u32 fl, bool track) {
    if (!track) return;
    if (fl & fp::kOvf) {
        fcr31 |= kO | kSO;
        return;
    }
    fcr31 &= ~kO;
    if (fl & fp::kUnf) fcr31 |= kU | kSU;
    else fcr31 &= ~kU;
}

void Cpu::exec_cop1(u32 inst) {
    const u32 fmt = (inst >> 21) & 31, rt = (inst >> 16) & 31, fs = (inst >> 11) & 31, fd = (inst >> 6) & 31;
    switch (fmt) {
        case 0: if (rt) r[rt].d[0] = sext32(f[fs]); return;             // MFC1
        case 2:                                                          // CFC1
            if (!rt) return;
            if (fs == 31) r[rt].d[0] = sext32(fcr31);
            else if (fs == 0) r[rt].d[0] = 0x2E00;
            else r[rt].d[0] = 0;
            return;
        case 4: f[fs] = r[rt].w[0]; return;                              // MTC1
        case 6: if (fs == 31) fcr31 = r[rt].w[0]; return;                // CTC1
        case 8: {                                                        // BC1F/BC1T/BC1FL/BC1TL
            const u32 t = rt & 3;
            const bool c = (fcr31 & kC) != 0;
            branch((t & 1) ? c : !c, cur_pc + 4 + u32(s32(s16(inst)) << 2), (t & 2) != 0);
            return;
        }
        case 16: exec_cop1_s(inst); return;
        case 20:                                                         // W: only CVT.S exists
            if ((inst & 63) == 32) {
                f[fd] = fp::from_i32(s32(f[fs]));
                return;
            }
            trap(TrapKind::Reserved, "reserved COP1.W instruction " + hex(inst));
        default: trap(TrapKind::Reserved, "reserved COP1 instruction " + hex(inst));
    }
}

void Cpu::exec_cop1_s(u32 inst) {
    const u32 ft = (inst >> 16) & 31, fs = (inst >> 11) & 31, fd = (inst >> 6) & 31;
    const u32 a = f[fs], b = f[ft];
    u32 fl = 0;
    switch (inst & 63) {
        case 0x00: f[fd] = fp::add(a, b, fl); fpu_result_flags(fl, true); return;              // ADD.S
        case 0x01: f[fd] = fp::sub(a, b, fl); fpu_result_flags(fl, true); return;              // SUB.S
        case 0x02: f[fd] = fp::mul(a, b, fl); fpu_result_flags(fl, true); return;              // MUL.S
        case 0x03: {                                                                             // DIV.S
            fcr31 &= ~(kI | kD);
            if ((b & 0x7F800000u) == 0) {
                fcr31 |= ((a & 0x7F800000u) == 0) ? (kI | kSI) : (kD | kSD);
                f[fd] = ((a ^ b) & 0x80000000u) | fp::kFmax;
                return;
            }
            f[fd] = fp::div_nearest(a, b, fl);
            return;
        }
        case 0x04:                                                                              // SQRT.S (operand is ft)
            fcr31 &= ~(kI | kD);
            if (b & 0x80000000u) fcr31 |= kI | kSI;
            f[fd] = fp::sqrt_nearest_abs(b);
            return;
        case 0x05: f[fd] = a & 0x7FFFFFFFu; fcr31 &= ~(kO | kU); return;                       // ABS.S
        case 0x06: f[fd] = a; return;                                                          // MOV.S
        case 0x07: f[fd] = a ^ 0x80000000u; fcr31 &= ~(kO | kU); return;                       // NEG.S
        case 0x16: {                                                                            // RSQRT.S: fs / sqrt(ft)
            fcr31 &= ~(kD | kI);
            if (b & 0x80000000u) fcr31 |= kI | kSI;
            if ((b & 0x7F800000u) == 0) {
                fcr31 |= ((a & 0x7F800000u) == 0) ? (kI | kSI) : (kD | kSD);
                f[fd] = (a & 0x80000000u) | fp::kFmax;
                return;
            }
            f[fd] = fp::div(a, fp::sqrt_abs(b), fl);
            return;
        }
        case 0x18: fpu_acc = fp::add(a, b, fl); fpu_result_flags(fl, true); return;            // ADDA.S
        case 0x19: fpu_acc = fp::sub(a, b, fl); fpu_result_flags(fl, true); return;            // SUBA.S
        case 0x1A: fpu_acc = fp::mul(a, b, fl); fpu_result_flags(fl, true); return;            // MULA.S
        case 0x1C: {                                                                            // MADD.S
            u32 ig = 0;
            f[fd] = fp::add(fpu_acc, fp::mul(a, b, ig), fl);
            fpu_result_flags(fl, true);
            return;
        }
        case 0x1D: {                                                                            // MSUB.S
            u32 ig = 0;
            f[fd] = fp::sub(fpu_acc, fp::mul(a, b, ig), fl);
            fpu_result_flags(fl, true);
            return;
        }
        case 0x1E: {                                                                            // MADDA.S
            u32 ig = 0;
            fpu_acc = fp::add(fpu_acc, fp::mul(a, b, ig), fl);
            fpu_result_flags(fl, true);
            return;
        }
        case 0x1F: {                                                                            // MSUBA.S
            u32 ig = 0;
            fpu_acc = fp::sub(fpu_acc, fp::mul(a, b, ig), fl);
            fpu_result_flags(fl, true);
            return;
        }
        case 0x24: f[fd] = u32(fp::to_i32(a)); return;                                         // CVT.W.S
        case 0x28: f[fd] = fp::max_bits(a, b); fcr31 &= ~(kO | kU); return;                    // MAX.S
        case 0x29: f[fd] = fp::min_bits(a, b); fcr31 &= ~(kO | kU); return;                    // MIN.S
        case 0x30: fcr31 &= ~kC; return;                                                       // C.F.S
        case 0x32: fcr31 = fp::eq(a, b) ? (fcr31 | kC) : (fcr31 & ~kC); return;                // C.EQ.S
        case 0x34: fcr31 = fp::lt(a, b) ? (fcr31 | kC) : (fcr31 & ~kC); return;                // C.LT.S
        case 0x36: fcr31 = fp::le(a, b) ? (fcr31 | kC) : (fcr31 & ~kC); return;                // C.LE.S
        default: trap(TrapKind::Reserved, "reserved COP1.S instruction " + hex(inst));
    }
}

}  // namespace nf::ee

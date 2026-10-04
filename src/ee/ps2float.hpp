#pragma once

// PS2 single-precision arithmetic (EE FPU, VU0), implemented in integer arithmetic so the result never depends on
// the host FPU or its rounding mode. EE COP1 ADD/SUB use PCSX2's one-guard-bit operand reduction before the Chop
// operation; VU macro ADD/SUB use the direct Chop operation.
//  * Values are IEEE-754 binary32 bit patterns without Inf/NaN: exponent 0 is zero (denormal inputs flush to a
//    signed zero) and exponent 255 is treated as +-Fmax (0x7F7FFFFF) on input.
//  * Most operations Chop toward zero. PCSX2's default EE DIV.S uses its separate nearest FPUDivFPCR; SQRT.S also
//    temporarily uses nearest. The `div`/`sqrt_abs` helpers remain Chop for VU operations and EE RSQRT.S.
//  * Fused forms (MADD, MSUB, ...) are a truncated multiply followed by the corresponding EE-FPU or VU add/sub.
//  * Overflow returns +-Fmax and reports kOvf; results below the normal range flush to a signed zero and report
//    kUnf.

#include "ee/types.hpp"

namespace nf::ee::fp {

constexpr u32 kFmax = 0x7F7FFFFFu;
constexpr u32 kSignBit = 0x80000000u;
enum : u32 { kOvf = 1, kUnf = 2 };

// Input mapping ("fpuDouble"/"vuDouble"): denormal -> signed zero, exponent 255 -> +-Fmax.
constexpr u32 in(u32 f) {
    const u32 e = f & 0x7F800000u;
    if (e == 0) return f & kSignBit;
    if (e == 0x7F800000u) return (f & kSignBit) | kFmax;
    return f;
}

u32 add(u32 a, u32 b, u32& flags); // EE COP1 ADD.S (one-guard-bit operand reduction)
u32 sub(u32 a, u32 b, u32& flags); // EE COP1 SUB.S
u32 vu_add(u32 a, u32 b, u32& flags);
u32 vu_sub(u32 a, u32 b, u32& flags);
u32 mul(u32 a, u32 b, u32& flags);
// a / b for a normal, non-zero b (callers handle division by zero themselves).
u32 div(u32 a, u32 b, u32& flags);
// I-FPU DIV.S uses PCSX2's separate nearest-rounding FPUDivFPCR by default.
u32 div_nearest(u32 a, u32 b, u32& flags);
// sqrt of a non-negative value with VU/RSQRT Chop semantics or I-FPU SQRT.S round-to-nearest.
u32 sqrt_abs(u32 x);
u32 sqrt_nearest_abs(u32 x);

u32 from_i32(s32 v);              // CVT.S / ITOF0, truncating
s32 to_i32(u32 f);                // trunc toward zero with saturation at +-2^31 (CVT.W / FTOI0)

// Ordered comparisons on mapped operands (no NaNs exist).
bool eq(u32 a, u32 b);
bool lt(u32 a, u32 b);
bool le(u32 a, u32 b);

// Sign-magnitude max/min on the raw bit patterns (FPU MAX.S/MIN.S, VU MAX/MINI).
u32 max_bits(u32 a, u32 b);
u32 min_bits(u32 a, u32 b);

}  // namespace nf::ee::fp

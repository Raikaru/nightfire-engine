#pragma once

// PS2 single-precision arithmetic (EE FPU, VU0), implemented in integer arithmetic so the result never depends on
// the host FPU or its rounding mode. Model (docs/ee.md, "Arithmetic model"):
//  * Values are IEEE-754 binary32 bit patterns without Inf/NaN: exponent 0 is zero (denormal inputs flush to a
//    signed zero) and exponent 255 is treated as +-Fmax (0x7F7FFFFF) on input.
//  * Every operation rounds toward zero (truncates) exactly once. Fused forms (MADD, MSUB, ...) are a truncated
//    multiply followed by a truncated add, like the PCSX2 interpreters and the microVU/EE recompilers.
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

u32 add(u32 a, u32 b, u32& flags);
u32 sub(u32 a, u32 b, u32& flags);
u32 mul(u32 a, u32 b, u32& flags);
// a / b for a normal, non-zero b (callers handle division by zero themselves).
u32 div(u32 a, u32 b, u32& flags);
// sqrt of a non-negative value (sign is ignored, zero returns +-0).
u32 sqrt_abs(u32 x);

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

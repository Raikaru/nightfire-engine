#include "ee/ps2float.hpp"

#include <algorithm>
#include <bit>
#include <climits>

namespace nf::ee::fp {

namespace {

constexpr u32 pack_zero(u32 sign) { return sign; }

// Packs sign/exponent/24-bit mantissa (implicit bit set), handling overflow and underflow.
u32 pack(u32 sign, int exp, u32 mant24, u32& flags) {
    if (exp >= 255) {
        flags |= kOvf;
        return sign | kFmax;
    }
    if (exp <= 0) {
        flags |= kUnf;
        return sign;
    }
    return sign | (u32(exp) << 23) | (mant24 & 0x7FFFFFu);
}

int msb(u128 v) {
    const u64 hi = u64(v >> 64);
    return hi ? 127 - std::countl_zero(hi) : 63 - std::countl_zero(u64(v));
}

}  // namespace

namespace {

// Exact binary32 add/sub with truncation toward zero. VU macro operations use this directly;
// the EE COP1 FPU applies its one-guard-bit operand reduction before reaching this helper.
u32 add_exact(u32 a, u32 b, u32& flags) {
    int ea = int((a >> 23) & 0xFF), eb = int((b >> 23) & 0xFF);
    if (ea == 0 && eb == 0) return a & b;                    // -0 only when both are -0
    if (ea == 0) return b;
    if (eb == 0) return a;

    // Order by magnitude so that `a` is the larger operand.
    if ((b & 0x7FFFFFFFu) > (a & 0x7FFFFFFFu)) {
        std::swap(a, b);
        std::swap(ea, eb);
    }
    const u32 sign = a & kSignBit;
    const bool subtract = (a ^ b) & kSignBit;
    const u128 ma = u128((a & 0x7FFFFFu) | 0x800000u) << 64;
    u128 mb = u128((b & 0x7FFFFFu) | 0x800000u) << 64;

    // Align b. With 64 guard bits every shift up to 64 is exact; beyond that a sticky bit is kept so that the
    // subtraction still truncates correctly (the truncated result of A - b with b's tail lost equals A - (b' + 1)).
    const int d = ea - eb;
    bool lost = false;
    if (d >= 128) {
        lost = true;
        mb = 0;
    } else if (d > 0) {
        lost = (mb & ((u128(1) << d) - 1)) != 0;
        mb >>= d;
    }
    u128 r;
    if (subtract) {
        if (lost) mb += 1;
        r = ma - mb;
        if (r == 0) return pack_zero(0);                      // exact cancellation is +0 when rounding toward zero
    } else {
        r = ma + mb;
    }
    const int p = msb(r);
    const int e = ea + (p - 87);
    const u32 m = p >= 23 ? u32(r >> (p - 23)) : u32(r << (23 - p));
    return pack(sign, e, m, flags);
}

u32 add_fpu(u32 a, u32 b, bool subtract, u32& flags) {
    a = in(a);
    b = in(b);

    // PCSX2's FPU_ADD_SUB leaves only the PS2's single guard bit when the
    // exponent gap would otherwise expose lower significand bits.
    const int delta = int((a >> 23) & 0xFF) - int((b >> 23) & 0xFF);
    if (delta >= 25) {
        b &= kSignBit;                                         // smaller operand becomes signed zero
    } else if (delta > 0) {
        b &= 0xFFFFFFFFu << (delta - 1);
    } else if (delta <= -25) {
        a &= kSignBit;
    } else if (delta < 0) {
        a &= 0xFFFFFFFFu << (-delta - 1);
    }

    if (subtract) b ^= kSignBit;
    return add_exact(a, b, flags);
}

}  // namespace

u32 add(u32 a, u32 b, u32& flags) { return add_fpu(a, b, false, flags); }
u32 sub(u32 a, u32 b, u32& flags) { return add_fpu(a, b, true, flags); }

u32 vu_add(u32 a, u32 b, u32& flags) { return add_exact(in(a), in(b), flags); }
u32 vu_sub(u32 a, u32 b, u32& flags) { return add_exact(in(a), in(b) ^ kSignBit, flags); }

u32 mul(u32 a, u32 b, u32& flags) {
    a = in(a);
    b = in(b);
    const u32 sign = (a ^ b) & kSignBit;
    const int ea = int((a >> 23) & 0xFF), eb = int((b >> 23) & 0xFF);
    if (ea == 0 || eb == 0) return pack_zero(sign);
    const u64 p = u64((a & 0x7FFFFFu) | 0x800000u) * u64((b & 0x7FFFFFu) | 0x800000u);
    int e = ea + eb - 127;
    u32 m;
    if (p & (u64(1) << 47)) {
        m = u32(p >> 24);
        ++e;
    } else {
        m = u32(p >> 23);
    }
    return pack(sign, e, m, flags);
}

namespace {
u32 div_impl(u32 a, u32 b, u32& flags, bool nearest) {
    a = in(a);
    b = in(b);
    const u32 sign = (a ^ b) & kSignBit;
    const int ea = int((a >> 23) & 0xFF), eb = int((b >> 23) & 0xFF);
    if (ea == 0) return pack_zero(sign);
    const u64 ma = (a & 0x7FFFFFu) | 0x800000u, mb = (b & 0x7FFFFFu) | 0x800000u;
    const bool ratio_below_one = ma < mb;
    const u64 numerator = ma << (ratio_below_one ? 24 : 23);
    u64 m = numerator / mb;
    const u64 remainder = numerator % mb;
    if (nearest && (2 * remainder > mb || (2 * remainder == mb && (m & 1)))) ++m;
    int e = ea - eb + 127 - int(ratio_below_one);
    if (m == (u64(1) << 24)) {
        m >>= 1;
        ++e;
    }
    return pack(sign, e, u32(m), flags);
}
}  // namespace

u32 div(u32 a, u32 b, u32& flags) { return div_impl(a, b, flags, false); }
u32 div_nearest(u32 a, u32 b, u32& flags) { return div_impl(a, b, flags, true); }

namespace {
u32 sqrt_abs_impl(u32 x, bool nearest) {
    x = in(x) & 0x7FFFFFFFu;
    const int ex = int(x >> 23);
    if (ex == 0) return 0;
    u64 m = (x & 0x7FFFFFu) | 0x800000u;
    int e = ex - 127;
    if (e & 1) {
        m <<= 1;
        --e;
    }
    // sqrt(m * 2^23) in [2^23, 2^24): the 24-bit result mantissa.
    const u64 n = m << 23;
    u64 lo = 0, hi = u64(1) << 25;
    while (lo + 1 < hi) {
        const u64 mid = (lo + hi) / 2;
        if (mid * mid <= n) lo = mid; else hi = mid;
    }
    if (nearest && 4 * n > (2 * lo + 1) * (2 * lo + 1)) ++lo;
    int result_exp = e / 2 + 127;
    if (lo == (u64(1) << 24)) {
        lo >>= 1;
        ++result_exp;
    }
    return (u32(result_exp) << 23) | (u32(lo) & 0x7FFFFFu);
}
}  // namespace

u32 sqrt_abs(u32 x) { return sqrt_abs_impl(x, false); }
u32 sqrt_nearest_abs(u32 x) { return sqrt_abs_impl(x, true); }

u32 from_i32(s32 v) {
    if (v == 0) return 0;
    const u32 sign = v < 0 ? kSignBit : 0;
    const u32 mag = v < 0 ? 0u - u32(v) : u32(v);
    const int p = 31 - std::countl_zero(mag);
    const u32 m = p >= 23 ? mag >> (p - 23) : mag << (23 - p);
    return sign | (u32(127 + p) << 23) | (m & 0x7FFFFFu);
}

s32 to_i32(u32 f) {
    const u32 e = f & 0x7F800000u;
    if (e > 0x4E800000u) return (f & kSignBit) ? INT32_MIN : INT32_MAX;
    const int ex = int(e >> 23) - 127;
    if (ex < 0) return 0;
    const u64 m = (f & 0x7FFFFFu) | 0x800000u;
    const u64 mag = ex >= 23 ? m << (ex - 23) : m >> (23 - ex);
    return (f & kSignBit) ? s32(0u - u32(mag)) : s32(u32(mag));
}

namespace {
// Signed integer key with the same ordering as the float values (both zeros map to 0).
s64 key(u32 f) {
    f = in(f);
    const s64 mag = f & 0x7FFFFFFFu;
    return (f & kSignBit) ? -mag : mag;
}
}  // namespace

bool eq(u32 a, u32 b) { return key(a) == key(b); }
bool lt(u32 a, u32 b) { return key(a) < key(b); }
bool le(u32 a, u32 b) { return key(a) <= key(b); }

u32 max_bits(u32 a, u32 b) {
    return (s32(a) < 0 && s32(b) < 0) ? u32(std::min<s32>(s32(a), s32(b))) : u32(std::max<s32>(s32(a), s32(b)));
}
u32 min_bits(u32 a, u32 b) {
    return (s32(a) < 0 && s32(b) < 0) ? u32(std::max<s32>(s32(a), s32(b))) : u32(std::min<s32>(s32(a), s32(b)));
}

}  // namespace nf::ee::fp

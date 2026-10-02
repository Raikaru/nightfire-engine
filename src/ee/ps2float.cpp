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

u32 add(u32 a, u32 b, u32& flags) {
    a = in(a);
    b = in(b);
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

u32 sub(u32 a, u32 b, u32& flags) { return add(a, b ^ kSignBit, flags); }

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

u32 div(u32 a, u32 b, u32& flags) {
    a = in(a);
    b = in(b);
    const u32 sign = (a ^ b) & kSignBit;
    const int ea = int((a >> 23) & 0xFF), eb = int((b >> 23) & 0xFF);
    if (ea == 0) return pack_zero(sign);
    const u64 ma = (a & 0x7FFFFFu) | 0x800000u, mb = (b & 0x7FFFFFu) | 0x800000u;
    const u64 q = (ma << 26) / mb;                            // 2^25 <= q < 2^27
    int e = ea - eb + 127;
    u32 m;
    if (q >= (u64(1) << 26)) {
        m = u32(q >> 3);
    } else {
        m = u32(q >> 2);
        --e;
    }
    return pack(sign, e, m, flags);
}

u32 sqrt_abs(u32 x) {
    x = in(x) & 0x7FFFFFFFu;
    const int ex = int(x >> 23);
    if (ex == 0) return 0;
    u64 m = (x & 0x7FFFFFu) | 0x800000u;
    int e = ex - 127;
    if (e & 1) {
        m <<= 1;
        --e;
    }
    // sqrt(m * 2^23) in [2^23, 2^24): the 24-bit result mantissa, exact floor.
    const u64 n = m << 23;
    u64 lo = 0, hi = u64(1) << 25;
    while (lo + 1 < hi) {
        const u64 mid = (lo + hi) / 2;
        if (mid * mid <= n) lo = mid; else hi = mid;
    }
    return (u32(e / 2 + 127) << 23) | (u32(lo) & 0x7FFFFFu);
}

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

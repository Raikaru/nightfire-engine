#include "ee/ps2float_test.hpp"

#include <cstdio>

#include "ee/ps2float.hpp"
#include "ee/vu0.hpp"
#include "ee/cpu.hpp"

namespace nf::ee::fp {
namespace {

bool expect(const char* name, u32 actual, u32 expected) {
    if (actual == expected) return true;
    std::printf("PS2 float self-test %s: expected 0x%08x, got 0x%08x\n", name, expected, actual);
    return false;
}

}  // namespace

bool self_test() {
    static constexpr u32 kAdd[30] = {
        0x34400000u, 0x3F000003u, 0x3F400003u, 0x3F600003u, 0x3F700003u, 0x3F780003u, 0x3F7C0003u, 0x3F7E0003u,
        0x3F7F0003u, 0x3F7F8003u, 0x3F7FC003u, 0x3F7FE003u, 0x3F7FF003u, 0x3F7FF803u, 0x3F7FFC03u, 0x3F7FFE03u,
        0x3F7FFF03u, 0x3F7FFF83u, 0x3F7FFFC3u, 0x3F7FFFE3u, 0x3F7FFFF3u, 0x3F7FFFFBu, 0x3F7FFFFFu, 0x3F800000u,
        0x3F800001u, 0x3F800001u, 0x3F800001u, 0x3F800001u, 0x3F800001u, 0x3F800001u,
    };
    static constexpr u32 kSub[30] = {
        0x40000000u, 0x3FC00000u, 0x3FA00000u, 0x3F900000u, 0x3F880000u, 0x3F840000u, 0x3F820000u, 0x3F810000u,
        0x3F808000u, 0x3F804000u, 0x3F802000u, 0x3F801000u, 0x3F800800u, 0x3F800400u, 0x3F800200u, 0x3F800100u,
        0x3F800080u, 0x3F800040u, 0x3F800020u, 0x3F800010u, 0x3F800008u, 0x3F800004u, 0x3F800002u, 0x3F800001u,
        0x3F800001u, 0x3F800001u, 0x3F800001u, 0x3F800001u, 0x3F800001u, 0x3F800001u,
    };

    bool ok = true;
    for (int delta = 1; delta <= 30; ++delta) {
        const u32 a = 0x3F800001u;
        const u32 b = 0x80000000u | (u32(127 - delta) << 23) | 0x7FFFFFu;
        char name[32];
        std::snprintf(name, sizeof name, "EE add gap %d", delta);
        u32 flags = 0;
        ok &= expect(name, add(a, b, flags), kAdd[delta - 1]);
        std::snprintf(name, sizeof name, "EE add reverse gap %d", delta);
        flags = 0;
        ok &= expect(name, add(b, a, flags), kAdd[delta - 1]);
        std::snprintf(name, sizeof name, "EE sub gap %d", delta);
        flags = 0;
        ok &= expect(name, sub(a, b, flags), kSub[delta - 1]);
        std::snprintf(name, sizeof name, "EE reverse sub gap %d", delta);
        flags = 0;
        ok &= expect(name, sub(b, a, flags), kSub[delta - 1] | kSignBit);
    }

    u32 flags = 0;
    ok &= expect("FPU add guard reduction", add(0x3D800001u, 0xBE800000u, flags), 0xBE400000u);
    flags = 0;
    ok &= expect("VU add direct Chop", vu_add(0x3D800001u, 0xBE800000u, flags), 0xBE3FFFFFu);
    flags = 0;
    ok &= expect("FPU multiply", mul(0x3F9E0419u, 0x40490FDBu, flags), 0x40783602u);
    flags = 0;
    ok &= expect("Chop divide", div(0x40400000u, 0x40E00000u, flags), 0x3EDB6DB6u);
    flags = 0;
    ok &= expect("I-FPU DIV nearest", div_nearest(0x40400000u, 0x40E00000u, flags), 0x3EDB6DB7u);
    ok &= expect("VU sqrt Chop", sqrt_abs(0x3F000BAFu), 0x3F350D35u);
    ok &= expect("I-FPU sqrt nearest", sqrt_nearest_abs(0x3F000BAFu), 0x3F350D36u);
    flags = 0;
    ok &= expect("I-FPU rsqrt sequence", div(0x3F800000u, sqrt_abs(0x3F000BAFu), flags), 0x3FB4FCB1u);
    flags = 0;
    const u32 product = mul(0x3F800001u, 0xBF7FFFFFu, flags);
    flags = 0;
    ok &= expect("I-FPU MADD rounds multiply then add", add(0x3F800001u, product, flags), 0x34000000u);
    flags = 0;
    ok &= expect("I-FPU MSUB rounds multiply then subtract", sub(0x3F800001u, product, flags), 0x40000000u);

    ok &= expect("CVT.S signed integer", from_i32(-16777219), 0xCB800001u);
    ok &= expect("CVT.W positive truncation", u32(to_i32(0x3FC00000u)), 1u);
    ok &= expect("CVT.W negative truncation", u32(to_i32(0xBFC00000u)), 0xFFFFFFFFu);
    ok &= expect("CVT.W positive saturation", u32(to_i32(0x4F000000u)), 0x7FFFFFFFu);
    ok &= expect("CVT.W negative saturation", u32(to_i32(0xCF000000u)), 0x80000000u);
    if (!eq(0x00000000u, 0x80000000u)) { std::printf("PS2 float self-test signed zero compare failed\n"); ok = false; }
    if (!lt(0xBF800000u, 0x00000000u)) { std::printf("PS2 float self-test C.LT ordering failed\n"); ok = false; }
    if (!le(0xBF800000u, 0xBF800000u)) { std::printf("PS2 float self-test C.LE equality failed\n"); ok = false; }
    if (!eq(0x7F800000u, kFmax)) { std::printf("PS2 float self-test finite input clamp failed\n"); ok = false; }

    flags = 0;
    ok &= expect("overflow saturation", mul(kFmax, 0x40000000u, flags), kFmax);
    if ((flags & kOvf) == 0) { std::printf("PS2 float self-test overflow flag missing\n"); ok = false; }
    flags = 0;
    ok &= expect("underflow flush", mul(0x00800000u, 0x3F000000u, flags), 0u);
    if ((flags & kUnf) == 0) { std::printf("PS2 float self-test underflow flag missing\n"); ok = false; }
    ::nf::ee::Vu0 vu;
    constexpr u32 kDiv = 0x3BCu | (1u << 11) | (2u << 16);
    constexpr u32 kSqrt = 0x3BDu | (2u << 16);
    constexpr u32 kRsqrt = 0x3BEu | (1u << 11) | (2u << 16);
    constexpr u32 kVadd = 0x28u | (1u << 24) | (2u << 16) | (1u << 11) | (3u << 6);
    vu.vf[1].w[0] = 0x3D800001u;
    vu.vf[2].w[0] = 0xBE800000u;
    vu.macro(kVadd);
    ok &= expect("VU VADD macro instruction", vu.vf[3].w[0], 0xBE3FFFFFu);
    vu.vf[1].w[0] = 0x40400000u;
    vu.vf[2].w[0] = 0x40E00000u;
    vu.macro(kDiv);
    ok &= expect("VU DIV Chop", vu.vi[::nf::ee::Vu0::kQ], 0x3EDB6DB6u);
    vu.vf[1].w[0] = 0;
    vu.vf[2].w[0] = 0;
    vu.macro(kDiv);
    ok &= expect("VU DIV zero over zero", vu.vi[::nf::ee::Vu0::kQ], kFmax);
    vu.macro(kRsqrt);
    ok &= expect("VU RSQRT zero over zero", vu.vi[::nf::ee::Vu0::kQ], kFmax);
    vu.vf[1].w[0] = 0x80000000u;
    vu.vf[2].w[0] = 0x80000000u;
    vu.macro(kRsqrt);
    ok &= expect("VU RSQRT signed-zero result", vu.vi[::nf::ee::Vu0::kQ], 0xFF7FFFFFu);
    vu.vf[2].w[0] = 0x80000000u;
    vu.macro(kSqrt);
    ok &= expect("VU SQRT negative zero", vu.vi[::nf::ee::Vu0::kQ], 0u);
    ::nf::ee::Memory memory;
    ::nf::ee::Cpu cpu(memory);
    memory.write<::nf::ee::u32>(0, 0x46000003u | (2u << 16) | (1u << 11) | (3u << 6));
    cpu.f[1] = 0x40400000u;
    cpu.f[2] = 0x40E00000u;
    cpu.step();
    ok &= expect("COP1 DIV.S instruction", cpu.f[3], 0x3EDB6DB7u);

    if (ok) std::printf("PS2 float self-test passed (30 exponent gaps, EE/VU arithmetic, conversion, compare, edge flags)\n");
    return ok;
}

}  // namespace nf::ee::fp

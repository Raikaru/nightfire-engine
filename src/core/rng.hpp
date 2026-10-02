#pragma once

#include <cmath>
#include <cstdint>
// Shared game RNG (`Rand_Random` / `Rand_Rand` / `Rand_FRand` / `Rand_FRand_MVar2`,
// ACTION.ELF 0x1e36f0/0x1e3738/0x1e3780/0x1e38b0; GC/Xbox pairs all high confidence).
// The original is global mutable state (SEED_X @0x30d0a0, SEED_Y @0x30d0a4); this class
// models one such stream. Combined Wichmann-Hill-ish recurrence, 32-bit wrap:
//
//   X = 0x4650 * (X & 0xFFFF) + (X >> 16)
//   Y = 0x78B7 * (Y & 0xFFFF) + (Y >> 16)
//   v1 = (X << 16) | (Y & 0xFFFF)
//   Rand_Random() = v1
//   Rand_Rand(n)   = ((v1 & 0xFFFF) * n) >> 16            (integer path, bit-exact)
//   Rand_FRand(r)  = r * (conv(v1) * 2^-32)               (float path, see below)
//   MVar2(a, b)    = a * (conv(v1) * 2^-32) - b
//
// conv(v1): v1 < 0 (signed 32) goes through the doubling idiom
// Boot seed is the ELF image words (X0 = 0x1F123BB5, Y0 = 0x159A55E5, verified);
// there is no game-side reseed (streams self-update only) and RandTable1/2 are
// shipped .data, never written (EeInterp-verified, no init function exists).
//
// Fidelity: the integer paths are bit-exact; the float paths truncate every op
// like the EE (double-exact intermediates + pull toward zero) and verify bit-exact
// by differential.

namespace nf {

class GameRng {
public:
    static constexpr std::uint32_t kBootX = 0x1F123BB5u;
    static constexpr std::uint32_t kBootY = 0x159A55E5u;
    // 2^-32 as the original's float literal (0x2F800000).
    static constexpr float kInv2Pow32 = 2.3283064e-10f;

    GameRng() = default;  // boot seed
    GameRng(std::uint32_t x, std::uint32_t y) : x_(x), y_(y) {}

    void seed(std::uint32_t x, std::uint32_t y) {
        x_ = x;
        y_ = y;
    }
    // Raw state words for differential poking (EE layout: X @+0, Y @+4).
    std::uint32_t seed_x() const { return x_; }
    std::uint32_t seed_y() const { return y_; }

    // `Rand_Random`: advance both words, return the combined 32 bits.
    std::uint32_t random() {
        step();
        return (x_ << 16) | (y_ & 0xFFFFu);
    }

    // `Rand_Rand(n)`: uniform in [0, n). Integer path: exact.
    std::uint32_t rand_int(std::uint32_t n) {
        step();
        return (((x_ << 16) | (y_ & 0xFFFFu)) & 0xFFFFu) * n >> 16;
    }

    // `Rand_FRand(range)`: uniform in [0, range). Same association as the original.
    float frand(float range) {
        step();
        return ee_mul(range, ee_mul(convert(), kInv2Pow32));
    }

    // `Rand_FRand_MVar2(a, b)`: a * uniform - b, original op order.
    float mvar2(float a, float b) {
        step();
        return ee_sub(ee_mul(a, ee_mul(convert(), kInv2Pow32)), b);
    }

private:
    std::uint32_t x_ = kBootX, y_ = kBootY;

    void step() {
        x_ = 0x4650u * (x_ & 0xFFFFu) + (x_ >> 16);
        y_ = 0x78B7u * (y_ & 0xFFFFu) + (y_ >> 16);
    }

    // Truncate a double-exact value to float toward zero (EE model). Exact whenever
    // the double holds the true result (int32 converts, float products); sums go the
    // same route and are verified bit-exact by differential. Pull only when the
    // conversion rounded away from zero (a toward-zero round is already truncated).
    static float truncf(double d) {
        float r = float(d);
        if (std::abs(double(r)) > std::abs(d)) r = std::nextafterf(r, 0.0f);
        return r;
    }
    static float ee_mul(float a, float b) { return truncf(double(a) * double(b)); }
    static float ee_sub(float a, float b) { return truncf(double(a) - double(b)); }
    // Combined draw converted to float exactly like the original's branch.
    float convert() const {
        const std::uint32_t v1 = (x_ << 16) | (y_ & 0xFFFFu);
        if (std::int32_t(v1) < 0) {
            const std::uint32_t h = (v1 & 1u) | (v1 >> 1);  // < 2^31, fits int32
            return truncf(double(std::int32_t(h))) + truncf(double(std::int32_t(h)));
        }
        return truncf(double(std::int32_t(v1)));
    }
};

// Process-global draw stream: the original's RNG is global mutable state (seeded at
// boot, never reseeded), so all runtime consumers draw here in tick order. Starts at
// the boot seed; frontends reseed once per match (`--seed`). Single-threaded use only.
inline GameRng& game_rng() {
    static GameRng instance;
    return instance;
}

}  // namespace nf

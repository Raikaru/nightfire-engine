#pragma once

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <source_location>

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

enum class GameRngCall : std::uint8_t { Random, RandInt, FRand, FRandHalf, MVar2 };

struct GameRngTraceRecord {
    std::uint64_t frame;
    const char* file;
    const char* function;
    std::uint32_t line;
    std::uint32_t result_bits;
    GameRngCall call;
};

class GameRngTrace {
public:
    static constexpr std::size_t kCapacity = 4096;

    void set_enabled(bool enabled) {
        if (enabled && !records_) records_ = std::make_unique<GameRngTraceRecord[]>(kCapacity);
        enabled_ = enabled;
    }
    bool enabled() const { return enabled_; }
    void set_frame(std::uint64_t frame) { frame_ = frame; }
    void reset() { count_ = 0; }
    std::uint64_t count() const { return count_; }
    std::uint64_t first_sequence() const { return count_ > kCapacity ? count_ - kCapacity : 0; }
    const GameRngTraceRecord& at(std::uint64_t sequence) const {
        return records_[std::size_t(sequence % kCapacity)];
    }

    void record(GameRngCall call, std::uint32_t result_bits, const std::source_location& loc) {
        if (!enabled_) return;
        records_[std::size_t(count_ % kCapacity)] =
            {frame_, loc.file_name(), loc.function_name(), loc.line(), result_bits, call};
        ++count_;
    }

private:
    std::unique_ptr<GameRngTraceRecord[]> records_;
    std::uint64_t frame_ = 0;
    std::uint64_t count_ = 0;
    bool enabled_ = false;
};

class GameRng {
public:
    static constexpr std::uint32_t kBootX = 0x1F123BB5u;
    static constexpr std::uint32_t kBootY = 0x159A55E5u;
    // 2^-32 as the original's float literal (0x2F800000).
    static constexpr float kInv2Pow32 = 2.3283064e-10f;

    GameRng() = default;  // boot seed
    GameRng(std::uint32_t x, std::uint32_t y) : x_(x), y_(y) {}

    void attach_trace(GameRngTrace* trace) { trace_ = trace; }
    void set_trace_frame(std::uint64_t frame) {
        if (trace_) trace_->set_frame(frame);
    }

    void seed(std::uint32_t x, std::uint32_t y) {
        x_ = x;
        y_ = y;
    }
    // Raw state words for differential poking (EE layout: X @+0, Y @+4).
    std::uint32_t seed_x() const { return x_; }
    std::uint32_t seed_y() const { return y_; }

    // `Rand_Random`: advance both words, return the combined 32 bits.
    std::uint32_t random(const std::source_location& loc = std::source_location::current()) {
        step();
        const std::uint32_t result = (x_ << 16) | (y_ & 0xFFFFu);
        if (trace_) trace_->record(GameRngCall::Random, result, loc);
        return result;
    }

    // `Rand_Rand(n)`: uniform in [0, n). Integer path: exact.
    std::uint32_t rand_int(std::uint32_t n, const std::source_location& loc = std::source_location::current()) {
        step();
        const std::uint32_t result = (((x_ << 16) | (y_ & 0xFFFFu)) & 0xFFFFu) * n >> 16;
        if (trace_) trace_->record(GameRngCall::RandInt, result, loc);
        return result;
    }

    // `Rand_FRand(range)`: uniform in [0, range). Same association as the original.
    float frand(float range, const std::source_location& loc = std::source_location::current()) {
        step();
        const float result = ee_mul(range, ee_mul(convert(), kInv2Pow32));
        if (trace_) trace_->record(GameRngCall::FRand, std::bit_cast<std::uint32_t>(result), loc);
        return result;
    }

    // `Rand_FRandHalf(range)`: range * uniform - range * 0.5, in source operation order.
    float frand_half(float range, const std::source_location& loc = std::source_location::current()) {
        step();
        const float result = ee_sub(ee_mul(range, ee_mul(convert(), kInv2Pow32)), ee_mul(range, 0.5f));
        if (trace_) trace_->record(GameRngCall::FRandHalf, std::bit_cast<std::uint32_t>(result), loc);
        return result;
    }

    // `Rand_FRand_MVar2(a, b)`: a * uniform - b, original op order.
    float mvar2(float a, float b, const std::source_location& loc = std::source_location::current()) {
        step();
        const float result = ee_sub(ee_mul(a, ee_mul(convert(), kInv2Pow32)), b);
        if (trace_) trace_->record(GameRngCall::MVar2, std::bit_cast<std::uint32_t>(result), loc);
        return result;
    }

private:
    std::uint32_t x_ = kBootX, y_ = kBootY;
    GameRngTrace* trace_ = nullptr;

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

// The normal game path preserves the original process-global stream. A runtime may bind a
// separate deterministic stream to one worker thread (for example an in-process server) so
// concurrent client/server ticks cannot race or interleave the shared draw order.
inline GameRng*& scoped_game_rng_binding() {
    static thread_local GameRng* binding = nullptr;
    return binding;
}

class ScopedGameRng {
public:
    explicit ScopedGameRng(GameRng& rng) : previous_(scoped_game_rng_binding()) { scoped_game_rng_binding() = &rng; }
    ~ScopedGameRng() { scoped_game_rng_binding() = previous_; }

    ScopedGameRng(const ScopedGameRng&) = delete;
    ScopedGameRng& operator=(const ScopedGameRng&) = delete;

private:
    GameRng* previous_;
};

inline GameRng& game_rng() {
    if (GameRng* binding = scoped_game_rng_binding()) return *binding;
    static GameRng instance;
    return instance;
}

}  // namespace nf

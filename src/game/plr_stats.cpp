// Player mission statistics: counters, scoring tables and the GetScore port.
#include "game/plr_stats.hpp"

#include <cmath>
#include <cstring>
#include <stdexcept>
#include <cstdio>

namespace nf {

namespace {

// GetScore row layout: ScoringTable (12 x 15 u32) + per-row live buffer.
constexpr std::uint32_t kRowWords = 15, kRowCount = 12;
constexpr std::uint32_t kScoringTableAddr = 0x2bc640;  // `ScoringTable` symbol (720 bytes)

// fptoui (0x224608, software): truncation toward zero, negatives clamp to 0,
// 64-bit wraparound (calibrated: 0.5->0, 2.7->2, -1.5->0, 3e9->0xB2D05E00).
std::uint64_t fptoui_soft(float f) {
    if (!(f > 0.0f)) return 0;
    return std::uint64_t(f);
}

// The original's int -> float idiom: signed values convert directly, negative
// bit patterns go through the halve/double path (exact on single-precision).
float itof_exact(std::int32_t v) {
    if (v >= 0) return float(v);
    const std::uint32_t u = std::uint32_t(v);
    return 2.0f * float((u & 1u) | (u >> 1));
}

// Round half away from zero in single precision.
float round_half_away(float v) {
    const float t = float(std::int32_t(v));
    if (std::fabs(v - t) >= 0.5f) return v >= 0.0f ? t + 1.0f : t - 1.0f;
    return t;
}

float buf_f(const std::uint8_t* buf, std::size_t word) {
    float f;
    std::memcpy(&f, buf + word * 4, 4);
    return f;
}
void set_buf_f(std::uint8_t* buf, std::size_t word, float f) { std::memcpy(buf + word * 4, &f, 4); }
std::uint32_t buf_u(const std::uint8_t* buf, std::size_t word) {
    std::uint32_t v;
    std::memcpy(&v, buf + word * 4, 4);
    return v;
}

// Per-category weight tail (the common LABEL_56/104 path): ratio of scaled to
// denom, rounded to a 0..1 weight. Returns the weight; scaled is already stored.
float tail_weight(std::uint8_t* buf, int i) {
    const std::size_t o = std::size_t(i) * 5;
    const float denom = itof_exact(std::int32_t(buf_u(buf, o + 1)));
    if (denom == 0.0f) return 0.0f;  // LABEL_122 with +12 left zero
    const float ratio = buf_f(buf, o + 4) / denom * 100.0f;
    float w = round_half_away(ratio) * 0.01f;
    if (w < 0.0f) return 0.0f;
    if (w > 1.0f) w = 1.0f;
    return w;
}

}  // namespace

void PlrStats::log_shot_hit_scenery() {
    if (shots_ > 0) --shots_;
    if (hits_ > shots_) hits_ = shots_;
}

void PlrStats::reset_for_mission() {
    detections_ = shots_ = hits_ = dispatched_ = disabled_ = surrendered_ = spawned_ = bond_ = 0;
    health_ = 0;
    elapsed_100ths_ = 0;
}

const PlrScoreRow* PlrScoreTables::find(std::uint32_t level) const {
    for (const PlrScoreRow& r : rows)
        if (r.level == level) return &r;
    return nullptr;
}

PlrScoreTables load_plr_score_tables(const Elf32& elf) {
    std::uint32_t base = kScoringTableAddr, bytes = kRowCount * kRowWords * 4;
    if (const auto sym = elf.symbol("ScoringTable")) {
        base = sym->value;
        bytes = sym->size;
    }
    if (bytes < kRowCount * kRowWords * 4) throw std::runtime_error("ScoringTable too short");
    const Bytes image = elf.at(base, kRowCount * kRowWords * 4);
    auto word = [&](std::size_t i) {
        std::uint32_t v;
        std::memcpy(&v, image.data() + i * 4, 4);
        return v;
    };
    auto word_f = [&](std::size_t i) {
        float f;
        std::memcpy(&f, image.data() + i * 4, 4);
        return f;
    };
    PlrScoreTables tables;
    for (std::uint32_t r = 0; r < kRowCount; ++r) {
        PlrScoreRow row;
        row.level = word(r * kRowWords);
        for (int k = 0; k < 4; ++k) row.totals[std::size_t(k)] = word(r * kRowWords + 1 + std::size_t(k));
        row.factor = word_f(r * kRowWords + 5);
        // The live buffer follows through the row's buffer pointer (pristine disc data).
        const std::uint32_t buf_addr = word(r * kRowWords + 6);
        const Bytes buf = elf.at(buf_addr, 180);
        std::memcpy(row.buffer.data(), buf.data(), 180);
        tables.rows.push_back(row);
    }
    return tables;
}

std::string separate_number(std::uint32_t value) {
    const std::uint32_t lo = value % 1000, mid = value / 1000 % 1000, hi = value / 1000000;
    char out[16];
    if (hi != 0) std::snprintf(out, sizeof(out), "%u,%.3u,%.3u", hi, mid, lo);
    else if (mid != 0) std::snprintf(out, sizeof(out), "%u,%.3u", mid, lo);
    else std::snprintf(out, sizeof(out), "%u", lo);
    return out;
}

PlrStats::Score PlrStats::compute(const PlrScoreTables& tables, std::uint32_t level, int difficulty,
                                   bool succeeded) const {
    Score out;
    const PlrScoreRow* row_in = tables.find(level);
    if (!row_in) return out;
    out.valid = true;
    // Driving rows score from the shared driving data on the DRIVING.ELF side (the action
    // engine has no writers for it); not computed here.
    if ((level & 0xff000000u) == 0x09000000u) return out;
    std::array<std::uint8_t, 180> buf = row_in->buffer;
    std::uint8_t* b = buf.data();
    auto cu = [&](int i, int w) -> std::uint32_t& { return *reinterpret_cast<std::uint32_t*>(&b[std::size_t(i) * 20 + std::size_t(w) * 4]); };

    // Targets from the live counters (the action fill).
    cu(0, 2) = bond_;
    cu(1, 2) = dispatched_;
    cu(2, 2) = disabled_;
    cu(3, 2) = surrendered_;
    cu(4, 2) = detections_;
    cu(5, 2) = cu(5, 0);  // default: par; overridden by accuracy when any shot was fired
    if (shots_ != 0) cu(5, 2) = std::uint32_t((std::uint64_t)hits_ * cu(5, 0) / shots_);
    cu(6, 2) = health_ <= 0.0f ? 0 : std::uint32_t(health_);  // fptoui(health float)
    cu(7, 2) = elapsed_100ths_;
    cu(8, 2) = bond_;
    // Difficulty factor (+36): GameState+0x28 selects {1:0, 2:1, else:2} (oracle-calibrated).
    const std::uint32_t mult = difficulty == 1 ? 0 : difficulty == 2 ? 1 : 2;

    std::uint32_t accum = 0;  // row+44
    auto accum_fold = [&](int i) {
        const float f = itof_exact(std::int32_t(accum)) + buf_f(b, std::size_t(i) * 5 + 4);
        accum = std::uint32_t(fptoui_soft(f));
    };
    for (int i = 0; i < 9; ++i) {
        const std::size_t o = std::size_t(i) * 5;
        set_buf_f(b, o + 3, 0.0f);
        set_buf_f(b, o + 4, 0.0f);
        if (buf_f(b, o) == 0.0f) {
            accum_fold(i);  // skip still folds (+16 is zero)
            out.scaled[std::size_t(i)] = 0;
            continue;
        }
        if (i == 7) {
            // Time category: par points scaled by par time over elapsed quarters, ceiled.
            // Both original branches (under/over par) reduce to this; q=0 cannot divide.
            const std::uint32_t target = cu(7, 2), par = cu(7, 0);
            const std::uint32_t denom = cu(7, 1);
            const std::uint32_t q = target / 100u;
            const float v = float(std::uint64_t(denom) * par) / float(q == 0 ? 1u : q);
            std::int32_t t = std::int32_t(v);
            if (std::fabs(v - float(t)) >= 1e-5f) ++t;
            set_buf_f(b, o + 4, float(t));
        } else if (i >= 8) {
            // Static par category: full marks (denom, weight 1) when the target reaches
            // the par, else zero.
            if (cu(8, 2) >= cu(8, 0)) {
                set_buf_f(b, o + 4, itof_exact(std::int32_t(cu(8, 1))));
                set_buf_f(b, o + 3, 1.0f);
                accum_fold(i);
                out.scaled[std::size_t(i)] = buf_u(b, o + 4);
                continue;
            }
        } else if (i != 4) {
            // Generic accuracy category: scaled = ceil(min*denom/count).
            const std::uint32_t target = cu(i, 2), count = cu(i, 0), denom = cu(i, 1);
            const std::uint32_t m = target < count ? target : count;
            const float dm = itof_exact(std::int32_t(denom)), cm = itof_exact(std::int32_t(count));
            float v = cm != 0.0f ? dm * itof_exact(std::int32_t(m)) / cm : 0.0f;
            std::int32_t t = std::int32_t(v);
            if (std::fabs(v - float(t)) >= 1e-5f) ++t;
            set_buf_f(b, o + 4, float(t));
        } else {
            // Category 4 (detections): over par holds full marks; under par decays by the
            // row factor per extra detection (0.03 when the row factor is 0).
            if (cu(4, 0) < cu(4, 2)) {
                const float k = row_in->factor != 0.0f ? row_in->factor : 0.03f;
                float w = 1.0f - itof_exact(std::int32_t(cu(4, 2) - cu(4, 0))) * k;
                if (w < 0.0f) w = 0.0f;
                float v = w * itof_exact(std::int32_t(cu(4, 1)));
                std::int32_t t = std::int32_t(v);
                if (std::fabs(v - float(t)) >= 0.5f) t += (v >= 0.0f ? 1 : -1);
                set_buf_f(b, o + 4, float(t));
            } else {
                set_buf_f(b, o + 4, itof_exact(std::int32_t(cu(4, 1))));
                set_buf_f(b, o + 3, 1.0f);
                accum_fold(i);
                out.scaled[std::size_t(i)] = buf_u(b, o + 4);
                continue;
            }
        }
        set_buf_f(b, o + 3, tail_weight(b, i));
        accum_fold(i);
        out.scaled[std::size_t(i)] = buf_u(b, o + 4);
    }

    const std::uint32_t base = accum;
    const std::uint32_t bonus = std::uint32_t((std::uint64_t)base * mult);
    const std::uint32_t total = base + bonus;
    bool done_better = total != 0;
    if (total < best_) done_better = false;
    if (total > best_) best_ = total;
    out.categories = base;
    out.mult = mult;
    out.total = total;
    out.done_better = done_better;
    if (!succeeded) {
        // Mission_Status() != 6: the original zeroes row+36/40/44/52 after the
        // best update; per-category words and the best survive for the debrief.
        out.mult = 0;
        out.categories = 0;
        out.total = 0;
        out.done_better = false;
    }
    return out;
}

}  // namespace nf

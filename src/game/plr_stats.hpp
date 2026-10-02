// Player mission statistics (PlrStat_* / PlrStats_*, ACTION.ELF 0x198xxx): the
// per-mission counters, the 12-row level scoring tables with their per-level
// 9-category live buffers from the ELF, and the GetScore computation with the
// original float/int semantics (EE FPU: single precision, truncation toward
// zero, negatives clamp to 0, 64-bit wraparound; see docs for the calibration
// battery against nfmips `call PlrStat_GetScore__FUc`).
//
// Counter semantics (PlrStat_Log*, gated on playing, slot 0 = Bond):
//   detections  LogEnemyDetectedPlayer: a drone first spotted the player
//   shots       LogShotFired: trigger pulls (player slot)
//   hits        LogShotHitEnemy: bullets that hit an enemy (clamped to shots)
//   scenery     LogShotHitScenery: DECREMENTS shots (floor 0) and clamps hits
//   dispatched  LogEnemyDispatched: kills; disabled/surrendered tracked apart
//   spawned     LogEnemySpawned: mission-global enemy spawns (kill par source)
//   bond        LogBondBonus/DoneBondMoment: one-shot Bond moments (also drives the HUD flag)
//   health      LogHealth: last logged health fraction
//   elapsed     LogUpdateElapsedTime: mission wall time in 100ths of a second
// What the session cannot observe stays 0 (detections, disabled/surrendered
// detail, health, bond ids) and is noted at the feed points.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "assets/elf.hpp"

namespace nf {

// One level scoring row: ScoringTable (12 x 15 u32) + its 180-byte live
// buffer (9 categories x {count, denom, target, ratio, scaled}).
struct PlrScoreRow {
    std::uint32_t level = 0;                // [0] base-map hcode the row matches
    std::array<std::uint32_t, 4> totals{};  // [1..4] rank cutoffs (GetLevelTotals)
    float factor = 0;                       // [5] per-row float (0 or 0.35 on disc)
    std::array<std::uint8_t, 180> buffer{};  // pristine live buffer ([6] points at it)
    std::array<std::uint32_t, 7> runtime{};  // [7..13] accumulators/best/total (zeroed per compute)
};

struct PlrScoreTables {
    std::vector<PlrScoreRow> rows;  // 12, in ELF order
    const PlrScoreRow* find(std::uint32_t level) const;
};

// Loads the scoring tables (+ pristine buffers) from ACTION.ELF. Throws on missing/short data.
PlrScoreTables load_plr_score_tables(const Elf32& elf);


// Thousands separators exactly like SeparateNumber ("%d,%.3d,%.3d").
std::string separate_number(std::uint32_t value);

class PlrStats {
public:
    // fptoui: truncation toward zero, negatives clamp to 0, 64-bit wraparound.
    static std::uint64_t fptoui(float f) { return f <= 0.0f ? 0 : std::uint64_t(f); }
    // Unsigned int -> float over the full 32-bit range (the original's halve/double idiom).
    static float utof(std::uint32_t v) { return float(v & 1) + float(v >> 1) + float(v >> 1); }

    void reset_for_mission();

    // Log* mirrors (call only while playing; slot 0 = the local player).
    void log_shot_fired() { ++shots_; }
    void log_shot_hit_enemy() { ++hits_; }
    void log_shot_hit_scenery();  // shots-- (floor 0), hits clamped to shots
    void log_kill() { ++dispatched_; }
    void log_disabled() { ++disabled_; }
    void log_surrendered() { ++surrendered_; }
    void log_spawned() { ++spawned_; }
    void log_detection() { ++detections_; }
    void log_health(float fraction) { health_ = fraction; }
    void log_bond() { ++bond_; }
    void add_elapsed_100ths(std::uint32_t ticks) { elapsed_100ths_ += ticks; }
    void set_elapsed_100ths(std::uint32_t v) { elapsed_100ths_ = v; }
    struct Score {
        std::uint32_t categories = 0;  // row+44 accumulator (pre-multiplier)
        std::uint32_t mult = 0;        // row+36 difficulty factor
        std::uint32_t total = 0;       // row+52: categories + categories*mult
        bool done_better = false;
        bool valid = false;  // a scoring row matched the level
        std::array<std::uint32_t, 9> scaled{};  // per-category +16 values (calibration/debugging)
    };
    // PlrStat_GetScore__FUc(0): score the mission. `succeeded` = Mission_Status() == 6
    // (anything else zeroes the score); `difficulty` = GameState+0x28 (1/2/3).
    Score compute(const PlrScoreTables& tables, std::uint32_t level, int difficulty, bool succeeded) const;

    // Counters for the results stats page.
    std::uint32_t shots() const { return shots_; }
    std::uint32_t hits() const { return hits_; }
    std::uint32_t kills() const { return dispatched_; }

private:
    std::uint32_t detections_ = 0, shots_ = 0, hits_ = 0, dispatched_ = 0, disabled_ = 0, surrendered_ = 0,
                  spawned_ = 0, bond_ = 0;
    float health_ = 0;
    std::uint32_t elapsed_100ths_ = 0;
    mutable std::uint32_t best_ = 0;  // session best (row+48); updated by compute(), profile owns the rest
};

}  // namespace nf

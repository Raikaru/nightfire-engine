#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "game/actions.hpp"

namespace nf {

class ArenaSession;
class PlayerAnimator;
class World;
struct MatchLaunch;
struct FrameTiming;
namespace bots { class BotMatch; }

// Imports one versioned oracle MP JSONL frame and supplies its subsequent recorded pads.
class MpSeedImporter {
public:
    explicit MpSeedImporter(const std::string& spec);
    ~MpSeedImporter();
    MpSeedImporter(MpSeedImporter&&) noexcept;
    MpSeedImporter& operator=(MpSeedImporter&&) noexcept;
    MpSeedImporter(const MpSeedImporter&) = delete;
    MpSeedImporter& operator=(const MpSeedImporter&) = delete;

    std::uint64_t frame() const;
    std::uint64_t available_frames() const;
    void configure(MatchLaunch& launch) const;
    void restore(World& world, ArenaSession& session, bots::BotMatch* bots) const;
    void restore_at(std::uint64_t frame, World& world, ArenaSession& session, bots::BotMatch* bots,
                    bool allow_body_anim_gap = false) const;
    void restore_player_animation(std::uint64_t frame, std::size_t slot, PlayerAnimator& animator,
                                  int current_weapon, int category) const;
    // True when any captured human lacks exact body AnimSet state and keeps the live engine lifecycle instead.
    bool body_anim_sets_unseeded(std::uint64_t frame, std::size_t humans) const;
    bool input_for(std::uint64_t frame, PadInputs& pads, FrameTiming& timing, float& elapsed,
                   float& total_elapsed) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace nf

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "game/actions.hpp"

namespace nf {

class ArenaSession;
class World;
struct MatchLaunch;
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
    void restore_at(std::uint64_t frame, World& world, ArenaSession& session, bots::BotMatch* bots) const;
    bool input_for(std::uint64_t frame, PadInputs& pads, float& rate) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace nf

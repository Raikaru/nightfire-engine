#pragma once

#include <filesystem>

#include "assets/game_files.hpp"

namespace nf {

// MP bot data checks (docs/ai-bots.md): default_bot_stats @0x26d2f0 against the spec dump and the decoded MpData,
// MP_skins @0x26d488 (every skin resolves in every bot-capable arena bank; reports the ones that do not),
// bot_state_types @0x26f460, the weapon ranking table @0x2f3e10 and the heavy-weapon override list, plus
// BotArmoury / stat-rule invariants. Prints a summary and returns the failure count.
std::size_t validate_bots(GameFiles& files, const std::filesystem::path& gamedir);

}  // namespace nf

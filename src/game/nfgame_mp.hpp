#pragma once

#include <array>
#include <filesystem>
#include <string>

#include "game/arena_session.hpp"

namespace nf {

// nfgame's multiplayer mode (`--mp`, `--mode`, `--players`, ...): a match on one of the arena maps with up to four local
// players in split screen. Lives outside nf_game because it draws.
struct MatchLaunch {
    std::filesystem::path gamedir;
    std::string level_bin;                        // 07000024.bin ... (empty: Skyrail)
    MatchOptions options;
    std::string shot;                             // --shot out.bmp: render the split screen once after the scripted frames
    long frames = -1;                             // --frames N: scripted length without input files
    std::array<std::string, 4> inputs;            // --inputs, --inputs2 .. --inputs4: scripted pads per player
    std::string mp_trace;                     // --mp-trace out.jsonl: per-frame oracle-comparable state dump
    std::string mp_seed;                         // --mp-seed recording.jsonl:frame
    bool mp_seed_each = false;                   // --mp-seed-each: reseed one step at a time
    bool collision_wireframe = false;
    // MP bots (--bots N is in `options`): --bot-char a,b,c, --bot-log, --bot-log-states.
    std::string bot_characters;
    bool bot_log = false, bot_log_states = false;
    // Screenshot camera (--cam x y z yaw pitch: game yaw, forward = (sin, cos), pitch up positive; --follow-bot N: behind bot N).
    bool has_cam = false;
    float cam[5] = {0, 0, 0, 0, 0};
    int follow_bot = -1;
};

int run_match(const MatchLaunch& launch);

}  // namespace nf

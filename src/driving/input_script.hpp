#pragma once

#include <string_view>
#include <vector>

#include "game/input.hpp"

namespace nf::driving {

// Scripted controller input for headless runs (`nfdrive --inputs file`). One line per span of ticks:
//
//   <ticks> [lx=<0..255>] [ly=<0..255>] [rx=..] [ry=..] [button ...]      # comment
//
// Buttons: cross square circle triangle l1 l2 r1 r2 l3 r3 start select up down left right. Sticks default
// to centre (0x80). The result holds one PadState per 60 Hz simulation tick. Throws std::runtime_error on
// syntax errors (with the line number).
std::vector<PadState> parse_input_script(std::string_view text);

}  // namespace nf::driving

// The credits roll (P_CREDITS, Menu_SetupCredits): one row per table entry, in order.
// a/b resolve to text (label hash via StringTable, 0x30xxxx via ACTION.ELF .rodata, 0 blank);
// span rows are single centered lines, the rest left/right column pairs. The roll assigns one
// row every 14 ticks and slides every pair up 2px per tick (see Frontend::p_credits).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "assets/elf.hpp"
#include "assets/strings.hpp"

namespace nf {

struct CreditRow {
    std::string left;
    std::string right;  // empty for span rows (and blank spacers)
    bool span = false;
};

std::vector<CreditRow> load_credits(const Elf32& action_elf, const StringTable& strings);

}  // namespace nf

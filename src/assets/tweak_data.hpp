// Developer menu tables from ACTION.ELF: the TWEAKS/TWEAKS2 labels and scroll defaults and
// the NIS sequence list. All values are read from the ELF at load (English strings at their
// .rodata addresses, boot floats at their named symbols); the static tables below only record
// which control reads which address (P_TWEAKS/P_TWEAKS2/C_NIS_Handler).
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "assets/elf.hpp"

namespace nf {

// One C_NIS list row (C_NIS_Handler 0x51): the sequence name at its .rodata address and the script
// hash the row carries (0 = section header). Accept plays a nonzero hash in-engine; the frontend
// reports it through Frontend::take_nis_request().
struct NisEntry {
    std::string name;
    std::uint32_t script = 0;
};

struct TweakData {
    std::map<std::uint32_t, std::string> labels;  // label control -> English caption (TWEAKS pages)
    std::map<std::uint32_t, int> scrolls;         // scroll control -> boot value shown (0x2e)
    std::map<std::uint32_t, float> scales;        // scroll control -> display scale (store divides)
    std::map<std::uint32_t, std::uint32_t> values;  // scroll control -> value readout label (0x50 tick)
    std::vector<NisEntry> nis;                    // C_NIS rows in original order (headers included)
};

TweakData load_tweak_data(const Elf32& action_elf);

}  // namespace nf

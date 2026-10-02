#pragma once

#include <string>

#include "ee/types.hpp"

namespace nf::ee {

// One-line disassembly of an R5900 instruction (MIPS III/IV, MMI, COP0/1/2 macro mode). Branch and jump targets
// are printed as absolute addresses. Unknown encodings print as ".word 0x...".
std::string disassemble(u32 inst, u32 pc);

const char* gpr_name(int i);

}  // namespace nf::ee

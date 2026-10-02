#pragma once

// Basic types shared by the R5900 (PS2 Emotion Engine) interpreter, see docs/ee.md.

#include <cstdint>
#include <stdexcept>
#include <string>

namespace nf::ee {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8 = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;
using u128 = unsigned __int128;

// 128-bit register view (EE GPRs, VU0 VF registers, HI/LO). Element 0 is the least significant.
union Reg128 {
    u64 d[2];
    s64 sd[2];
    u32 w[4];
    s32 sw[4];
    u16 h[8];
    s16 sh[8];
    u8 b[16];
    s8 sb[16];
    float f[4];
};
static_assert(sizeof(Reg128) == 16);

// Anything the interpreter refuses to (or cannot) execute: it is never silently ignored.
enum class TrapKind {
    Unsupported,        // instruction/feature the interpreter does not implement (CALLMS, TLB ops, ...)
    Reserved,           // reserved/undefined encoding (would raise Reserved Instruction on hardware)
    AddressError,       // misaligned access
    BusError,           // unmapped address / hardware register range without a handler
    IntegerOverflow,    // ADD/ADDI/SUB/DADD/... overflow
    Syscall,            // SYSCALL without a handler
    Break,              // BREAK
    ConditionalTrap,    // TGE/TEQ/... fired
    StepLimit,          // Machine::call step budget exhausted
    Other,
};

const char* trap_kind_name(TrapKind k);

class Trap : public std::runtime_error {
public:
    Trap(TrapKind k, std::string msg, u32 fault_addr = 0)
        : std::runtime_error(std::move(msg)), kind(k), addr(fault_addr) {}
    Trap(TrapKind k, std::string full_msg, u32 pc_, u32 inst_, u32 fault_addr)
        : std::runtime_error(std::move(full_msg)), kind(k), addr(fault_addr), pc(pc_), inst(inst_), located(true) {}
    TrapKind kind;
    u32 addr;          // faulting data address, when there is one
    u32 pc = 0;        // instruction that trapped (set by Machine)
    u32 inst = 0;
    bool located = false;
};

}  // namespace nf::ee

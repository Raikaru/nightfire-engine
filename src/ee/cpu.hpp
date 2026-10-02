#pragma once

// R5900 (PS2 EE core) interpreter: MIPS III/IV integer ISA with 64-bit registers, branch-likely, 128-bit GPRs
// (LQ/SQ/MMI), COP0 subset, COP1 FPU with PS2 semantics, COP2 macro mode (Vu0). One instruction per step().

#include <functional>

#include "ee/memory.hpp"
#include "ee/types.hpp"
#include "ee/vu0.hpp"

namespace nf::ee {

struct Cpu {
    explicit Cpu(Memory& memory) : mem(memory) { reset(); }
    void reset();

    Memory& mem;

    Reg128 r[32];                 // GPRs; r[0] is kept at zero
    Reg128 hi, lo;                // HI/LO: element 0 = HI0/LO0, element 1 = HI1/LO1 (MULT1/DIV1/MMI)
    u32 sa = 0;                   // shift amount register (bytes 0..15, MTSAB/MTSAH/QFSRV)
    u32 pc = 0;                   // instruction fetched by the next step()
    u32 npc = 4;                  // instruction after it (differs from pc+4 while a delay slot is pending)
    u32 cur_pc = 0;               // pc of the instruction being executed (valid inside handlers and after a trap)
    u32 cur_inst = 0;

    u32 f[32] = {};               // FPU registers as raw bits
    u32 fpu_acc = 0;              // FPU accumulator
    u32 fcr31 = 0;                // FPU control/status (bit 23 = C)
    u32 cop0[32] = {};
    Vu0 vu0;
    u64 steps = 0;                // instructions executed since reset()

    // SYSCALL handler: return true if the call was handled (execution continues after the syscall).
    std::function<bool(Cpu&, u32 code)> on_syscall;

    // Executes exactly one instruction (including branch bookkeeping). Throws Trap.
    void step();

    // Convenience accessors.
    u64 gpr(int i) const { return r[i].d[0]; }
    void set_gpr(int i, u64 v) {
        if (i) r[i].d[0] = v;
    }
    void set_pc(u32 addr) {
        pc = addr;
        npc = addr + 4;
    }

private:
    [[noreturn]] void trap(TrapKind k, const std::string& msg, u32 addr = 0) const;
    [[noreturn]] void unsupported(const char* what) const;
    void exec_special(u32 inst);
    void exec_regimm(u32 inst);
    void exec_mmi(u32 inst);
    void exec_mmi0(u32 inst);
    void exec_mmi1(u32 inst);
    void exec_mmi2(u32 inst);
    void exec_mmi3(u32 inst);
    void exec_cop0(u32 inst);
    void exec_cop1(u32 inst);
    void exec_cop1_s(u32 inst);
    void exec_cop2(u32 inst);
    void branch(bool taken, u32 target, bool likely);
    void fpu_result_flags(u32 flags, bool track_over_under);
    void pmfhl(u32 inst);
    u64 mfc0(int reg);
    void mtc0(int reg, u32 value);
};

}  // namespace nf::ee

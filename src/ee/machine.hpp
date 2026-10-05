#pragma once

// Headless R5900 harness: loads a PS2 ELF (ACTION.ELF) into EE RAM, calls its
// functions with the ABI the game was compiled with, seeds RAM from PCSX2
// savestates, and traces execution. See docs/ee.md.
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "ee/cpu.hpp"
#include "ee/hostfile.hpp"
#include "ee/types.hpp"
namespace nf::ee {

// One loaded ELF symbol.
struct EeSymbol {
    std::string name;
    u32 value = 0;  // virtual address
    u32 size = 0;
    // True when the address comes only from a .SYM file with no exact ELF
    // entry beneath it (stale link risk): verify with disasm before calling.
    bool approx = false;
};

// Arguments for Machine::call. Integer/pointer arguments fill a0-a3 then the
// o32 stack area; float arguments fill f12, f13, ... in order (hard-float ABI
// as emitted by the game's GCC: AccelFunc0 takes its six floats in f12-f17).
// Mixed functions count each class separately (ints skip no FPU slot).
struct CallArgs {
    std::vector<u64> ints;
    std::vector<u32> floats;  // raw bit patterns
    CallArgs& i(u64 v) {
        ints.push_back(v);
        return *this;
    }
    CallArgs& f(float v) {
        u32 b;
        __builtin_memcpy(&b, &v, 4);
        floats.push_back(b);
        return *this;
    }
    CallArgs& fbits(u32 b) {
        floats.push_back(b);
        return *this;
    }
    CallArgs& reg(int r, u64 v) {
        reg_init[r] = v;
        return *this;
    }
    std::map<int, u64> reg_init; // initial GPR values by MIPS number (applied after a0-a3)
    std::map<int, u32> fpreg_init; // initial FPU regs by number (fragment entry floats like f20/f21)
};

struct CallResult {
    u64 v0 = 0, v1 = 0;  // integer result registers
    u32 f0 = 0;          // float result register (raw bits)
    float f0f() const {
        float v;
        __builtin_memcpy(&v, &f0, 4);
        return v;
    }
};

// A hook runs when execution reaches its address. Return true to skip the
// original function (pc is moved to ra, results come from the registers the
// hook set); return false to execute the instruction normally (e.g. counting).
// The hook may read arguments from cpu.r[4..7] / cpu.f[12..] / RAM.
using Hook = std::function<bool(Cpu& cpu)>;
using InstructionObserver = std::function<void(const Cpu&, u32 pc, u32 inst)>;

class Machine {
public:
    // Loads the ELF image into RAM (PT_LOAD segments, BSS zeroed), installs
    // $gp from the _gp symbol and points $sp at the game's _stack symbol (or
    // the top of RAM when absent). Throws on I/O or format errors.
    explicit Machine(const std::string& elf_path);

    Memory mem;
    Cpu cpu{mem};

    // Symbol tables (from the ELF symtab; LOCAL and GLOBAL kept).
    const std::vector<EeSymbol>& symbols() const { return symbols_; }
    std::optional<EeSymbol> symbol(const std::string& name) const;
    // Smallest symbol containing addr (for traces), if any.
    std::optional<EeSymbol> symbol_at(u32 addr) const;
    u32 addr(const std::string& name) const;  // throws when missing

    // Calls a function: sets up registers/stack, runs until it returns to the
    // sentinel (jr ra), restores registers, rolls RAM back to the pre-call
    // state, and returns v0/v1/f0. Leaf functions only: syscalls, DMA/GIF
    // hardware and TLB ops trap. `step_limit` bounds execution.
    CallResult call(u32 entry, const CallArgs& args = {}, u64 step_limit = 10'000'000);
    CallResult call(const std::string& sym, const CallArgs& args = {}, u64 step_limit = 10'000'000);
    // Like call() but keeps RAM changes (registers are still restored). Used
    // by the CLI so --dump/--poke can observe results.
    CallResult call_keep(u32 entry, const CallArgs& args = {}, u64 step_limit = 10'000'000);
    CallResult call_keep(const std::string& sym, const CallArgs& args = {}, u64 step_limit = 10'000'000);

    // Runs the whole-file static initializer (__static_initialization_and_
    // destruction_0 at 0x1B3488 with a0=1, a1=0xFFFF). RAM changes are KEPT.
    void run_static_init(u64 step_limit = 500'000'000);

    // Merges a linker symbol dump (u32 value, u32 name-offset records plus
    // a trailing string table, e.g. DRIVING/DRIVING.SYM) into the symbol
    // tables so stripped ELFs (DRIVING.ELF) resolve by name too. ELF symtab
    // wins ties. Throws on I/O or format errors.
    void load_sym_file(const std::string& path);
    // Hooks keyed by address.
    void hook(u32 entry, Hook fn) { hooks_[entry] = std::move(fn); }
    bool hook(const std::string& sym, Hook fn);
    // Native memset/memcpy/memmove/strlen at their ELF addresses (fast and
    // exact for byte copies; the interpreter can also execute them).
    void install_libc_hooks();

    // Bump allocator for call arguments/structs (16-byte aligned). Lives below
    // the stack; reset() rewinds it. Rolled back with everything else by call().
    u32 alloc(u32 bytes);
    void reset_alloc() { alloc_ptr_ = kAllocBase; }
    // Struct/vector helpers.
    void write_block(u32 va, const void* p, u32 n) { mem.write_block(va, p, n); }
    void write_f32(u32 va, float v) { mem.write<u32>(va, fbits(v)); }
    void write_vec3(u32 va, float x, float y, float z, float w = 0.0f);
    static u32 fbits(float v) {
        u32 b;
        __builtin_memcpy(&b, &v, 4);
        return b;
    }

    // Replaces RAM (+scratchpad, when present) from a PCSX2 .p2s savestate
    // (zip members eeMemory.bin 32 MB, Scratchpad.bin 16 KB, vu0Memory.bin).
    // Also resets $gp/$sp reasonably; call() state itself is unaffected.
    void load_p2s(const std::string& path);
    // Raw ≤32 MB RAM dump loaded at address 0.
    void load_ram_dump(const std::string& path);
    // Direct RAM access for verification (read-only view of the image).
    std::vector<u8> dump(u32 va, u32 len);
    // Serves disc files from a host directory (see ee/hostfile.hpp): installs
    // open/read/write/lseek/close + sce* hooks backed by root. The HostFs is
    // owned by the Machine so hook captures stay valid.
    void set_fs_root(const std::string& root);

    // Disassembles `count` instructions at `entry` without executing.
    std::string disasm_range(u32 entry, u32 count) const;
    // Executes and prints one line per instruction (pc, word, disasm).
    // Runs until `entry` returns (like call) or `max_steps` is hit.
    void trace(u32 entry, const CallArgs& args, u64 max_steps, FILE* out);
    void trace(const std::string& sym, const CallArgs& args, u64 max_steps, FILE* out);
    // Observes executed instructions inside call()/call_keep() without changing semantics.
    void set_instruction_observer(InstructionObserver observer) {
        instruction_observer_ = std::move(observer);
    }

    static constexpr u32 kStackBase = 0x01FE0000u;  // ACTION.ELF _stack, passed to SetupThread
    static constexpr u32 kStackSize = 0x00020000u;  // ACTION.ELF _stack_size
    static constexpr u32 kStackTop = kStackBase + kStackSize;
    static constexpr u32 kAllocBase = 0x01E00000u;  // scratch region below the stack

private:
    struct SavedRegs {
        Reg128 r[32], hi, lo;
        u32 sa, pc, npc, f[32], fpu_acc, fcr31, cop0[32];
        Reg128 vf[32];
        u32 vi[32];
        Reg128 vacc;
        u8 vumem[4096];
    };
    SavedRegs save_regs() const;
    void restore_regs(const SavedRegs& s);
    CallResult run_until_return(u64 step_limit);
    void setup_call(u32 entry, const CallArgs& args);
    void load_elf(const std::string& path);

    std::vector<EeSymbol> symbols_;
    std::map<u32, EeSymbol> symbols_by_addr_;
    std::map<u32, Hook> hooks_;
    u32 alloc_ptr_ = kAllocBase;
    u32 sentinel_ = 0xFFFFFFF0u;
    InstructionObserver instruction_observer_;
    std::unique_ptr<class HostFs> fs_;
};

}  // namespace nf::ee

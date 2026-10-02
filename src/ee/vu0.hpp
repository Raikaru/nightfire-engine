#pragma once

// VU0 in COP2 macro mode: the vector/integer instructions executed directly by the EE (VADD..VMR32 etc.),
// with the VU float rules from ps2float.hpp. Micro-program execution (VCALLMS/VCALLMSR, CTC2 CMSAR1) is not
// implemented and traps.

#include "ee/types.hpp"

namespace nf::ee {

class Vu0 {
public:
    // Control register indices (CFC2/CTC2 numbering).
    enum : int {
        kStatus = 16, kMac = 17, kClip = 18, kR = 20, kI = 21, kQ = 22, kP = 23,
        kTpc = 26, kCmsar0 = 27, kFbrst = 28, kVpuStat = 29, kCmsar1 = 31,
    };

    Vu0() { reset(); }
    void reset();

    Reg128 vf[32];
    u32 vi[32];            // 0..15 integer registers (16 bit, upper half kept like PCSX2), 16.. control registers
    Reg128 acc;
    alignas(16) u8 mem[4096];   // VU0 data memory (4 KB)

    // COP2 CO=1 instruction (bit 25 set): SPECIAL1 (funct < 0x3C) and SPECIAL2 (funct >= 0x3C).
    void macro(u32 inst);

    u32 cfc2(int idx) const;
    void ctc2(int idx, u32 value);

private:
    enum class Op { Add, Sub, Mul, Madd, Msub };
    void fmac(Op op, u32 inst, int sel, bool to_acc);
    u32 mac_lane(int lane, u32 v, u32 flags);
    void stat_update();
    void unary(u32 inst, int kind);
    void div_family(u32 inst, int kind);
    void min_max(u32 inst, int sel, bool is_max);
    void lower_special2(u32 inst, u32 index);
    u32* mem_ptr(u32 vi_value, u32 inst);
    void set_vi16(int idx, u32 v);
    u32 vi16(int idx) const { return vi[idx & 15] & 0xFFFFu; }
};

}  // namespace nf::ee

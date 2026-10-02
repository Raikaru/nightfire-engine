#include "ee/cpu.hpp"

#include <cstdio>
#include <cstring>

namespace nf::ee {

namespace {

std::string hex(u64 v, int width = 8) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "0x%0*llx", width, static_cast<unsigned long long>(v));
    return buf;
}

constexpr u64 sext32(u32 v) { return u64(s64(s32(v))); }

// Unaligned load/store merge tables (little-endian). Index = address & 3 (words) or & 7 (doublewords).
constexpr u32 kLwlMask[4] = {0x00FFFFFFu, 0x0000FFFFu, 0x000000FFu, 0x00000000u};
constexpr int kLwlShift[4] = {24, 16, 8, 0};
constexpr u32 kLwrMask[4] = {0x00000000u, 0xFF000000u, 0xFFFF0000u, 0xFFFFFF00u};
constexpr int kLwrShift[4] = {0, 8, 16, 24};
constexpr u32 kSwlMask[4] = {0xFFFFFF00u, 0xFFFF0000u, 0xFF000000u, 0x00000000u};
constexpr int kSwlShift[4] = {24, 16, 8, 0};
constexpr u32 kSwrMask[4] = {0x00000000u, 0x000000FFu, 0x0000FFFFu, 0x00FFFFFFu};
constexpr int kSwrShift[4] = {0, 8, 16, 24};

constexpr u64 ldl_mask(int i) { return i == 7 ? 0 : (~u64(0) >> (8 * (i + 1))); }
constexpr u64 ldr_mask(int i) { return i == 0 ? 0 : (~u64(0) << (8 * (8 - i))); }
constexpr u64 sdl_mask(int i) { return i == 7 ? 0 : (~u64(0) << (8 * (i + 1))); }
constexpr u64 sdr_mask(int i) { return i == 0 ? 0 : (~u64(0) >> (8 * (8 - i))); }

}  // namespace

void Cpu::reset() {
    std::memset(r, 0, sizeof r);
    hi.d[0] = hi.d[1] = lo.d[0] = lo.d[1] = 0;
    sa = 0;
    pc = 0;
    npc = 4;
    std::memset(f, 0, sizeof f);
    fpu_acc = 0;
    fcr31 = 0;
    std::memset(cop0, 0, sizeof cop0);
    cop0[12] = 0x70000000u;      // Status: CU0-2 usable, kernel mode, interrupts masked
    cop0[15] = 0x00002E20u;      // PRId (R5900)
    vu0.reset();
    steps = 0;
}

void Cpu::trap(TrapKind k, const std::string& msg, u32 addr) const { throw Trap(k, msg, addr); }

void Cpu::unsupported(const char* what) const {
    throw Trap(TrapKind::Unsupported, std::string(what) + " is not implemented (instruction " + hex(cur_inst) + ")");
}

void Cpu::branch(bool taken, u32 target, bool likely) {
    if (taken) {
        npc = target;
    } else if (likely) {
        pc = npc;                // annul the delay slot
        npc = pc + 4;
    }
}

u64 Cpu::mfc0(int reg) {
    switch (reg) {
        case 9: return sext32(u32(steps));       // Count: one tick per instruction (deterministic)
        case 12: return sext32(cop0[12] & 0xF0C79C1Fu);
        default: return sext32(cop0[reg]);
    }
}

void Cpu::mtc0(int reg, u32 value) {
    if (reg == 9) return;                        // Count is derived from the instruction counter
    if (reg == 15) return;                       // PRId is read-only
    cop0[reg] = value;
}

void Cpu::exec_cop0(u32 inst) {
    const u32 rs = (inst >> 21) & 31, rt = (inst >> 16) & 31, rd = (inst >> 11) & 31;
    switch (rs) {
        case 0:  // MFC0
            if (rt) r[rt].d[0] = mfc0(int(rd));
            return;
        case 4:  // MTC0
            mtc0(int(rd), u32(r[rt].d[0]));
            return;
        case 8: {  // BC0F/BC0T/BC0FL/BC0TL: CPCOND0 = "no DMA transfer pending", always true here
            const u32 t = (inst >> 16) & 3;
            const u32 target = cur_pc + 4 + u32(s32(s16(inst)) << 2);
            branch((t & 1) != 0, target, (t & 2) != 0);
            return;
        }
        case 16:  // CO
            switch (inst & 63) {
                case 0x38: cop0[12] |= (1u << 16); return;        // EI
                case 0x39: cop0[12] &= ~(1u << 16); return;       // DI
                case 0x18: unsupported("ERET");
                case 0x01: case 0x02: case 0x06: case 0x08: unsupported("TLB instruction");
                default: trap(TrapKind::Reserved, "reserved COP0 instruction " + hex(inst));
            }
        default: trap(TrapKind::Reserved, "reserved COP0 instruction " + hex(inst));
    }
}

void Cpu::exec_regimm(u32 inst) {
    const u32 rs = (inst >> 21) & 31, rt = (inst >> 16) & 31;
    const s64 v = s64(r[rs].d[0]);
    const s32 imm = s16(inst);
    const u32 target = cur_pc + 4 + u32(imm << 2);
    switch (rt) {
        case 0: branch(v < 0, target, false); return;                // BLTZ
        case 1: branch(v >= 0, target, false); return;               // BGEZ
        case 2: branch(v < 0, target, true); return;                 // BLTZL
        case 3: branch(v >= 0, target, true); return;                // BGEZL
        case 8: if (v >= s64(imm)) trap(TrapKind::ConditionalTrap, "TGEI fired"); return;
        case 9: if (u64(v) >= u64(s64(imm))) trap(TrapKind::ConditionalTrap, "TGEIU fired"); return;
        case 10: if (v < s64(imm)) trap(TrapKind::ConditionalTrap, "TLTI fired"); return;
        case 11: if (u64(v) < u64(s64(imm))) trap(TrapKind::ConditionalTrap, "TLTIU fired"); return;
        case 12: if (v == s64(imm)) trap(TrapKind::ConditionalTrap, "TEQI fired"); return;
        case 14: if (v != s64(imm)) trap(TrapKind::ConditionalTrap, "TNEI fired"); return;
        case 16: r[31].d[0] = cur_pc + 8; branch(v < 0, target, false); return;   // BLTZAL
        case 17: r[31].d[0] = cur_pc + 8; branch(v >= 0, target, false); return;  // BGEZAL
        case 18: r[31].d[0] = cur_pc + 8; branch(v < 0, target, true); return;    // BLTZALL
        case 19: r[31].d[0] = cur_pc + 8; branch(v >= 0, target, true); return;   // BGEZALL
        case 24: sa = (u32(r[rs].d[0]) & 0xF) ^ (u32(imm) & 0xF); return;         // MTSAB
        case 25: sa = ((u32(r[rs].d[0]) & 0x7) ^ (u32(imm) & 0x7)) << 1; return;  // MTSAH
        default: trap(TrapKind::Reserved, "reserved REGIMM instruction " + hex(inst));
    }
}

void Cpu::exec_special(u32 inst) {
    const u32 rs = (inst >> 21) & 31, rt = (inst >> 16) & 31, rd = (inst >> 11) & 31, shamt = (inst >> 6) & 31;
    const u64 vs = r[rs].d[0], vt = r[rt].d[0];
    auto W = [&](u64 v) {
        if (rd) r[rd].d[0] = v;
    };
    switch (inst & 63) {
        case 0x00: W(sext32(u32(vt) << shamt)); return;                              // SLL
        case 0x02: W(sext32(u32(vt) >> shamt)); return;                              // SRL
        case 0x03: W(sext32(u32(s32(u32(vt)) >> shamt))); return;                    // SRA
        case 0x04: W(sext32(u32(vt) << (vs & 31))); return;                          // SLLV
        case 0x06: W(sext32(u32(vt) >> (vs & 31))); return;                          // SRLV
        case 0x07: W(sext32(u32(s32(u32(vt)) >> (vs & 31)))); return;                // SRAV
        case 0x08: npc = u32(vs); return;                                            // JR
        case 0x09: {                                                                 // JALR
            const u32 target = u32(vs);
            W(cur_pc + 8);
            npc = target;
            return;
        }
        case 0x0A: if (vt == 0) W(vs); return;                                       // MOVZ
        case 0x0B: if (vt != 0) W(vs); return;                                       // MOVN
        case 0x0C: {                                                                 // SYSCALL
            const u32 code = (inst >> 6) & 0xFFFFF;
            if (on_syscall && on_syscall(*this, code)) return;
            trap(TrapKind::Syscall, "SYSCALL (v1 = " + std::to_string(s32(r[3].w[0])) + ", code " + hex(code, 5) + ") has no handler");
        }
        case 0x0D: trap(TrapKind::Break, "BREAK code " + hex((inst >> 6) & 0xFFFFF, 5));
        case 0x0F: return;                                                           // SYNC
        case 0x10: W(hi.d[0]); return;                                               // MFHI
        case 0x11: hi.d[0] = vs; return;                                             // MTHI
        case 0x12: W(lo.d[0]); return;                                               // MFLO
        case 0x13: lo.d[0] = vs; return;                                             // MTLO
        case 0x14: W(vt << (vs & 63)); return;                                       // DSLLV
        case 0x16: W(vt >> (vs & 63)); return;                                       // DSRLV
        case 0x17: W(u64(s64(vt) >> (vs & 63))); return;                             // DSRAV
        case 0x18: {                                                                 // MULT (rd receives LO)
            const s64 p = s64(s32(u32(vs))) * s64(s32(u32(vt)));
            lo.d[0] = sext32(u32(p));
            hi.d[0] = sext32(u32(u64(p) >> 32));
            W(lo.d[0]);
            return;
        }
        case 0x19: {                                                                 // MULTU
            const u64 p = u64(u32(vs)) * u64(u32(vt));
            lo.d[0] = sext32(u32(p));
            hi.d[0] = sext32(u32(p >> 32));
            W(lo.d[0]);
            return;
        }
        case 0x1A: {                                                                 // DIV
            const s32 a = s32(u32(vs)), b = s32(u32(vt));
            if (a == INT32_MIN && b == -1) {
                lo.d[0] = sext32(0x80000000u);
                hi.d[0] = 0;
            } else if (b != 0) {
                lo.d[0] = sext32(u32(a / b));
                hi.d[0] = sext32(u32(a % b));
            } else {
                lo.d[0] = a < 0 ? 1 : ~u64(0);
                hi.d[0] = sext32(u32(a));
            }
            return;
        }
        case 0x1B: {                                                                 // DIVU
            const u32 a = u32(vs), b = u32(vt);
            if (b != 0) {
                lo.d[0] = sext32(a / b);
                hi.d[0] = sext32(a % b);
            } else {
                lo.d[0] = ~u64(0);
                hi.d[0] = sext32(a);
            }
            return;
        }
        case 0x20: {                                                                 // ADD
            s32 res;
            if (__builtin_add_overflow(s32(u32(vs)), s32(u32(vt)), &res)) trap(TrapKind::IntegerOverflow, "ADD overflow");
            W(sext32(u32(res)));
            return;
        }
        case 0x21: W(sext32(u32(vs) + u32(vt))); return;                             // ADDU
        case 0x22: {                                                                 // SUB
            s32 res;
            if (__builtin_sub_overflow(s32(u32(vs)), s32(u32(vt)), &res)) trap(TrapKind::IntegerOverflow, "SUB overflow");
            W(sext32(u32(res)));
            return;
        }
        case 0x23: W(sext32(u32(vs) - u32(vt))); return;                             // SUBU
        case 0x24: W(vs & vt); return;                                               // AND
        case 0x25: W(vs | vt); return;                                               // OR
        case 0x26: W(vs ^ vt); return;                                               // XOR
        case 0x27: W(~(vs | vt)); return;                                            // NOR
        case 0x28: W(sa); return;                                                    // MFSA
        case 0x29: sa = u32(vs) & 0xF; return;                                       // MTSA
        case 0x2A: W(s64(vs) < s64(vt) ? 1 : 0); return;                             // SLT
        case 0x2B: W(vs < vt ? 1 : 0); return;                                       // SLTU
        case 0x2C: {                                                                 // DADD
            s64 res;
            if (__builtin_add_overflow(s64(vs), s64(vt), &res)) trap(TrapKind::IntegerOverflow, "DADD overflow");
            W(u64(res));
            return;
        }
        case 0x2D: W(vs + vt); return;                                               // DADDU
        case 0x2E: {                                                                 // DSUB
            s64 res;
            if (__builtin_sub_overflow(s64(vs), s64(vt), &res)) trap(TrapKind::IntegerOverflow, "DSUB overflow");
            W(u64(res));
            return;
        }
        case 0x2F: W(vs - vt); return;                                               // DSUBU
        case 0x30: if (s64(vs) >= s64(vt)) trap(TrapKind::ConditionalTrap, "TGE fired"); return;
        case 0x31: if (vs >= vt) trap(TrapKind::ConditionalTrap, "TGEU fired"); return;
        case 0x32: if (s64(vs) < s64(vt)) trap(TrapKind::ConditionalTrap, "TLT fired"); return;
        case 0x33: if (vs < vt) trap(TrapKind::ConditionalTrap, "TLTU fired"); return;
        case 0x34: if (vs == vt) trap(TrapKind::ConditionalTrap, "TEQ fired"); return;
        case 0x36: if (vs != vt) trap(TrapKind::ConditionalTrap, "TNE fired"); return;
        case 0x38: W(vt << shamt); return;                                           // DSLL
        case 0x3A: W(vt >> shamt); return;                                           // DSRL
        case 0x3B: W(u64(s64(vt) >> shamt)); return;                                 // DSRA
        case 0x3C: W(vt << (shamt + 32)); return;                                    // DSLL32
        case 0x3E: W(vt >> (shamt + 32)); return;                                    // DSRL32
        case 0x3F: W(u64(s64(vt) >> (shamt + 32))); return;                          // DSRA32
        default: trap(TrapKind::Reserved, "reserved SPECIAL instruction " + hex(inst));
    }
}

void Cpu::exec_cop2(u32 inst) {
    const u32 rs = (inst >> 21) & 31, rt = (inst >> 16) & 31, rd = (inst >> 11) & 31;
    if (inst & (1u << 25)) {
        vu0.macro(inst);
        return;
    }
    switch (rs) {
        case 1:  // QMFC2
            if (rt) r[rt].d[0] = vu0.vf[rd].d[0], r[rt].d[1] = vu0.vf[rd].d[1];
            return;
        case 2: {  // CFC2
            if (!rt) return;
            const u32 v = vu0.cfc2(int(rd));
            r[rt].d[0] = (rd == Vu0::kR) ? u64(v) : sext32(v);
            return;
        }
        case 5:  // QMTC2
            if (rd) vu0.vf[rd].d[0] = r[rt].d[0], vu0.vf[rd].d[1] = r[rt].d[1];
            return;
        case 6:  // CTC2
            vu0.ctc2(int(rd), u32(r[rt].d[0]));
            return;
        case 8: {  // BC2F/BC2T/BC2FL/BC2TL: condition = "VU0 microprogram running" (never true)
            const u32 t = (inst >> 16) & 3;
            const u32 target = cur_pc + 4 + u32(s32(s16(inst)) << 2);
            branch((t & 1) != 0 ? false : true, target, (t & 2) != 0);
            return;
        }
        default: trap(TrapKind::Reserved, "reserved COP2 instruction " + hex(inst));
    }
}

void Cpu::step() {
    const u32 addr = pc;
    const u8* p = (addr & 3) ? nullptr : mem.host(addr, 4);
    if (!p) {
        throw Trap((addr & 3) ? TrapKind::AddressError : TrapKind::BusError,
                   "instruction fetch from " + std::string((addr & 3) ? "misaligned " : "unmapped ") + "address " + hex(addr), addr);
    }
    u32 inst;
    std::memcpy(&inst, p, 4);
    cur_pc = addr;
    cur_inst = inst;
    pc = npc;
    npc = pc + 4;
    ++steps;

    const u32 op = inst >> 26;
    const u32 rs = (inst >> 21) & 31, rt = (inst >> 16) & 31;
    const s32 imm = s16(inst);
    const u32 uimm = inst & 0xFFFF;

    switch (op) {
        case 0x00: exec_special(inst); return;
        case 0x01: exec_regimm(inst); return;
        case 0x02: npc = ((cur_pc + 4) & 0xF0000000u) | ((inst & 0x03FFFFFFu) << 2); return;            // J
        case 0x03:                                                                                          // JAL
            r[31].d[0] = cur_pc + 8;
            npc = ((cur_pc + 4) & 0xF0000000u) | ((inst & 0x03FFFFFFu) << 2);
            return;
        case 0x04: branch(r[rs].d[0] == r[rt].d[0], cur_pc + 4 + u32(imm << 2), false); return;         // BEQ
        case 0x05: branch(r[rs].d[0] != r[rt].d[0], cur_pc + 4 + u32(imm << 2), false); return;         // BNE
        case 0x06: branch(s64(r[rs].d[0]) <= 0, cur_pc + 4 + u32(imm << 2), false); return;             // BLEZ
        case 0x07: branch(s64(r[rs].d[0]) > 0, cur_pc + 4 + u32(imm << 2), false); return;              // BGTZ
        case 0x08: {                                                                                        // ADDI
            s32 res;
            if (__builtin_add_overflow(s32(r[rs].w[0]), imm, &res)) trap(TrapKind::IntegerOverflow, "ADDI overflow");
            if (rt) r[rt].d[0] = sext32(u32(res));
            return;
        }
        case 0x09: if (rt) r[rt].d[0] = sext32(r[rs].w[0] + u32(imm)); return;                           // ADDIU
        case 0x0A: if (rt) r[rt].d[0] = s64(r[rs].d[0]) < s64(imm) ? 1 : 0; return;                     // SLTI
        case 0x0B: if (rt) r[rt].d[0] = r[rs].d[0] < u64(s64(imm)) ? 1 : 0; return;                     // SLTIU
        case 0x0C: if (rt) r[rt].d[0] = r[rs].d[0] & uimm; return;                                       // ANDI
        case 0x0D: if (rt) r[rt].d[0] = r[rs].d[0] | uimm; return;                                       // ORI
        case 0x0E: if (rt) r[rt].d[0] = r[rs].d[0] ^ uimm; return;                                       // XORI
        case 0x0F: if (rt) r[rt].d[0] = sext32(uimm << 16); return;                                      // LUI
        case 0x10: exec_cop0(inst); return;
        case 0x11: exec_cop1(inst); return;
        case 0x12: exec_cop2(inst); return;
        case 0x14: branch(r[rs].d[0] == r[rt].d[0], cur_pc + 4 + u32(imm << 2), true); return;          // BEQL
        case 0x15: branch(r[rs].d[0] != r[rt].d[0], cur_pc + 4 + u32(imm << 2), true); return;          // BNEL
        case 0x16: branch(s64(r[rs].d[0]) <= 0, cur_pc + 4 + u32(imm << 2), true); return;              // BLEZL
        case 0x17: branch(s64(r[rs].d[0]) > 0, cur_pc + 4 + u32(imm << 2), true); return;               // BGTZL
        case 0x18: {                                                                                        // DADDI
            s64 res;
            if (__builtin_add_overflow(s64(r[rs].d[0]), s64(imm), &res)) trap(TrapKind::IntegerOverflow, "DADDI overflow");
            if (rt) r[rt].d[0] = u64(res);
            return;
        }
        case 0x19: if (rt) r[rt].d[0] = r[rs].d[0] + u64(s64(imm)); return;                              // DADDIU
        case 0x1A: {                                                                                        // LDL
            const u32 a = r[rs].w[0] + u32(imm);
            const int i = int(a & 7);
            const u64 m = mem.read<u64>(a & ~7u);
            if (rt) r[rt].d[0] = (r[rt].d[0] & ldl_mask(i)) | (m << (56 - 8 * i));
            return;
        }
        case 0x1B: {                                                                                        // LDR
            const u32 a = r[rs].w[0] + u32(imm);
            const int i = int(a & 7);
            const u64 m = mem.read<u64>(a & ~7u);
            if (rt) r[rt].d[0] = (r[rt].d[0] & ldr_mask(i)) | (m >> (8 * i));
            return;
        }
        case 0x1C: exec_mmi(inst); return;
        case 0x1E: {                                                                                        // LQ
            const u32 a = r[rs].w[0] + u32(imm);
            const Reg128 v = mem.read128(a & ~15u);
            if (rt) r[rt] = v;
            return;
        }
        case 0x1F: mem.write128((r[rs].w[0] + u32(imm)) & ~15u, r[rt]); return;                        // SQ
        case 0x20: { const u8 v = mem.read<u8>(r[rs].w[0] + u32(imm)); if (rt) r[rt].d[0] = u64(s64(s8(v))); return; }    // LB
        case 0x21: { const u16 v = mem.read<u16>(r[rs].w[0] + u32(imm)); if (rt) r[rt].d[0] = u64(s64(s16(v))); return; } // LH
        case 0x22: {                                                                                        // LWL
            const u32 a = r[rs].w[0] + u32(imm);
            const int i = int(a & 3);
            const u32 m = mem.read<u32>(a & ~3u);
            if (rt) r[rt].d[0] = sext32((r[rt].w[0] & kLwlMask[i]) | (m << kLwlShift[i]));
            return;
        }
        case 0x23: { const u32 v = mem.read<u32>(r[rs].w[0] + u32(imm)); if (rt) r[rt].d[0] = sext32(v); return; }         // LW
        case 0x24: { const u8 v = mem.read<u8>(r[rs].w[0] + u32(imm)); if (rt) r[rt].d[0] = v; return; }                    // LBU
        case 0x25: { const u16 v = mem.read<u16>(r[rs].w[0] + u32(imm)); if (rt) r[rt].d[0] = v; return; }                  // LHU
        case 0x26: {                                                                                        // LWR
            const u32 a = r[rs].w[0] + u32(imm);
            const int i = int(a & 3);
            const u32 m = mem.read<u32>(a & ~3u);
            const u32 merged = (r[rt].w[0] & kLwrMask[i]) | (m >> kLwrShift[i]);
            if (rt) {
                if (i == 0) r[rt].d[0] = sext32(merged);
                else r[rt].w[0] = merged;               // partial merge keeps the upper 32 bits
            }
            return;
        }
        case 0x27: { const u32 v = mem.read<u32>(r[rs].w[0] + u32(imm)); if (rt) r[rt].d[0] = v; return; }                  // LWU
        case 0x28: mem.write<u8>(r[rs].w[0] + u32(imm), u8(r[rt].w[0])); return;                        // SB
        case 0x29: mem.write<u16>(r[rs].w[0] + u32(imm), u16(r[rt].w[0])); return;                      // SH
        case 0x2A: {                                                                                        // SWL
            const u32 a = r[rs].w[0] + u32(imm);
            const int i = int(a & 3);
            const u32 m = mem.read<u32>(a & ~3u);
            mem.write<u32>(a & ~3u, (m & kSwlMask[i]) | (r[rt].w[0] >> kSwlShift[i]));
            return;
        }
        case 0x2B: mem.write<u32>(r[rs].w[0] + u32(imm), r[rt].w[0]); return;                           // SW
        case 0x2C: {                                                                                        // SDL
            const u32 a = r[rs].w[0] + u32(imm);
            const int i = int(a & 7);
            const u64 m = mem.read<u64>(a & ~7u);
            mem.write<u64>(a & ~7u, (m & sdl_mask(i)) | (r[rt].d[0] >> (56 - 8 * i)));
            return;
        }
        case 0x2D: {                                                                                        // SDR
            const u32 a = r[rs].w[0] + u32(imm);
            const int i = int(a & 7);
            const u64 m = mem.read<u64>(a & ~7u);
            mem.write<u64>(a & ~7u, (m & sdr_mask(i)) | (r[rt].d[0] << (8 * i)));
            return;
        }
        case 0x2E: {                                                                                        // SWR
            const u32 a = r[rs].w[0] + u32(imm);
            const int i = int(a & 3);
            const u32 m = mem.read<u32>(a & ~3u);
            mem.write<u32>(a & ~3u, (m & kSwrMask[i]) | (r[rt].w[0] << kSwrShift[i]));
            return;
        }
        case 0x2F: return;                                                                                  // CACHE
        case 0x33: return;                                                                                  // PREF
        case 0x31: f[rt] = mem.read<u32>(r[rs].w[0] + u32(imm)); return;                                // LWC1
        case 0x39: mem.write<u32>(r[rs].w[0] + u32(imm), f[rt]); return;                                // SWC1
        case 0x36: {                                                                                        // LQC2
            const Reg128 v = mem.read128(r[rs].w[0] + u32(imm));
            if (rt) vu0.vf[rt] = v;
            return;
        }
        case 0x3E: mem.write128(r[rs].w[0] + u32(imm), vu0.vf[rt]); return;                            // SQC2
        case 0x37: { const u64 v = mem.read<u64>(r[rs].w[0] + u32(imm)); if (rt) r[rt].d[0] = v; return; }                  // LD
        case 0x3F: mem.write<u64>(r[rs].w[0] + u32(imm), r[rt].d[0]); return;                           // SD
        default: trap(TrapKind::Reserved, "reserved opcode " + hex(op, 2) + " (" + hex(inst) + ")");
    }
}

}  // namespace nf::ee

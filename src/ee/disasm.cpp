#include "ee/disasm.hpp"

#include <cstdarg>
#include <cstdio>

namespace nf::ee {

namespace {

const char* const kGpr[32] = {"zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
                              "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"};

std::string fmt(const char* f, ...) {
    char buf[160];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

std::string dest_suffix(u32 inst) {
    std::string s = ".";
    if (inst & (1u << 24)) s += 'x';
    if (inst & (1u << 23)) s += 'y';
    if (inst & (1u << 22)) s += 'z';
    if (inst & (1u << 21)) s += 'w';
    return s;
}

const char* bc_name(u32 i) {
    static const char* const n[6] = {"x", "y", "z", "w", "i", "q"};
    return n[i];
}

std::string vu(u32 inst) {
    const u32 funct = inst & 63;
    const u32 ft = (inst >> 16) & 31, fs = (inst >> 11) & 31, fd = (inst >> 6) & 31;
    const std::string d = dest_suffix(inst);
    auto three = [&](const char* name, const char* suffix) {
        return fmt("v%s%s%s vf%u, vf%u, vf%u", name, suffix, d.c_str(), fd, fs, ft);
    };
    auto bc3 = [&](const char* name, u32 bc) {
        const bool special = bc >= 4;
        return fmt("v%s%s%s vf%u, vf%u, %s", name, bc_name(bc), d.c_str(), fd, fs,
                   special ? (bc == 4 ? "I" : "Q") : ("vf" + std::to_string(ft) + bc_name(bc)).c_str());
    };
    auto acc3 = [&](const char* name, u32 bc) {
        return fmt("v%s%s%s ACC, vf%u, %s", name, bc == 8 ? "" : bc_name(bc), d.c_str(), fs,
                   bc >= 4 ? (bc == 4 ? "I" : (bc == 5 ? "Q" : "vf")) : ("vf" + std::to_string(ft) + bc_name(bc)).c_str());
    };
    if (funct < 0x3C) {
        static const char* const grp[] = {"ADD", "SUB", "MADD", "MSUB", "MAX", "MINI", "MUL"};
        if (funct < 0x1C) return bc3(grp[funct / 4], funct & 3);
        switch (funct) {
            case 0x1C: return bc3("MUL", 5);
            case 0x1D: return bc3("MAX", 4);
            case 0x1E: return bc3("MUL", 4);
            case 0x1F: return bc3("MINI", 4);
            case 0x20: return bc3("ADD", 5);
            case 0x21: return bc3("MADD", 5);
            case 0x22: return bc3("ADD", 4);
            case 0x23: return bc3("MADD", 4);
            case 0x24: return bc3("SUB", 5);
            case 0x25: return bc3("MSUB", 5);
            case 0x26: return bc3("SUB", 4);
            case 0x27: return bc3("MSUB", 4);
            case 0x28: return three("ADD", "");
            case 0x29: return three("MADD", "");
            case 0x2A: return three("MUL", "");
            case 0x2B: return three("MAX", "");
            case 0x2C: return three("SUB", "");
            case 0x2D: return three("MSUB", "");
            case 0x2E: return fmt("vopmsub.xyz vf%u, vf%u, vf%u", fd, fs, ft);
            case 0x2F: return three("MINI", "");
            case 0x30: return fmt("viadd vi%u, vi%u, vi%u", fd & 15, fs & 15, ft & 15);
            case 0x31: return fmt("visub vi%u, vi%u, vi%u", fd & 15, fs & 15, ft & 15);
            case 0x32: return fmt("viaddi vi%u, vi%u, %d", ft & 15, fs & 15, int((inst >> 6) & 0x10 ? int((inst >> 6) & 0x1F) - 32 : int((inst >> 6) & 0x1F)));
            case 0x34: return fmt("viand vi%u, vi%u, vi%u", fd & 15, fs & 15, ft & 15);
            case 0x35: return fmt("vior vi%u, vi%u, vi%u", fd & 15, fs & 15, ft & 15);
            case 0x38: return fmt("vcallms 0x%x", (inst >> 6) & 0x7FFF);
            case 0x39: return "vcallmsr";
            default: break;
        }
        return fmt(".word 0x%08x", inst);
    }
    const u32 idx = (inst & 3) | ((inst >> 4) & 0x7C);
    static const char* const grpa[] = {"ADDA", "SUBA", "MADDA", "MSUBA"};
    if (idx < 0x10) return acc3(grpa[idx / 4] + 1, idx & 3).insert(0, "");   // placeholder replaced below
    return fmt(".word 0x%08x", inst);
}

}  // namespace

const char* gpr_name(int i) { return kGpr[i & 31]; }

std::string disassemble(u32 inst, u32 pc) {
    const u32 op = inst >> 26, rs = (inst >> 21) & 31, rt = (inst >> 16) & 31, rd = (inst >> 11) & 31, sa = (inst >> 6) & 31;
    const int imm = int(std::int16_t(inst));
    const u32 uimm = inst & 0xFFFF;
    const u32 btarget = pc + 4 + u32(imm * 4);
    auto rrr = [&](const char* n) { return fmt("%s %s, %s, %s", n, kGpr[rd], kGpr[rs], kGpr[rt]); };
    auto shift = [&](const char* n) { return fmt("%s %s, %s, %u", n, kGpr[rd], kGpr[rt], sa); };
    auto shiftv = [&](const char* n) { return fmt("%s %s, %s, %s", n, kGpr[rd], kGpr[rt], kGpr[rs]); };
    auto ri = [&](const char* n) { return fmt("%s %s, %s, %d", n, kGpr[rt], kGpr[rs], imm); };
    auto rui = [&](const char* n) { return fmt("%s %s, %s, 0x%x", n, kGpr[rt], kGpr[rs], uimm); };
    auto mem = [&](const char* n) { return fmt("%s %s, %d(%s)", n, kGpr[rt], imm, kGpr[rs]); };
    auto fmem = [&](const char* n) { return fmt("%s f%u, %d(%s)", n, rt, imm, kGpr[rs]); };
    auto br2 = [&](const char* n) { return fmt("%s %s, %s, 0x%08x", n, kGpr[rs], kGpr[rt], btarget); };
    auto br1 = [&](const char* n) { return fmt("%s %s, 0x%08x", n, kGpr[rs], btarget); };

    if (inst == 0) return "nop";
    switch (op) {
        case 0x00:
            switch (inst & 63) {
                case 0x00: return shift("sll");
                case 0x02: return shift("srl");
                case 0x03: return shift("sra");
                case 0x04: return shiftv("sllv");
                case 0x06: return shiftv("srlv");
                case 0x07: return shiftv("srav");
                case 0x08: return fmt("jr %s", kGpr[rs]);
                case 0x09: return rd == 31 ? fmt("jalr %s", kGpr[rs]) : fmt("jalr %s, %s", kGpr[rd], kGpr[rs]);
                case 0x0A: return rrr("movz");
                case 0x0B: return rrr("movn");
                case 0x0C: return fmt("syscall 0x%x", (inst >> 6) & 0xFFFFF);
                case 0x0D: return fmt("break 0x%x", (inst >> 6) & 0xFFFFF);
                case 0x0F: return "sync";
                case 0x10: return fmt("mfhi %s", kGpr[rd]);
                case 0x11: return fmt("mthi %s", kGpr[rs]);
                case 0x12: return fmt("mflo %s", kGpr[rd]);
                case 0x13: return fmt("mtlo %s", kGpr[rs]);
                case 0x14: return shiftv("dsllv");
                case 0x16: return shiftv("dsrlv");
                case 0x17: return shiftv("dsrav");
                case 0x18: return rd ? fmt("mult %s, %s, %s", kGpr[rd], kGpr[rs], kGpr[rt]) : fmt("mult %s, %s", kGpr[rs], kGpr[rt]);
                case 0x19: return rd ? fmt("multu %s, %s, %s", kGpr[rd], kGpr[rs], kGpr[rt]) : fmt("multu %s, %s", kGpr[rs], kGpr[rt]);
                case 0x1A: return fmt("div %s, %s", kGpr[rs], kGpr[rt]);
                case 0x1B: return fmt("divu %s, %s", kGpr[rs], kGpr[rt]);
                case 0x20: return rrr("add");
                case 0x21: return rt == 0 ? fmt("move %s, %s", kGpr[rd], kGpr[rs]) : rrr("addu");
                case 0x22: return rrr("sub");
                case 0x23: return rrr("subu");
                case 0x24: return rrr("and");
                case 0x25: return rt == 0 ? fmt("move %s, %s", kGpr[rd], kGpr[rs]) : rrr("or");
                case 0x26: return rrr("xor");
                case 0x27: return rrr("nor");
                case 0x28: return fmt("mfsa %s", kGpr[rd]);
                case 0x29: return fmt("mtsa %s", kGpr[rs]);
                case 0x2A: return rrr("slt");
                case 0x2B: return rrr("sltu");
                case 0x2C: return rrr("dadd");
                case 0x2D: return rt == 0 ? fmt("move %s, %s", kGpr[rd], kGpr[rs]) : rrr("daddu");
                case 0x2E: return rrr("dsub");
                case 0x2F: return rrr("dsubu");
                case 0x30: return fmt("tge %s, %s", kGpr[rs], kGpr[rt]);
                case 0x31: return fmt("tgeu %s, %s", kGpr[rs], kGpr[rt]);
                case 0x32: return fmt("tlt %s, %s", kGpr[rs], kGpr[rt]);
                case 0x33: return fmt("tltu %s, %s", kGpr[rs], kGpr[rt]);
                case 0x34: return fmt("teq %s, %s", kGpr[rs], kGpr[rt]);
                case 0x36: return fmt("tne %s, %s", kGpr[rs], kGpr[rt]);
                case 0x38: return shift("dsll");
                case 0x3A: return shift("dsrl");
                case 0x3B: return shift("dsra");
                case 0x3C: return shift("dsll32");
                case 0x3E: return shift("dsrl32");
                case 0x3F: return shift("dsra32");
                default: break;
            }
            break;
        case 0x01:
            switch (rt) {
                case 0: return br1("bltz");
                case 1: return br1("bgez");
                case 2: return br1("bltzl");
                case 3: return br1("bgezl");
                case 8: return fmt("tgei %s, %d", kGpr[rs], imm);
                case 9: return fmt("tgeiu %s, %d", kGpr[rs], imm);
                case 10: return fmt("tlti %s, %d", kGpr[rs], imm);
                case 11: return fmt("tltiu %s, %d", kGpr[rs], imm);
                case 12: return fmt("teqi %s, %d", kGpr[rs], imm);
                case 14: return fmt("tnei %s, %d", kGpr[rs], imm);
                case 16: return br1("bltzal");
                case 17: return br1("bgezal");
                case 18: return br1("bltzall");
                case 19: return br1("bgezall");
                case 24: return fmt("mtsab %s, %d", kGpr[rs], imm);
                case 25: return fmt("mtsah %s, %d", kGpr[rs], imm);
                default: break;
            }
            break;
        case 0x02: return fmt("j 0x%08x", ((pc + 4) & 0xF0000000u) | ((inst & 0x03FFFFFFu) << 2));
        case 0x03: return fmt("jal 0x%08x", ((pc + 4) & 0xF0000000u) | ((inst & 0x03FFFFFFu) << 2));
        case 0x04: return rs == 0 && rt == 0 ? fmt("b 0x%08x", btarget) : br2("beq");
        case 0x05: return br2("bne");
        case 0x06: return br1("blez");
        case 0x07: return br1("bgtz");
        case 0x08: return ri("addi");
        case 0x09: return rs == 0 ? fmt("li %s, %d", kGpr[rt], imm) : ri("addiu");
        case 0x0A: return ri("slti");
        case 0x0B: return ri("sltiu");
        case 0x0C: return rui("andi");
        case 0x0D: return rs == 0 ? fmt("li %s, 0x%x", kGpr[rt], uimm) : rui("ori");
        case 0x0E: return rui("xori");
        case 0x0F: return fmt("lui %s, 0x%x", kGpr[rt], uimm);
        case 0x10:
            switch (rs) {
                case 0: return fmt("mfc0 %s, $%u", kGpr[rt], rd);
                case 4: return fmt("mtc0 %s, $%u", kGpr[rt], rd);
                case 8: return fmt("bc0%s 0x%08x", rt == 0 ? "f" : rt == 1 ? "t" : rt == 2 ? "fl" : "tl", btarget);
                case 16:
                    switch (inst & 63) {
                        case 0x18: return "eret";
                        case 0x38: return "ei";
                        case 0x39: return "di";
                        case 0x01: return "tlbr";
                        case 0x02: return "tlbwi";
                        case 0x06: return "tlbwr";
                        case 0x08: return "tlbp";
                        default: break;
                    }
                    break;
                default: break;
            }
            break;
        case 0x11:
            switch (rs) {
                case 0: return fmt("mfc1 %s, f%u", kGpr[rt], rd);
                case 2: return fmt("cfc1 %s, $%u", kGpr[rt], rd);
                case 4: return fmt("mtc1 %s, f%u", kGpr[rt], rd);
                case 6: return fmt("ctc1 %s, $%u", kGpr[rt], rd);
                case 8: return fmt("bc1%s 0x%08x", rt == 0 ? "f" : rt == 1 ? "t" : rt == 2 ? "fl" : "tl", btarget);
                case 20: if ((inst & 63) == 32) return fmt("cvt.s.w f%u, f%u", sa, rd); break;
                case 16: {
                    const u32 ft = rt, fs = rd, fd = sa;
                    auto d3 = [&](const char* n) { return fmt("%s f%u, f%u, f%u", n, fd, fs, ft); };
                    auto d2 = [&](const char* n) { return fmt("%s f%u, f%u", n, fd, fs); };
                    auto a2 = [&](const char* n) { return fmt("%s f%u, f%u", n, fs, ft); };
                    switch (inst & 63) {
                        case 0x00: return d3("add.s");
                        case 0x01: return d3("sub.s");
                        case 0x02: return d3("mul.s");
                        case 0x03: return d3("div.s");
                        case 0x04: return fmt("sqrt.s f%u, f%u", fd, ft);
                        case 0x05: return d2("abs.s");
                        case 0x06: return d2("mov.s");
                        case 0x07: return d2("neg.s");
                        case 0x16: return d3("rsqrt.s");
                        case 0x18: return a2("adda.s");
                        case 0x19: return a2("suba.s");
                        case 0x1A: return a2("mula.s");
                        case 0x1C: return d3("madd.s");
                        case 0x1D: return d3("msub.s");
                        case 0x1E: return a2("madda.s");
                        case 0x1F: return a2("msuba.s");
                        case 0x24: return d2("cvt.w.s");
                        case 0x28: return d3("max.s");
                        case 0x29: return d3("min.s");
                        case 0x30: return a2("c.f.s");
                        case 0x32: return a2("c.eq.s");
                        case 0x34: return a2("c.lt.s");
                        case 0x36: return a2("c.le.s");
                        default: break;
                    }
                    break;
                }
                default: break;
            }
            break;
        case 0x12:
            switch (rs) {
                case 1: return fmt("qmfc2 %s, vf%u", kGpr[rt], rd);
                case 2: return fmt("cfc2 %s, vi%u", kGpr[rt], rd);
                case 5: return fmt("qmtc2 %s, vf%u", kGpr[rt], rd);
                case 6: return fmt("ctc2 %s, vi%u", kGpr[rt], rd);
                case 8: return fmt("bc2%s 0x%08x", rt == 0 ? "f" : rt == 1 ? "t" : rt == 2 ? "fl" : "tl", btarget);
                default: break;
            }
            if (inst & (1u << 25)) return vu(inst);
            break;
        case 0x14: return br2("beql");
        case 0x15: return br2("bnel");
        case 0x16: return br1("blezl");
        case 0x17: return br1("bgtzl");
        case 0x18: return ri("daddi");
        case 0x19: return ri("daddiu");
        case 0x1A: return mem("ldl");
        case 0x1B: return mem("ldr");
        case 0x1C: {
            const u32 funct = inst & 63;
            switch (funct) {
                case 0x00: return fmt("madd %s, %s, %s", kGpr[rd], kGpr[rs], kGpr[rt]);
                case 0x01: return fmt("maddu %s, %s, %s", kGpr[rd], kGpr[rs], kGpr[rt]);
                case 0x04: return fmt("plzcw %s, %s", kGpr[rd], kGpr[rs]);
                case 0x10: return fmt("mfhi1 %s", kGpr[rd]);
                case 0x11: return fmt("mthi1 %s", kGpr[rs]);
                case 0x12: return fmt("mflo1 %s", kGpr[rd]);
                case 0x13: return fmt("mtlo1 %s", kGpr[rs]);
                case 0x18: return fmt("mult1 %s, %s, %s", kGpr[rd], kGpr[rs], kGpr[rt]);
                case 0x19: return fmt("multu1 %s, %s, %s", kGpr[rd], kGpr[rs], kGpr[rt]);
                case 0x1A: return fmt("div1 %s, %s", kGpr[rs], kGpr[rt]);
                case 0x1B: return fmt("divu1 %s, %s", kGpr[rs], kGpr[rt]);
                case 0x20: return fmt("madd1 %s, %s, %s", kGpr[rd], kGpr[rs], kGpr[rt]);
                case 0x21: return fmt("maddu1 %s, %s, %s", kGpr[rd], kGpr[rs], kGpr[rt]);
                case 0x30: {
                    static const char* const n[] = {"lw", "uw", "slw", "lh", "sh"};
                    return sa < 5 ? fmt("pmfhl.%s %s", n[sa], kGpr[rd]) : fmt(".word 0x%08x", inst);
                }
                case 0x31: return fmt("pmthl.lw %s", kGpr[rs]);
                case 0x34: return shift("psllh");
                case 0x36: return shift("psrlh");
                case 0x37: return shift("psrah");
                case 0x3C: return shift("psllw");
                case 0x3E: return shift("psrlw");
                case 0x3F: return shift("psraw");
                default: break;
            }
            const char* name = nullptr;
            if (funct == 0x08) {
                static const char* const t[32] = {"paddw", "psubw", "pcgtw", "pmaxw", "paddh", "psubh", "pcgth", "pmaxh", "paddb", "psubb", "pcgtb", nullptr,
                                                  nullptr, nullptr, nullptr, nullptr, "paddsw", "psubsw", "pextlw", "ppacw", "paddsh", "psubsh", "pextlh", "ppach",
                                                  "paddsb", "psubsb", "pextlb", "ppacb", nullptr, nullptr, "pext5", "ppac5"};
                name = t[sa];
            } else if (funct == 0x28) {
                static const char* const t[32] = {nullptr, "pabsw", "pceqw", "pminw", "padsbh", "pabsh", "pceqh", "pminh", nullptr, nullptr, "pceqb", nullptr,
                                                  nullptr, nullptr, nullptr, nullptr, "padduw", "psubuw", "pextuw", nullptr, "padduh", "psubuh", "pextuh", nullptr,
                                                  "paddub", "psubub", "pextub", "qfsrv", nullptr, nullptr, nullptr, nullptr};
                name = t[sa];
            } else if (funct == 0x09) {
                static const char* const t[32] = {"pmaddw", nullptr, "psllvw", "psrlvw", "pmsubw", nullptr, nullptr, nullptr, "pmfhi", "pmflo", "pinth", nullptr,
                                                  "pmultw", "pdivw", "pcpyld", nullptr, "pmaddh", "phmadh", "pand", "pxor", "pmsubh", "phmsbh", nullptr, nullptr,
                                                  nullptr, nullptr, "pexeh", "prevh", "pmulth", "pdivbw", "pexew", "prot3w"};
                name = t[sa];
            } else if (funct == 0x29) {
                static const char* const t[32] = {"pmadduw", nullptr, nullptr, "psravw", nullptr, nullptr, nullptr, nullptr, "pmthi", "pmtlo", "pinteh", nullptr,
                                                  "pmultuw", "pdivuw", "pcpyud", nullptr, nullptr, nullptr, "por", "pnor", nullptr, nullptr, nullptr, nullptr,
                                                  nullptr, nullptr, "pexch", "pcpyh", nullptr, nullptr, "pexcw", nullptr};
                name = t[sa];
            }
            if (!name) break;
            const std::string n = name;
            if (n == "pmfhi" || n == "pmflo") return fmt("%s %s", name, kGpr[rd]);
            if (n == "pmthi" || n == "pmtlo") return fmt("%s %s", name, kGpr[rs]);
            if (n == "pdivw" || n == "pdivuw" || n == "pdivbw") return fmt("%s %s, %s", name, kGpr[rs], kGpr[rt]);
            if (n == "pabsw" || n == "pabsh" || n == "pext5" || n == "ppac5" || n == "pexeh" || n == "prevh" || n == "pexew" || n == "prot3w" ||
                n == "pexch" || n == "pexcw" || n == "pcpyh")
                return fmt("%s %s, %s", name, kGpr[rd], kGpr[rt]);
            return fmt("%s %s, %s, %s", name, kGpr[rd], kGpr[rs], kGpr[rt]);
        }
        case 0x1E: return fmt("lq %s, %d(%s)", kGpr[rt], imm, kGpr[rs]);
        case 0x1F: return fmt("sq %s, %d(%s)", kGpr[rt], imm, kGpr[rs]);
        case 0x20: return mem("lb");
        case 0x21: return mem("lh");
        case 0x22: return mem("lwl");
        case 0x23: return mem("lw");
        case 0x24: return mem("lbu");
        case 0x25: return mem("lhu");
        case 0x26: return mem("lwr");
        case 0x27: return mem("lwu");
        case 0x28: return mem("sb");
        case 0x29: return mem("sh");
        case 0x2A: return mem("swl");
        case 0x2B: return mem("sw");
        case 0x2C: return mem("sdl");
        case 0x2D: return mem("sdr");
        case 0x2E: return mem("swr");
        case 0x2F: return fmt("cache 0x%x, %d(%s)", rt, imm, kGpr[rs]);
        case 0x31: return fmem("lwc1");
        case 0x33: return fmt("pref 0x%x, %d(%s)", rt, imm, kGpr[rs]);
        case 0x36: return fmt("lqc2 vf%u, %d(%s)", rt, imm, kGpr[rs]);
        case 0x37: return mem("ld");
        case 0x39: return fmem("swc1");
        case 0x3E: return fmt("sqc2 vf%u, %d(%s)", rt, imm, kGpr[rs]);
        case 0x3F: return mem("sd");
        default: break;
    }
    return fmt(".word 0x%08x", inst);
}

}  // namespace nf::ee

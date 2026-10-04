#include "ee/vu0.hpp"

#include <cstdio>
#include <cstring>

#include "ee/ps2float.hpp"

namespace nf::ee {

namespace {

constexpr int kBcI = 4, kBcQ = 5, kBcNone = -1;

std::string hex32(u32 v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%08x", v);
    return buf;
}

bool lane_on(u32 inst, int lane) { return (inst >> (24 - lane)) & 1; }

}  // namespace

void Vu0::reset() {
    std::memset(vf, 0, sizeof vf);
    std::memset(vi, 0, sizeof vi);
    std::memset(mem, 0, sizeof mem);
    acc.d[0] = acc.d[1] = 0;
    vf[0].w[3] = 0x3F800000u;
}

void Vu0::set_vi16(int idx, u32 v) {
    idx &= 15;
    if (idx == 0) return;
    vi[idx] = (vi[idx] & 0xFFFF0000u) | (v & 0xFFFFu);
}

u32 Vu0::cfc2(int idx) const {
    if (idx == kR) return vi[kR] & 0x7FFFFFu;
    return vi[idx & 31];
}

void Vu0::ctc2(int idx, u32 v) {
    idx &= 31;
    if (idx == 0) return;
    switch (idx) {
        case kMac:
        case kTpc:
        case kVpuStat: return;                          // read-only
        case kR: vi[kR] = (v & 0x7FFFFFu) | 0x3F800000u; return;
        case kFbrst:
            vi[kFbrst] = v & 0x0C0C;
            if (v & 2) {                                // VU0 reset
                const u32 fbrst = vi[kFbrst];
                reset();
                vi[kFbrst] = fbrst;
            }
            return;
        case kCmsar1:
            throw Trap(TrapKind::Unsupported, "CTC2 to VI31 (CMSAR1) would start a VU1 microprogram; VU1 is not emulated");
        default: vi[idx] = v; return;
    }
}

u32 Vu0::mac_lane(int lane, u32 v, u32 fl) {
    const int shift = 3 - lane;
    u32& mac = vi[kMac];
    if (v & 0x80000000u) mac |= 0x10u << shift;
    else mac &= ~(0x10u << shift);
    if (fl & fp::kUnf) mac = (mac & ~(0x1000u << shift)) | (0x0101u << shift);
    else if (fl & fp::kOvf) mac = (mac & ~(0x0101u << shift)) | (0x1000u << shift);
    else if ((v & 0x7FFFFFFFu) == 0) mac = (mac & ~(0x1100u << shift)) | (0x0001u << shift);
    else mac &= ~(0x1101u << shift);
    return v;
}

void Vu0::stat_update() {
    const u32 mac = vi[kMac];
    u32 n = 0;
    if (mac & 0x000F) n |= 1;
    if (mac & 0x00F0) n |= 2;
    if (mac & 0x0F00) n |= 4;
    if (mac & 0xF000) n |= 8;
    vi[kStatus] = (vi[kStatus] & 0xFF0u) | n | (n << 6);
}

void Vu0::fmac(Op op, u32 inst, int sel, bool to_acc) {
    const int fs = (inst >> 11) & 31, ft = (inst >> 16) & 31, fd = (inst >> 6) & 31;
    u32 bcv = 0;
    if (sel >= 0 && sel < 4) bcv = vf[ft].w[sel];
    else if (sel == kBcI) bcv = vi[kI];
    else if (sel == kBcQ) bcv = vi[kQ];
    const Reg128 s = vf[fs], t = vf[ft];
    for (int lane = 0; lane < 4; ++lane) {
        if (!lane_on(inst, lane)) {
            vi[kMac] &= ~(0x1111u << (3 - lane));
            continue;
        }
        const u32 a = s.w[lane];
        const u32 b = sel == kBcNone ? t.w[lane] : bcv;
        u32 fl = 0, v;
        switch (op) {
            case Op::Add: v = fp::vu_add(a, b, fl); break;
            case Op::Sub: v = fp::vu_sub(a, b, fl); break;
            case Op::Mul: v = fp::mul(a, b, fl); break;
            case Op::Madd: {
                u32 ignore = 0;
                v = fp::vu_add(acc.w[lane], fp::mul(a, b, ignore), fl);
                break;
            }
            default: {
                u32 ignore = 0;
                v = fp::vu_sub(acc.w[lane], fp::mul(a, b, ignore), fl);
                break;
            }
        }
        v = mac_lane(lane, v, fl);
        if (to_acc) acc.w[lane] = v;
        else if (fd) vf[fd].w[lane] = v;
    }
    stat_update();
}

void Vu0::min_max(u32 inst, int sel, bool is_max) {
    const int fs = (inst >> 11) & 31, ft = (inst >> 16) & 31, fd = (inst >> 6) & 31;
    if (fd == 0) return;
    for (int lane = 0; lane < 4; ++lane) {
        if (!lane_on(inst, lane)) continue;
        u32 b;
        if (sel == kBcNone) b = vf[ft].w[lane];
        else if (sel == kBcI) b = vi[kI];
        else b = vf[ft].w[sel];
        const u32 a = vf[fs].w[lane];
        vf[fd].w[lane] = is_max ? fp::max_bits(a, b) : fp::min_bits(a, b);
    }
}

// kind: 0 ITOF0, 1 ITOF4, 2 ITOF12, 3 ITOF15, 4 FTOI0, 5 FTOI4, 6 FTOI12, 7 FTOI15, 8 ABS
void Vu0::unary(u32 inst, int kind) {
    const int fs = (inst >> 11) & 31, ft = (inst >> 16) & 31;
    if (ft == 0) return;
    static constexpr int shifts[4] = {0, 4, 12, 15};
    for (int lane = 0; lane < 4; ++lane) {
        if (!lane_on(inst, lane)) continue;
        const u32 x = vf[fs].w[lane];
        u32 r;
        if (kind == 8) {
            r = x & 0x7FFFFFFFu;
        } else if (kind < 4) {
            r = fp::from_i32(s32(x));
            if (r & 0x7FFFFFFFu) r -= u32(shifts[kind]) << 23;
        } else {
            const int n = shifts[kind - 4];
            const u32 e = (x >> 23) & 0xFF;
            if (e == 0) r = 0;
            else if (e == 255 || e + u32(n) > 254) r = (x & 0x80000000u) ? 0x80000000u : 0x7FFFFFFFu;
            else r = u32(fp::to_i32((x & 0x807FFFFFu) | ((e + u32(n)) << 23)));
        }
        vf[ft].w[lane] = r;
    }
}

// kind: 0 DIV, 1 SQRT, 2 RSQRT
void Vu0::div_family(u32 inst, int kind) {
    const int fs = (inst >> 11) & 31, ft = (inst >> 16) & 31;
    const u32 fsf = (inst >> 21) & 3, ftf = (inst >> 23) & 3;
    const u32 rs = vf[fs].w[fsf], rt = vf[ft].w[ftf];
    const u32 s = fp::in(rs), t = fp::in(rt);
    const bool t_zero = (t & 0x7FFFFFFFu) == 0, s_zero = (s & 0x7FFFFFFFu) == 0;
    const u32 sign_xor = (rt ^ rs) & 0x80000000u;
    u32 flags = 0, q;
    u32 fl = 0;
    switch (kind) {
        case 0:
            if (t_zero) {
                flags = s_zero ? 0x10u : 0x20u;
                q = sign_xor ? 0xFF7FFFFFu : 0x7F7FFFFFu;
            } else {
                q = fp::div(s, t, fl);
            }
            break;
        case 1:
            if (t & 0x80000000u) flags = 0x10u;
            q = fp::sqrt_abs(t);
            break;
        default:
            if (t & 0x80000000u) flags = 0x10u;
            q = fp::sqrt_abs(t);
            if (t_zero) {
                flags = s_zero ? 0x10u : 0x20u;
                q = (s & 0x80000000u) | 0x7F7FFFFFu;
            } else {
                q = fp::div(s, q, fl);
            }
            break;
    }
    vi[kQ] = q;
    vi[kStatus] = (vi[kStatus] & 0xFCFu) | flags | (flags << 6);
}

u32* Vu0::mem_ptr(u32 vi_value, u32 inst) {
    const u32 addr = (vi_value * 16u) & 0xFFFFu;
    if (addr & 0x4000u)
        throw Trap(TrapKind::Unsupported, "VU0 memory access to the VU1 register window (0x4000+) in " + hex32(inst));
    return reinterpret_cast<u32*>(mem + (addr & 0xFFF));
}

void Vu0::lower_special2(u32 inst, u32 index) {
    const int fs = (inst >> 11) & 31, ft = (inst >> 16) & 31;
    const int is = fs & 15, it = ft & 15;
    const u32 fsf = (inst >> 21) & 3;
    switch (index) {
        case 0x30:  // VMOVE
            if (ft) for (int l = 0; l < 4; ++l) if (lane_on(inst, l)) vf[ft].w[l] = vf[fs].w[l];
            return;
        case 0x31: {  // VMR32
            if (!ft) return;
            const Reg128 s = vf[fs];
            const u32 rot[4] = {s.w[1], s.w[2], s.w[3], s.w[0]};
            for (int l = 0; l < 4; ++l) if (lane_on(inst, l)) vf[ft].w[l] = rot[l];
            return;
        }
        case 0x34: {  // VLQI
            const u32* p = mem_ptr(vi16(is), inst);
            if (ft) for (int l = 0; l < 4; ++l) if (lane_on(inst, l)) vf[ft].w[l] = p[l];
            if (fs) set_vi16(is, vi16(is) + 1);
            return;
        }
        case 0x35: {  // VSQI
            u32* p = mem_ptr(vi16(it), inst);
            for (int l = 0; l < 4; ++l) if (lane_on(inst, l)) p[l] = vf[fs].w[l];
            if (ft) set_vi16(it, vi16(it) + 1);
            return;
        }
        case 0x36: {  // VLQD
            if (is) set_vi16(is, vi16(is) - 1);
            if (!ft) return;
            const u32* p = mem_ptr(vi16(is), inst);
            for (int l = 0; l < 4; ++l) if (lane_on(inst, l)) vf[ft].w[l] = p[l];
            return;
        }
        case 0x37: {  // VSQD
            if (ft) set_vi16(it, vi16(it) - 1);
            u32* p = mem_ptr(vi16(it), inst);
            for (int l = 0; l < 4; ++l) if (lane_on(inst, l)) p[l] = vf[fs].w[l];
            return;
        }
        case 0x38: div_family(inst, 0); return;   // VDIV
        case 0x39: div_family(inst, 1); return;   // VSQRT
        case 0x3A: div_family(inst, 2); return;   // VRSQRT
        case 0x3B: return;                        // VWAITQ: results are available immediately
        case 0x3C:                                // VMTIR
            set_vi16(it, vf[fs].w[fsf] & 0xFFFFu);
            return;
        case 0x3D:                                // VMFIR
            if (ft) for (int l = 0; l < 4; ++l) if (lane_on(inst, l)) vf[ft].sw[l] = s32(s16(vi16(is)));
            return;
        case 0x3E: {                              // VILWR
            if (!it) return;
            const u16* p = reinterpret_cast<const u16*>(mem_ptr(vi16(is), inst));
            for (int l = 0; l < 4; ++l) if (lane_on(inst, l)) set_vi16(it, p[l * 2]);
            return;
        }
        case 0x3F: {                              // VISWR
            u16* p = reinterpret_cast<u16*>(mem_ptr(vi16(is), inst));
            for (int l = 0; l < 4; ++l)
                if (lane_on(inst, l)) {
                    p[l * 2] = u16(vi16(it));
                    p[l * 2 + 1] = 0;
                }
            return;
        }
        case 0x40:                                // VRNEXT
        case 0x41: {                              // VRGET
            if (index == 0x40) {
                const u32 r = vi[kR];
                const u32 x = (r >> 4) & 1, y = (r >> 22) & 1;
                vi[kR] = (((r << 1) ^ x ^ y) & 0x7FFFFFu) | 0x3F800000u;
            }
            if (ft) for (int l = 0; l < 4; ++l) if (lane_on(inst, l)) vf[ft].w[l] = vi[kR];
            return;
        }
        case 0x42:                                // VRINIT
            vi[kR] = 0x3F800000u | (vf[fs].w[fsf] & 0x7FFFFFu);
            return;
        case 0x43:                                // VRXOR
            vi[kR] = 0x3F800000u | ((vi[kR] ^ vf[fs].w[fsf]) & 0x7FFFFFu);
            return;
        default:
            throw Trap(TrapKind::Unsupported, "COP2 SPECIAL2 op " + hex32(index) + " (" + hex32(inst) + ") not present on VU0 macro mode");
    }
}

void Vu0::macro(u32 inst) {
    const u32 funct = inst & 63;
    const int fs = (inst >> 11) & 31, ft = (inst >> 16) & 31, fd = (inst >> 6) & 31;

    if (funct < 0x3C) {
        if (funct < 0x04) return fmac(Op::Add, inst, int(funct & 3), false);
        if (funct < 0x08) return fmac(Op::Sub, inst, int(funct & 3), false);
        if (funct < 0x0C) return fmac(Op::Madd, inst, int(funct & 3), false);
        if (funct < 0x10) return fmac(Op::Msub, inst, int(funct & 3), false);
        if (funct < 0x14) return min_max(inst, int(funct & 3), true);
        if (funct < 0x18) return min_max(inst, int(funct & 3), false);
        if (funct < 0x1C) return fmac(Op::Mul, inst, int(funct & 3), false);
        switch (funct) {
            case 0x1C: return fmac(Op::Mul, inst, kBcQ, false);
            case 0x1D: return min_max(inst, kBcI, true);
            case 0x1E: return fmac(Op::Mul, inst, kBcI, false);
            case 0x1F: return min_max(inst, kBcI, false);
            case 0x20: return fmac(Op::Add, inst, kBcQ, false);
            case 0x21: return fmac(Op::Madd, inst, kBcQ, false);
            case 0x22: return fmac(Op::Add, inst, kBcI, false);
            case 0x23: return fmac(Op::Madd, inst, kBcI, false);
            case 0x24: return fmac(Op::Sub, inst, kBcQ, false);
            case 0x25: return fmac(Op::Msub, inst, kBcQ, false);
            case 0x26: return fmac(Op::Sub, inst, kBcI, false);
            case 0x27: return fmac(Op::Msub, inst, kBcI, false);
            case 0x28: return fmac(Op::Add, inst, kBcNone, false);
            case 0x29: return fmac(Op::Madd, inst, kBcNone, false);
            case 0x2A: return fmac(Op::Mul, inst, kBcNone, false);
            case 0x2B: return min_max(inst, kBcNone, true);
            case 0x2C: return fmac(Op::Sub, inst, kBcNone, false);
            case 0x2D: return fmac(Op::Msub, inst, kBcNone, false);
            case 0x2E: {  // VOPMSUB: Fd.xyz = ACC.xyz - (Fs.yzx * Ft.zxy)
                const Reg128 s = vf[fs], t = vf[ft];
                u32 fl[3] = {0, 0, 0}, ig = 0;
                const u32 r[3] = {fp::vu_sub(acc.w[0], fp::mul(s.w[1], t.w[2], ig), fl[0]),
                                  fp::vu_sub(acc.w[1], fp::mul(s.w[2], t.w[0], ig), fl[1]),
                                  fp::vu_sub(acc.w[2], fp::mul(s.w[0], t.w[1], ig), fl[2])};
                for (int l = 0; l < 3; ++l) {
                    const u32 v = mac_lane(l, r[l], fl[l]);
                    if (fd) vf[fd].w[l] = v;
                }
                stat_update();
                return;
            }
            case 0x2F: return min_max(inst, kBcNone, false);
            case 0x30: set_vi16(fd & 15, vi16(fs & 15) + vi16(ft & 15)); return;            // VIADD
            case 0x31: set_vi16(fd & 15, vi16(fs & 15) - vi16(ft & 15)); return;            // VISUB
            case 0x32: {                                                                     // VIADDI
                u32 imm = (inst >> 6) & 0x1F;
                imm = (imm & 0x10 ? 0xFFF0u : 0u) | (imm & 0xF);
                set_vi16(ft & 15, vi16(fs & 15) + imm);
                return;
            }
            case 0x34: set_vi16(fd & 15, vi16(fs & 15) & vi16(ft & 15)); return;            // VIAND
            case 0x35: set_vi16(fd & 15, vi16(fs & 15) | vi16(ft & 15)); return;            // VIOR
            case 0x38:
            case 0x39:
                throw Trap(TrapKind::Unsupported, std::string(funct == 0x38 ? "VCALLMS" : "VCALLMSR") +
                                                      ": VU0 microprogram execution is not emulated (" + hex32(inst) + ")");
            default:
                throw Trap(TrapKind::Reserved, "reserved COP2 SPECIAL1 encoding " + hex32(inst));
        }
    }

    const u32 index = (inst & 3) | ((inst >> 4) & 0x7C);
    if (index < 0x04) return fmac(Op::Add, inst, int(index & 3), true);
    if (index < 0x08) return fmac(Op::Sub, inst, int(index & 3), true);
    if (index < 0x0C) return fmac(Op::Madd, inst, int(index & 3), true);
    if (index < 0x10) return fmac(Op::Msub, inst, int(index & 3), true);
    if (index < 0x18) return unary(inst, int(index - 0x10));          // ITOF0/4/12/15, FTOI0/4/12/15
    if (index < 0x1C) return fmac(Op::Mul, inst, int(index & 3), true);
    switch (index) {
        case 0x1C: return fmac(Op::Mul, inst, kBcQ, true);
        case 0x1D: return unary(inst, 8);                                // VABS
        case 0x1E: return fmac(Op::Mul, inst, kBcI, true);
        case 0x1F: {                                                     // VCLIPw
            s32 value = s32(vf[ft].w[3]);
            value = (value & 0x7F800000) ? (value & 0x7FFFFFFF) : 0x007FFFFF;
            u32 clip = vi[kClip] << 6;
            static constexpr u32 kNeg = 0x80000000u;
            const u32 x = vf[fs].w[0], y = vf[fs].w[1], z = vf[fs].w[2];
            if (s32(x) > value) clip |= 0x01;
            if (s32(x ^ kNeg) > value) clip |= 0x02;
            if (s32(y) > value) clip |= 0x04;
            if (s32(y ^ kNeg) > value) clip |= 0x08;
            if (s32(z) > value) clip |= 0x10;
            if (s32(z ^ kNeg) > value) clip |= 0x20;
            vi[kClip] = clip & 0xFFFFFFu;
            return;
        }
        case 0x20: return fmac(Op::Add, inst, kBcQ, true);
        case 0x21: return fmac(Op::Madd, inst, kBcQ, true);
        case 0x22: return fmac(Op::Add, inst, kBcI, true);
        case 0x23: return fmac(Op::Madd, inst, kBcI, true);
        case 0x24: return fmac(Op::Sub, inst, kBcQ, true);
        case 0x25: return fmac(Op::Msub, inst, kBcQ, true);
        case 0x26: return fmac(Op::Sub, inst, kBcI, true);
        case 0x27: return fmac(Op::Msub, inst, kBcI, true);
        case 0x28: return fmac(Op::Add, inst, kBcNone, true);
        case 0x29: return fmac(Op::Madd, inst, kBcNone, true);
        case 0x2A: return fmac(Op::Mul, inst, kBcNone, true);
        case 0x2C: return fmac(Op::Sub, inst, kBcNone, true);
        case 0x2D: return fmac(Op::Msub, inst, kBcNone, true);
        case 0x2E: {  // VOPMULA: ACC.xyz = Fs.yzx * Ft.zxy
            const Reg128 s = vf[fs], t = vf[ft];
            u32 fl[3] = {0, 0, 0};
            const u32 r[3] = {fp::mul(s.w[1], t.w[2], fl[0]), fp::mul(s.w[2], t.w[0], fl[1]), fp::mul(s.w[0], t.w[1], fl[2])};
            for (int l = 0; l < 3; ++l) acc.w[l] = mac_lane(l, r[l], fl[l]);
            stat_update();
            return;
        }
        case 0x2F: return;                                               // VNOP
        default: return lower_special2(inst, index);
    }
}

}  // namespace nf::ee

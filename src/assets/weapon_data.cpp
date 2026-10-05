#include "assets/weapon_data.hpp"

#include <cstring>
#include <string>
#include <unordered_map>

#include "assets/reader.hpp"

namespace nf {

namespace {

// A small MIPS/R5900 interpreter: exactly the instruction subset the compiler emitted for the weapon_data
// static initializer (immediate loads, 64-bit unaligned struct copies with ldl/ldr/sdl/sdr, quadword copies,
// a few FPU ops, branches with delay slots) plus the other common integer/FPU ops so an unlucky compiler
// choice does not matter. Calls to `memset` are executed natively. Anything else throws.
class R5900 {
public:
    R5900(const Elf32& elf, std::uint32_t memset_addr) : elf_(elf), memset_(memset_addr) {}

    void set_gpr(int r, std::uint64_t v) {
        if (r) gpr_[r] = v;
    }
    // Runs from `entry` until the code returns to the sentinel address.
    void run(std::uint32_t entry) {
        constexpr std::uint32_t kSentinel = 0xFFFFFFF0u;
        gpr_[31] = kSentinel;
        pc_ = entry;
        for (std::uint64_t steps = 0; pc_ != kSentinel; ++steps) {
            if (steps > 20'000'000) throw FormatError("weapon_data initializer did not terminate");
            step();
        }
    }
    std::vector<std::uint8_t> read(std::uint32_t addr, std::uint32_t size) {
        std::vector<std::uint8_t> out(size);
        for (std::uint32_t i = 0; i < size; ++i) out[i] = load8(addr + i);
        return out;
    }

private:
    static constexpr std::uint32_t kPage = 4096;
    using Page = std::vector<std::uint8_t>;

    Page& page(std::uint32_t addr) {
        auto [it, fresh] = pages_.try_emplace(addr / kPage);
        if (fresh) it->second = elf_.image_range(addr / kPage * kPage, kPage);
        return it->second;
    }
    std::uint8_t load8(std::uint32_t a) { return page(a)[a % kPage]; }
    void store8(std::uint32_t a, std::uint8_t v) { page(a)[a % kPage] = v; }
    std::uint64_t loadn(std::uint32_t a, int n) {
        std::uint64_t v = 0;
        for (int i = 0; i < n; ++i) v |= std::uint64_t(load8(a + std::uint32_t(i))) << (8 * i);
        return v;
    }
    void storen(std::uint32_t a, std::uint64_t v, int n) {
        for (int i = 0; i < n; ++i) store8(a + std::uint32_t(i), std::uint8_t(v >> (8 * i)));
    }

    static std::uint64_t sext32(std::uint32_t v) { return std::uint64_t(std::int64_t(std::int32_t(v))); }
    float fpr(int r) const {
        float f;
        std::memcpy(&f, &fpr_[r], 4);
        return f;
    }
    void set_fpr(int r, float f) { std::memcpy(&fpr_[r], &f, 4); }

    void memset_call() {
        const std::uint32_t dst = std::uint32_t(gpr_[4]), n = std::uint32_t(gpr_[6]);
        for (std::uint32_t i = 0; i < n; ++i) store8(dst + i, std::uint8_t(gpr_[5]));
        gpr_[2] = gpr_[4];
    }

    void step() {
        const std::uint32_t pc = pc_;
        const std::uint32_t w = std::uint32_t(loadn(pc, 4));
        // Branch handling: every branch/jump executes its delay slot, then transfers.
        const auto delay_slot = [&] {
            pc_ = pc + 4;
            const std::uint32_t saved_next = pc + 8;
            exec(std::uint32_t(loadn(pc + 4, 4)), pc + 4);
            pc_ = saved_next;
        };
        const unsigned op = w >> 26;
        const int rs = int((w >> 21) & 31), rt = int((w >> 16) & 31);
        const std::int32_t simm = std::int16_t(w & 0xFFFF);
        auto branch_target = [&] { return pc + 4 + std::uint32_t(simm * 4); };
        switch (op) {
            case 0x04:
            case 0x05: {
                const bool taken = (gpr_[rs] == gpr_[rt]) == (op == 0x04);
                const std::uint32_t target = branch_target();
                delay_slot();
                if (taken) pc_ = target;
                return;
            }
            case 0x02:
            case 0x03: {
                const std::uint32_t target = (pc & 0xF0000000u) | ((w & 0x3FFFFFF) << 2);
                if (op == 0x03) {
                    const std::uint32_t link = pc + 8;
                    delay_slot();
                    if (target == memset_) {
                        memset_call();
                    } else {
                        gpr_[31] = link;
                        pc_ = target;
                        return;
                    }
                    return;
                }
                delay_slot();
                pc_ = target;
                return;
            }
            case 0x00: {
                const unsigned fn = w & 63;
                if (fn == 0x08 || fn == 0x09) {
                    const std::uint32_t target = std::uint32_t(gpr_[rs]);
                    const std::uint32_t link = pc + 8;
                    delay_slot();
                    if (fn == 0x09) gpr_[int((w >> 11) & 31)] = link;
                    pc_ = target;
                    return;
                }
                break;
            }
            default: break;
        }
        pc_ = pc + 4;
        exec(w, pc);
    }

    void exec(std::uint32_t w, std::uint32_t pc) {
        const unsigned op = w >> 26;
        const int rs = int((w >> 21) & 31), rt = int((w >> 16) & 31), rd = int((w >> 11) & 31), sa = int((w >> 6) & 31);
        const std::int32_t simm = std::int16_t(w & 0xFFFF);
        const std::uint32_t uimm = w & 0xFFFF;
        const std::uint32_t ea = std::uint32_t(gpr_[rs]) + std::uint32_t(simm);
        auto fail = [&]() -> void {
            char buf[80];
            std::snprintf(buf, sizeof buf, "weapon_data initializer: unsupported instruction %08x at %08x", w, pc);
            throw FormatError(buf);
        };
        switch (op) {
            case 0x00: {
                const std::uint64_t a = gpr_[rs], b = gpr_[rt];
                switch (w & 63) {
                    case 0x00: set_gpr(rd, sext32(std::uint32_t(b) << sa)); break;             // sll
                    case 0x02: set_gpr(rd, sext32(std::uint32_t(b) >> sa)); break;             // srl
                    case 0x03: set_gpr(rd, sext32(std::uint32_t(std::int32_t(b) >> sa))); break;   // sra
                    case 0x10: set_gpr(rd, hi_); break;                                        // mfhi
                    case 0x12: set_gpr(rd, lo_); break;                                        // mflo
                    case 0x18: {                                                               // mult
                        const std::int64_t p = std::int64_t(std::int32_t(a)) * std::int64_t(std::int32_t(b));
                        lo_ = sext32(std::uint32_t(p));
                        hi_ = sext32(std::uint32_t(p >> 32));
                        set_gpr(rd, lo_);
                        break;
                    }
                    case 0x21: set_gpr(rd, sext32(std::uint32_t(a) + std::uint32_t(b))); break;   // addu
                    case 0x23: set_gpr(rd, sext32(std::uint32_t(a) - std::uint32_t(b))); break;   // subu
                    case 0x24: set_gpr(rd, a & b); break;                                      // and
                    case 0x25: set_gpr(rd, a | b); break;                                      // or
                    case 0x26: set_gpr(rd, a ^ b); break;                                      // xor
                    case 0x27: set_gpr(rd, ~(a | b)); break;                                   // nor
                    case 0x2A: set_gpr(rd, std::int64_t(a) < std::int64_t(b)); break;          // slt
                    case 0x2B: set_gpr(rd, a < b); break;                                      // sltu
                    case 0x2D: set_gpr(rd, a + b); break;                                      // daddu
                    case 0x2F: set_gpr(rd, a - b); break;                                      // dsubu
                    default: fail();
                }
                break;
            }
            case 0x09: set_gpr(rt, sext32(std::uint32_t(gpr_[rs]) + std::uint32_t(simm))); break;   // addiu
            case 0x0A: set_gpr(rt, std::int64_t(gpr_[rs]) < std::int64_t(simm)); break;              // slti
            case 0x0B: set_gpr(rt, gpr_[rs] < std::uint64_t(std::int64_t(simm))); break;             // sltiu
            case 0x0C: set_gpr(rt, gpr_[rs] & uimm); break;                                          // andi
            case 0x0D: set_gpr(rt, gpr_[rs] | uimm); break;                                          // ori
            case 0x0E: set_gpr(rt, gpr_[rs] ^ uimm); break;                                          // xori
            case 0x0F: set_gpr(rt, sext32(uimm << 16)); break;                                       // lui
            case 0x1A: {                                                                             // ldl
                std::uint64_t v = gpr_[rt];
                const std::uint32_t base = ea & ~7u;
                for (std::uint32_t i = 0; i <= (ea & 7); ++i) {
                    const int byte = int(7 - (ea & 7) + i);
                    v = (v & ~(std::uint64_t(0xFF) << (8 * byte))) | (std::uint64_t(load8(base + i)) << (8 * byte));
                }
                set_gpr(rt, v);
                break;
            }
            case 0x1B: {                                                                             // ldr
                std::uint64_t v = gpr_[rt];
                for (std::uint32_t i = ea & 7, k = 0; i < 8; ++i, ++k)
                    v = (v & ~(std::uint64_t(0xFF) << (8 * k))) | (std::uint64_t(load8((ea & ~7u) + i)) << (8 * k));
                set_gpr(rt, v);
                break;
            }
            case 0x1E: {                                                                             // lq
                const std::uint32_t a = ea & ~15u;
                set_gpr(rt, loadn(a, 8));
                if (rt) hi_gpr_[rt] = loadn(a + 8, 8);
                break;
            }
            case 0x1F: {                                                                             // sq
                const std::uint32_t a = ea & ~15u;
                storen(a, gpr_[rt], 8);
                storen(a + 8, hi_gpr_[rt], 8);
                break;
            }
            case 0x20: set_gpr(rt, std::uint64_t(std::int64_t(std::int8_t(loadn(ea, 1))))); break;    // lb
            case 0x21: set_gpr(rt, std::uint64_t(std::int64_t(std::int16_t(loadn(ea, 2))))); break;   // lh
            case 0x23: set_gpr(rt, sext32(std::uint32_t(loadn(ea, 4)))); break;                       // lw
            case 0x24: set_gpr(rt, loadn(ea, 1)); break;                                              // lbu
            case 0x25: set_gpr(rt, loadn(ea, 2)); break;                                              // lhu
            case 0x27: set_gpr(rt, loadn(ea, 4)); break;                                              // lwu
            case 0x28: storen(ea, gpr_[rt], 1); break;                                                // sb
            case 0x29: storen(ea, gpr_[rt], 2); break;                                                // sh
            case 0x2B: storen(ea, gpr_[rt], 4); break;                                                // sw
            case 0x2C: {                                                                              // sdl
                const std::uint32_t base = ea & ~7u;
                for (std::uint32_t i = 0; i <= (ea & 7); ++i)
                    store8(base + i, std::uint8_t(gpr_[rt] >> (8 * (7 - (ea & 7) + i))));
                break;
            }
            case 0x2D:                                                                                // sdr
                for (std::uint32_t i = ea & 7, k = 0; i < 8; ++i, ++k) store8((ea & ~7u) + i, std::uint8_t(gpr_[rt] >> (8 * k)));
                break;
            case 0x31: fpr_[rt] = std::uint32_t(loadn(ea, 4)); break;                                 // lwc1
            case 0x37: set_gpr(rt, loadn(ea, 8)); break;                                              // ld
            case 0x39: storen(ea, fpr_[rt], 4); break;                                                // swc1
            case 0x3F: storen(ea, gpr_[rt], 8); break;                                                // sd
            case 0x11: {                                                                              // cop1
                const int fmt = rs, fs = rd, ft = rt, fd = sa;
                if (fmt == 0x00) set_gpr(rt, sext32(fpr_[fs]));                                       // mfc1
                else if (fmt == 0x04) fpr_[fs] = std::uint32_t(gpr_[rt]);                             // mtc1
                else if (fmt == 0x10) {
                    switch (w & 63) {
                        case 0x00: set_fpr(fd, fpr(fs) + fpr(ft)); break;
                        case 0x01: set_fpr(fd, fpr(fs) - fpr(ft)); break;
                        case 0x02: set_fpr(fd, fpr(fs) * fpr(ft)); break;
                        case 0x03: set_fpr(fd, fpr(fs) / fpr(ft)); break;
                        case 0x06: fpr_[fd] = fpr_[fs]; break;                                        // mov.s
                        default: fail();
                    }
                } else fail();
                break;
            }
            default: fail();
        }
    }

    const Elf32& elf_;
    std::uint32_t memset_;
    std::unordered_map<std::uint32_t, Page> pages_;
    std::array<std::uint64_t, 32> gpr_{}, hi_gpr_{};
    std::array<std::uint32_t, 32> fpr_{};
    std::uint64_t hi_ = 0, lo_ = 0;
    std::uint32_t pc_ = 0;
};

template <typename T>
T get(const std::vector<std::uint8_t>& b, std::size_t off) {
    return load<T>(Bytes(b), off);
}

WeaponDef decode(const std::vector<std::uint8_t>& b, std::size_t o) {
    auto at = [&](std::size_t off) { return o + off; };
    WeaponDef d;
    d.id = get<std::uint16_t>(b, at(0));
    d.base = get<std::uint16_t>(b, at(2));
    d.selectable = b.at(at(4));
    d.alt = std::int8_t(b.at(at(5)));
    d.category = b.at(at(6));
    d.blast_radius = get<float>(b, at(8));
    d.damage = get<float>(b, at(12));
    d.class_flags = get<std::uint16_t>(b, at(16));
    d.autoaim = get<float>(b, at(20));
    d.pellets = b.at(at(24));
    d.range = get<float>(b, at(28));
    d.speed = get<float>(b, at(32));
    d.spread = get<float>(b, at(36));
    for (std::size_t i = 0; i < 4; ++i) d.fire_count[i] = get<std::uint16_t>(b, at(40 + 2 * i));
    d.mode_label = get<std::uint32_t>(b, at(48));
    d.name_label = get<std::uint32_t>(b, at(56));
    d.mp_name_label = get<std::uint32_t>(b, at(60));
    d.fire_interval = get<std::uint32_t>(b, at(64));
    d.fire_delay = get<std::uint16_t>(b, at(68));
    d.muzzle_script = get<std::uint32_t>(b, at(72));
    d.drop_bone = b.at(at(80));
    d.flash_b = b.at(at(84));
    d.flash_g = b.at(at(85));
    d.flash_r = b.at(at(86));
    d.projectile_gfx = get<std::uint32_t>(b, at(92));
    d.fire_sound = get<std::uint32_t>(b, at(96));
    d.flags1 = get<std::uint32_t>(b, at(104));
    d.flags2 = get<std::uint32_t>(b, at(108));
    d.flags3 = get<std::uint32_t>(b, at(112));
    d.trail_length = get<std::int16_t>(b, at(118));
    d.casing_speed = b.at(at(120));
    d.datum0_gfx = get<std::uint32_t>(b, at(124));
    d.pickup_celglist = get<std::uint32_t>(b, at(128));
    d.zoom_max = get<float>(b, at(136));
    d.ammo_type = b.at(at(144));
    d.rounds_per_shot = b.at(at(145));
    d.clip_size = get<std::int16_t>(b, at(146));
    d.rumble = b.at(at(148));
    d.spread_growth = get<float>(b, at(152));
    d.anim_idle = get<std::uint32_t>(b, at(160));
    d.anim_reload = get<std::uint32_t>(b, at(164));
    d.anim_reload_start = get<std::uint32_t>(b, at(168));
    d.anim_reload_end = get<std::uint32_t>(b, at(172));
    d.anim_fire = get<std::uint32_t>(b, at(176));
    d.anim_fire_alt = get<std::uint32_t>(b, at(180));
    d.anim_aim = get<std::uint32_t>(b, at(184));
    d.anim_aim_alt = get<std::uint32_t>(b, at(188));
    d.anim_draw = get<std::uint32_t>(b, at(192));
    d.anim_holster = get<std::uint32_t>(b, at(196));
    d.unk200 = get<std::uint32_t>(b, at(200));
    d.anim_deepidle = get<std::uint32_t>(b, at(204));
    d.anim_settle = get<std::uint32_t>(b, at(208));
    d.anim_misc = get<std::uint32_t>(b, at(212));
    d.anim_holster_alt = get<std::uint32_t>(b, at(216));
    d.model_gfx = get<std::uint32_t>(b, at(220));
    for (std::size_t i = 0; i < 3; ++i) {
        d.gun_offset[i] = get<float>(b, at(224 + 4 * i));
        d.gun_offset_aim[i] = get<float>(b, at(236 + 4 * i));
    }
    return d;
}

}  // namespace

WeaponTable WeaponTable::from_elf(const Elf32& elf) {
    const auto need = [&](const char* name) {
        const auto s = elf.symbol(name);
        if (!s) throw FormatError(std::string("ACTION.ELF has no symbol ") + name);
        return *s;
    };
    constexpr std::uint32_t kStride = 268;
    const auto table = need("weapon_data");
    if (table.size != kWeaponCount * kStride) throw FormatError("weapon_data has an unexpected size");

    R5900 cpu(elf, need("memset").value);
    cpu.set_gpr(28, need("_gp").value);
    cpu.set_gpr(29, 0x01FF0000u);   // sp: top of the 32 MB EE RAM
    cpu.run(need("_GLOBAL_$I$weapon_data").value);
    const std::vector<std::uint8_t> raw = cpu.read(table.value, table.size);

    WeaponTable t;
    for (int i = 0; i < kWeaponCount; ++i) {
        t.weapons_.push_back(decode(raw, std::size_t(i) * kStride));
        if (t.weapons_.back().id != i) throw FormatError("weapon_data initializer left a bad row " + std::to_string(i));
    }
    // Two bytes the game patches at runtime after the static initializer (nfmips live-RAM diff): row 1
    // (fists) points its viewmodel at skin 0x050000B0, and the Remote Mine (55) alt-selects 57, not 56.
    t.weapons_.at(1).model_gfx = 0x050000B0;
    t.weapons_.at(55).alt = 2;
    const auto ammo = need("ammo_data");
    const std::vector<std::uint8_t> ammo_raw = cpu.read(ammo.value, ammo.size);
    for (int i = 0; i < kAmmoCount; ++i) {
        const std::size_t o = std::size_t(i) * 12;
        t.ammo_.push_back({get<std::int16_t>(ammo_raw, o), get<std::int16_t>(ammo_raw, o + 2), get<std::uint32_t>(ammo_raw, o + 4),
                           get<std::uint32_t>(ammo_raw, o + 8)});
    }

    const auto best = need("BestWeapon");
    const Bytes best_bytes = elf.at(best.value, best.size);
    for (std::size_t o = 0; o + 2 <= best_bytes.size(); o += 2) t.best_.push_back(load<std::uint16_t>(best_bytes, o));

    const auto effects = need("EffectInfo");
    const Bytes fx = elf.at(effects.value, effects.size);
    for (std::size_t o = 0; o + 136 <= fx.size(); o += 136) {
        SurfaceEffect e;
        const std::uint32_t name = load<std::uint32_t>(fx, o);
        if (name) e.name = std::string(load_cstr(elf.at(name, 16), 0));
        e.decal_gfx = load<std::uint32_t>(fx, o + 4);
        e.puff_gfx[0] = load<std::uint32_t>(fx, o + 8);
        e.puff_gfx[1] = load<std::uint32_t>(fx, o + 12);
        e.spark_gfx = load<std::uint32_t>(fx, o + 16);
        e.impact_sound = load<std::uint16_t>(fx, o + 20);
        e.ricochet_sound = load<std::uint16_t>(fx, o + 24);
        e.ricochet_prob = fx[o + 26];
        e.restitution = load<float>(fx, o + 28);
        e.emitter_id = load<std::uint32_t>(fx, o + 132);
        t.surfaces_.push_back(std::move(e));
    }

    struct Named {
        const char* symbol;
        int base;
    };
    // Upgrade_Weapon's dispatch (docs/spec-weapons.md 3.3): base id -> table.
    for (const Named& n : {Named{"UpgradedHandguns", 6}, Named{"UpgradedSnipers", 30}, Named{"UpgradedSilencedSnipers", 36},
                           Named{"UpgradedDartGuns", 67}, Named{"UpgradedTasers", 74}, Named{"UpgradedLasers", 78},
                           Named{"UpgradedPDAs", 86}}) {
        const auto s = need(n.symbol);
        const Bytes b = elf.at(s.value, 16);
        Upgrade u{n.base, {}};
        for (std::size_t i = 0; i < 4; ++i) u.to[i] = load<std::uint32_t>(b, i * 4);
        t.upgrades_.push_back(u);
    }
    return t;
}

int WeaponTable::upgrade(int base_id, unsigned level) const {
    for (const Upgrade& u : upgrades_)
        if (u.base == base_id) return int(u.to[std::min(level, 3u)]);
    if (base_id == 10) return level == 0 ? 12 : 10;   // Upgrade_Weapon: 12 unless byte_2A37BA is set
    return base_id;
}

int WeaponTable::upgrade(int base_id) const { return upgrade(base_id, 0); }

}  // namespace nf

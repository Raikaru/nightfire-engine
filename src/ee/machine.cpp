#include "ee/machine.hpp"

#include <cstdio>
#include <cstring>
#include <stdexcept>

#include <zlib.h>
#include <zstd.h>
#include "ee/disasm.hpp"
#include "ee/hostfile.hpp"

namespace nf::ee {

namespace {

[[noreturn]] void fail(const std::string& what) { throw std::runtime_error("nfmips: " + what); }

std::vector<u8> read_file(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) fail("cannot open " + path);
    std::vector<u8> out;
    char buf[65536];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof buf, f)) != 0) out.insert(out.end(), buf, buf + n);
    std::fclose(f);
    return out;
}

u16 rd16(const std::vector<u8>& d, size_t o) {
    u16 v;
    std::memcpy(&v, d.data() + o, 2);
    return v;
}
u32 rd32(const std::vector<u8>& d, size_t o) {
    u32 v;
    std::memcpy(&v, d.data() + o, 4);
    return v;
}

// ---- minimal ELF32-LE reader (EXEC, MIPS) ---------------------------------
struct Phdr {
    u32 type, offset, vaddr, filesz, memsz;
};

void check_elf(const std::vector<u8>& img) {
    if (img.size() < 52 || rd32(img, 0) != 0x464C457Fu || img[4] != 1 || img[5] != 1) fail("not a 32-bit LE ELF");
    if (rd16(img, 18) != 8) fail("not a MIPS ELF");
}

}  // namespace

void Machine::load_elf(const std::string& path) {
    const std::vector<u8> img = read_file(path);
    check_elf(img);
    const u32 phoff = rd32(img, 28), phnum = rd16(img, 44), phent = rd16(img, 42);
    const u32 shoff = rd32(img, 32), shnum = rd16(img, 48), shent = rd16(img, 46), shstr = rd16(img, 50);
    (void)shstr;
    if (phoff == 0 || phnum == 0) fail("ELF has no program headers");

    std::memset(mem.ram(), 0, Memory::kRamSize);
    std::memset(mem.spr(), 0, Memory::kSprSize);
    for (unsigned i = 0; i < phnum; ++i) {
        const size_t o = phoff + size_t(i) * phent;
        if (o + 32 > img.size()) fail("truncated program header");
        Phdr p{rd32(img, o), rd32(img, o + 4), rd32(img, o + 8), rd32(img, o + 16), rd32(img, o + 20)};
        if (p.type != 1) continue;  // PT_LOAD
        if (p.memsz > Memory::kRamSize || p.vaddr + p.memsz > Memory::kRamSize) fail("LOAD segment outside 32 MB RAM");
        if (p.offset + p.filesz > img.size()) fail("truncated LOAD segment");
        std::memcpy(mem.ram() + p.vaddr, img.data() + p.offset, p.filesz);
        std::memset(mem.ram() + p.vaddr + p.filesz, 0, p.memsz - p.filesz);  // .bss
    }

    symbols_.clear();
    symbols_by_addr_.clear();
    if (shoff != 0 && shnum != 0) {
        for (unsigned i = 0; i < shnum; ++i) {
            const size_t o = shoff + size_t(i) * shent;
            if (o + 40 > img.size()) break;
            const u32 type = rd32(img, o + 4);
            if (type != 2) continue;  // SYMTAB
            const u32 symoff = rd32(img, o + 16), symsz = rd32(img, o + 20), link = rd32(img, o + 24);
            const size_t so = shoff + size_t(link) * shent;
            if (so + 40 > img.size()) break;
            const u32 stroff = rd32(img, so + 16), strsz = rd32(img, so + 20);
            if (stroff + strsz > img.size()) break;
            for (u32 k = 0; k + 16 <= symsz; k += 16) {
                const size_t e = symoff + k;
                if (e + 16 > img.size()) break;
                const u32 nameoff = rd32(img, e), value = rd32(img, e + 4), size = rd32(img, e + 8);
                const u8 info = img[e + 12];
                if (nameoff >= strsz) continue;
                std::string name(reinterpret_cast<const char*>(img.data() + stroff + nameoff));
                if (name.empty() || (info & 0xF) == 4 /*FILE*/) continue;
                EeSymbol s{name, value, size};
                symbols_.push_back(s);
                if (value != 0) symbols_by_addr_.try_emplace(value, s);
            }
            break;  // first SYMTAB only
        }
    }

    // _start passes _stack and _stack_size to SetupThread, which returns the top.
    if (auto g = symbol("_gp")) cpu.r[28].d[0] = cpu.r[28].d[1] = 0, cpu.r[28].d[0] = g->value;
    else std::fprintf(stderr, "nfmips: no _gp symbol in %s ($gp left 0; set it manually)\n", path.c_str());
    cpu.r[29].d[0] = kStackTop;
    cpu.r[29].d[1] = 0;
    cpu.set_pc(0);
}

Machine::Machine(const std::string& elf_path) { load_elf(elf_path); }

std::optional<EeSymbol> Machine::symbol(const std::string& name) const {
    for (const auto& s : symbols_)
        if (s.name == name) return s;
    return std::nullopt;
}

std::optional<EeSymbol> Machine::symbol_at(u32 addr) const {
    auto it = symbols_by_addr_.upper_bound(addr);
    if (it == symbols_by_addr_.begin()) return std::nullopt;
    --it;
    return it->second;
}

u32 Machine::addr(const std::string& name) const {
    if (auto s = symbol(name)) return s->value;
    fail("ELF has no symbol " + name);
}

Machine::SavedRegs Machine::save_regs() const {
    SavedRegs s{};
    std::memcpy(s.r, cpu.r, sizeof s.r);
    s.hi = cpu.hi;
    s.lo = cpu.lo;
    s.sa = cpu.sa;
    s.pc = cpu.pc;
    s.npc = cpu.npc;
    std::memcpy(s.f, cpu.f, sizeof s.f);
    s.fpu_acc = cpu.fpu_acc;
    s.fcr31 = cpu.fcr31;
    std::memcpy(s.cop0, cpu.cop0, sizeof s.cop0);
    std::memcpy(s.vf, cpu.vu0.vf, sizeof s.vf);
    std::memcpy(s.vi, cpu.vu0.vi, sizeof s.vi);
    s.vacc = cpu.vu0.acc;
    std::memcpy(s.vumem, cpu.vu0.mem, sizeof s.vumem);
    return s;
}

void Machine::restore_regs(const SavedRegs& s) {
    std::memcpy(cpu.r, s.r, sizeof s.r);
    cpu.r[0].d[0] = cpu.r[0].d[1] = 0;
    cpu.hi = s.hi;
    cpu.lo = s.lo;
    cpu.sa = s.sa;
    cpu.pc = s.pc;
    cpu.npc = s.npc;
    std::memcpy(cpu.f, s.f, sizeof s.f);
    cpu.fpu_acc = s.fpu_acc;
    cpu.fcr31 = s.fcr31;
    std::memcpy(cpu.cop0, s.cop0, sizeof s.cop0);
    std::memcpy(cpu.vu0.vf, s.vf, sizeof s.vf);
    std::memcpy(cpu.vu0.vi, s.vi, sizeof s.vi);
    cpu.vu0.acc = s.vacc;
    std::memcpy(cpu.vu0.mem, s.vumem, sizeof s.vumem);
}

void Machine::setup_call(u32 entry, const CallArgs& args) {
    if (args.floats.size() > 8) fail("more than 8 float args (f12-f19) is not supported");
    cpu.set_pc(entry);
    for (int i = 0; i < 4 && i < int(args.ints.size()); ++i) {
        cpu.r[4 + i].d[0] = args.ints[size_t(i)];
        cpu.r[4 + i].d[1] = 0;
    }
    for (const auto& [r, v] : args.reg_init) { // explicit presets win (fragment entry regs like s0/v0)
        if (r >= 0 && r < 32 && r != 29 && r != 31) {
            cpu.r[r].d[0] = v;
            cpu.r[r].d[1] = 0;
        }
    }
    cpu.r[29].d[0] = kStackTop - 512;  // private frame below the game's stack top
    cpu.r[29].d[1] = 0;
    for (size_t k = 4; k < args.ints.size(); ++k)  // o32: arg5+ at 16(sp)
        mem.write<u32>(u32(cpu.r[29].d[0] + 16 + 4 * (k - 4)), u32(args.ints[k]));
    for (size_t k = 0; k < args.floats.size(); ++k) cpu.f[12 + k] = args.floats[k];
    for (const auto& [r, v] : args.fpreg_init)
        if (r >= 0 && r < 32) cpu.f[r] = v;
    cpu.r[31].d[0] = sentinel_;
    cpu.r[31].d[1] = 0;
}

CallResult Machine::run_until_return(u64 step_limit) {
    u64 n = 0;
    while (cpu.pc != sentinel_) {
        if (n++ >= step_limit) throw Trap(TrapKind::StepLimit, "step limit exhausted at pc=" + std::to_string(cpu.pc));
        auto h = hooks_.find(cpu.pc);
        if (h != hooks_.end() && h->second(cpu)) {
            cpu.pc = u32(cpu.r[31].d[0]);  // hook handled it: emulate `jr ra`
            cpu.npc = cpu.pc + 4;
            continue;
        }
        if (instruction_observer_) {
            if (const u8* p = mem.host(cpu.pc, 4)) {
                u32 inst;
                std::memcpy(&inst, p, sizeof(inst));
                instruction_observer_(cpu, cpu.pc, inst);
            }
        }
        try {
            cpu.step();
        } catch (Trap& t) {
            if (!t.located) throw Trap(t.kind, std::string(t.what()), cpu.cur_pc, cpu.cur_inst, t.addr);
            throw;
        }
    }
    CallResult r;
    r.v0 = cpu.r[2].d[0];
    r.v1 = cpu.r[3].d[0];
    r.f0 = cpu.f[0];
    return r;
}

CallResult Machine::call(u32 entry, const CallArgs& args, u64 step_limit) {
    const SavedRegs saved = save_regs();
    mem.begin_journal();
    try {
        setup_call(entry, args);
        const CallResult r = run_until_return(step_limit);
        restore_regs(saved);
        mem.rollback();
        return r;
    } catch (...) {
        restore_regs(saved);
        mem.rollback();
        throw;
    }
}
CallResult Machine::call_keep(u32 entry, const CallArgs& args, u64 step_limit) {
    const SavedRegs saved = save_regs();
    mem.begin_journal();
    try {
        setup_call(entry, args);
        const CallResult r = run_until_return(step_limit);
        restore_regs(saved);
        mem.end_journal();
        return r;
    } catch (...) {
        restore_regs(saved);
        mem.rollback();
        throw;
    }
}

CallResult Machine::call(const std::string& sym, const CallArgs& args, u64 step_limit) {
    return call(addr(sym), args, step_limit);
}

CallResult Machine::call_keep(const std::string& sym, const CallArgs& args, u64 step_limit) {
    return call_keep(addr(sym), args, step_limit);
}

void Machine::run_static_init(u64 step_limit) {
    // __static_initialization_and_destruction_0(a0=1, a1=0xFFFF): the file's
    // 0x1B3488 instance runs every constructor, weapon_data's included.
    u32 entry = 0x1B3488u;
    if (auto s = symbol("__static_initialization_and_destruction_0")) {
        if (s->value == entry) entry = s->value;
        else {
            for (const auto& c : symbols_)
                if (c.name == "__static_initialization_and_destruction_0" && c.value == 0x1B3488u) entry = c.value;
        }
    }
    const SavedRegs saved = save_regs();
    CallArgs args;
    args.i(1).i(0xFFFFu);
    setup_call(entry, args);
    run_until_return(step_limit);
    restore_regs(saved);
}

bool Machine::hook(const std::string& sym, Hook fn) {
    auto s = symbol(sym);
    if (!s) return false;
    hook(s->value, std::move(fn));
    return true;
}

void Machine::install_libc_hooks() {
    hook("memset", [](Cpu& c) {
        const u32 dst = c.r[4].w[0], len = c.r[6].w[0];
        const u8 v = u8(c.r[5].w[0]);
        for (u32 k = 0; k < len; ++k) c.mem.write<u8>(dst + k, v);
        c.r[2].d[0] = dst;  // v0 = dst
        c.r[2].d[1] = 0;
        return true;
    });
    const auto copy = [](Cpu& c) {
        const u32 dst = c.r[4].w[0], src = c.r[5].w[0], n = c.r[6].w[0];
        std::vector<u8> tmp(n);
        for (u32 k = 0; k < n; ++k) tmp[k] = c.mem.read<u8>(src + k);
        for (u32 k = 0; k < n; ++k) c.mem.write<u8>(dst + k, tmp[k]);
        c.r[2].d[0] = dst;
        c.r[2].d[1] = 0;
        return true;
    };
    hook("memcpy", copy);
    hook("memmove", copy);
    hook("strlen", [](Cpu& c) {
        const u32 s = c.r[4].w[0];
        u32 n = 0;
        while (c.mem.read<u8>(s + n) != 0) ++n;
        c.r[2].d[0] = n;
        c.r[2].d[1] = 0;
        return true;
    });
}

u32 Machine::alloc(u32 bytes) {
    const u32 p = (alloc_ptr_ + 15) & ~15u;
    alloc_ptr_ = p + ((bytes + 15) & ~15u);
    if (alloc_ptr_ >= kStackBase - 4096) fail("EE scratch allocator exhausted");
    return p;
}

void Machine::write_vec3(u32 va, float x, float y, float z, float w) {
    write_f32(va, x);
    write_f32(va + 4, y);
    write_f32(va + 8, z);
    write_f32(va + 12, w);
}

std::vector<u8> Machine::dump(u32 va, u32 len) {
    std::vector<u8> out(len);
    mem.read_block(va, out.data(), len);
    return out;
}

std::string Machine::disasm_range(u32 entry, u32 count) const {
    std::string out;
    for (u32 k = 0; k < count; ++k) {
        const u32 va = entry + 4 * k;
        const u8* p = mem.host(va, 4);
        if (!p) break;
        u32 w;
        std::memcpy(&w, p, 4);
        char line[128];
        std::snprintf(line, sizeof line, "%08x  %08x  %s\n", va, w, disassemble(w, va).c_str());
        out += line;
    }
    return out;
}

void Machine::trace(u32 entry, const CallArgs& args, u64 max_steps, FILE* out) {
    const SavedRegs saved = save_regs();
    mem.begin_journal();
    try {
        setup_call(entry, args);
        u64 n = 0;
        while (cpu.pc != sentinel_ && n < max_steps) {
            const u32 pc = cpu.pc;
            const u8* p = mem.host(pc, 4);
            if (!p) {
                std::fprintf(out, "%08x  <unmapped>\n", pc);
                break;
            }
            u32 w;
            std::memcpy(&w, p, 4);
            auto name = symbol_at(pc);
            std::fprintf(out, "%08x  %08x  %-28s ; a0=%08x a1=%08x a2=%08x a3=%08x%s\n", pc, w,
                         disassemble(w, pc).c_str(), cpu.r[4].w[0], cpu.r[5].w[0], cpu.r[6].w[0],
                         cpu.r[7].w[0], name ? ("  <" + name->name + ">").c_str() : "");
            auto h = hooks_.find(pc);
            if (h != hooks_.end() && h->second(cpu)) {
                cpu.pc = u32(cpu.r[31].d[0]);
                cpu.npc = cpu.pc + 4;
                continue;
            }
            cpu.step();
            ++n;
        }
        restore_regs(saved);
        mem.rollback();
    } catch (...) {
        restore_regs(saved);
        mem.rollback();
        throw;
    }
}

void Machine::trace(const std::string& sym, const CallArgs& args, u64 max_steps, FILE* out) {
    trace(addr(sym), args, max_steps, out);
}

// ---- PCSX2 .p2s savestates (plain zips) -----------------------------------
namespace {

struct ZipMember {
    std::string name;
    size_t data_off = 0, comp_size = 0, uncomp_size = 0;
    int method = 0;
};

std::vector<ZipMember> zip_members(const std::vector<u8>& d) {
    std::vector<ZipMember> out;
    size_t o = 0;
    while (o + 30 <= d.size()) {
        if (rd32(d, o) == 0x02014B50u) break;  // central directory
        if (rd32(d, o) != 0x04034B50u) fail(".p2s is not a zip (bad local header)");
        const int method = rd16(d, o + 8);
        const u32 comp = rd32(d, o + 18), uncomp = rd32(d, o + 22);
        const u32 nl = rd16(d, o + 26), el = rd16(d, o + 28);
        if (o + 30 + nl + el + comp > d.size()) fail("truncated .p2s member");
        ZipMember m;
        m.name.assign(reinterpret_cast<const char*>(d.data() + o + 30), nl);
        m.data_off = o + 30 + nl + el;
        m.comp_size = comp;
        m.uncomp_size = uncomp;
        m.method = method;
        out.push_back(std::move(m));
        o = m.data_off + comp;
    }
    return out;
}

std::vector<u8> zip_extract(const std::vector<u8>& d, const ZipMember& m) {
    if (m.method == 0) {
        if (m.comp_size != m.uncomp_size) fail("stored .p2s member has mismatched sizes");
        return std::vector<u8>(d.begin() + m.data_off, d.begin() + m.data_off + m.comp_size);
    }
    if (m.method == 93) {  // zstd: what PCSX2 writes for compressible members
        std::vector<u8> out(m.uncomp_size);
        const size_t r =
            ZSTD_decompress(out.data(), out.size(), d.data() + m.data_off, m.comp_size);
        if (ZSTD_isError(r) || r != m.uncomp_size) fail("failed to zstd-decode " + m.name);
        return out;
    }
    if (m.method != 8) fail("unsupported .p2s compression method " + std::to_string(m.method));
    std::vector<u8> out(m.uncomp_size);
    z_stream z{};
    if (inflateInit2(&z, -15) != Z_OK) fail("zlib init failed");
    z.next_in = const_cast<Bytef*>(d.data() + m.data_off);
    z.avail_in = uInt(m.comp_size);
    z.next_out = out.data();
    z.avail_out = uInt(out.size());
    const int r = inflate(&z, Z_FINISH);
    inflateEnd(&z);
    if (r != Z_STREAM_END) fail("failed to inflate " + m.name);
    return out;
}

}  // namespace

void Machine::load_p2s(const std::string& path) {
    const std::vector<u8> d = read_file(path);
    const std::vector<ZipMember> ms = zip_members(d);
    const ZipMember* ee = nullptr;
    const ZipMember* spr = nullptr;
    const ZipMember* vu0m = nullptr;
    for (const auto& m : ms) {
        if (m.name == "eeMemory.bin") ee = &m;
        if (m.name == "Scratchpad.bin") spr = &m;
        if (m.name == "vu0Memory.bin") vu0m = &m;
    }
    if (!ee) fail(".p2s has no eeMemory.bin");
    const std::vector<u8> ram = zip_extract(d, *ee);
    if (ram.size() != Memory::kRamSize) fail("eeMemory.bin is not 32 MB");
    std::memcpy(mem.ram(), ram.data(), ram.size());
    if (spr) {
        const std::vector<u8> s = zip_extract(d, *spr);
        if (s.size() != Memory::kSprSize) fail("Scratchpad.bin is not 16 KB");
        std::memcpy(mem.spr(), s.data(), s.size());
    }
    if (vu0m) {
        const std::vector<u8> v = zip_extract(d, *vu0m);
        if (v.size() != sizeof cpu.vu0.mem) fail("vu0Memory.bin has an unexpected size");
        std::memcpy(cpu.vu0.mem, v.data(), v.size());
    }
}

void Machine::load_ram_dump(const std::string& path) {
    const std::vector<u8> d = read_file(path);
    if (d.size() > Memory::kRamSize) fail("RAM dump is larger than 32 MB");
    std::memcpy(mem.ram(), d.data(), d.size());
}
void Machine::set_fs_root(const std::string& root) {
    if (root.empty()) return;
    fs_ = std::make_unique<HostFs>(root);
    fs_->install(*this);
}
void Machine::load_sym_file(const std::string& path) {
    const std::vector<u8> d = read_file(path);
    if (d.size() < 8) fail(path + " is too small for a symbol table");
    // Records are (u32 value, u32 name offset); the string table starts at the first record whose
    // name offset points at itself, i.e. records run until an offset that lands past all previous
    // ones. In practice (DRIVING.SYM) records fill [0, strtab) exactly with 8-byte stride.
    auto rd32 = [&](size_t o) {
        u32 v;
        std::memcpy(&v, d.data() + o, 4);
        return v;
    };
    // Records are (u32 value, u32 name offset); the string table follows the records. Scan record
    // candidates while the name offset points forward into the file at a plausible string; the
    // first failure ends the table (DRIVING.SYM: 12 719 records, then strings).
    size_t n = 0;
    while ((n + 1) * 8 <= d.size()) {
        const u32 off = rd32(n * 8 + 4);
        if (off < (n + 1) * 8 || off >= d.size()) break;
        size_t e = off;
        while (e < d.size() && d[e] != 0 && e - off < 512) ++e;
        if (e >= d.size() || e - off < 1) break;
        ++n;
    }
    if (n == 0) fail(path + " has no symbol records");
    for (size_t k = 0; k < n; ++k) {
        const u32 value = rd32(k * 8), off = rd32(k * 8 + 4);
        if (value == 0) continue;
        EeSymbol s{std::string(reinterpret_cast<const char*>(d.data() + off)), value, 0,
                   symbols_by_addr_.find(value) == symbols_by_addr_.end()};
        if (s.name.empty()) continue;
        symbols_.push_back(s);
        symbols_by_addr_.try_emplace(value, s);
    }
}

}  // namespace nf::ee

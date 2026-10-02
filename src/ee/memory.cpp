#include "ee/memory.hpp"

#include <cstdio>

namespace nf::ee {

const char* trap_kind_name(TrapKind k) {
    switch (k) {
        case TrapKind::Unsupported: return "unsupported";
        case TrapKind::Reserved: return "reserved-instruction";
        case TrapKind::AddressError: return "address-error";
        case TrapKind::BusError: return "bus-error";
        case TrapKind::IntegerOverflow: return "integer-overflow";
        case TrapKind::Syscall: return "syscall";
        case TrapKind::Break: return "break";
        case TrapKind::ConditionalTrap: return "conditional-trap";
        case TrapKind::StepLimit: return "step-limit";
        case TrapKind::Other: return "error";
    }
    return "error";
}

namespace {
std::string hex(u32 v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%08x", v);
    return buf;
}
constexpr u32 kPageSize = 1u << Memory::kPageShift;
constexpr u32 kRamPages = Memory::kRamSize >> Memory::kPageShift;
}  // namespace

Memory::Memory() : ram_(new u8[kRamSize]()), spr_(new u8[kSprSize]()), page_saved_(kRamPages + (kSprSize >> kPageShift), 0) {}

void Memory::misaligned(u32 va, u32 size, bool is_write) const {
    throw Trap(TrapKind::AddressError,
               std::string("misaligned ") + std::to_string(size) + "-byte " + (is_write ? "store to " : "load from ") + hex(va), va);
}

MmioHandler* Memory::find_mmio(u32 va) {
    // Fold mirrors: KSEG0/1 and the 0x2/0x3 windows map onto the same physical window.
    u32 pa = va;
    if (va >= 0x80000000u && va < 0xC0000000u) pa = va & 0x1FFFFFFFu;
    for (auto& r : mmio_)
        if (pa - r.base < r.size) return &r.h;
    return nullptr;
}

u64 Memory::mmio_read(u32 va, int size) {
    if (auto* h = find_mmio(va)) {
        if (h->read) return h->read(va, size);
    }
    throw Trap(TrapKind::BusError, std::string("load from unmapped/hardware address ") + hex(va) + " (size " + std::to_string(size) + ")", va);
}

void Memory::mmio_write(u32 va, u64 v, int size) {
    if (auto* h = find_mmio(va)) {
        if (h->write) {
            h->write(va, v, size);
            return;
        }
    }
    char buf[64];
    std::snprintf(buf, sizeof buf, " value 0x%llx (size %d)", static_cast<unsigned long long>(v), size);
    throw Trap(TrapKind::BusError, std::string("store to unmapped/hardware address ") + hex(va) + buf, va);
}

Reg128 Memory::read128(u32 va) {
    check_align(va, 16, false);
    if (watching_) watch_(va, 16, false);
    Reg128 r;
    if (const u8* p = host(va, 16)) {
        std::memcpy(&r, p, 16);
    } else {
        r.d[0] = mmio_read(va, 8);
        r.d[1] = mmio_read(va + 8, 8);
    }
    return r;
}

void Memory::write128(u32 va, const Reg128& v) {
    check_align(va, 16, true);
    if (watching_) watch_(va, 16, true);
    if (u8* p = host(va, 16)) {
        journal_touch(p, 16);
        std::memcpy(p, &v, 16);
    } else {
        mmio_write(va, v.d[0], 8);
        mmio_write(va + 8, v.d[1], 8);
    }
}

void Memory::read_block(u32 va, void* dst, size_t n) {
    u8* out = static_cast<u8*>(dst);
    while (n) {
        // Chunk at 4 KB so a block may straddle a mapping boundary of the decode function.
        const size_t chunk = std::min<size_t>(n, kPageSize - (va & (kPageSize - 1)));
        const u8* p = host(va, u32(chunk));
        if (!p) throw Trap(TrapKind::BusError, "block read from unmapped address " + hex(va), va);
        std::memcpy(out, p, chunk);
        out += chunk;
        va += u32(chunk);
        n -= chunk;
    }
}

void Memory::write_block(u32 va, const void* src, size_t n) {
    const u8* in = static_cast<const u8*>(src);
    while (n) {
        const size_t chunk = std::min<size_t>(n, kPageSize - (va & (kPageSize - 1)));
        u8* p = host(va, u32(chunk));
        if (!p) throw Trap(TrapKind::BusError, "block write to unmapped address " + hex(va), va);
        journal_touch(p, chunk);
        std::memcpy(p, in, chunk);
        in += chunk;
        va += u32(chunk);
        n -= chunk;
    }
}

std::string Memory::read_cstr(u32 va, size_t max) {
    std::string s;
    for (size_t i = 0; i < max; ++i) {
        const u8 c = read<u8>(va + u32(i));
        if (!c) break;
        s.push_back(char(c));
    }
    return s;
}

void Memory::map_mmio(u32 phys_base, u32 size, MmioHandler h) { mmio_.push_back({phys_base, size, std::move(h)}); }

void Memory::stub_hw_window(u32 phys_base, u32 size, const u8* initial) {
    auto store = std::make_shared<std::vector<u8>>(size, 0);
    if (initial) std::memcpy(store->data(), initial, size);
    MmioHandler h;
    h.read = [store, phys_base, size](u32 va, int n) {
        const u32 off = (va & 0x1FFFFFFFu) - phys_base;
        u64 v = 0;
        if (off + u32(n) <= size) std::memcpy(&v, store->data() + off, size_t(n));
        return v;
    };
    h.write = [store, phys_base, size](u32 va, u64 v, int n) {
        const u32 off = (va & 0x1FFFFFFFu) - phys_base;
        if (off + u32(n) <= size) std::memcpy(store->data() + off, &v, size_t(n));
    };
    map_mmio(phys_base, size, std::move(h));
}

void Memory::set_watch(std::function<void(u32, u32, bool)> fn) {
    watch_ = std::move(fn);
    watching_ = bool(watch_);
}

void Memory::journal_touch_slow(u8* p, size_t n) {
    // p points into RAM or scratchpad; find the region and iterate the touched 4 KB pages.
    u8* base;
    u32 page_bias;
    size_t limit;
    if (p >= ram_.get() && p < ram_.get() + kRamSize) {
        base = ram_.get();
        page_bias = 0;
        limit = kRamSize;
    } else {
        base = spr_.get();
        page_bias = kRamPages;
        limit = kSprSize;
    }
    const size_t first = size_t(p - base) >> kPageShift;
    const size_t last = std::min(limit - 1, size_t(p - base) + n - 1) >> kPageShift;
    for (size_t pg = first; pg <= last; ++pg) {
        if (page_saved_[page_bias + pg]) continue;
        page_saved_[page_bias + pg] = 1;
        SavedPage s;
        s.where = base + (pg << kPageShift);
        s.bytes.assign(s.where, s.where + kPageSize);
        saved_.push_back(std::move(s));
    }
}

void Memory::begin_journal() {
    saved_.clear();
    std::fill(page_saved_.begin(), page_saved_.end(), u8(0));
    journal_on_ = true;
}

void Memory::rollback() {
    for (auto& s : saved_) std::memcpy(s.where, s.bytes.data(), kPageSize);
    saved_.clear();
    std::fill(page_saved_.begin(), page_saved_.end(), u8(0));
    journal_on_ = false;
}

void Memory::end_journal() {
    journal_on_ = false;
    saved_.clear();
    std::fill(page_saved_.begin(), page_saved_.end(), u8(0));
}

}  // namespace nf::ee

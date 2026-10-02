#pragma once

// EE physical memory model: 32 MB RAM (mirrored at the KSEG0/KSEG1/uncached windows), 16 KB scratchpad,
// stubbable hardware register ranges. No TLB. Accesses that hit anything else raise a Trap (BusError).

#include <cstring>
#include <functional>
#include <memory>
#include <vector>

#include "ee/types.hpp"

namespace nf::ee {

// A hardware register range (timers, DMA, GS, ...). Handlers get the virtual address they were called with.
struct MmioHandler {
    std::function<u64(u32 addr, int size)> read;
    std::function<void(u32 addr, u64 value, int size)> write;
};

class Memory {
public:
    static constexpr u32 kRamSize = 32u << 20;
    static constexpr u32 kSprBase = 0x70000000u, kSprSize = 16u << 10;
    static constexpr u32 kPageShift = 12;

    Memory();

    // Raw views (physical RAM / scratchpad).
    u8* ram() { return ram_.get(); }
    const u8* ram() const { return ram_.get(); }
    u8* spr() { return spr_.get(); }
    const u8* spr() const { return spr_.get(); }

    // Address decode: returns the host pointer if [va, va+size) lies entirely in RAM or scratchpad, else nullptr.
    u8* host(u32 va, u32 size) {
        u32 pa;
        if (va < 0x10000000u) pa = va;
        else if (va - 0x20000000u < 0x20000000u) pa = va & 0x0FFFFFFFu;       // uncached / uncached-accelerated mirrors
        else if (va - kSprBase < kSprSize) return va - kSprBase + size <= kSprSize ? spr_.get() + (va - kSprBase) : nullptr;
        else if (va - 0x80000000u < 0x40000000u) pa = va & 0x1FFFFFFFu;       // KSEG0 / KSEG1
        else return nullptr;
        return (pa < kRamSize && size <= kRamSize - pa) ? ram_.get() + pa : nullptr;
    }
    const u8* host(u32 va, u32 size) const { return const_cast<Memory*>(this)->host(va, size); }
    // Physical RAM offset of a virtual address, or ~0u if it does not map to RAM.
    u32 ram_offset(u32 va) const {
        const u8* p = host(va, 1);
        return (p && p >= ram_.get() && p < ram_.get() + kRamSize) ? u32(p - ram_.get()) : ~0u;
    }

    template <typename T>
    T read(u32 va) {
        check_align(va, sizeof(T), false);
        if (watching_) watch_(va, sizeof(T), false);
        if (const u8* p = host(va, sizeof(T))) {
            T v;
            std::memcpy(&v, p, sizeof v);
            return v;
        }
        return T(mmio_read(va, sizeof(T)));
    }
    template <typename T>
    void write(u32 va, T v) {
        check_align(va, sizeof(T), true);
        if (watching_) watch_(va, sizeof(T), true);
        if (u8* p = host(va, sizeof(T))) {
            journal_touch(p, sizeof(T));
            std::memcpy(p, &v, sizeof v);
            return;
        }
        mmio_write(va, u64(v), sizeof(T));
    }
    Reg128 read128(u32 va);
    void write128(u32 va, const Reg128& v);

    // Bulk helpers for the harness (byte-granular, no alignment requirement, RAM/scratchpad only; throw Trap).
    void read_block(u32 va, void* dst, size_t n);
    void write_block(u32 va, const void* src, size_t n);
    std::string read_cstr(u32 va, size_t max = 4096);

    // Hardware registers. Ranges are physical addresses given in the 0x10000000.. window (any mirror matches).
    void map_mmio(u32 phys_base, u32 size, MmioHandler h);
    // Simple backing store for a hardware window: reads return the last written value (zero initially).
    void stub_hw_window(u32 phys_base, u32 size, const u8* initial = nullptr);

    // Access watch (debugging / tracing). Called for every data access through read()/write()/read128()/write128().
    void set_watch(std::function<void(u32 va, u32 size, bool write)> fn);

    // Write journal: after begin_journal() the first write to every 4 KB page saves the page; rollback() restores
    // them all. Makes repeated calls on a seeded state deterministic and cheap.
    void begin_journal();
    void rollback();
    void end_journal();          // keep changes, stop journalling
    bool journalling() const { return journal_on_; }

private:
    void check_align(u32 va, u32 size, bool is_write) const {
        if (va & (size - 1)) misaligned(va, size, is_write);
    }
    [[noreturn]] void misaligned(u32 va, u32 size, bool is_write) const;
    u64 mmio_read(u32 va, int size);
    void mmio_write(u32 va, u64 v, int size);
    MmioHandler* find_mmio(u32 va);
    void journal_touch(u8* p, size_t n) {
        if (!journal_on_) return;
        journal_touch_slow(p, n);
    }
    void journal_touch_slow(u8* p, size_t n);

    struct MmioRange {
        u32 base, size;
        MmioHandler h;
    };
    struct SavedPage {
        u8* where;
        std::vector<u8> bytes;
    };
    std::unique_ptr<u8[]> ram_, spr_;
    std::vector<MmioRange> mmio_;
    bool watching_ = false;
    std::function<void(u32, u32, bool)> watch_;
    bool journal_on_ = false;
    std::vector<SavedPage> saved_;
    std::vector<u8> page_saved_;          // per RAM page (+ scratchpad pages appended)
};

}  // namespace nf::ee

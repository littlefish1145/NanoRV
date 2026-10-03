// SPDX-License-Identifier: MIT
//
// The physical address space: an ordered list of named regions plus flat
// helpers used by the loaders. MMU translation is layered on top of this
// class later (see docs/ARCHITECTURE.md) and must not be implemented here.
#pragma once

#include "memory/memory_region.hpp"
#include "memory/spin_mutex.hpp"

#include <span>
#include <string>
#include <vector>
#include <atomic>
#include <array>
#include <memory>
#include <mutex>
#include <cstring>

namespace rvvm::memory {

struct Permissions {
    bool read = true;
    bool write = true;
    bool execute = true;
};

struct RegionEntry {
    u64 base = 0;
    u64 size = 0;
    std::string name;
    MemoryRegion* region = nullptr;
    Permissions permissions;
};

class AddressSpace {
public:
    // Regions must be added before any access. Overlapping additions are rejected.
    bool add(u64 base, u64 size, MemoryRegion* region, std::string name,
             Permissions permissions = Permissions{});

    // Flat physical accessors. `width_bits` must be 8/16/32/64.
    // Returns false for unmapped addresses, bad widths or permission violations.
    bool load(u64 phys, unsigned width_bits, u64& value, bool for_exec = false) const noexcept;
    bool store(u64 phys, unsigned width_bits, u64 value) const noexcept;
    u8* direct(u64 phys, u64 count, Access access) const noexcept;
    void set_concurrent(bool enabled) noexcept { concurrent_=enabled; }
    bool concurrent() const noexcept { return concurrent_; }
    u64 host_physical(const u8* ptr) const noexcept {return ram_base_+static_cast<u64>(ptr-ram_data_);}
    // Every concurrent RAM access uses the same aligned 64-bit atomic storage,
    // including subword CPU accesses, instruction fetch and device DMA.
    u64 read_host(const u8*,unsigned width) const noexcept;
    void write_host(u8*,unsigned width,u64) const noexcept;
    template<unsigned Width> u64 read_fast(const u8* ptr) const noexcept {
        static_assert(Width==8 || Width==16 || Width==32 || Width==64);
        if(!concurrent_) {u64 value=0;std::memcpy(&value,ptr,Width/8);return value;}
        const auto address=reinterpret_cast<std::uintptr_t>(ptr),offset=address&7;
        if(offset+Width/8>8)return read_host(ptr,Width);
        auto& word=*reinterpret_cast<u64*>(address&~std::uintptr_t{7});
        return (std::atomic_ref<u64>(word).load(std::memory_order_acquire)>>(offset*8))&(~0ull>>(64-Width));
    }
    template<unsigned Width> void write_fast(u8* ptr,u64 value) const noexcept {
        static_assert(Width==8 || Width==16 || Width==32 || Width==64);
        const u64 physical=ram_base_+static_cast<u64>(ptr-ram_data_);
        if(!concurrent_) {std::memcpy(ptr,&value,Width/8);notify_write(physical,Width/8);return;}
        const auto address=reinterpret_cast<std::uintptr_t>(ptr),offset=address&7;
        if(offset+Width/8>8) {write_host(ptr,Width,value);return;}
        auto guard=atomic_guard(physical);
        write_locked<Width>(ptr,value,physical);
    }
    using AtomicGuard=std::unique_lock<SpinMutex>;
    AtomicGuard atomic_guard(u64 address) const {
        return AtomicGuard((*stripes_)[stripe_index(address)].mutex);
    }
    // Naturally aligned RAM stores performed inside an AMO/SC/PTE critical
    // section. Validate the lock token instead of recursively acquiring it.
    bool store_locked(u64 address,unsigned width,u64 value,const AtomicGuard&) const noexcept;
    u64 reservation_version(u64 address) const noexcept { return (*stripes_)[stripe_index(address)].version; }
private:
    template<unsigned Width> void write_locked(u8* ptr,u64 value,u64 physical) const noexcept {
        const auto address=reinterpret_cast<std::uintptr_t>(ptr),offset=address&7;
        auto& word=*reinterpret_cast<u64*>(address&~std::uintptr_t{7});
        std::atomic_ref<u64> atomic(word);
        if constexpr(Width==64)atomic.store(value,std::memory_order_release);
        else {
            const u64 mask=(~0ull>>(64-Width))<<(offset*8);
            atomic.store((atomic.load(std::memory_order_relaxed)&~mask)|((value<<(offset*8))&mask),std::memory_order_release);
        }
        ++(*stripes_)[stripe_index(physical)].version;
        notify_write(physical,Width/8);
    }
public:
    bool compare_exchange(u64 address,unsigned width,u64& expected,u64 desired) const noexcept;
    u64 code_generation(u64 address) const noexcept;
    const std::atomic<u64>* code_generation_counter(u64 address) const noexcept {
        const auto offset=address-ram_base_;
        return code_pages_ && offset<ram_size_?&code_pages_[offset>>12]:nullptr;
    }
    void invalidate_code() const noexcept;
    using MmioDispatch=bool(*)(void*,MemoryRegion*,u64,unsigned,u64&,bool);
    void set_mmio_dispatch(void* state,MmioDispatch callback) noexcept { mmio_state_=state;mmio_dispatch_=callback; }
    void set_write_observer(void* state, void(*callback)(void*,u64,u64)) noexcept {
        observer_state_=state; observer_=callback;
    }
    void notify_write(u64 address,u64 bytes) const noexcept;
    void notify_dma_write(u64 address,u64 bytes) const noexcept;

    // Convenience narrow accessors, all little endian.
    u8 load_u8(u64 phys) const noexcept {
        u64 v = 0;
        return load(phys, 8, v) ? static_cast<u8>(v) : 0;
    }
    u16 load_u16(u64 phys) const noexcept {
        u64 v = 0;
        return load(phys, 16, v) ? static_cast<u16>(v) : 0;
    }
    u32 load_u32(u64 phys) const noexcept {
        u64 v = 0;
        return load(phys, 32, v) ? static_cast<u32>(v) : 0;
    }

    bool read_bytes(u64 phys, std::span<std::uint8_t> out) const noexcept;
    bool write_bytes(u64 phys, std::span<const std::uint8_t> data) const noexcept;

    // Zero `length` bytes, chunked so it also works for devices with narrow registers.
    bool fill_zero(u64 phys, u64 length) const noexcept;

    // Locates the region containing `phys` and returns its region-relative offset.
    // `required_bytes` guards against accesses that straddle the region end.
    const RegionEntry* find_entry(u64 phys, u64& offset, u64 required_bytes) const noexcept;
    MemoryRegion* find(u64 phys, u64& offset, u64 required_bytes) const noexcept;
    bool contains(u64 phys) const noexcept {
        u64 offset = 0;
        return find(phys, offset, 1) != nullptr;
    }

    std::size_t region_count() const noexcept { return regions_.size(); }
    const std::vector<RegionEntry>& regions() const noexcept { return regions_; }
    const RegionEntry* entry_at(std::size_t index) const noexcept;
    const RegionEntry* find_region_named(std::string_view name) const noexcept;

private:
    static std::size_t stripe_index(u64 address) noexcept {
        const auto line=address>>6;
        // Fold upper bits so unrelated stacks/pages 256 KiB apart do not all
        // contend on one lock. The reservation granule remains a cache line.
        return static_cast<std::size_t>((line^(line>>12)^(line>>24))&4095);
    }
    std::vector<RegionEntry> regions_; // sorted by base, kept disjoint
    struct MmioPage {u64 tag=~0ull;const RegionEntry* entry=nullptr;};
    std::array<MmioPage,4096> mmio_pages_{};
    u64 ram_base_ = 0, ram_size_ = 0;
    u8* ram_data_ = nullptr;
    Permissions ram_permissions_{};
    void* observer_state_=nullptr;
    void(*observer_)(void*,u64,u64)=nullptr;
    bool concurrent_=false;
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable:4324) // Intentional cache-line padding avoids false sharing.
#endif
    struct alignas(64) Stripe {SpinMutex mutex;u64 version=0;};
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    mutable std::unique_ptr<std::array<Stripe,4096>> stripes_=std::make_unique<std::array<Stripe,4096>>();
    mutable std::unique_ptr<std::atomic<u64>[]> code_pages_;
    // Compact first-level filter for the fine-grained code generation table.
    mutable std::unique_ptr<std::atomic<u64>[]> code_summary_;
    u64 page_count_=0;
    void* mmio_state_=nullptr;
    MmioDispatch mmio_dispatch_=nullptr;
};

} // namespace rvvm::memory

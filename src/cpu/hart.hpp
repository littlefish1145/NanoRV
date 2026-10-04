// SPDX-License-Identifier: MIT
//
// Architectural state of a single hart.
#pragma once

#include "core/types.hpp"
#include "cpu/csr.hpp"
#include "cpu/privilege.hpp"

namespace nanorv::cpu {

// Architectural register numbers that carry special meaning.
enum : u32 {
    kRegZero = 0,
    kRegRa = 1,
    kRegSp = 2,
    kRegGp = 3,
    kRegTp = 4,
    kRegT0 = 5,
    kRegT1 = 6,
    kRegT2 = 7,
    kRegS0 = 8,
    kRegFp = 8,
    kRegS1 = 9,
    kRegA0 = 10,
    kRegA1 = 11,
    kRegA2 = 12,
    kRegA3 = 13,
    kRegA4 = 14,
    kRegA5 = 15,
    kRegA6 = 16,
    kRegA7 = 17,
    kRegS2 = 18,
    kRegS11 = 19,
    kRegT3 = 28,
    kRegT6 = 31,
};

struct Hart {
    // Integer registers. x0 is permanently zero; use set_reg() to keep it that way.
    u64 x[32] = {};
    u64 pc = 0;
    u64 next_pc = 0;

    // Raw FLEN=64 bits; single precision results are NaN boxed in the upper half.
    u64 f[32] = {};

    Privilege privilege = Privilege::Machine;

    // Control and status registers (Zicsr subset; stage 4 adds the machine and
    // supervisor registers).
    CsrFile csr;

    // Index of this hart, reported through the read-only mhartid CSR.
    u32 hart_id = 0;

    // LR/SC reservation (A extension). A reservation is only ever held by one
    // hart at a time; any store overlapping the reserved granule clears it.
    u64 reservation_address = 0;
    bool reservation_valid = false;
    unsigned reservation_width = 64;
    u64 reservation_version = 0;

    void set_reservation(u64 address, unsigned width=64) noexcept {
        reservation_address = address;
        reservation_valid = true;
        reservation_width=width;
    }

    void clear_reservation() noexcept { reservation_valid = false; }

    bool reservation_matches(u64 address) const noexcept {
        return reservation_valid && reservation_address == address;
    }

    // Called after a successful bus store by the interpreter. A store that
    // overlaps the reserved address invalidates the reservation, which is what
    // makes an SC fail when another hart (or this one) wrote the location.
    void note_store(u64 address, unsigned width_bits) noexcept;

    // Loads do not invalidate an LR/SC reservation; the hook exists so that the
    // reservation policy has a single place to grow (stage 9 adds the cross-hart
    // rules here).
    void note_load(u64 address, unsigned width_bits) noexcept;

    // Executed instruction counter, for tracing and benchmarking only. Also the
    // value returned by the `instret` and `cycle` CSRs.
    u64 retired = 0;
    bool waiting = false;
    u64 translation_epoch = 0;
    // Separate access classes avoid reusing a read translation for writes.
    struct Translation {
        u64 vpn = ~0ull, ppn = 0, satp = 0, context = 0, epoch = ~0ull;
        const void* bus = nullptr;
        u8* host = nullptr;
    };
    static constexpr unsigned kTlbEntries=512;
    static constexpr unsigned kVictimTlbEntries=8;
    static_assert((kTlbEntries&(kTlbEntries-1))==0);
    Translation tlb[3][kTlbEntries]{};
    // Small fully associative miss cache, as in QEMU's victim_tlb_hit. Only the
    // owning hart accesses it; the same epoch/context checks apply to both tiers.
    Translation victim_tlb[3][kVictimTlbEntries]{};
    unsigned victim_replacement[3]{};
    bool translation_cache_enabled = false;
    bool native_fp = false;

    // Data-only JIT descriptors. The context serial occupies bits 3..11 of
    // tag; the low three bits remain zero for the native alignment check.
    // Context changes advance the serial instead of clearing 1024 entries.
    // Clear before serial reuse; this is an exact tag, never a context hash.
    struct NativeTranslation {
        u64 tag = ~0ull;
        u64 addend = 0;
    };
    static_assert(sizeof(NativeTranslation)==16);
    NativeTranslation native_tlb[2][kTlbEntries]{};
    u64 native_tlb_tag = 0;
    u64 native_tlb_epoch = ~0ull, native_tlb_satp = 0, native_tlb_context = 0;
    const void* native_tlb_bus = nullptr;
    bool native_tlb_enabled = false;

    void prepare_native_tlb(const void* bus,u64 satp,u64 context) noexcept {
        if(native_tlb_bus==bus && native_tlb_epoch==translation_epoch &&
           native_tlb_satp==satp && native_tlb_context==context &&
           native_tlb_enabled==translation_cache_enabled)return;
        native_tlb_tag+=8;
        // Serial 511 is reserved: even an unaligned access to the last
        // virtual page must never match the invalid all-ones tag.
        if(native_tlb_tag>=511*8) {
            for(auto& access:native_tlb)for(auto& entry:access)entry.tag=~0ull;
            native_tlb_tag=8;
        }
        native_tlb_bus=bus;native_tlb_epoch=translation_epoch;
        native_tlb_satp=satp;native_tlb_context=context;
        native_tlb_enabled=translation_cache_enabled;
    }
    void publish_native_translation(unsigned access,const Translation& entry) noexcept {
        if(access>=2)return; // Fetch privilege may differ under MPRV.
        prepare_native_tlb(entry.bus,entry.satp,entry.context);
        auto& native=native_tlb[access][entry.vpn&(kTlbEntries-1)];
        native.addend=reinterpret_cast<std::uintptr_t>(entry.host)-(entry.vpn<<12);
        native.tag=(entry.vpn<<12)|native_tlb_tag;
    }

    void set_reg(u32 index, u64 value) noexcept {
        if (index != kRegZero) {
            x[index] = value;
        }
    }

    u64 get_reg(u32 index) const noexcept { return index == kRegZero ? 0 : x[index]; }

    void reset(u64 entry) noexcept {
        for (u64& r : x) {
            r = 0;
        }
        for (u64& r : f) {
            r = 0;
        }
        pc = entry;
        next_pc = entry;
        privilege = Privilege::Machine;
        csr.reset();
        clear_reservation();
        reservation_address = 0;
        retired = 0;
        waiting = false;
        ++translation_epoch;
    }
};

inline void Hart::note_store(u64 address, unsigned width_bits) noexcept {
    if (!reservation_valid) {
        return;
    }
    const u64 bytes = width_bits / 8;
    // A reservation covers exactly one naturally aligned granule; any store that
    // touches those bytes at all invalidates it.
    const u64 granule_end = reservation_address + reservation_width/8;
    const u64 store_end = address + bytes;
    if (address < granule_end && reservation_address < store_end) {
        clear_reservation();
    }
}

inline void Hart::note_load(u64, unsigned) noexcept {
    // Per the RISC-V memory model a load does not break a reservation; only
    // stores (and stage 9's cross-hart rules) do.
}

} // namespace nanorv::cpu

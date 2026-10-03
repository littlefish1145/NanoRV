// SPDX-License-Identifier: MIT
//
// Helpers shared by the instruction group implementations (RV64I, M, A, ...).
// Internal to the CPU core; not part of the public interface.
#pragma once

#include "cpu/execute.hpp"
#include "cpu/trap.hpp"
#include "mmu/mmu.hpp"

namespace rvvm::cpu::detail {

constexpr u32 bits(u32 value, unsigned hi, unsigned lo) noexcept {
    return (value >> lo) & ((1u << (hi - lo + 1)) - 1u);
}

inline Trap illegal(const Decoded& insn) noexcept { return Trap::illegal(insn.raw); }

// Every unimplemented encoding funnels through here so that it always produces
// a legal illegal-instruction trap carrying the raw instruction bits.
inline bool illegal_insn(const Decoded& insn, Trap& trap) noexcept {
    trap = illegal(insn);
    return false;
}

// Bus access wrappers. A failed bus access becomes a load/store access fault
// instead of being ignored; hart.retired is only bumped by the caller.
inline bool do_load(Hart& hart, const memory::AddressSpace& memory, u64 address, unsigned width,
                    u64& value, Trap& trap) noexcept {
    if (!mmu::load(hart, memory, address, width, value, trap)) {
        return false;
    }
    hart.note_load(address, width);
    return true;
}

inline bool do_store(Hart& hart, const memory::AddressSpace& memory, u64 address, unsigned width,
                     u64 value, Trap& trap) noexcept {
    if (!mmu::store(hart, memory, address, width, value, trap)) {
        return false;
    }
    // An AMO or a plain store to the reserved granule breaks an LR/SC pair.
    u64 physical=address;
    if(hart.reservation_valid)mmu::translate(hart,memory,address,width,memory::Access::Write,physical,trap);
    hart.note_store(physical, width);
    return true;
}

// Naturally aligned access check shared by LR, SC and AMO.
inline bool check_natural_alignment(u64 address, unsigned width_bits, bool is_load, Trap& trap) noexcept {
    const u64 bytes = width_bits / 8;
    if ((address & (bytes - 1)) == 0) {
        return true;
    }
    trap = Trap{is_load ? TrapCause::LoadAddressMisaligned : TrapCause::StoreAddressMisaligned, address,
                true};
    return false;
}

} // namespace rvvm::cpu::detail

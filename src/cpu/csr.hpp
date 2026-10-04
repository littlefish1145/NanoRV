// SPDX-License-Identifier: MIT
//
// Control and status registers.
//
// Machine/supervisor CSRs, counters, floating-point status and 16 PMP entries.
//
// Permission model (RISC-V Privileged Architecture, "Zicsr"):
//   * only implemented addresses exist (there is no address alignment rule);
//   * the lowest privilege that may touch a CSR is part of its definition;
//   * a read-only CSR may never be written, but reading it through
//     CSRRS/CSRRC with rs1 = x0 is a legal no-op read.
#pragma once

#include "cpu/privilege.hpp"
#include "core/types.hpp"

#include <string>
#include <array>

namespace nanorv::cpu {

// CSR addresses implemented in this stage.
enum CsrAddress : u32 {
    kCsrFflags = 0x001,
    kCsrFrm = 0x002,
    kCsrFcsr = 0x003,
    kCsrSstatus = 0x100, kCsrSie = 0x104, kCsrStvec = 0x105, kCsrScounteren = 0x106,
    kCsrSscratch = 0x140, kCsrSepc = 0x141, kCsrScause = 0x142, kCsrStval = 0x143,
    kCsrSip = 0x144, kCsrSatp = 0x180,
    kCsrMstatus = 0x300, kCsrMisa = 0x301, kCsrMedeleg = 0x302, kCsrMideleg = 0x303,
    kCsrMie = 0x304, kCsrMtvec = 0x305, kCsrMcounteren = 0x306, kCsrMcountinhibit = 0x320,
    kCsrMscratch = 0x340, kCsrMepc = 0x341, kCsrMcause = 0x342, kCsrMtval = 0x343,
    kCsrMip = 0x344,
    kCsrCycle = 0xc00,
    kCsrTime = 0xc01,
    kCsrInstret = 0xc02,
    kCsrMvendorid = 0xf11,
    kCsrMarchid = 0xf12,
    kCsrMimpid = 0xf13,
    kCsrMhartid = 0xf14,
};

// Project identification. mvendorid/marchid/mimpid are opaque values; marchid
// spells "NanoRV" so that guest software can recognise the platform.
inline constexpr u64 kVendorId = 0x00000000'00000000ull;
inline constexpr u64 kArchId = 0x5256'564dull; // "NanoRV"
inline constexpr u64 kImplId = 0x0000'0001'0000'0000ull;

struct CsrInfo {
    u32 address = 0;
    const char* name = "";
    Privilege privilege = Privilege::Machine; // lowest privilege allowed
    bool writable = false;
    u64 write_mask = 0;
};

// Returns the descriptor for `address`, or nullptr when the CSR does not exist.
const CsrInfo* find_csr(u32 address) noexcept;

// Name for tracing and dumps; "unknown" when the address is not implemented.
const char* csr_name(u32 address) noexcept;

class CsrFile {
public:
    CsrFile() { reset(); }
    CsrFile(u32 hart_id, u64 timebase_frequency)
        : hart_id_(hart_id), timebase_frequency_(timebase_frequency == 0 ? 1 : timebase_frequency) { reset(); }

    // Both accessors return false when the access is illegal (unknown CSR,
    // wrong privilege, misaligned address, or a write to a read-only CSR); the
    // caller turns that into an illegal-instruction trap carrying the
    // instruction bits.
    bool read(u32 address, Privilege privilege, u64 instret, u64& value) const;
    bool write(u32 address, Privilege privilege, u64 value, u64 instret=0);

    // fcsr, fflags and frm are the same storage: fcsr = fflags | (frm << 5).
    u64 fflags() const noexcept { return fcsr_ & 0x1fu; }
    u64 frm() const noexcept { return (fcsr_ >> 5) & 0x7u; }
    u64 fcsr() const noexcept { return fcsr_; }

    void set_fflags(u64 value) noexcept;
    void set_frm(u64 value) noexcept;
    void set_fcsr(u64 value) noexcept;

    u32 hart_id() const noexcept { return hart_id_; }
    void set_hart_id(u32 id) noexcept { hart_id_ = id; }
    u64 timebase_frequency() const noexcept { return timebase_frequency_; }

    void reset() noexcept {
        fcsr_ = 0; registers_.fill(0);
        registers_[kCsrMstatus] = (2ull << 32) | (2ull << 34);
        registers_[kCsrMisa] = (2ull << 62) | (1ull << 0) | (1ull << 1) | (1ull << 2) | (1ull << 3) | (1ull << 5) |
            (1ull << 8) | (1ull << 12) | (1ull << 18) | (1ull << 20);
    }
    u64 raw(u32 address) const noexcept { return registers_[address]; }
    // Immutable address used only to calculate generated-code field offsets.
    const u64* raw_address(u32 address) const noexcept { return &registers_[address]; }
    void set_raw(u32 address, u64 value) noexcept { registers_[address] = value; }
    bool virtual_clock = false;
    u64 ticks = 0;

private:
    u64 fcsr_ = 0;
    std::array<u64, 4096> registers_{};
    u32 hart_id_ = 0;
    // Until the CLINT exists (stage 5) the timebase is 1 GHz, which makes the
    // `time` CSR read in nanoseconds.
    u64 timebase_frequency_ = 1'000'000'000ull;
};

// Host clock scaled to the timebase, exposed for the `time` CSR and for tests.
u64 read_timebase_ticks(u64 timebase_frequency) noexcept;

// Human readable dump of every implemented CSR, for the register dump.
std::string format_csr_dump(const CsrFile& csr, u64 instret);

} // namespace nanorv::cpu

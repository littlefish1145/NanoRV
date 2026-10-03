// SPDX-License-Identifier: MIT
//
// Synchronous exception causes (RISC-V Privileged Architecture, table 3.6).
// Interrupts use cause codes 1..15 with bit 63 set, handled in stage 4.
#pragma once

#include "core/types.hpp"

namespace rvvm::cpu {

enum class TrapCause : u64 {
    InstructionAddressMisaligned = 0,
    InstructionAccessFault = 1,
    IllegalInstruction = 2,
    Breakpoint = 3,
    LoadAddressMisaligned = 4,
    LoadAccessFault = 5,
    StoreAddressMisaligned = 6,
    StoreAccessFault = 7,
    EnvironmentCallUser = 8,
    EnvironmentCallSupervisor = 9,
    EnvironmentCallMachine = 11,
    InstructionPageFault = 12,
    LoadPageFault = 13,
    StorePageFault = 15,
};

const char* to_string(TrapCause cause) noexcept;

// A synchronous trap produced while executing one instruction.
struct Trap {
    TrapCause cause = TrapCause::IllegalInstruction;
    u64 tval = 0;      // faulting address or offending instruction bits
    bool has_tval = false;

    static Trap illegal(u64 instruction) noexcept {
        return Trap{TrapCause::IllegalInstruction, instruction, true};
    }
};

} // namespace rvvm::cpu
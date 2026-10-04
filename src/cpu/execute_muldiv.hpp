// SPDX-License-Identifier: MIT
//
// RV64M dispatch. `is_muldiv` lets the main interpreter decide whether an
// OP/OP-32 encoding with funct7 == 0x01 belongs to this group.
#pragma once

#include "cpu/execute.hpp"

namespace nanorv::cpu {

// True when `insn` belongs to the RV64M group (OP/OP-32 with funct7 == 0x01).
// The funct3 sub-encoding is validated inside execute_muldiv, so an unsupported
// funct3 still reaches the illegal-instruction path.
bool is_muldiv(const Decoded& insn) noexcept;

// Executes an RV64M instruction. Returns false with `trap` set for an
// unsupported encoding or a bus fault.
bool execute_muldiv(Hart& hart, const memory::AddressSpace& memory, const Decoded& insn,
                    Trap& trap) noexcept;

// Mnemonic for the disassembler ("mul", "divw", ...), or "?" when unsupported.
const char* muldiv_mnemonic(const Decoded& insn) noexcept;

} // namespace nanorv::cpu
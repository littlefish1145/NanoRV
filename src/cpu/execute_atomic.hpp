// SPDX-License-Identifier: MIT
//
// RV64A: atomic memory operations (ISA manual, "A Standard Extension").
//
// Encoding facts (verified against the LLVM RISC-V assembler, see
// docs/ISA_STATUS.md):
//   opcode = 0x2f
//   funct3 = 0b010 -> .W (32 bit), 0b011 -> .D (64 bit)
//   funct5 = funct7[6:2]
//   funct7[1] = aq, funct7[0] = rl
#pragma once

#include "cpu/execute.hpp"

namespace rvvm::cpu {

// True when `insn` is an opcode 0x2f encoding. The funct5/funct3 validity check
// happens in execute_atomic so unsupported encodings raise an
// illegal-instruction trap instead of being skipped.
bool is_atomic(const Decoded& insn) noexcept;

bool execute_atomic(Hart& hart, const memory::AddressSpace& memory, const Decoded& insn,
                    Trap& trap) noexcept;

// Mnemonic for the disassembler ("amoadd.d.aq", "lr.w", ...), or "?" when the
// encoding is not defined by RV64A.
const char* atomic_mnemonic(const Decoded& insn) noexcept;

} // namespace rvvm::cpu
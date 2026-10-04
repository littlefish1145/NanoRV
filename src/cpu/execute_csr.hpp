// SPDX-License-Identifier: MIT
//
// Zicsr: CSR read and write instructions.
//
// Encoding (verified against the LLVM RISC-V assembler, see docs/ISA_STATUS.md):
//   opcode 0x73, funct3 = 0b001 CSRRW / 0b010 CSRRS / 0b011 CSRRC
//                     0b101 CSRRWI / 0b110 CSRRSI / 0b111 CSRRCI
//   csr address in bits [31:20], rs1 field holds the 5-bit zero extended
//   immediate for the *I forms.
#pragma once

#include "cpu/execute.hpp"

namespace nanorv::cpu {

// True when `insn` is a Zicsr encoding. funct3 == 0b100 is reserved and is
// rejected inside execute_csr so that it raises an illegal-instruction trap.
bool is_csrinstr(const Decoded& insn) noexcept;

bool execute_csr(Hart& hart, const Decoded& insn, Trap& trap) noexcept;

// Mnemonic for the disassembler.
const char* csr_mnemonic(const Decoded& insn) noexcept;

} // namespace nanorv::cpu
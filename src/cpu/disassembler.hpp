// SPDX-License-Identifier: MIT
//
// Textual disassembler used by the execution trace and the debugger.
// Covers what stage 1 implements (RV64I) plus names for the encodings that
// later stages will add, so a trace never hides an instruction.
#pragma once

#include "cpu/decoder.hpp"

#include <string>

namespace rvvm::cpu {

std::string disassemble(const Decoded& insn);

// Convenience overload for raw instruction bits (picks the length automatically).
std::string disassemble(u32 raw);

} // namespace rvvm::cpu
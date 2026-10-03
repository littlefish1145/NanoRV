// SPDX-License-Identifier: MIT
//
// Execution trace. One line per retired instruction, in the same shape as
// `qemu-system-riscv64 -d in_asm` so that traces can be diffed.
#pragma once

#include "cpu/hart.hpp"
#include "cpu/decoder.hpp"

namespace rvvm::debug {

void trace_instruction(const cpu::Hart& hart, const cpu::Decoded& insn);

} // namespace rvvm::debug
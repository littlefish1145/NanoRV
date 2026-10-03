// SPDX-License-Identifier: MIT
//
// Reference interpreter for a single instruction.
//
// Invariants that must not be broken by later optimisation stages:
//   * every unsupported encoding raises an IllegalInstruction trap with tval set
//     to the raw instruction bits; nothing is ever silently skipped;
//   * the only way a trap escapes is through the returned Trap, never by
//     returning success;
//   * `hart.next_pc` is left untouched when a trap is raised so that the
//     trap handler can use `hart.pc`.
#pragma once

#include "cpu/decoder.hpp"
#include "cpu/hart.hpp"
#include "cpu/trap.hpp"
#include "memory/address_space.hpp"

namespace rvvm::cpu {

// Fetches and decodes the instruction at `pc`.
// Returns false and fills `trap` on fetch fault / misaligned PC / long encoding.
bool fetch(const memory::AddressSpace& memory, u64 pc, Decoded& out, Trap& trap) noexcept;
bool fetch(Hart& hart, const memory::AddressSpace& memory, Decoded& out, Trap& trap) noexcept;

// Executes one already-decoded instruction.
bool execute(Hart& hart, const memory::AddressSpace& memory, const Decoded& insn, Trap& trap) noexcept;

} // namespace rvvm::cpu

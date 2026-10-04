// SPDX-License-Identifier: MIT
#pragma once
#include "cpu/execute.hpp"
namespace nanorv::cpu {
bool execute_privileged(Hart &, const Decoded &, Trap &) noexcept;
void enter_trap(Hart &, const Trap &) noexcept;
bool pending_interrupt(const Hart &, Trap &) noexcept;
} // namespace nanorv::cpu

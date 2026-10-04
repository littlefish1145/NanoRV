// SPDX-License-Identifier: MIT
#pragma once
#include "cpu/execute.hpp"
namespace nanorv::cpu {
bool execute_fp(Hart &, const memory::AddressSpace &, const Decoded &,
                Trap &) noexcept;
}

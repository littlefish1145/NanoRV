// SPDX-License-Identifier: MIT
#pragma once
#include "cpu/execute.hpp"
namespace rvvm::cpu {
// Returns false for encodings outside the implemented Zba/Zbb/Zbs sets.
bool execute_bitmanip(Hart&,const Decoded&) noexcept;
}

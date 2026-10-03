// SPDX-License-Identifier: MIT
#pragma once
#include "cpu/hart.hpp"
#include "cpu/decoder.hpp"
namespace rvvm::cpu {
u64 aes_round(unsigned function,u64 a,u64 b,bool native) noexcept;
u64 carryless(unsigned function,u64 a,u64 b,bool native) noexcept;
bool execute_crypto(Hart&,const Decoded&) noexcept;
}

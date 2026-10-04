// SPDX-License-Identifier: MIT
#pragma once
#include "core/types.hpp"
namespace nanorv::cpu {
// Expand an RV64C encoding to its I/D equivalent. Zero denotes reserved.
u32 expand_compressed(u16 raw) noexcept;
} // namespace nanorv::cpu

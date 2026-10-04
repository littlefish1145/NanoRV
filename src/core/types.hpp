// SPDX-License-Identifier: MIT
//
// Fundamental machine-independent types used by every layer.
#pragma once

#include <cstddef>
#include <cstdint>

namespace nanorv {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i8 = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

using reg_t = u64; // XLEN

inline constexpr u64 kPageSize = 4096;
inline constexpr u64 kPageMask = ~static_cast<u64>(kPageSize - 1);

// QEMU `virt` compatible machine constants (see docs/ARCHITECTURE.md).
inline constexpr u64 kDramBase = 0x8000'0000ull;
inline constexpr u64 kClintBase = 0x0200'0000ull;
inline constexpr u64 kPlicBase  = 0x0c00'0000ull;
inline constexpr u64 kUart0Base = 0x1000'0000ull;
inline constexpr u64 kVirtioMmioBase = 0x1000'1000ull;
inline constexpr u64 kFdtBase = 0x0870'0000ull; // where the DTB is placed by default

inline constexpr u64 kHz = 1'000'000ull;
inline constexpr u64 kMhz = kHz * 1'000ull;
inline constexpr u64 kGhz = kMhz * 1'000ull;

constexpr bool is_power_of_two(u64 value) noexcept {
    return value != 0 && (value & (value - 1)) == 0;
}

// Power-of-two alignment helper used by the MMIO decoders.
constexpr u64 round_up_pow2(u64 value, u64 alignment) noexcept {
    return (value + alignment - 1) & ~(alignment - 1);
}

} // namespace nanorv
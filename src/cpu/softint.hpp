// SPDX-License-Identifier: MIT
//
// Portable 64-bit integer helpers for the M extension.
//
// __int128 is not available on MSVC, so the 64x64 -> 128 products are computed
// from 32-bit halves. Every function is constexpr so that the unit tests can
// also evaluate them at compile time.
#pragma once

#include "core/types.hpp"

namespace nanorv::cpu {

struct WideProduct {
    u64 low = 0;
    u64 high = 0;
};

// High 64 bits of the unsigned 64x64 product.
constexpr u64 mul_high_unsigned(u64 a, u64 b) noexcept {
    const u64 a_low = static_cast<u32>(a);
    const u64 a_high = a >> 32;
    const u64 b_low = static_cast<u32>(b);
    const u64 b_high = b >> 32;

    const u64 p00 = a_low * b_low;
    const u64 p01 = a_low * b_high;
    const u64 p10 = a_high * b_low;
    const u64 p11 = a_high * b_high;

    // The three partial middle terms can carry into each other; accumulating them
    // in 64 bits cannot overflow because each term is below 2^32.
    const u64 middle = (p00 >> 32) + static_cast<u32>(p01) + static_cast<u32>(p10);
    return p11 + (p01 >> 32) + (p10 >> 32) + (middle >> 32);
}

constexpr WideProduct mul_wide_unsigned(u64 a, u64 b) noexcept {
    return WideProduct{a * b, mul_high_unsigned(a, b)};
}

// High 64 bits of the signed 64x64 product.
constexpr u64 mul_high_signed(i64 a, i64 b) noexcept {
    const u64 ua = static_cast<u64>(a);
    const u64 ub = static_cast<u64>(b);
    u64 high = mul_high_unsigned(ua, ub);
    if (a < 0) {
        high -= ub;
    }
    if (b < 0) {
        high -= ua;
    }
    return high;
}

// High 64 bits of the signed x unsigned 64x64 product (MULHSU).
constexpr u64 mul_high_signed_unsigned(i64 a, u64 b) noexcept {
    const u64 ua = static_cast<u64>(a);
    u64 high = mul_high_unsigned(ua, b);
    if (a < 0) {
        high -= b;
    }
    return high;
}

// ---------------------------------------------------------------------------
// Division and remainder (XLEN = 64)
//
// The RISC-V specification pins down three cases that C++ leaves undefined or
// implementation defined:
//   * division by zero      -> DIV/DIVU yield all ones, REM/REMU yield rs1
//   * signed overflow       -> DIV yields the dividend, REM yields zero
//   * rounding              -> always toward zero
// ---------------------------------------------------------------------------

constexpr u64 div_unsigned(u64 a, u64 b) noexcept {
    return b == 0 ? ~static_cast<u64>(0) : a / b;
}

constexpr u64 rem_unsigned(u64 a, u64 b) noexcept {
    return b == 0 ? a : a % b;
}

constexpr u64 div_signed(i64 a, i64 b) noexcept {
    if (b == 0) {
        return ~static_cast<u64>(0);
    }
    if (b == -1) {
        // a / -1 == -a. Computed with unsigned negation so that
        // -INT64_MIN == INT64_MIN is well defined, which is also the value the
        // specification requires for the overflow case.
        return ~static_cast<u64>(a) + 1;
    }
    return static_cast<u64>(a / b);
}

constexpr u64 rem_signed(i64 a, i64 b) noexcept {
    if (b == 0) {
        return static_cast<u64>(a);
    }
    if (b == -1) {
        return 0; // INT64_MIN % -1
    }
    return static_cast<u64>(a % b);
}

constexpr u64 sext32_to_64(u64 value) noexcept {
    return static_cast<u64>(static_cast<i64>(static_cast<i32>(static_cast<u32>(value))));
}

constexpr u64 zext32(u64 value) noexcept { return static_cast<u32>(value); }

} // namespace nanorv::cpu
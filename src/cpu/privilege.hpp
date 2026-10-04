// SPDX-License-Identifier: MIT
//
// Privilege levels (RISC-V Privileged Architecture, "Modes of Operation").
// Kept in its own header because both the hart state and the CSR file need it
// and they must not include each other.
#pragma once

#include "core/types.hpp"

namespace nanorv::cpu {

enum class Privilege : u8 {
    User = 0,
    Supervisor = 1,
    Machine = 3,
};

inline const char* to_string(Privilege p) noexcept {
    switch (p) {
    case Privilege::User: return "U";
    case Privilege::Supervisor: return "S";
    case Privilege::Machine: return "M";
    }
    return "?";
}

// Machine >= Supervisor >= User, matching the encoding in mstatus/CSR addresses.
constexpr bool privilege_at_least(Privilege have, Privilege need) noexcept {
    return static_cast<u8>(have) >= static_cast<u8>(need);
}

} // namespace nanorv::cpu
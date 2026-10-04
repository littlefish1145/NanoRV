// SPDX-License-Identifier: MIT
//
// Human readable register dump, also used by the GDB stub and the tests.
#pragma once

#include "cpu/hart.hpp"

#include <string>

namespace nanorv::debug {

std::string format_registers(const cpu::Hart& hart);
void dump_registers(const cpu::Hart& hart);

} // namespace nanorv::debug
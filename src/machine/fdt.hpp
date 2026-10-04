// SPDX-License-Identifier: MIT
#pragma once
#include "core/types.hpp"
#include <string>
#include <vector>
namespace nanorv::machine {
std::vector<u8> make_fdt(u64 ram_size, u64 initrd_start, u64 initrd_size,
                         const std::string &args, bool disk = false, bool net = false, u32 harts = 1, bool graphics=false);
}

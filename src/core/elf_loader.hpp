// SPDX-License-Identifier: MIT
//
// ELF64 little-endian RISC-V image loader. Validates the header and every
// program header before touching the address space; a malformed image must
// fail loudly, never load partially.
#pragma once

#include "core/types.hpp"
#include "memory/address_space.hpp"

#include <expected>
#include <optional>
#include <string>
#include <vector>

namespace nanorv::core {

struct LoadedImage {
    u64 entry = 0;
    u64 lowest_address = 0;
    u64 highest_address = 0; // exclusive
    u32 segment_count = 0;
    u64 bytes_written = 0;
};

using LoadResult = std::expected<LoadedImage, std::string>;

// Accepts ET_EXEC and ET_DYN (PIE) ELF64 RISC-V images.
LoadResult load_elf(memory::AddressSpace& memory, const std::vector<std::uint8_t>& image);

// Loads a flat binary at `load_address`; `entry` defaults to `load_address`.
LoadResult load_raw(memory::AddressSpace& memory, const std::vector<std::uint8_t>& image,
                    u64 load_address, std::optional<u64> entry);

} // namespace nanorv::core
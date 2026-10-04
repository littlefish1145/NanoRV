// SPDX-License-Identifier: MIT
//
// Every bus-attached object (RAM, UART, CLINT, PLIC, VirtIO MMIO) implements
// this interface. Offsets are region relative.
#pragma once

#include "core/types.hpp"

namespace nanorv::memory {

enum class Access : u8 {
    Read = 0,
    Write = 1,
    Fetch = 2,
};

// Supported access widths in bits.
inline constexpr unsigned kWidthMask = 0x7; // 1,2,4,8 bytes

class MemoryRegion {
public:
    virtual ~MemoryRegion() = default;

    virtual const char* name() const noexcept = 0;
    virtual u64 size() const noexcept = 0;
    // Only RAM exposes contiguous bytes. Devices always retain bus callbacks.
    virtual u8* direct_data() noexcept { return nullptr; }

    // Both return false when the access cannot be served (out of range, unsupported
    // width, device rejection). They never silently ignore a request.
    virtual bool load(u64 offset, unsigned width_bits, u64& value) noexcept = 0;
    virtual bool store(u64 offset, unsigned width_bits, u64 value) noexcept = 0;
};

} // namespace nanorv::memory

// SPDX-License-Identifier: MIT
//
// Guest RAM backed by a single host allocation. Misaligned accesses are
// supported, matching QEMU's default `virt` behaviour.
#pragma once

#include "memory/memory_region.hpp"
#include "platform/platform.hpp"

#include <string>

namespace nanorv::memory {

class RamRegion final : public MemoryRegion {
public:
    // `size_bytes` must be non-zero; the allocation is rounded up to host pages.
    RamRegion(std::string name, u64 size_bytes, u64 base_address);
    ~RamRegion() override;

    RamRegion(const RamRegion&) = delete;
    RamRegion& operator=(const RamRegion&) = delete;

    const char* name() const noexcept override { return name_.c_str(); }
    u64 size() const noexcept override { return size_; }
    u64 base() const noexcept { return base_; }
    u64 allocated_bytes() const noexcept { return allocated_; }
    u8* direct_data() noexcept override { return data_; }

    bool load(u64 offset, unsigned width_bits, u64& value) noexcept override;
    bool store(u64 offset, unsigned width_bits, u64 value) noexcept override;

    // Host pointer for the MMIO fast path. Only valid for RAM-internal offsets.
    std::uint8_t* host_data() noexcept { return data_; }
    const std::uint8_t* host_data() const noexcept { return data_; }

private:
    std::string name_;
    u64 size_ = 0;
    u64 base_ = 0;
    u64 allocated_ = 0;
    std::uint8_t* data_ = nullptr;
};

} // namespace nanorv::memory

// SPDX-License-Identifier: MIT
#include "memory/ram_region.hpp"

#include "core/log.hpp"

#include <cstring>

namespace nanorv::memory {

RamRegion::RamRegion(std::string name, u64 size_bytes, u64 base_address)
    : name_(std::move(name)), size_(size_bytes), base_(base_address) {
    allocated_ = platform::align_up_pages(size_);
    data_ = static_cast<std::uint8_t*>(platform::allocate(allocated_, platform::Protect::ReadWrite));
    if (data_ == nullptr) {
        NanoRV_FATAL("ram: failed to allocate %llu bytes for '%s': %s",
                   static_cast<unsigned long long>(allocated_), name_.c_str(),
                   platform::last_error().c_str());
    }
    // VirtualAlloc guarantees zeroed pages; assert the invariant the loaders rely on
    // for .bss handling.
    NanoRV_DEBUG("ram: '%s' %llu MiB at 0x%016llx", name_.c_str(),
               static_cast<unsigned long long>(size_ / (1024 * 1024)),
               static_cast<unsigned long long>(base_));
}

RamRegion::~RamRegion() {
    if (data_ != nullptr) {
        platform::release(data_, allocated_);
        data_ = nullptr;
    }
}

bool RamRegion::load(u64 offset, unsigned width_bits, u64& value) noexcept {
    const unsigned bytes = width_bits / 8;
    if ((width_bits != 8 && width_bits != 16 && width_bits != 32 && width_bits != 64) ||
        offset + bytes > size_) {
        NanoRV_ERROR("ram: bad %u-bit load at offset 0x%llx (size 0x%llx)", width_bits,
                   static_cast<unsigned long long>(offset), static_cast<unsigned long long>(size_));
        return false;
    }
    const void* const src = static_cast<const void*>(data_ + offset);
    switch (bytes) {
    case 1: {
        std::uint8_t v = 0;
        std::memcpy(&v, src, 1);
        value = v;
        break;
    }
    case 2: {
        std::uint16_t v = 0;
        std::memcpy(&v, src, 2);
        value = v;
        break;
    }
    case 4: {
        std::uint32_t v = 0;
        std::memcpy(&v, src, 4);
        value = v;
        break;
    }
    default: {
        u64 v = 0;
        std::memcpy(&v, src, 8);
        value = v;
        break;
    }
    }
    return true;
}

bool RamRegion::store(u64 offset, unsigned width_bits, u64 value) noexcept {
    const unsigned bytes = width_bits / 8;
    if ((width_bits != 8 && width_bits != 16 && width_bits != 32 && width_bits != 64) ||
        offset + bytes > size_) {
        NanoRV_ERROR("ram: bad %u-bit store at offset 0x%llx (size 0x%llx)", width_bits,
                   static_cast<unsigned long long>(offset), static_cast<unsigned long long>(size_));
        return false;
    }
    void* const dst = static_cast<void*>(data_ + offset);
    switch (bytes) {
    case 1: {
        const auto v = static_cast<std::uint8_t>(value);
        std::memcpy(dst, &v, 1);
        break;
    }
    case 2: {
        const auto v = static_cast<std::uint16_t>(value);
        std::memcpy(dst, &v, 2);
        break;
    }
    case 4: {
        const auto v = static_cast<std::uint32_t>(value);
        std::memcpy(dst, &v, 4);
        break;
    }
    default:
        std::memcpy(dst, &value, 8);
        break;
    }
    return true;
}

} // namespace nanorv::memory
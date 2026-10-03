// SPDX-License-Identifier: MIT
#include "memory/ram_region.hpp"
#include "test_framework.hpp"

#include <cstring>

using namespace rvvm;
using namespace rvvm::memory;

TEST(ram, allocates_and_zeroes) {
    RamRegion ram("dram", 4096, 0x80000000);
    CHECK_EQ(ram.size(), std::uint64_t{4096});
    CHECK_EQ(ram.base(), std::uint64_t{0x80000000});
    CHECK_EQ(ram.allocated_bytes(), std::uint64_t{4096});

    u64 value = 0xffffffffffffffffull;
    CHECK(ram.load(0, 64, value));
    CHECK_EQ(value, std::uint64_t{0});
}

TEST(ram, partial_megabyte_allocation_rounds_up) {
    RamRegion ram("small", 4097, 0x80000000);
    CHECK_EQ(ram.size(), std::uint64_t{4097});
    CHECK_EQ(ram.allocated_bytes(), std::uint64_t{8192});
    u64 value = 0;
    CHECK(ram.load(4096, 8, value)); // the last byte of the requested range
}

TEST(ram, store_then_load_all_widths) {
    RamRegion ram("dram", 4096, 0x80000000);

    CHECK(ram.store(0x10, 8, 0xa5));
    CHECK(ram.store(0x11, 8, 0x5a));
    CHECK(ram.store(0x12, 16, 0x1234));
    CHECK(ram.store(0x14, 32, 0xdeadbeef));

    u64 value = 0;
    CHECK(ram.load(0x10, 8, value));
    CHECK_EQ(value, std::uint64_t{0xa5});
    CHECK(ram.load(0x12, 16, value));
    CHECK_EQ(value, std::uint64_t{0x1234});
    CHECK(ram.load(0x14, 32, value));
    CHECK_EQ(value, std::uint64_t{0xdeadbeef});

    // Little endian byte order must hold for overlapping accesses.
    CHECK(ram.store(0x20, 64, 0x1122334455667788ull));
    CHECK(ram.load(0x20, 8, value));
    CHECK_EQ(value, std::uint64_t{0x88});
    CHECK(ram.load(0x27, 8, value));
    CHECK_EQ(value, std::uint64_t{0x11});
}

TEST(ram, supports_misaligned_access) {
    RamRegion ram("dram", 4096, 0x80000000);
    CHECK(ram.store(0x10, 64, 0x1122334455667788ull));

    u64 value = 0;
    CHECK(ram.load(0x11, 32, value));
    CHECK_EQ(value, std::uint64_t{0x44556677});
    CHECK(ram.load(0x14, 32, value));
    CHECK_EQ(value, std::uint64_t{0x11223344});

    CHECK(ram.store(0x11, 32, 0x11223344));
    CHECK(ram.load(0x12, 32, value));
    CHECK_EQ(value, std::uint64_t{0x33112233});
}

TEST(ram, rejects_out_of_range_and_bad_widths) {
    RamRegion ram("dram", 4096, 0x80000000);
    u64 value = 0;
    CHECK(!ram.load(4096, 8, value));
    CHECK(!ram.load(4095, 64, value));
    CHECK(!ram.load(0, 24, value));
    CHECK(!ram.store(0xfff, 64, 1));
    CHECK(!ram.store(0, 4, 1));
}

TEST(ram, host_data_points_at_allocation) {
    RamRegion ram("dram", 4096, 0x80000000);
    CHECK(ram.host_data() != nullptr);
    CHECK(ram.store(0x30, 64, 0xfeedfacecafebeefull));
    u64 mirrored = 0;
    std::memcpy(&mirrored, ram.host_data() + 0x30, sizeof(mirrored));
    CHECK_EQ(mirrored, std::uint64_t{0xfeedfacecafebeefull});
}
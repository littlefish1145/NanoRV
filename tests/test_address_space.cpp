// SPDX-License-Identifier: MIT
#include "memory/address_space.hpp"
#include "memory/ram_region.hpp"
#include "test_framework.hpp"

#include <array>

using namespace rvvm;
using namespace rvvm::memory;

namespace {

class CountingRegion final : public MemoryRegion {
public:
    const char* name() const noexcept override { return "counter"; }
    u64 size() const noexcept override { return 0x1000; }

    bool load(u64 offset, unsigned width_bits, u64& value) noexcept override {
        ++loads;
        last_offset = offset;
        last_width = width_bits;
        value = 0xa5a5a5a5a5a5a5a5ull;
        return ok;
    }
    bool store(u64 offset, unsigned width_bits, u64 value) noexcept override {
        ++stores;
        last_offset = offset;
        last_width = width_bits;
        last_value = value;
        return ok;
    }

    bool ok = true;
    int loads = 0;
    int stores = 0;
    u64 last_offset = 0;
    u64 last_value = 0;
    unsigned last_width = 0;
};

} // namespace

TEST(address_space, rejects_overlapping_regions) {
    AddressSpace space;
    RamRegion ram("ram", 0x10000, 0x80000000);
    CHECK(space.add(0x80000000, 0x10000, &ram, "dram"));
    CHECK(!space.add(0x80008000, 0x10000, &ram, "shadow"));
    CHECK_EQ(space.region_count(), std::size_t{1});
}
TEST(address_space, code_summary_tracks_cpu_dma_and_bitmap_boundary_writes) {
    AddressSpace space;RamRegion ram("ram",128*4096,kDramBase);
    CHECK(space.add(kDramBase,128*4096,&ram,"dram"));
    space.set_concurrent(true);
    const u64 first=kDramBase+63*4096,second=first+4096;
    const auto a=space.code_generation(first),b=space.code_generation(second);
    auto* ptr=space.direct(first,8,Access::Write);
    space.write_fast<64>(ptr,123);
    CHECK_EQ(space.code_generation(first),a+1);
    CHECK_EQ(space.code_generation(second),b);
    // Cross the boundary between two summary words and two marked pages.
    const std::array<u8,16> payload{};
    CHECK(space.write_bytes(second-8,payload));
    CHECK(space.code_generation(first)>a+1);
    CHECK(space.code_generation(second)>b);
    const auto c=space.code_generation(second);
    space.notify_dma_write(second,16);
    CHECK_EQ(space.code_generation(second),c+1);
    space.invalidate_code();
    CHECK_EQ(space.code_generation(second),c+2);
}

TEST(address_space, rejects_invalid_regions) {
    AddressSpace space;
    CHECK(!space.add(0x1000, 0, nullptr, "nothing"));
}

TEST(address_space, regions_are_sorted_by_base) {
    AddressSpace space;
    CountingRegion a;
    CountingRegion b;
    CountingRegion c;
    CHECK(space.add(0x2000, 0x1000, &c, "c"));
    CHECK(space.add(0x1000, 0x1000, &b, "b"));
    CHECK(space.add(0x3000, 0x1000, &a, "a"));
    CHECK_EQ(space.regions()[0].name, std::string("b"));
    CHECK_EQ(space.regions()[1].name, std::string("c"));
    CHECK_EQ(space.regions()[2].name, std::string("a"));
}

TEST(address_space, access_outside_any_region_fails) {
    AddressSpace space;
    RamRegion ram("ram", 0x1000, 0x80000000);
    CHECK(space.add(0x80000000, 0x1000, &ram, "dram"));

    u64 value = 0;
    CHECK(!space.load(0x80001000, 32, value));
    CHECK(!space.load(0x7ffffff8, 32, value));
    CHECK(!space.store(0x90000000, 64, 1));
}

TEST(address_space, access_straddling_region_end_fails) {
    AddressSpace space;
    RamRegion ram("ram", 0x1000, 0x80000000);
    CHECK(space.add(0x80000000, 0x1000, &ram, "dram"));

    u64 value = 0;
    CHECK(!space.load(0x80000ffc, 64, value)); // 64-bit access crossing the end
    CHECK(space.load(0x80000ff8, 64, value));
    CHECK_EQ(value, std::uint64_t{0});
}

TEST(address_space, rejects_bad_widths) {
    AddressSpace space;
    CountingRegion region;
    CHECK(space.add(0x1000, 0x1000, &region, "counter"));
    u64 value = 0;
    CHECK(!space.load(0x1000, 24, value));
    CHECK(!space.store(0x1000, 12, 0));
}

TEST(address_space, honours_permissions) {
    AddressSpace space;
    CountingRegion region;
    memory::Permissions read_only{};
    read_only.read = true;
    read_only.write = false;
    read_only.execute = false;
    CHECK(space.add(0x1000, 0x1000, &region, "rom", read_only));

    u64 value = 0;
    CHECK(space.load(0x1008, 64, value));
    CHECK(!space.store(0x1008, 64, 0));
    CHECK(!space.load(0x1008, 64, value, /*for_exec=*/true));
    CHECK_EQ(region.loads, 1);
    CHECK_EQ(region.stores, 0);
}

TEST(address_space, propagates_device_failure) {
    AddressSpace space;
    CountingRegion region;
    region.ok = false;
    CHECK(space.add(0x1000, 0x1000, &region, "counter"));
    u64 value = 0;
    CHECK(!space.load(0x1000, 32, value));
    CHECK(!space.store(0x1000, 32, 0));
}

TEST(address_space, byte_helpers_round_trip) {
    AddressSpace space;
    RamRegion ram("ram", 0x1000, 0x80000000);
    CHECK(space.add(0x80000000, 0x1000, &ram, "dram"));

    CHECK(space.store(0x80000010, 8, 0x12));
    CHECK(space.store(0x80000011, 8, 0x34));
    CHECK_EQ(space.load_u16(0x80000010), std::uint16_t{0x3412});

    const std::array<std::uint8_t, 4> payload{0xde, 0xad, 0xbe, 0xef};
    CHECK(space.write_bytes(0x80000020, payload));
    CHECK_EQ(space.load_u32(0x80000020), std::uint32_t{0xefbeadde});

    std::array<std::uint8_t, 4> readback{};
    CHECK(space.read_bytes(0x80000020, readback));
    CHECK(readback == payload);

    CHECK(space.fill_zero(0x80000040, 0x100));
    for (u32 i = 0; i < 0x100; ++i) {
        CHECK_EQ(space.load_u8(0x80000040 + i), std::uint8_t{0});
    }
}

TEST(address_space, find_reports_offset) {
    AddressSpace space;
    RamRegion ram("ram", 0x2000, 0x80000000);
    CHECK(space.add(0x80000000, 0x2000, &ram, "dram"));
    u64 offset = 0;
    CHECK(space.find(0x80000123, offset, 4) == &ram);
    CHECK_EQ(offset, std::uint64_t{0x123});
    CHECK(space.contains(0x80001fff));
    CHECK(!space.contains(0x80002000));
}

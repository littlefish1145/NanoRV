// SPDX-License-Identifier: MIT
#include "core/elf_loader.hpp"
#include "memory/ram_region.hpp"
#include "test_framework.hpp"
#include "test_helpers.hpp"

#include <cstring>

using namespace rvvm;
using namespace rvvm::core;

namespace {

void put16(std::vector<std::uint8_t>& image, std::size_t offset, u16 value) {
    image[offset + 0] = static_cast<std::uint8_t>(value);
    image[offset + 1] = static_cast<std::uint8_t>(value >> 8);
}

void put32(std::vector<std::uint8_t>& image, std::size_t offset, u32 value) {
    for (int i = 0; i < 4; ++i) {
        image[offset + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(value >> (i * 8));
    }
}

void put64(std::vector<std::uint8_t>& image, std::size_t offset, u64 value) {
    for (int i = 0; i < 8; ++i) {
        image[offset + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(value >> (i * 8));
    }
}

constexpr std::size_t kEhdrSize = 64;
constexpr std::size_t kPhdrSize = 56;
constexpr std::size_t kContentOffset = 0x1000;

// Builds a minimal but completely valid ELF64 little-endian RISC-V image with a
// single PT_LOAD segment.
std::vector<std::uint8_t> build_elf(u16 machine, u16 type, u64 entry, u64 vaddr,
                                    const std::vector<std::uint8_t>& payload, u64 memsz,
                                    u16 phnum = 1) {
    std::vector<std::uint8_t> image(kContentOffset + payload.size(), 0);
    image[0] = 0x7f;
    image[1] = 'E';
    image[2] = 'L';
    image[3] = 'F';
    image[4] = 2; // ELFCLASS64
    image[5] = 1; // ELFDATA2LSB
    image[6] = 1; // EV_CURRENT
    put16(image, 16, type);
    put16(image, 18, machine);
    put32(image, 20, 1);
    put64(image, 24, entry);
    put64(image, 32, kEhdrSize); // e_phoff
    put64(image, 40, 0);          // e_shoff
    put32(image, 48, 0);          // e_flags
    put16(image, 52, static_cast<u16>(kEhdrSize));
    put16(image, 54, static_cast<u16>(kPhdrSize));
    put16(image, 56, phnum);
    put16(image, 58, 0);
    put16(image, 60, 0);

    if (phnum != 0) {
        const std::size_t ph = kEhdrSize;
        put32(image, ph + 0, 1); // PT_LOAD
        put32(image, ph + 4, 0x7); // RWX
        put64(image, ph + 8, kContentOffset);
        put64(image, ph + 16, vaddr);
        put64(image, ph + 24, vaddr); // p_paddr
        put64(image, ph + 32, payload.size());
        put64(image, ph + 40, memsz);
        put64(image, ph + 48, 0x1000); // p_align
    }

    for (std::size_t i = 0; i < payload.size(); ++i) {
        image[kContentOffset + i] = payload[i];
    }
    return image;
}

std::vector<std::uint8_t> program_payload(const std::vector<u32>& words) {
    std::vector<std::uint8_t> bytes(words.size() * 4, 0);
    for (std::size_t i = 0; i < words.size(); ++i) {
        for (int b = 0; b < 4; ++b) {
            bytes[i * 4 + static_cast<std::size_t>(b)] =
                static_cast<std::uint8_t>(words[i] >> (b * 8));
        }
    }
    return bytes;
}

} // namespace

TEST(elf_loader, loads_a_valid_image_and_zeroes_bss) {
    rvvm::test::CpuFixture fixture(1u << 20);
    const u64 entry = rvvm::kDramBase + 0x400;
    const std::vector<u32> words = {0x00b50513u, 0x00100073u}; // addi a0, a0, 11; ebreak
    const auto payload = program_payload(words);
    const auto image = build_elf(243, 2, entry, entry, payload, payload.size() + 16);

    const LoadResult result = load_elf(fixture.space, image);
    CHECK(result.has_value());
    CHECK_EQ(result->entry, entry);
    CHECK_EQ(result->segment_count, std::uint32_t{1});
    CHECK_EQ(result->lowest_address, entry);
    CHECK_EQ(result->highest_address, entry + payload.size() + 16);

    u64 value = 0;
    CHECK(fixture.space.load(entry, 32, value));
    CHECK_EQ(value, std::uint64_t{0x00b50513u});
    for (std::size_t i = payload.size(); i < payload.size() + 16; ++i) {
        CHECK(fixture.space.load(entry + i, 8, value));
        CHECK_EQ(value, std::uint64_t{0});
    }
}

TEST(elf_loader, honours_p_paddr_when_it_differs) {
    rvvm::test::CpuFixture fixture(1u << 20);
    auto image = build_elf(243, 2, rvvm::kDramBase, rvvm::kDramBase, program_payload({0x00000013u}), 4);
    // p_paddr = 0x80001000 while p_vaddr = 0x80000000: stage 1 has no MMU, so the
    // physical address is what gets written, exactly like QEMU without a MMU.
    put64(image, kEhdrSize + 24, rvvm::kDramBase + 0x1000);

    const LoadResult result = load_elf(fixture.space, image);
    CHECK(result.has_value());
    u64 value = 0;
    CHECK(fixture.space.load(rvvm::kDramBase + 0x1000, 32, value));
    CHECK_EQ(value, std::uint64_t{0x00000013u});
    CHECK(fixture.space.load(rvvm::kDramBase, 32, value));
    CHECK_EQ(value, std::uint64_t{0}); // nothing was written at p_vaddr
}

TEST(elf_loader, rejects_bad_headers) {
    rvvm::test::CpuFixture fixture(1u << 20);
    const auto payload = program_payload({0x00000013u});
    const auto good = build_elf(243, 2, rvvm::kDramBase, rvvm::kDramBase, payload, 4);

    auto truncated = good;
    truncated.resize(32);
    CHECK(!load_elf(fixture.space, truncated).has_value());

    auto bad_magic = good;
    bad_magic[1] = 'X';
    CHECK(!load_elf(fixture.space, bad_magic).has_value());

    auto bad_class = good;
    bad_class[4] = 1; // ELFCLASS32
    CHECK(!load_elf(fixture.space, bad_class).has_value());

    auto bad_endian = good;
    bad_endian[5] = 2; // ELFDATA2MSB
    CHECK(!load_elf(fixture.space, bad_endian).has_value());

    auto bad_machine = good;
    put16(bad_machine, 18, 62); // EM_X86_64
    CHECK(!load_elf(fixture.space, bad_machine).has_value());

    auto bad_type = good;
    put16(bad_type, 16, 1); // ET_REL
    CHECK(!load_elf(fixture.space, bad_type).has_value());

    auto no_phdr = good;
    put16(no_phdr, 56, 0);
    CHECK(!load_elf(fixture.space, no_phdr).has_value());

    auto short_phentsize = good;
    put16(short_phentsize, 54, 32);
    CHECK(!load_elf(fixture.space, short_phentsize).has_value());

    auto phdr_out_of_range = good;
    put64(phdr_out_of_range, 32, 0x100000);
    CHECK(!load_elf(fixture.space, phdr_out_of_range).has_value());
}

TEST(elf_loader, rejects_segments_outside_ram) {
    rvvm::test::CpuFixture fixture(1u << 20);
    const auto payload = program_payload({0x00000013u});

    auto high_segment = build_elf(243, 2, rvvm::kDramBase + 0x200000, rvvm::kDramBase + 0x200000,
                                  payload, 4);
    const LoadResult result = load_elf(fixture.space, high_segment);
    CHECK(!result.has_value());
    CHECK(result.error().find("not backed by RAM") != std::string::npos);

    auto bad_entry = build_elf(243, 2, 0x40000000, rvvm::kDramBase, payload, 4);
    const LoadResult entry_result = load_elf(fixture.space, bad_entry);
    CHECK(!entry_result.has_value());
    CHECK(entry_result.error().find("entry point") != std::string::npos);
}

TEST(elf_loader, rejects_inconsistent_segments) {
    rvvm::test::CpuFixture fixture(1u << 20);
    const auto payload = program_payload({0x00000013u});
    auto image = build_elf(243, 2, rvvm::kDramBase, rvvm::kDramBase, payload, 4);

    put64(image, kEhdrSize + 32, 0x100); // p_filesz > the real file size
    CHECK(!load_elf(fixture.space, image).has_value());

    auto filesz_gt_memsz = build_elf(243, 2, rvvm::kDramBase, rvvm::kDramBase, payload, 4);
    put64(filesz_gt_memsz, kEhdrSize + 40, 2); // p_memsz < p_filesz
    CHECK(!load_elf(fixture.space, filesz_gt_memsz).has_value());
}

TEST(raw_loader, loads_flat_binaries) {
    rvvm::test::CpuFixture fixture(1u << 20);
    const std::vector<std::uint8_t> image = {0x13, 0x00, 0x00, 0x00};

    const LoadResult result = load_raw(fixture.space, image, rvvm::kDramBase, std::nullopt);
    CHECK(result.has_value());
    CHECK_EQ(result->entry, rvvm::kDramBase);
    CHECK_EQ(result->highest_address, rvvm::kDramBase + 4);

    u64 value = 0;
    CHECK(fixture.space.load(rvvm::kDramBase, 32, value));
    CHECK_EQ(value, std::uint64_t{0x13});

    const LoadResult with_entry = load_raw(fixture.space, image, rvvm::kDramBase + 0x10, 0x80000100ull);
    CHECK(with_entry.has_value());
    CHECK_EQ(with_entry->entry, std::uint64_t{0x80000100});
}

TEST(raw_loader, rejects_bad_placements) {
    rvvm::test::CpuFixture fixture(1u << 20);
    const std::vector<std::uint8_t> image = {0x13, 0x00, 0x00, 0x00};

    CHECK(!load_raw(fixture.space, image, rvvm::kDramBase + (1u << 20) - 2, std::nullopt).has_value());
    CHECK(!load_raw(fixture.space, {}, rvvm::kDramBase, std::nullopt).has_value());
    CHECK(!load_raw(fixture.space, image, rvvm::kDramBase, 0x10).has_value());
}
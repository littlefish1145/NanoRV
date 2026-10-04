// SPDX-License-Identifier: MIT
#include "core/elf_loader.hpp"

#include "core/log.hpp"

#include <algorithm>
#include <span>

namespace nanorv::core {
namespace {

// ELF constants (generic + RISC-V specific). Kept local so that no <elf.h> is
// required; MSVC does not ship one.
constexpr unsigned kElfClass64 = 2;
constexpr unsigned kElfData2Lsb = 1;
constexpr u16 kEmRiscv = 243;
constexpr u16 kEtExec = 2;
constexpr u16 kEtDyn = 3;
constexpr u32 kPtLoad = 1;
constexpr u64 kEhdrSize = 64;
constexpr u64 kPhdrSize = 56;

u16 read_u16(const std::uint8_t* p) noexcept {
    return static_cast<u16>(static_cast<u16>(p[0]) | (static_cast<u16>(p[1]) << 8));
}

u32 read_u32(const std::uint8_t* p) noexcept {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) |
           (static_cast<u32>(p[3]) << 24);
}

u64 read_u64(const std::uint8_t* p) noexcept {
    return static_cast<u64>(read_u32(p)) | (static_cast<u64>(read_u32(p + 4)) << 32);
}

struct ProgramHeader {
    u32 type = 0;
    u32 flags = 0;
    u64 offset = 0;
    u64 vaddr = 0;
    u64 paddr = 0;
    u64 filesz = 0;
    u64 memsz = 0;
    u64 align = 0;
};

std::string describe_error(const std::vector<std::uint8_t>& image, u64 required, const char* what) {
    return std::string("invalid ELF image: ") + what + " (file is " + std::to_string(image.size()) +
           " bytes, need at least " + std::to_string(required) + ")";
}

} // namespace

LoadResult load_elf(memory::AddressSpace& memory, const std::vector<std::uint8_t>& image) {
    using R = std::unexpected<std::string>;
    if (image.size() < kEhdrSize) {
        return R(describe_error(image, kEhdrSize, "truncated ELF header"));
    }
    const std::uint8_t* const base = image.data();

    if (base[0] != 0x7f || base[1] != 'E' || base[2] != 'L' || base[3] != 'F') {
        return R("invalid ELF image: bad magic");
    }
    if (base[4] != kElfClass64) {
        return R("invalid ELF image: only ELFCLASS64 is supported");
    }
    if (base[5] != kElfData2Lsb) {
        return R("invalid ELF image: only little endian (ELFDATA2LSB) is supported");
    }

    const u16 type = read_u16(base + 16);
    const u16 machine = read_u16(base + 18);
    if (machine != kEmRiscv) {
        return R("invalid ELF image: e_machine is " + std::to_string(machine) +
                 ", expected 243 (EM_RISCV)");
    }
    if (type != kEtExec && type != kEtDyn) {
        return R("invalid ELF image: e_type " + std::to_string(type) +
                 " is not ET_EXEC or ET_DYN");
    }

    const u64 entry = read_u64(base + 24);
    const u64 phoff = read_u64(base + 32);
    const u16 phentsize = read_u16(base + 54);
    const u16 phnum = read_u16(base + 56);

    if (phnum == 0) {
        return R("invalid ELF image: no program headers");
    }
    if (phentsize < kPhdrSize) {
        return R("invalid ELF image: e_phentsize " + std::to_string(phentsize) + " is too small");
    }
    const u64 table_end = phoff + static_cast<u64>(phnum) * phentsize;
    if (phoff < kEhdrSize || table_end < phoff || table_end > image.size()) {
        return R(describe_error(image, table_end, "program header table out of range"));
    }

    LoadedImage result{};
    result.entry = entry;
    result.segment_count = phnum;
    u64 lowest = ~static_cast<u64>(0);
    u64 highest = 0;
    u32 loaded = 0;

    for (u16 i = 0; i < phnum; ++i) {
        const std::uint8_t* const ph = base + phoff + static_cast<u64>(i) * phentsize;
        ProgramHeader header{};
        header.type = read_u32(ph + 0);
        header.flags = read_u32(ph + 4);
        header.offset = read_u64(ph + 8);
        header.vaddr = read_u64(ph + 16);
        header.paddr = read_u64(ph + 24);
        header.filesz = read_u64(ph + 32);
        header.memsz = read_u64(ph + 40);
        header.align = read_u64(ph + 48);

        if (header.type != kPtLoad) {
            NanoRV_DEBUG("elf: skipping program header %u of type %u", i, header.type);
            continue;
        }
        if (header.memsz == 0) {
            continue;
        }
        if (header.filesz > header.memsz) {
            return R("invalid ELF image: segment " + std::to_string(i) +
                     " has p_filesz > p_memsz");
        }
        if (header.offset + header.filesz < header.offset ||
            header.offset + header.filesz > image.size()) {
            return R("invalid ELF image: segment " + std::to_string(i) +
                     " file contents are out of range");
        }
        if (header.vaddr + header.memsz < header.vaddr) {
            return R("invalid ELF image: segment " + std::to_string(i) + " wraps the address space");
        }

        // Stage 1 has no MMU, so p_paddr == p_vaddr is required; Sv39 support in
        // stage 6 keeps honouring p_paddr the same way QEMU does.
        const u64 physical = header.paddr != 0 ? header.paddr : header.vaddr;

        if (!memory.contains(physical) || !memory.contains(physical + header.memsz - 1)) {
            return R("ELF segment " + std::to_string(i) + " [0x" + std::to_string(physical) +
                     ", 0x" + std::to_string(physical + header.memsz) +
                     ") is not backed by RAM; use -m to give the machine more memory");
        }

        if (header.filesz != 0 &&
            !memory.write_bytes(physical, std::span<const std::uint8_t>(
                                             base + header.offset, static_cast<std::size_t>(header.filesz)))) {
            return R("failed to write ELF segment " + std::to_string(i) + " into RAM");
        }
        if (header.memsz > header.filesz &&
            !memory.fill_zero(physical + header.filesz, header.memsz - header.filesz)) {
            return R("failed to zero the .bss tail of ELF segment " + std::to_string(i));
        }

        NanoRV_DEBUG("elf: segment %u paddr=0x%llx vaddr=0x%llx filesz=0x%llx memsz=0x%llx flags=0x%x", i,
                   static_cast<unsigned long long>(physical),
                   static_cast<unsigned long long>(header.vaddr),
                   static_cast<unsigned long long>(header.filesz),
                   static_cast<unsigned long long>(header.memsz), header.flags);

        lowest = std::min(lowest, physical);
        highest = std::max(highest, physical + header.memsz);
        result.bytes_written += header.memsz;
        ++loaded;
    }

    if (loaded == 0) {
        return R("invalid ELF image: no PT_LOAD segments to load");
    }
    if (!memory.contains(entry)) {
        return R("ELF entry point 0x" + std::to_string(entry) + " is not backed by RAM");
    }
    result.lowest_address = lowest;
    result.highest_address = highest;
    return result;
}

LoadResult load_raw(memory::AddressSpace& memory, const std::vector<std::uint8_t>& image, u64 load_address,
                    std::optional<u64> entry) {
    if (image.empty()) {
        return std::unexpected<std::string>("raw image is empty");
    }
    if (!memory.contains(load_address) || !memory.contains(load_address + image.size() - 1)) {
        return std::unexpected<std::string>("raw image does not fit in RAM at 0x" +
                                            std::to_string(load_address));
    }
    if (!memory.write_bytes(load_address, std::span<const std::uint8_t>(image.data(), image.size()))) {
        return std::unexpected<std::string>("failed to write the raw image into RAM");
    }

    LoadedImage result{};
    result.entry = entry.value_or(load_address);
    result.lowest_address = load_address;
    result.highest_address = load_address + image.size();
    result.segment_count = 1;
    result.bytes_written = image.size();
    if (!memory.contains(result.entry)) {
        return std::unexpected<std::string>("raw entry point is not backed by RAM");
    }
    return result;
}

} // namespace nanorv::core
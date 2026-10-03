// SPDX-License-Identifier: MIT
#include "debug/register_dump.hpp"

#include "cpu/csr.hpp"
#include "cpu/disassembler.hpp"
#include "core/log.hpp"

#include <cstdio>

namespace rvvm::debug {
namespace {

std::string hex(u64 value, int width) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "0x%0*llx", width, static_cast<unsigned long long>(value));
    return buffer;
}

std::string signed_decimal(u64 value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(static_cast<i64>(value)));
    return buffer;
}

} // namespace

std::string format_registers(const cpu::Hart& hart) {
    static const char* const kNames[32] = {
        "zero", "ra", "sp", "gp", "tp",  "t0",  "t1", "t2",  "s0", "s1", "a0",  "a1",
        "a2",   "a3", "a4", "a5", "a6",  "a7",  "s2", "s3",  "s4", "s5", "s6",  "s7",
        "s8",   "s9", "s10", "s11", "t3", "t4", "t5", "t6",
    };

    std::string out;
    out.reserve(4096);

    char line[256];
    std::snprintf(line, sizeof(line), "\npc       %s  priv %s  retired %llu\n", hex(hart.pc, 16).c_str(),
                  cpu::to_string(hart.privilege), static_cast<unsigned long long>(hart.retired));
    out += line;

    std::snprintf(line, sizeof(line), "fcsr     0x%02llx (frm %llu, fflags 0x%02llx)\n",
                  static_cast<unsigned long long>(hart.csr.fcsr()),
                  static_cast<unsigned long long>(hart.csr.frm()),
                  static_cast<unsigned long long>(hart.csr.fflags()));
    out += line;

    for (u32 i = 0; i < 32; i += 2) {
        std::snprintf(line, sizeof(line), "%-4s %016llx %-11s %-4s %016llx\n", kNames[i],
                      static_cast<unsigned long long>(hart.x[i]), signed_decimal(hart.x[i]).c_str(),
                      kNames[i + 1], static_cast<unsigned long long>(hart.x[i + 1]));
        out += line;
    }

    std::snprintf(line, sizeof(line), "\nLR/SC reservation: %s\n",
                  hart.reservation_valid ? hex(hart.reservation_address, 16).c_str() : "none");
    out += line;

    out += format_csr_dump(hart.csr, hart.retired);

    std::snprintf(line, sizeof(line), "\nfp registers (raw FLEN=64 bits):\n");
    out += line;
    static const char* const kFNames[32] = {
        "ft0", "ft1", "ft2",  "ft3",  "ft4", "ft5", "ft6",  "ft7",  "fs0", "fs1", "fa0", "fa1",
        "fa2", "fa3", "fa4", "fa5", "fa6", "fa7", "fs2", "fs3",  "fs4", "fs5", "fs6", "fs7",
        "fs8", "fs9", "fs10","fs11", "ft8", "ft9", "ft10", "ft11",
    };
    for (u32 i = 0; i < 32; i += 2) {
        std::snprintf(line, sizeof(line), "%-4s %016llx %-4s %016llx\n", kFNames[i],
                      static_cast<unsigned long long>(hart.f[i]), kFNames[i + 1],
                      static_cast<unsigned long long>(hart.f[i + 1]));
        out += line;
    }
    return out;
}

void dump_registers(const cpu::Hart& hart) { RVVM_INFO("%s", format_registers(hart).c_str()); }

} // namespace rvvm::debug

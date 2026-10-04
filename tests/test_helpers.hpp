// SPDX-License-Identifier: MIT
//
// Shared fixtures for the CPU tests: a RAM backed address space plus a hart,
// with helpers for placing instructions and stepping the reference interpreter.
#pragma once

#include "cpu/execute.hpp"
#include "cpu/hart.hpp"
#include "memory/address_space.hpp"
#include "memory/ram_region.hpp"

#include <vector>

namespace nanorv::test {

struct CpuFixture {
    CpuFixture(u64 ram_bytes = 1u << 20, u64 ram_base = nanorv::kDramBase)
        : ram("dram", ram_bytes, ram_base) {
        memory::Permissions permissions{};
        space.add(ram_base, ram_bytes, &ram, "dram", permissions);
        hart.reset(ram_base);
        hart.csr.set_raw(cpu::kCsrMstatus,hart.csr.raw(cpu::kCsrMstatus)|(1ull<<13));
    }

    void place32(u32 word_index, u32 insn) {
        for (int i = 0; i < 4; ++i) {
            space.store(ram.base() + word_index * 4u + static_cast<u64>(i), 8, (insn >> (i * 8)) & 0xffu);
        }
    }

    void place16(u32 halfword_index, u16 insn) {
        space.store(ram.base() + static_cast<u64>(halfword_index) * 2, 16, insn);
    }

    // Steps once. Returns true when the instruction retired.
    bool step() {
        cpu::Decoded decoded{};
        trap = cpu::Trap{};
        if (!cpu::fetch(space, hart.pc, decoded, trap)) {
            return false;
        }
        hart.next_pc = hart.pc + decoded.length;
        if (!cpu::execute(hart, space, decoded, trap)) {
            return false;
        }
        hart.pc = hart.next_pc;
        return true;
    }

    u64 run(u64 count) {
        u64 done = 0;
        while (done < count && step()) {
            ++done;
        }
        return done;
    }

    memory::AddressSpace space;
    memory::RamRegion ram;
    cpu::Hart hart;
    cpu::Trap trap;
};

// Explicit RISC-V instruction encoders. They mirror the field layout of the
// ISA manual so that the tests do not depend on an installed assembler.
namespace enc {

constexpr u32 opcode_u(u32 imm20, u32 rd, u32 opcode) {
    return ((imm20 & 0xfffffu) << 12) | (rd << 7) | opcode;
}
constexpr u32 opcode_j(i32 imm, u32 rd) {
    const u32 u = static_cast<u32>(imm);
    return (((u >> 20) & 1u) << 31) | (((u >> 1) & 0x3ffu) << 21) | (((u >> 11) & 1u) << 20) |
           (((u >> 12) & 0xffu) << 12) | (rd << 7) | 0x6fu;
}
constexpr u32 opcode_b(i32 imm, u32 rs2, u32 rs1, u32 funct3) {
    const u32 u = static_cast<u32>(imm);
    return (((u >> 12) & 1u) << 31) | (((u >> 5) & 0x3fu) << 25) | (rs2 << 20) | (rs1 << 15) |
           (funct3 << 12) | (((u >> 1) & 0xfu) << 8) | (((u >> 11) & 1u) << 7) | 0x63u;
}
constexpr u32 opcode_s(i32 imm, u32 rs2, u32 rs1, u32 funct3) {
    const u32 u = static_cast<u32>(imm);
    return (((u >> 5) & 0x7fu) << 25) | (rs2 << 20) | (rs1 << 15) | (funct3 << 12) |
           ((u & 0x1fu) << 7) | 0x23u;
}
constexpr u32 opcode_i(i32 imm, u32 rs1, u32 funct3, u32 rd, u32 opcode) {
    return ((static_cast<u32>(imm) & 0xfffu) << 20) | (rs1 << 15) | (funct3 << 12) | (rd << 7) | opcode;
}
constexpr u32 opcode_r(u32 funct7, u32 rs2, u32 rs1, u32 funct3, u32 rd, u32 opcode) {
    return ((funct7 & 0x7fu) << 25) | (rs2 << 20) | (rs1 << 15) | (funct3 << 12) | (rd << 7) | opcode;
}

// Named shorthands used by the tests.
constexpr u32 lui(u32 rd, u32 imm20) { return opcode_u(imm20, rd, 0x37u); }
constexpr u32 auipc(u32 rd, u32 imm20) { return opcode_u(imm20, rd, 0x17u); }
constexpr u32 jal(u32 rd, i32 imm) { return opcode_j(imm, rd); }
constexpr u32 jalr(u32 rd, u32 rs1, i32 imm) { return opcode_i(imm, rs1, 0, rd, 0x67u); }
constexpr u32 branch(i32 imm, u32 rs2, u32 rs1, u32 funct3) { return opcode_b(imm, rs2, rs1, funct3); }
constexpr u32 addi(u32 rd, u32 rs1, i32 imm) { return opcode_i(imm, rs1, 0, rd, 0x13u); }
constexpr u32 slti(u32 rd, u32 rs1, i32 imm) { return opcode_i(imm, rs1, 2, rd, 0x13u); }
constexpr u32 sltiu(u32 rd, u32 rs1, i32 imm) { return opcode_i(imm, rs1, 3, rd, 0x13u); }
constexpr u32 xori(u32 rd, u32 rs1, i32 imm) { return opcode_i(imm, rs1, 4, rd, 0x13u); }
constexpr u32 ori(u32 rd, u32 rs1, i32 imm) { return opcode_i(imm, rs1, 6, rd, 0x13u); }
constexpr u32 andi(u32 rd, u32 rs1, i32 imm) { return opcode_i(imm, rs1, 7, rd, 0x13u); }
constexpr u32 slli(u32 rd, u32 rs1, u32 shamt) { return opcode_i(static_cast<i32>(shamt), rs1, 1, rd, 0x13u); }
constexpr u32 srli(u32 rd, u32 rs1, u32 shamt) { return opcode_i(static_cast<i32>(shamt), rs1, 5, rd, 0x13u); }
constexpr u32 srai(u32 rd, u32 rs1, u32 shamt) {
    return opcode_i(static_cast<i32>(0x400 | shamt), rs1, 5, rd, 0x13u);
}
constexpr u32 addi32(u32 rd, u32 rs1, i32 imm) { return opcode_i(imm, rs1, 0, rd, 0x1bu); }
constexpr u32 slliw(u32 rd, u32 rs1, u32 shamt) { return opcode_i(static_cast<i32>(shamt), rs1, 1, rd, 0x1bu); }
constexpr u32 srliw(u32 rd, u32 rs1, u32 shamt) { return opcode_i(static_cast<i32>(shamt), rs1, 5, rd, 0x1bu); }
constexpr u32 sraiw(u32 rd, u32 rs1, u32 shamt) {
    return opcode_i(static_cast<i32>(0x400 | shamt), rs1, 5, rd, 0x1bu);
}
constexpr u32 add(u32 rd, u32 rs1, u32 rs2) { return opcode_r(0x00, rs2, rs1, 0, rd, 0x33u); }
constexpr u32 sub(u32 rd, u32 rs1, u32 rs2) { return opcode_r(0x20, rs2, rs1, 0, rd, 0x33u); }
constexpr u32 sll(u32 rd, u32 rs1, u32 rs2) { return opcode_r(0x00, rs2, rs1, 1, rd, 0x33u); }
constexpr u32 slt(u32 rd, u32 rs1, u32 rs2) { return opcode_r(0x00, rs2, rs1, 2, rd, 0x33u); }
constexpr u32 sltu(u32 rd, u32 rs1, u32 rs2) { return opcode_r(0x00, rs2, rs1, 3, rd, 0x33u); }
constexpr u32 xor_(u32 rd, u32 rs1, u32 rs2) { return opcode_r(0x00, rs2, rs1, 4, rd, 0x33u); }
constexpr u32 srl(u32 rd, u32 rs1, u32 rs2) { return opcode_r(0x00, rs2, rs1, 5, rd, 0x33u); }
constexpr u32 sra(u32 rd, u32 rs1, u32 rs2) { return opcode_r(0x20, rs2, rs1, 5, rd, 0x33u); }
constexpr u32 or_(u32 rd, u32 rs1, u32 rs2) { return opcode_r(0x00, rs2, rs1, 6, rd, 0x33u); }
constexpr u32 and_(u32 rd, u32 rs1, u32 rs2) { return opcode_r(0x00, rs2, rs1, 7, rd, 0x33u); }
constexpr u32 addw(u32 rd, u32 rs1, u32 rs2) { return opcode_r(0x00, rs2, rs1, 0, rd, 0x3bu); }
constexpr u32 subw(u32 rd, u32 rs1, u32 rs2) { return opcode_r(0x20, rs2, rs1, 0, rd, 0x3bu); }
constexpr u32 sllw(u32 rd, u32 rs1, u32 rs2) { return opcode_r(0x00, rs2, rs1, 1, rd, 0x3bu); }
constexpr u32 srlw(u32 rd, u32 rs1, u32 rs2) { return opcode_r(0x00, rs2, rs1, 5, rd, 0x3bu); }
constexpr u32 sraw(u32 rd, u32 rs1, u32 rs2) { return opcode_r(0x20, rs2, rs1, 5, rd, 0x3bu); }
constexpr u32 load(u32 rd, u32 rs1, i32 imm, u32 funct3) { return opcode_i(imm, rs1, funct3, rd, 0x03u); }
constexpr u32 store(u32 rs2, u32 rs1, i32 imm, u32 funct3) { return opcode_s(imm, rs2, rs1, funct3); }
constexpr u32 ecall() { return 0x00000073u; }
constexpr u32 ebreak() { return 0x00100073u; }
constexpr u32 mret() { return 0x30200073u; }
constexpr u32 wfi() { return 0x10500073u; }
constexpr u32 sfence_vma() { return 0x12000073u; }
constexpr u32 sret() { return 0x10200073u; }
constexpr u32 fence() { return 0x0ff0000fu; }
constexpr u32 fence_i() { return 0x0000100fu; }

// M extension: opcode 0x33 with funct7 == 0x01 (verified against the LLVM
// RISC-V assembler).
constexpr u32 mul(u32 rd, u32 rs1, u32 rs2) { return opcode_r(0x01, rs2, rs1, 0, rd, 0x33u); }
constexpr u32 mulh(u32 rd, u32 rs1, u32 rs2) { return opcode_r(0x01, rs2, rs1, 1, rd, 0x33u); }
constexpr u32 mulhsu(u32 rd, u32 rs1, u32 rs2) { return opcode_r(0x01, rs2, rs1, 2, rd, 0x33u); }
constexpr u32 mulhu(u32 rd, u32 rs1, u32 rs2) { return opcode_r(0x01, rs2, rs1, 3, rd, 0x33u); }
constexpr u32 div_(u32 rd, u32 rs1, u32 rs2) { return opcode_r(0x01, rs2, rs1, 4, rd, 0x33u); }
constexpr u32 divu(u32 rd, u32 rs1, u32 rs2) { return opcode_r(0x01, rs2, rs1, 5, rd, 0x33u); }
constexpr u32 rem_(u32 rd, u32 rs1, u32 rs2) { return opcode_r(0x01, rs2, rs1, 6, rd, 0x33u); }
constexpr u32 remu(u32 rd, u32 rs1, u32 rs2) { return opcode_r(0x01, rs2, rs1, 7, rd, 0x33u); }
constexpr u32 mulw(u32 rd, u32 rs1, u32 rs2) { return opcode_r(0x01, rs2, rs1, 0, rd, 0x3bu); }
constexpr u32 divw(u32 rd, u32 rs1, u32 rs2) { return opcode_r(0x01, rs2, rs1, 4, rd, 0x3bu); }
constexpr u32 divuw(u32 rd, u32 rs1, u32 rs2) { return opcode_r(0x01, rs2, rs1, 5, rd, 0x3bu); }
constexpr u32 remw(u32 rd, u32 rs1, u32 rs2) { return opcode_r(0x01, rs2, rs1, 6, rd, 0x3bu); }
constexpr u32 remuw(u32 rd, u32 rs1, u32 rs2) { return opcode_r(0x01, rs2, rs1, 7, rd, 0x3bu); }

// A extension (verified against the LLVM RISC-V assembler):
//   funct3 = 0b010 -> .W, 0b011 -> .D, funct5 = funct7[6:2],
//   funct7[1] = aq, funct7[0] = rl.
constexpr u32 amo_funct7(u32 funct5, bool aq, bool rl) {
    return ((funct5 & 0x1fu) << 2) | (aq ? 0x2u : 0u) | (rl ? 0x1u : 0u);
}
constexpr u32 amo(u32 funct5, bool aq, bool rl, u32 rd, u32 rs2, u32 rs1, bool word) {
    return (amo_funct7(funct5, aq, rl) << 25) | (rs2 << 20) | (rs1 << 15) | (word ? 2u : 3u) << 12 |
           (rd << 7) | 0x2fu;
}
constexpr u32 amoadd(u32 rd, u32 rs2, u32 rs1, bool word) { return amo(0x00, false, false, rd, rs2, rs1, word); }
constexpr u32 amoswap(u32 rd, u32 rs2, u32 rs1, bool word) { return amo(0x01, false, false, rd, rs2, rs1, word); }
constexpr u32 lr(u32 rd, u32 rs1, bool word) { return amo(0x02, false, false, rd, 0, rs1, word); }
constexpr u32 lr_aqrl(u32 rd, u32 rs1, bool word) { return amo(0x02, true, true, rd, 0, rs1, word); }
constexpr u32 sc(u32 rd, u32 rs2, u32 rs1, bool word) { return amo(0x03, false, false, rd, rs2, rs1, word); }
constexpr u32 amoxor(u32 rd, u32 rs2, u32 rs1, bool word) { return amo(0x04, false, false, rd, rs2, rs1, word); }
constexpr u32 amoor(u32 rd, u32 rs2, u32 rs1, bool word) { return amo(0x08, false, false, rd, rs2, rs1, word); }
constexpr u32 amoand(u32 rd, u32 rs2, u32 rs1, bool word) { return amo(0x0c, false, false, rd, rs2, rs1, word); }
constexpr u32 amomin(u32 rd, u32 rs2, u32 rs1, bool word) { return amo(0x10, false, false, rd, rs2, rs1, word); }
constexpr u32 amomax(u32 rd, u32 rs2, u32 rs1, bool word) { return amo(0x14, false, false, rd, rs2, rs1, word); }
constexpr u32 amominu(u32 rd, u32 rs2, u32 rs1, bool word) { return amo(0x18, false, false, rd, rs2, rs1, word); }
constexpr u32 amomaxu(u32 rd, u32 rs2, u32 rs1, bool word) { return amo(0x1c, false, false, rd, rs2, rs1, word); }

// Zicsr. The immediate is zero extended from 5 bits and lives in the rs1
// field, which matches the encodings produced by the LLVM RISC-V assembler.
constexpr u32 csr(u32 address, u32 rd, u32 funct3, u32 rs1) {
    return ((address & 0xfffu) << 20) | (rs1 << 15) | ((funct3 & 0x7u) << 12) | (rd << 7) | 0x73u;
}
constexpr u32 csrr(u32 rd, u32 address) { return csr(address, rd, 2, 0); }
constexpr u32 csrrs(u32 rd, u32 address, u32 rs1) { return csr(address, rd, 2, rs1); }
constexpr u32 csrrc(u32 rd, u32 address, u32 rs1) { return csr(address, rd, 3, rs1); }
constexpr u32 csrrw(u32 rd, u32 address, u32 rs1) { return csr(address, rd, 1, rs1); }
constexpr u32 csrrwi(u32 address, u32 rd, u32 zimm) { return csr(address, rd, 5, zimm & 0x1fu); }
constexpr u32 csrrsi(u32 address, u32 rd, u32 zimm) { return csr(address, rd, 6, zimm & 0x1fu); }
constexpr u32 csrrci(u32 address, u32 rd, u32 zimm) { return csr(address, rd, 7, zimm & 0x1fu); }
constexpr u32 csrw(u32 address, u32 rs1) { return csr(address, 0, 1, rs1); }
constexpr u32 csrs(u32 address, u32 rs1) { return csr(address, 0, 2, rs1); }
constexpr u32 csrc(u32 address, u32 rs1) { return csr(address, 0, 3, rs1); }

constexpr u32 fld(u32 rd, u32 rs1, i32 imm) { return opcode_i(imm, rs1, 3, rd, 0x07u); }
constexpr u32 fsd(u32 rs2, u32 rs1, i32 imm) { return opcode_s(imm, rs2, rs1, 3); }
constexpr u32 nop() { return addi(0, 0, 0); }

} // namespace enc

} // namespace nanorv::test

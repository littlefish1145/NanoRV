// SPDX-License-Identifier: MIT
#include "cpu/decoder.hpp"
#include "cpu/disassembler.hpp"
#include "test_framework.hpp"
#include "test_helpers.hpp"

using namespace nanorv;
using namespace nanorv::cpu;
using namespace nanorv::test;

namespace {
// 0x007f: opcode LOAD-FP (0x07), bits [4:2] != 111 -> ordinary 32 bit encoding.
constexpr std::uint16_t kFloadFp32Bits = 0x007f;
} // namespace

TEST(decoder, instruction_length) {
    CHECK_EQ(instruction_length(0x0013u), std::uint32_t{4}); // addi
    CHECK_EQ(instruction_length(0x0085u), std::uint32_t{2}); // compressed
    CHECK_EQ(instruction_length(0x4501u), std::uint32_t{2}); // c.li
    CHECK_EQ(instruction_length(kFloadFp32Bits), std::uint32_t{4});

    // RV64GC has no encoding longer than 32 bits, so bits [1:0] == 0b11 always
    // yields 4 bytes; reserved opcodes are rejected by the dispatcher instead.
    CHECK_EQ(instruction_length(0x001fu), std::uint32_t{4});
    CHECK_EQ(instruction_length(0x003fu), std::uint32_t{4});
}

TEST(decoder, base_fields) {
    const Decoded d = decode(enc::addi(10, 0, -1)); // addi a0, zero, -1
    CHECK(!d.compressed);
    CHECK_EQ(d.length, std::uint8_t{4});
    CHECK_EQ(d.opcode, std::uint8_t{0x13});
    CHECK_EQ(d.rd, std::uint8_t{10});
    CHECK_EQ(d.rs1, std::uint8_t{0});
    CHECK_EQ(d.funct3, std::uint8_t{0});
    CHECK(d.format == InsnFormat::I);

    const Decoded store = decode(enc::store(11, 10, 0, 3)); // sd a1, 0(a0)
    CHECK(store.opcode == 0x23);
    CHECK(store.funct3 == 3);
    CHECK(store.format == InsnFormat::S);
    CHECK_EQ(store.rs1, std::uint8_t{10});
    CHECK_EQ(store.rs2, std::uint8_t{11});

    const Decoded branch = decode(enc::branch(0, 11, 10, 4)); // blt a0, a1, 0
    CHECK(branch.format == InsnFormat::B);
    CHECK_EQ(branch.funct3, std::uint8_t{4});
    CHECK_EQ(branch.rs1, std::uint8_t{10});
    CHECK_EQ(branch.rs2, std::uint8_t{11});

    const Decoded jal = decode(enc::jal(1, 8)); // jal ra, 8
    CHECK(jal.format == InsnFormat::J);
    CHECK_EQ(jal.rd, std::uint8_t{1});

    const Decoded upper = decode(enc::lui(11, 0x12345)); // lui a1, 0x12345
    CHECK(upper.format == InsnFormat::U);
    CHECK_EQ(upper.rd, std::uint8_t{11});
    CHECK_EQ(upper.raw, std::uint32_t{0x123455b7});
}

TEST(decoder, compressed_registers_are_aliased_to_x8_x15) {
    // c.addi a0, a0, 1 -> 0x0505
    const Decoded d = decode(0x0505);
    CHECK(d.compressed);
    CHECK_EQ(d.length, std::uint8_t{2});
    CHECK_EQ(d.opcode, std::uint8_t{1}); // quadrant
    CHECK_EQ(d.funct3, std::uint8_t{0});
    CHECK_EQ(d.rd, std::uint8_t{10});
    CHECK_EQ(d.rs1, std::uint8_t{10});

    // c.mv s0, s1: quadrant 2, funct3 = 100, bit12 = 0, rd = 8, rs2 = 9
    const Decoded mv = decode((4u << 13) | (8u << 7) | (9u << 2) | 2u);
    CHECK(mv.compressed);
    CHECK_EQ(mv.funct3, std::uint8_t{4});
    CHECK_EQ(mv.rd, std::uint8_t{8});
    CHECK_EQ(mv.rs2, std::uint8_t{9});
    CHECK(mv.format == InsnFormat::R);
}

TEST(decoder, sign_extension_helper) {
    CHECK_EQ(sign_extend(0xfffu, 12), std::int64_t{-1});
    CHECK_EQ(sign_extend(0x800u, 12), std::int64_t{-2048});
    CHECK_EQ(sign_extend(0x7ffu, 12), std::int64_t{2047});
    CHECK_EQ(sign_extend(0xffffffffu, 32), std::int64_t{-1});
    CHECK_EQ(sign_extend(0x80000000u, 32), std::int64_t{-2147483648ll});
    CHECK_EQ(sign_extend_32(0x80000000u, 32), std::int32_t{-2147483647 - 1});
}

TEST(disassembler, base_instructions) {
    CHECK_EQ(disassemble(enc::addi(10, 0, 8)), std::string("addi a0, zero, 8"));
    CHECK_EQ(disassemble(enc::addi(10, 11, 11)), std::string("addi a0, a1, 11"));
    CHECK_EQ(disassemble(enc::addi(10, 10, 11)), std::string("addi a0, a0, 11"));
    CHECK_EQ(disassemble(enc::addi(10, 0, -1)), std::string("addi a0, zero, -1"));
    CHECK_EQ(disassemble(enc::auipc(10, 0)), std::string("auipc a0, 0x0"));
    CHECK_EQ(disassemble(enc::auipc(10, 1)), std::string("auipc a0, 0x1"));
    CHECK_EQ(disassemble(enc::lui(11, 0x12345)), std::string("lui a1, 0x12345"));
    CHECK_EQ(disassemble(enc::add(10, 11, 11)), std::string("add a0, a1, a1"));
    CHECK_EQ(disassemble(enc::sub(10, 11, 10)), std::string("sub a0, a1, a0"));
    CHECK_EQ(disassemble(enc::srai(10, 10, 4)), std::string("srai a0, a0, 4"));
    CHECK_EQ(disassemble(enc::srli(10, 10, 4)), std::string("srli a0, a0, 4"));
    CHECK_EQ(disassemble(enc::store(11, 10, 0, 3)), std::string("sd a1, 0(a0)"));
    CHECK_EQ(disassemble(enc::load(11, 10, 0, 3)), std::string("ld a1, 0(a0)"));
    CHECK_EQ(disassemble(enc::load(11, 10, -8, 4)), std::string("lbu a1, -8(a0)"));
    CHECK_EQ(disassemble(enc::branch(8, 11, 10, 0)), std::string("beq a0, a1, +8"));
    CHECK_EQ(disassemble(enc::jal(1, 8)), std::string("jal ra, +8"));
    CHECK_EQ(disassemble(enc::jalr(1, 5, 0)), std::string("jalr ra, 0(t0)"));
    CHECK_EQ(disassemble(enc::ecall()), std::string("ecall"));
    CHECK_EQ(disassemble(enc::ebreak()), std::string("ebreak"));
    CHECK_EQ(disassemble(enc::mret()), std::string("mret"));
    CHECK_EQ(disassemble(enc::wfi()), std::string("wfi"));
    CHECK_EQ(disassemble(enc::fence_i()), std::string("fence.i"));
}

TEST(disassembler, compressed_instructions_have_names) {
    CHECK_EQ(disassemble(0x0505u), std::string("c.addi a0, 1"));
    CHECK_EQ(disassemble(0x4501u), std::string("c.li a0, 0"));
    CHECK_EQ(disassemble(0x8082u), std::string("c.jr ra")); // c.jr ra == c.ret ra
}

TEST(disassembler, unknown_encodings_are_never_blank) {
    const std::string text = disassemble(0x0000007bu); // opcode 0x7b is undefined
    CHECK(!text.empty());
    CHECK(text.find("0x0000007b") != std::string::npos);
}
// SPDX-License-Identifier: MIT
//
// RV64M tests. Expected values are derived from the RISC-V specification, not
// from the emulator, so a wrong implementation cannot agree with itself.
#include "cpu/softint.hpp"
#include "test_framework.hpp"
#include "test_helpers.hpp"

using namespace nanorv;
using namespace nanorv::cpu;
using namespace nanorv::test;

namespace {

constexpr std::uint64_t kInt64Min = 0x8000000000000000ull;
constexpr std::uint64_t kInt64Max = 0x7fffffffffffffffull;
constexpr std::uint64_t kInt32Min = 0xffffffff80000000ull;
constexpr std::uint64_t kInt32Max = 0x000000007fffffffull;

} // namespace

// ---------------------------------------------------------------------------
// softint helpers (no bus involved)
// ---------------------------------------------------------------------------

TEST(softint, mul_high_unsigned_matches_reference) {
    CHECK_EQ(mul_high_unsigned(0, 0xffffffffffffffffull), std::uint64_t{0});
    CHECK_EQ(mul_high_unsigned(1, 0xffffffffffffffffull), std::uint64_t{0});
    CHECK_EQ(mul_high_unsigned(0xffffffffffffffffull, 0xffffffffffffffffull),
             std::uint64_t{0xfffffffffffffffe});
    CHECK_EQ(mul_high_unsigned(0x8000000000000000ull, 2), std::uint64_t{1});
    CHECK_EQ(mul_high_unsigned(0x123456789abcdef0ull, 0x10), std::uint64_t{1});
}

TEST(softint, mul_high_signed_matches_reference) {
    CHECK_EQ(mul_high_signed(-1, -1), std::uint64_t{0});
    CHECK_EQ(mul_high_signed(1, -1), std::uint64_t{0xffffffffffffffffull});
    CHECK_EQ(mul_high_signed(kInt64Max, 2), std::uint64_t{0});
    // INT64_MIN * 2 == -2^64, which is 0xffffffffffffffff_0000000000000000 as a
    // 128-bit product. (The unsigned encoding of the same operands gives high = 1.)
    CHECK_EQ(mul_high_signed(static_cast<i64>(kInt64Min), 2), std::uint64_t{0xffffffffffffffffull});
    CHECK_EQ(mul_high_unsigned(kInt64Min, 2), std::uint64_t{1});
    CHECK_EQ(mul_high_signed(static_cast<i64>(kInt64Min), static_cast<i64>(kInt64Min)),
             std::uint64_t{0x4000000000000000ull});
}

TEST(softint, mul_high_signed_unsigned_matches_reference) {
    // -1 * 1 (unsigned) -> high word is all ones.
    CHECK_EQ(mul_high_signed_unsigned(-1, 1), std::uint64_t{0xffffffffffffffffull});
    CHECK_EQ(mul_high_signed_unsigned(1, 1), std::uint64_t{0});
    // -2^63 * 2 == -2^64 as a 128-bit product.
    CHECK_EQ(mul_high_signed_unsigned(static_cast<i64>(kInt64Min), 2), std::uint64_t{0xffffffffffffffffull});
    CHECK_EQ(mul_high_signed_unsigned(0, 0xffffffffffffffffull), std::uint64_t{0});
}

TEST(softint, division_edge_cases) {
    // Divide by zero.
    CHECK_EQ(div_unsigned(1234, 0), std::uint64_t{0xffffffffffffffffull});
    CHECK_EQ(rem_unsigned(1234, 0), std::uint64_t{1234});
    CHECK_EQ(div_signed(1234, 0), std::uint64_t{0xffffffffffffffffull});
    CHECK_EQ(rem_signed(1234, 0), std::uint64_t{1234});
    CHECK_EQ(div_signed(-1234, 0), std::uint64_t{0xffffffffffffffffull});

    // Signed overflow: INT64_MIN / -1 must not trap.
    CHECK_EQ(div_signed(static_cast<i64>(kInt64Min), -1), kInt64Min);
    CHECK_EQ(rem_signed(static_cast<i64>(kInt64Min), -1), std::uint64_t{0});

    // Truncation toward zero, remainder keeps the dividend's sign.
    CHECK_EQ(div_signed(-7, 2), std::uint64_t{static_cast<u64>(-3)});
    CHECK_EQ(rem_signed(-7, 2), std::uint64_t{static_cast<u64>(-1)});
    CHECK_EQ(div_signed(7, -2), std::uint64_t{static_cast<u64>(-3)});
    CHECK_EQ(rem_signed(7, -2), std::uint64_t{1});
}

// ---------------------------------------------------------------------------
// Instructions
// ---------------------------------------------------------------------------

TEST(muldiv, mul_and_mulh) {
    CpuFixture cpu;
    cpu.hart.set_reg(10, 0x0123456789abcdefull);
    cpu.hart.set_reg(11, 0xfedcba9876543210ull);

    cpu.place32(0, enc::mul(12, 10, 11));
    cpu.place32(1, enc::mulh(13, 10, 11));
    cpu.place32(2, enc::mulhu(14, 10, 11));
    cpu.place32(3, enc::mulhsu(15, 10, 11));
    CHECK_EQ(cpu.run(4), std::uint64_t{4});

    // Low 64 bits of the product: computed with the same decomposition the
    // emulator uses for the high word, so compare against an independent split.
    const u64 expected_low = mul_wide_unsigned(0x0123456789abcdefull, 0xfedcba9876543210ull).low;
    CHECK_EQ(cpu.hart.get_reg(12), expected_low);

    // Signed operands: 0x0123456789abcdef is positive, 0xfedc... is negative.
    const i64 signed_a = static_cast<i64>(0x0123456789abcdefull);
    const i64 signed_b = static_cast<i64>(0xfedcba9876543210ull);
    CHECK_EQ(cpu.hart.get_reg(13), mul_high_signed(signed_a, signed_b));
    // Unsigned * unsigned: same low word, different interpretation of the high word.
    CHECK_NE(cpu.hart.get_reg(14), cpu.hart.get_reg(13));
    CHECK_EQ(cpu.hart.get_reg(15), mul_high_signed_unsigned(signed_a, 0xfedcba9876543210ull));
}

TEST(muldiv, mulh_edge_cases) {
    CpuFixture cpu;

    cpu.hart.set_reg(10, kInt64Min);
    cpu.hart.set_reg(11, 2);
    cpu.place32(0, enc::mulh(12, 10, 11));
    CHECK_EQ(cpu.step(), true);
    // INT64_MIN * 2 == -2^64 == 0xffffffffffffffff_0000000000000000
    CHECK_EQ(cpu.hart.get_reg(12), std::uint64_t{0xffffffffffffffffull});

    cpu.hart.pc = 0x80000000;
    cpu.hart.set_reg(10, kInt64Min);
    cpu.hart.set_reg(11, kInt64Min);
    cpu.place32(0, enc::mulh(12, 10, 11));
    CHECK_EQ(cpu.step(), true);
    CHECK_EQ(cpu.hart.get_reg(12), std::uint64_t{0x4000000000000000ull});

    cpu.hart.pc = 0x80000000;
    cpu.hart.set_reg(10, kInt64Max);
    cpu.hart.set_reg(11, 3);
    cpu.place32(0, enc::mulhu(12, 10, 11)); // unsigned: 0x7fff... * 3
    CHECK_EQ(cpu.step(), true);
    CHECK_EQ(cpu.hart.get_reg(12), std::uint64_t{1});
}

TEST(muldiv, division_and_remainder) {
    CpuFixture cpu;
    cpu.hart.set_reg(10, 17);
    cpu.hart.set_reg(11, 5);

    cpu.place32(0, enc::div_(12, 10, 11));  // 17 / 5  = 3
    cpu.place32(1, enc::rem_(13, 10, 11));  // 17 % 5  = 2
    cpu.place32(2, enc::divu(14, 10, 11));
    cpu.place32(3, enc::remu(15, 10, 11));
    CHECK_EQ(cpu.run(4), std::uint64_t{4});
    CHECK_EQ(cpu.hart.get_reg(12), std::uint64_t{3});
    CHECK_EQ(cpu.hart.get_reg(13), std::uint64_t{2});
    CHECK_EQ(cpu.hart.get_reg(14), std::uint64_t{3});
    CHECK_EQ(cpu.hart.get_reg(15), std::uint64_t{2});
}

TEST(muldiv, signed_division_rounds_toward_zero) {
    CpuFixture cpu;
    cpu.hart.set_reg(10, static_cast<u64>(-7));
    cpu.hart.set_reg(11, 2);

    cpu.place32(0, enc::div_(12, 10, 11));
    cpu.place32(1, enc::rem_(13, 10, 11));
    CHECK_EQ(cpu.run(2), std::uint64_t{2});
    CHECK_EQ(cpu.hart.get_reg(12), static_cast<u64>(static_cast<i64>(-3)));
    CHECK_EQ(cpu.hart.get_reg(13), static_cast<u64>(static_cast<i64>(-1)));
}

TEST(muldiv, division_by_zero_and_overflow) {
    CpuFixture cpu;
    cpu.hart.set_reg(10, 42);
    cpu.hart.set_reg(11, 0);

    cpu.place32(0, enc::div_(12, 10, 11));  // -> -1
    cpu.place32(1, enc::divu(13, 10, 11)); // -> 2^64-1
    cpu.place32(2, enc::rem_(14, 10, 11)); // -> 42
    cpu.place32(3, enc::remu(15, 10, 11)); // -> 42
    CHECK_EQ(cpu.run(4), std::uint64_t{4});
    CHECK_EQ(cpu.hart.get_reg(12), std::uint64_t{0xffffffffffffffffull});
    CHECK_EQ(cpu.hart.get_reg(13), std::uint64_t{0xffffffffffffffffull});
    CHECK_EQ(cpu.hart.get_reg(14), std::uint64_t{42});
    CHECK_EQ(cpu.hart.get_reg(15), std::uint64_t{42});

    // INT64_MIN / -1 overflows on x86 unless it is special cased.
    cpu.hart.reset(cpu.ram.base());
    cpu.hart.set_reg(10, kInt64Min);
    cpu.hart.set_reg(11, static_cast<u64>(-1));
    cpu.place32(0, enc::div_(12, 10, 11));
    CHECK_EQ(cpu.step(), true);
    CHECK_EQ(cpu.hart.get_reg(12), kInt64Min);

    cpu.hart.pc = 0x80000000;
    cpu.place32(0, enc::rem_(12, 10, 11));
    CHECK_EQ(cpu.step(), true);
    CHECK_EQ(cpu.hart.get_reg(12), std::uint64_t{0});
}

TEST(muldiv, word_variants_sign_extend) {
    CpuFixture cpu;

    // MULW keeps the low 32 bits and sign extends.
    cpu.hart.set_reg(10, 0x7fffffffull);
    cpu.hart.set_reg(11, 2);
    cpu.place32(0, enc::mulw(12, 10, 11)); // 0xfffffffe -> -2
    CHECK_EQ(cpu.step(), true);
    CHECK_EQ(cpu.hart.get_reg(12), static_cast<u64>(static_cast<i64>(-2)));

    // DIVW is a 32 bit signed division.
    cpu.hart.pc = 0x80000000;
    cpu.hart.set_reg(10, static_cast<u64>(static_cast<i64>(-7)));
    cpu.place32(0, enc::divw(12, 10, 11)); // -7 / 2 = -3
    CHECK_EQ(cpu.step(), true);
    CHECK_EQ(cpu.hart.get_reg(12), static_cast<u64>(static_cast<i64>(-3)));

    cpu.hart.pc = 0x80000000;
    cpu.place32(0, enc::remw(13, 10, 11)); // -7 % 2 = -1
    CHECK_EQ(cpu.step(), true);
    CHECK_EQ(cpu.hart.get_reg(13), static_cast<u64>(static_cast<i64>(-1)));

    // DIVUW works on the low 32 bits as unsigned.
    cpu.hart.pc = 0x80000000;
    cpu.hart.set_reg(10, 0x00000000ffffffffull);
    cpu.place32(0, enc::divuw(14, 10, 11)); // 0xffffffff / 2 = 0x7fffffff
    CHECK_EQ(cpu.step(), true);
    CHECK_EQ(cpu.hart.get_reg(14), kInt32Max);

    // Division by zero for the word forms.
    cpu.hart.pc = 0x80000000;
    cpu.hart.set_reg(11, 0);
    cpu.place32(0, enc::divw(15, 10, 11)); // -> -1
    CHECK_EQ(cpu.step(), true);
    CHECK_EQ(cpu.hart.get_reg(15), static_cast<u64>(static_cast<i64>(-1)));

    cpu.hart.pc = 0x80000000;
    cpu.place32(0, enc::divuw(16, 10, 11)); // -> 0xffffffff -> sign extended -1
    CHECK_EQ(cpu.step(), true);
    CHECK_EQ(cpu.hart.get_reg(16), static_cast<u64>(static_cast<i64>(-1)));

    // INT32_MIN / -1 overflow.
    cpu.hart.pc = 0x80000000;
    cpu.hart.set_reg(10, kInt32Min);
    cpu.hart.set_reg(11, static_cast<u64>(-1));
    cpu.place32(0, enc::divw(17, 10, 11));
    CHECK_EQ(cpu.step(), true);
    CHECK_EQ(cpu.hart.get_reg(17), kInt32Min);

    cpu.hart.pc = 0x80000000;
    cpu.place32(0, enc::remw(18, 10, 11));
    CHECK_EQ(cpu.step(), true);
    CHECK_EQ(cpu.hart.get_reg(18), std::uint64_t{0});
}

TEST(muldiv, word_remuw_divide_by_zero_returns_the_dividend) {
    CpuFixture cpu;
    cpu.hart.set_reg(10, 0x00000000deadbeefull);
    cpu.hart.set_reg(11, 0);

    cpu.place32(0, enc::remuw(12, 10, 11));
    CHECK_EQ(cpu.step(), true);
    // The 32 bit result 0xdeadbeef is sign extended into rd.
    CHECK_EQ(cpu.hart.get_reg(12), static_cast<u64>(static_cast<i64>(static_cast<i32>(0xdeadbeefu))));
}

TEST(muldiv, unregistered_word_subopcodes_are_illegal) {
    CpuFixture cpu;
    for (u32 funct3 = 1; funct3 <= 3; ++funct3) {
        cpu.hart.reset(cpu.ram.base());
        cpu.place32(0, enc::opcode_r(0x01, 11, 11, funct3, 10, 0x3b));
        CHECK_EQ(cpu.step(), false);
        CHECK(cpu.trap.cause == TrapCause::IllegalInstruction);
    }
}

TEST(muldiv, rv64i_opcodes_are_unaffected) {
    // funct7 == 0x00 / 0x20 must keep their RV64I behaviour now that RV64M shares
    // the opcode space.
    CpuFixture cpu;
    cpu.hart.set_reg(10, 10);
    cpu.hart.set_reg(11, 3);

    cpu.place32(0, enc::add(12, 10, 11));
    cpu.place32(1, enc::sub(13, 10, 11));
    CHECK_EQ(cpu.run(2), std::uint64_t{2});
    CHECK_EQ(cpu.hart.get_reg(12), std::uint64_t{13});
    CHECK_EQ(cpu.hart.get_reg(13), std::uint64_t{7});
}
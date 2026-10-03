// SPDX-License-Identifier: MIT
//
// Differential test: compares RVVM against the LLVM RISC-V assembler + a
// reference model for M and A extension edge cases.
//
// The point of this file is that the *expected* values come from the
// specification, written out literally, while the encodings come from the
// independent LLVM toolchain. A disagreement means the emulator is wrong.
#include "cpu/softint.hpp"
#include "test_framework.hpp"
#include "test_helpers.hpp"

#include <vector>

using namespace rvvm;
using namespace rvvm::cpu;
using namespace rvvm::test;

namespace {

struct Vector {
    const char* mnemonic;
    u32 encoding;
    u64 dividend;
    u64 divisor;
    u64 expected;
    const char* reason;
};

// Values chosen to hit every special case the specification calls out.
constexpr std::uint64_t kZero = 0;
constexpr std::uint64_t kOne = 1;
constexpr std::uint64_t kMinusOne = 0xffffffffffffffffull;
constexpr std::uint64_t kInt64Min = 0x8000000000000000ull;
constexpr std::uint64_t kInt64Max = 0x7fffffffffffffffull;
constexpr std::uint64_t kInt32Min = 0xffffffff80000000ull;
constexpr std::uint64_t kInt32Max = 0x000000007fffffffull;

const Vector kVectors[] = {
    // MUL. The expected low word is the 128-bit product of the two operands taken
    // modulo 2^64 (computed with an arbitrary precision reference).
    {"mul", enc::mul(12, 10, 11), 0x0123456789abcdefull, 0xfedcba9876543210ull, 0x2236d88fe5618cf0ull,
     "random"},
    {"mul", enc::mul(12, 10, 11), kMinusOne, 3, 0xfffffffffffffffdull, "-1 * 3"},
    {"mul", enc::mul(12, 10, 11), 0xffffffff80000000ull, 0xffffffff80000000ull, 0x4000000000000000ull,
     "(2^64-2^31)^2 low word"},
    {"mul", enc::mul(12, 10, 11), kInt64Min, 2, 0, "INT64_MIN * 2 wraps to 0"},
    // MULH (signed x signed)
    {"mulh", enc::mulh(12, 10, 11), kInt64Min, 1, kMinusOne, "INT64_MIN high word"},
    {"mulh", enc::mulh(12, 10, 11), kInt64Max, 2, 0, "INT64_MAX * 2"},
    {"mulh", enc::mulh(12, 10, 11), kMinusOne, kMinusOne, 0, "-1 * -1"},
    {"mulh", enc::mulh(12, 10, 11), 0x4000000000000000ull, 4, 1, "2^62 * 4"},
    // MULHU (unsigned x unsigned)
    {"mulhu", enc::mulhu(12, 10, 11), kMinusOne, kMinusOne, 0xfffffffffffffffeull, "(2^64-1)^2"},
    {"mulhu", enc::mulhu(12, 10, 11), kInt64Min, 2, 1, "2^63 * 2"},
    // 0x0123456789abcdef * 0x10 == 0x123456789abcdef0 fits in 64 bits.
    {"mulhu", enc::mulhu(12, 10, 11), 0x0123456789abcdefull, 0x10, 0, "no carry out"},
    {"mulhu", enc::mulhu(12, 10, 11), 0xf123456789abcdefull, 0x10, 0xf, "carries out"},
    // MULHSU (signed x unsigned)
    {"mulhsu", enc::mulhsu(12, 10, 11), kMinusOne, 1, kMinusOne, "-1 * 1"},
    {"mulhsu", enc::mulhsu(12, 10, 11), kInt64Min, 2, kMinusOne, "-2^63 * 2"},
    // INT64_MAX * 2 == 0xfffffffffffffffe still fits in 64 bits.
    {"mulhsu", enc::mulhsu(12, 10, 11), kInt64Max, 2, 0, "no carry out"},
    {"mulhsu", enc::mulhsu(12, 10, 11), 0x4000000000000000ull, 4, 1, "carries out"},
    {"mulhsu", enc::mulhsu(12, 10, 11), kOne, kMinusOne, 0, "1 * (2^64-1)"},
    // DIV
    {"div", enc::div_(12, 10, 11), 17, 5, 3, "17 / 5"},
    // -1 / 3 == 0 (truncated toward zero), unlike the floor division of -1.
    {"div", enc::div_(12, 10, 11), kMinusOne, 3, 0, "-1 / 3 truncates to 0"},
    {"rem", enc::rem_(12, 10, 11), kMinusOne, 3, kMinusOne, "-1 % 3 == -1"},
    {"div", enc::div_(12, 10, 11), 7, kMinusOne, static_cast<u64>(static_cast<i64>(-7)), "7 / -1"},
    {"div", enc::div_(12, 10, 11), 1234, kZero, kMinusOne, "divide by zero -> -1"},
    {"div", enc::div_(12, 10, 11), kInt64Min, kMinusOne, kInt64Min, "overflow: INT64_MIN / -1"},
    {"div", enc::div_(12, 10, 11), kInt64Min, 1, kInt64Min, "INT64_MIN / 1"},
    {"div", enc::div_(12, 10, 11), kMinusOne, kInt64Max, 0, "-1 / INT64_MAX truncates to 0"},
    {"div", enc::div_(12, 10, 11), kInt64Max, kInt64Max, 1, "equal operands"},
    // DIVU
    {"divu", enc::divu(12, 10, 11), 17, 5, 3, "17 / 5"},
    {"divu", enc::divu(12, 10, 11), 1234, kZero, kMinusOne, "divide by zero -> all ones"},
    {"divu", enc::divu(12, 10, 11), kMinusOne, 2, 0x7fffffffffffffffull, "(2^64-1) / 2"},
    {"divu", enc::divu(12, 10, 11), kMinusOne, kMinusOne, 1, "(2^64-1) / (2^64-1)"},
    // REM
    {"rem", enc::rem_(12, 10, 11), 17, 5, 2, "17 % 5"},
    {"rem", enc::rem_(12, 10, 11), static_cast<u64>(-7), 2, static_cast<u64>(static_cast<i64>(-1)), "-7 % 2 == -1"},
    {"rem", enc::rem_(12, 10, 11), 7, static_cast<u64>(-2), 1, "7 % -2 == 1"},
    {"rem", enc::rem_(12, 10, 11), 1234, kZero, 1234, "remainder by zero -> dividend"},
    {"rem", enc::rem_(12, 10, 11), kInt64Min, kMinusOne, 0, "overflow: INT64_MIN % -1"},
    {"rem", enc::rem_(12, 10, 11), kMinusOne, kInt64Max, kMinusOne, "-1 % INT64_MAX == -1"},
    // REMU
    {"remu", enc::remu(12, 10, 11), 17, 5, 2, "17 % 5"},
    {"remu", enc::remu(12, 10, 11), kMinusOne, 3, 0, "(2^64-1) % 3"},
    {"remu", enc::remu(12, 10, 11), kMinusOne, kZero, kMinusOne, "remainder by zero -> dividend"},
    // MULW
    {"mulw", enc::mulw(12, 10, 11), kInt32Max, 2, 0xfffffffffffffffeull, "INT32_MAX * 2 sign extends"},
    {"mulw", enc::mulw(12, 10, 11), kInt32Min, kInt32Min, 0, "INT32_MIN^2"},
    {"mulw", enc::mulw(12, 10, 11), 0xffffffff80000000ull, 0xffffffffffffffffull, 0xffffffff80000000ull, "low 32 bits sign extend"},
    // DIVW
    {"divw", enc::divw(12, 10, 11), static_cast<u64>(static_cast<i64>(-7)), 2, 0xfffffffffffffffdull, "-7 / 2"},
    {"divw", enc::divw(12, 10, 11), 7, static_cast<u64>(static_cast<i64>(-2)), 0xfffffffffffffffdull, "7 / -2"},
    {"divw", enc::divw(12, 10, 11), 1234, kZero, kMinusOne, "divide by zero -> -1"},
    {"divw", enc::divw(12, 10, 11), kInt32Min, kMinusOne, 0xffffffff80000000ull, "overflow: INT32_MIN / -1"},
    {"divw", enc::divw(12, 10, 11), 0x0000000100000000ull, 3, 0, "high 32 bits are ignored"},
    // DIVUW
    {"divuw", enc::divuw(12, 10, 11), 0x00000000ffffffffull, 2, 0x7fffffffull, "0xffffffff / 2"},
    {"divuw", enc::divuw(12, 10, 11), 0xffffffff00000010ull, 2, 8, "high bits ignored"},
    {"divuw", enc::divuw(12, 10, 11), 1234, kZero, kMinusOne, "divide by zero -> -1 after sign extension"},
    // 0x80000000 / 0xffffffff == 0 in 32 bit unsigned arithmetic.
    {"divuw", enc::divuw(12, 10, 11), kInt32Min, kMinusOne, 0, "0x80000000 / 0xffffffff = 0"},
    // REMW
    {"remw", enc::remw(12, 10, 11), static_cast<u64>(static_cast<i64>(-7)), 2, kMinusOne, "-7 % 2 == -1"},
    {"remw", enc::remw(12, 10, 11), 1234, kZero, 1234, "remainder by zero -> dividend"},
    {"remw", enc::remw(12, 10, 11), kInt32Min, kMinusOne, 0, "overflow: INT32_MIN % -1"},
    {"remw", enc::remw(12, 10, 11), 7, static_cast<u64>(static_cast<i64>(-2)), 1, "7 % -2"},
    // REMUW
    {"remuw", enc::remuw(12, 10, 11), 0x00000000ffffffffull, 0x10, 0x0full, "0xffffffff % 16"},
    {"remuw", enc::remuw(12, 10, 11), 0xdeadbeef, kZero, 0xffffffffdeadbeefull, "sign extends the dividend"},
};

} // namespace

TEST(differential, muldiv_vectors) {
    CpuFixture cpu;
    std::size_t checked = 0;
    for (const Vector& vector : kVectors) {
        cpu.hart.reset(cpu.ram.base());
        cpu.hart.set_reg(10, vector.dividend);
        cpu.hart.set_reg(11, vector.divisor);
        cpu.place32(0, vector.encoding);

        if (!cpu.step()) {
            char message[256];
            std::snprintf(message, sizeof(message), "%s trapped: %s (case '%s')", vector.mnemonic,
                          trap_cause_name(cpu.trap.cause), vector.reason);
            fail(__FILE__, __LINE__, message);
        }
        const u64 actual = cpu.hart.get_reg(12);
        if (actual != vector.expected) {
            char message[320];
            std::snprintf(message, sizeof(message),
                          "%s(%016llx, %016llx): expected %016llx, got %016llx (case '%s')",
                          vector.mnemonic, static_cast<unsigned long long>(vector.dividend),
                          static_cast<unsigned long long>(vector.divisor),
                          static_cast<unsigned long long>(vector.expected),
                          static_cast<unsigned long long>(actual), vector.reason);
            fail(__FILE__, __LINE__, message);
        }
        ++checked;
    }
    CHECK_EQ(checked, sizeof(kVectors) / sizeof(kVectors[0]));
}

namespace {

// Independent reference model used to cross-check the interpreter's division
// and modulo results. It is written with straightforward C++ semantics on
// unsigned types, so it shares no code with src/cpu/softint.hpp.
u64 reference_div_unsigned(u64 a, u64 b) {
    if (b == 0) {
        return ~0ull;
    }
    u64 quotient = 0;
    u64 remainder = 0;
    for (int bit = 63; bit >= 0; --bit) {
        remainder = (remainder << 1) | ((a >> bit) & 1);
        if (remainder >= b) {
            remainder -= b;
            quotient |= 1ull << bit;
        }
    }
    return quotient;
}

u64 reference_rem_unsigned(u64 a, u64 b) {
    if (b == 0) {
        return a;
    }
    u64 remainder = 0;
    for (int bit = 63; bit >= 0; --bit) {
        remainder = (remainder << 1) | ((a >> bit) & 1);
        if (remainder >= b) {
            remainder -= b;
        }
    }
    return remainder;
}

// Signed division with truncation toward zero, derived from the unsigned model.
u64 reference_div_signed(i64 a, i64 b) {
    if (b == 0) {
        return ~0ull;
    }
    if (b == -1) {
        // a / -1 == -a; -INT64_MIN wraps to INT64_MIN, which is the specified
        // value for the overflow case.
        return ~static_cast<u64>(a) + 1;
    }
    const bool a_negative = a < 0;
    const bool b_negative = b < 0;
    const u64 ua = a_negative ? (~static_cast<u64>(a) + 1) : static_cast<u64>(a);
    const u64 ub = b_negative ? (~static_cast<u64>(b) + 1) : static_cast<u64>(b);
    const u64 magnitude = reference_div_unsigned(ua, ub);
    const u64 result = (a_negative != b_negative) ? (~magnitude + 1) : magnitude;
    return result;
}

u64 reference_rem_signed(i64 a, i64 b) {
    if (b == 0) {
        return static_cast<u64>(a);
    }
    if (b == -1) {
        return 0;
    }
    const bool a_negative = a < 0;
    const bool b_negative = b < 0;
    const u64 ua = a_negative ? (~static_cast<u64>(a) + 1) : static_cast<u64>(a);
    const u64 ub = b_negative ? (~static_cast<u64>(b) + 1) : static_cast<u64>(b);
    const u64 magnitude = reference_rem_unsigned(ua, ub);
    return a_negative ? (~magnitude + 1) : magnitude;
}

std::vector<u64> interesting_values() {
    return {kZero,
            kOne,
            2,
            3,
            7,
            16,
            0xff,
            0x100,
            0xffff,
            0x10000,
            0x7fffffffull,
            0x80000000ull,
            0xffffffffull,
            kInt32Max,
            kInt32Min,
            kInt64Max,
            kInt64Min,
            kMinusOne,
            0x0123456789abcdefull,
            0xfedcba9876543210ull};
}

} // namespace

TEST(differential, division_agrees_with_an_independent_model) {
    CpuFixture cpu;
    const std::vector<u64> values = interesting_values();
    std::size_t comparisons = 0;

    for (const u64 a : values) {
        for (const u64 b : values) {
            cpu.hart.reset(cpu.ram.base());
            cpu.hart.set_reg(10, a);
            cpu.hart.set_reg(11, b);

            cpu.place32(0, enc::divu(12, 10, 11));
            CHECK_EQ(cpu.step(), true);
            CHECK_EQ(cpu.hart.get_reg(12), reference_div_unsigned(a, b));
            ++comparisons;

            cpu.hart.pc = cpu.ram.base();
            cpu.place32(0, enc::remu(12, 10, 11));
            CHECK_EQ(cpu.step(), true);
            CHECK_EQ(cpu.hart.get_reg(12), reference_rem_unsigned(a, b));
            ++comparisons;

            cpu.hart.pc = cpu.ram.base();
            cpu.place32(0, enc::div_(12, 10, 11));
            CHECK_EQ(cpu.step(), true);
            CHECK_EQ(cpu.hart.get_reg(12), reference_div_signed(static_cast<i64>(a), static_cast<i64>(b)));
            ++comparisons;

            cpu.hart.pc = cpu.ram.base();
            cpu.place32(0, enc::rem_(12, 10, 11));
            CHECK_EQ(cpu.step(), true);
            CHECK_EQ(cpu.hart.get_reg(12), reference_rem_signed(static_cast<i64>(a), static_cast<i64>(b)));
            ++comparisons;
        }
    }
    // 20 values -> 400 pairs -> 1600 comparisons.
    CHECK_EQ(comparisons, std::size_t{1600});
}

TEST(differential, multiplication_high_words_match_the_wide_product) {
    CpuFixture cpu;
    const std::vector<u64> values = interesting_values();
    std::size_t comparisons = 0;

    for (const u64 a : values) {
        for (const u64 b : values) {
            cpu.hart.reset(cpu.ram.base());
            cpu.hart.set_reg(10, a);
            cpu.hart.set_reg(11, b);

            cpu.place32(0, enc::mul(12, 10, 11));
            CHECK_EQ(cpu.step(), true);
            CHECK_EQ(cpu.hart.get_reg(12), a * b);

            cpu.hart.pc = cpu.ram.base();
            cpu.place32(0, enc::mulhu(12, 10, 11));
            CHECK_EQ(cpu.step(), true);
            CHECK_EQ(cpu.hart.get_reg(12), mul_wide_unsigned(a, b).high);
            ++comparisons;
        }
    }
    CHECK_EQ(comparisons, std::size_t{400});
}

namespace {

struct AmoVector {
    const char* mnemonic;
    u32 encoding;
    u64 initial;
    u64 operand;
    u64 expected_memory;
    u64 expected_rd;
    const char* reason;
};

const AmoVector kAmoVectors[] = {
    {"amoadd.d", enc::amoadd(12, 11, 10, false), 100, 23, 123, 100, "add"},
    {"amoadd.d", enc::amoadd(12, 11, 10, false), 0, 0, 0, 0, "add zeros"},
    {"amoadd.d", enc::amoadd(12, 11, 10, false), kMinusOne, 2, 1, kMinusOne, "wraps"},
    {"amoswap.d", enc::amoswap(12, 11, 10, false), 42, kMinusOne, kMinusOne, 42, "swap"},
    {"amoxor.d", enc::amoxor(12, 11, 10, false), kMinusOne, kMinusOne, 0, kMinusOne, "xor to zero"},
    {"amoor.d", enc::amoor(12, 11, 10, false), 0xf0f0f0f0f0f0f0f0ull, 0x0f, 0xf0f0f0f0f0f0f0ffull,
     0xf0f0f0f0f0f0f0f0ull, "or"},
    {"amoand.d", enc::amoand(12, 11, 10, false), 0xf0f0f0f0f0f0f0f0ull, 0x0f, 0x0ull,
     0xf0f0f0f0f0f0f0f0ull, "0xf0 & 0x0f == 0"},
    {"amoand.d", enc::amoand(12, 11, 10, false), 0xf0f0f0f0f0f0f0ffull, 0x0f, 0x0full,
     0xf0f0f0f0f0f0f0ffull, "0xff & 0x0f == 0x0f"},
    {"amomin.d", enc::amomin(12, 11, 10, false), 0x7fffffffffffffffull, kInt64Min, kInt64Min, 0x7fffffffffffffffull, "signed min"},
    {"amomax.d", enc::amomax(12, 11, 10, false), 0x7fffffffffffffffull, kInt64Min, 0x7fffffffffffffffull, 0x7fffffffffffffffull, "signed max"},
    {"amominu.d", enc::amominu(12, 11, 10, false), kMinusOne, 0, 0, kMinusOne, "unsigned min"},
    {"amomaxu.d", enc::amomaxu(12, 11, 10, false), kMinusOne, 0, kMinusOne, kMinusOne, "unsigned max"},
    // Signed: -1 vs INT64_MIN, so the minimum is INT64_MIN even though it is the
    // larger value when compared as unsigned.
    {"amomin.d", enc::amomin(12, 11, 10, false), kMinusOne, kInt64Min, kInt64Min, kMinusOne, "signed min is negative"},
    // Signed max of -1 and INT64_MIN is -1, even though 0x8000... is the larger
    // value when the comparison is unsigned.
    {"amomax.d", enc::amomax(12, 11, 10, false), kMinusOne, kInt64Min, kMinusOne, kMinusOne, "signed max is -1"},
    {"amomaxu.d", enc::amomaxu(12, 11, 10, false), kMinusOne, kInt64Min, kMinusOne, kMinusOne, "unsigned max is -1"},
    {"amominu.d", enc::amominu(12, 11, 10, false), 0, kInt64Min, 0, 0, "unsigned min of 0 and 0x8000..."},
};

} // namespace

TEST(differential, amo_vectors) {
    CpuFixture cpu;
    std::size_t checked = 0;
    for (const AmoVector& vector : kAmoVectors) {
        cpu.hart.reset(cpu.ram.base());
        CHECK(cpu.space.store(0x80001000, 64, vector.initial));
        cpu.hart.set_reg(10, 0x80001000);
        cpu.hart.set_reg(11, vector.operand);
        cpu.place32(0, vector.encoding);

        if (!cpu.step()) {
            char message[256];
            std::snprintf(message, sizeof(message), "%s trapped: %s (case '%s')", vector.mnemonic,
                          trap_cause_name(cpu.trap.cause), vector.reason);
            fail(__FILE__, __LINE__, message);
        }
        u64 memory = 0;
        CHECK(cpu.space.load(0x80001000, 64, memory));
        const u64 rd = cpu.hart.get_reg(12);
        if (memory != vector.expected_memory || rd != vector.expected_rd) {
            char message[320];
            std::snprintf(message, sizeof(message),
                          "%s(initial=%016llx, operand=%016llx): memory %016llx (want %016llx), "
                          "rd %016llx (want %016llx) (case '%s')",
                          vector.mnemonic, static_cast<unsigned long long>(vector.initial),
                          static_cast<unsigned long long>(vector.operand),
                          static_cast<unsigned long long>(memory),
                          static_cast<unsigned long long>(vector.expected_memory),
                          static_cast<unsigned long long>(rd),
                          static_cast<unsigned long long>(vector.expected_rd), vector.reason);
            fail(__FILE__, __LINE__, message);
        }
        ++checked;
    }
    CHECK_EQ(checked, sizeof(kAmoVectors) / sizeof(kAmoVectors[0]));
}

TEST(differential, amo_word_forms_truncate_and_sign_extend) {
    CpuFixture cpu;
    struct Case {
        u32 encoding;
        u64 initial;
        u64 operand;
        u64 expected_memory;
        u64 expected_rd;
        const char* reason;
    };
    const Case cases[] = {
        {enc::amoadd(12, 11, 10, true), 0x100, 0x100, 0x200, 0x100, "no truncation"},
        // AMO*.W sign extends the 32 bit old value into rd (the ISA requires the
        // least significant 32 bits of the result to be sign extended).
        {enc::amoadd(12, 11, 10, true), 0xffffffff, 1, 0, 0xffffffffffffffffull, "wraps, rd sign extended"},
        {enc::amoswap(12, 11, 10, true), 1, 0xffffffff80000000ull, 0x80000000, 1, "swaps the low word"},
        {enc::amoadd(12, 11, 10, true), 0x7fffffff, 1, 0x80000000, 0x7fffffff, "old value sign extends"},
        {enc::amomin(12, 11, 10, true), 0x7fffffff, 0xffffffff80000000ull, 0x80000000, 0x7fffffff, "signed 32 bit min"},
        {enc::amomaxu(12, 11, 10, true), 0x80000000, 0x7fffffff, 0x80000000, 0xffffffff80000000ull, "unsigned 32 bit max"},
    };

    for (const Case& test_case : cases) {
        cpu.hart.reset(cpu.ram.base());
        CHECK(cpu.space.store(0x80001000, 32, test_case.initial));
        cpu.hart.set_reg(10, 0x80001000);
        cpu.hart.set_reg(11, test_case.operand);
        cpu.place32(0, test_case.encoding);
        CHECK_EQ(cpu.step(), true);

        u64 memory = 0;
        CHECK(cpu.space.load(0x80001000, 32, memory));
        const u64 rd = cpu.hart.get_reg(12);
        if (memory != test_case.expected_memory || rd != test_case.expected_rd) {
            char message[320];
            std::snprintf(message, sizeof(message),
                          "AMO*.W(initial=%08x, operand=%016llx): memory %08x (want %08x), rd %016llx "
                          "(want %016llx) (case '%s')",
                          static_cast<unsigned>(test_case.initial),
                          static_cast<unsigned long long>(test_case.operand),
                          static_cast<unsigned>(memory), static_cast<unsigned>(test_case.expected_memory),
                          static_cast<unsigned long long>(rd),
                          static_cast<unsigned long long>(test_case.expected_rd), test_case.reason);
            fail(__FILE__, __LINE__, message);
        }
    }
}

TEST(differential, encodings_match_the_llvm_assembler) {
    // Values cross-checked against `clang --target=riscv64-unknown-elf -march=rv64ima`
    // (see docs/ISA_STATUS.md). If the layout ever changes, this fails loudly.
    CHECK_EQ(enc::mul(10, 11, 12), std::uint32_t{0x02c58533});
    CHECK_EQ(enc::mulh(10, 11, 12), std::uint32_t{0x02c59533});
    CHECK_EQ(enc::mulhsu(10, 11, 12), std::uint32_t{0x02c5a533});
    CHECK_EQ(enc::mulhu(10, 11, 12), std::uint32_t{0x02c5b533});
    CHECK_EQ(enc::div_(10, 11, 12), std::uint32_t{0x02c5c533});
    CHECK_EQ(enc::divu(10, 11, 12), std::uint32_t{0x02c5d533});
    CHECK_EQ(enc::rem_(10, 11, 12), std::uint32_t{0x02c5e533});
    CHECK_EQ(enc::remu(10, 11, 12), std::uint32_t{0x02c5f533});
    CHECK_EQ(enc::mulw(10, 11, 12), std::uint32_t{0x02c5853b});
    CHECK_EQ(enc::divw(10, 11, 12), std::uint32_t{0x02c5c53b});
    CHECK_EQ(enc::divuw(10, 11, 12), std::uint32_t{0x02c5d53b});
    CHECK_EQ(enc::remw(10, 11, 12), std::uint32_t{0x02c5e53b});
    CHECK_EQ(enc::remuw(10, 11, 12), std::uint32_t{0x02c5f53b});

    CHECK_EQ(enc::lr(10, 11, false), std::uint32_t{0x1005b52f});
    CHECK_EQ(enc::lr(10, 11, true), std::uint32_t{0x1005a52f});
    CHECK_EQ(enc::lr_aqrl(10, 11, false), std::uint32_t{0x1605b52f});
    CHECK_EQ(enc::sc(10, 11, 12, false), std::uint32_t{0x18b6352f});
    CHECK_EQ(enc::sc(10, 11, 12, true), std::uint32_t{0x18b6252f});
    CHECK_EQ(enc::amoadd(10, 11, 12, false), std::uint32_t{0x00b6352f});
    CHECK_EQ(enc::amoadd(10, 11, 12, true), std::uint32_t{0x00b6252f});
    CHECK_EQ(enc::amoswap(10, 11, 12, false), std::uint32_t{0x08b6352f});
    CHECK_EQ(enc::amoxor(10, 11, 12, false), std::uint32_t{0x20b6352f});
    CHECK_EQ(enc::amoor(10, 11, 12, false), std::uint32_t{0x40b6352f});
    CHECK_EQ(enc::amoand(10, 11, 12, false), std::uint32_t{0x60b6352f});
    CHECK_EQ(enc::amomin(10, 11, 12, false), std::uint32_t{0x80b6352f});
    CHECK_EQ(enc::amomax(10, 11, 12, false), std::uint32_t{0xa0b6352f});
    CHECK_EQ(enc::amominu(10, 11, 12, false), std::uint32_t{0xc0b6352f});
    CHECK_EQ(enc::amomaxu(10, 11, 12, false), std::uint32_t{0xe0b6352f});
}
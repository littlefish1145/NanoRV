// SPDX-License-Identifier: MIT
//
// RV64A tests: LR/SC reservations, the AMO read-modify-write family, the aq/rl
// bits, alignment traps and bus faults.
#include "cpu/disassembler.hpp"
#include "test_framework.hpp"
#include "test_helpers.hpp"

using namespace nanorv;
using namespace nanorv::cpu;
using namespace nanorv::test;

namespace {

// Steps a single instruction with a1 = address and a2 = operand. The hart is not
// reset, so LR/SC state survives between calls.
bool step_with(CpuFixture& cpu, u32 instruction, u64 address, u64 operand = 0) {
    cpu.hart.set_reg(10, address);
    cpu.hart.set_reg(11, operand);
    cpu.place32(0, instruction);
    cpu.hart.pc = cpu.ram.base();
    return cpu.step();
}

// Runs one AMO family instruction on a freshly reset hart and returns rd.
u64 run_amo(CpuFixture& cpu, u32 instruction, u64 address, u64 operand, u32 rd = 12,
            bool expect_trap = false, TrapCause cause = TrapCause::IllegalInstruction) {
    cpu.hart.reset(cpu.ram.base());
    const bool ok = step_with(cpu, instruction, address, operand);
    if (expect_trap) {
        CHECK(!ok);
        CHECK(cpu.trap.cause == cause);
        return 0;
    }
    CHECK(ok);
    return cpu.hart.get_reg(rd);
}

} // namespace

TEST(atomic, amoadd_round_trip) {
    CpuFixture cpu;
    CHECK(cpu.space.store(0x80001000, 64, 100));

    const u64 old = run_amo(cpu, enc::amoadd(12, 11, 10, /*word=*/false), 0x80001000, 23);
    CHECK_EQ(old, std::uint64_t{100});
    u64 value = 0;
    CHECK(cpu.space.load(0x80001000, 64, value));
    CHECK_EQ(value, std::uint64_t{123});
}

TEST(atomic, amoadd_word_sign_extends_the_old_value) {
    CpuFixture cpu;
    CHECK(cpu.space.store(0x80001000, 32, 0x80000000u));

    // rd receives the sign-extended 32 bit old value.
    const u64 old = run_amo(cpu, enc::amoadd(12, 11, 10, /*word=*/true), 0x80001000, 1);
    CHECK_EQ(old, std::uint64_t{0xffffffff80000000ull});

    u64 value = 0;
    CHECK(cpu.space.load(0x80001000, 32, value));
    CHECK_EQ(value, std::uint64_t{0x80000001ull});
}

TEST(atomic, amoswap_xor_or_and) {
    CpuFixture cpu;
    CHECK(cpu.space.store(0x80001000, 64, 0xf0f0f0f0f0f0f0f0ull));

    run_amo(cpu, enc::amoswap(12, 11, 10, false), 0x80001000, 0x0123456789abcdefull);
    u64 value = 0;
    CHECK(cpu.space.load(0x80001000, 64, value));
    CHECK_EQ(value, std::uint64_t{0x0123456789abcdefull});

    run_amo(cpu, enc::amoxor(12, 11, 10, false), 0x80001000, 0xffffffffffffffffull);
    CHECK(cpu.space.load(0x80001000, 64, value));
    CHECK_EQ(value, std::uint64_t{~0x0123456789abcdefull});

    run_amo(cpu, enc::amoor(12, 11, 10, false), 0x80001000, 0x0full);
    CHECK(cpu.space.load(0x80001000, 64, value));
    CHECK_EQ(value, (~0x0123456789abcdefull) | 0x0full);

    run_amo(cpu, enc::amoand(12, 11, 10, false), 0x80001000, 0x0full);
    CHECK(cpu.space.load(0x80001000, 64, value));
    CHECK_EQ(value, std::uint64_t{0x0full});

    // An AMO with an all-ones operand is a no-op for AND / OR.
    run_amo(cpu, enc::amoand(12, 11, 10, false), 0x80001000, 0xffffffffffffffffull);
    CHECK(cpu.space.load(0x80001000, 64, value));
    CHECK_EQ(value, std::uint64_t{0x0full});
}

TEST(atomic, signed_and_unsigned_min_max) {
    CpuFixture cpu;

    // Signed comparison: 0x8000... is negative, 0x7fff... is positive.
    CHECK(cpu.space.store(0x80001000, 64, 0x7fffffffffffffffull));
    run_amo(cpu, enc::amomin(12, 11, 10, false), 0x80001000, 0x8000000000000000ull);
    u64 value = 0;
    CHECK(cpu.space.load(0x80001000, 64, value));
    CHECK_EQ(value, std::uint64_t{0x8000000000000000ull});

    // Unsigned comparison flips the order: 0x8000... is now the larger value.
    run_amo(cpu, enc::amomaxu(12, 11, 10, false), 0x80001000, 0x7000000000000000ull);
    CHECK(cpu.space.load(0x80001000, 64, value));
    CHECK_EQ(value, std::uint64_t{0x8000000000000000ull});

    run_amo(cpu, enc::amomax(12, 11, 10, false), 0x80001000, 0x9000000000000000ull);
    CHECK(cpu.space.load(0x80001000, 64, value));
    CHECK_EQ(value, std::uint64_t{0x9000000000000000ull}); // signed max picks the positive one

    run_amo(cpu, enc::amominu(12, 11, 10, false), 0x80001000, 0x7000000000000000ull);
    CHECK(cpu.space.load(0x80001000, 64, value));
    CHECK_EQ(value, std::uint64_t{0x7000000000000000ull}); // unsigned min picks the smaller one
}

TEST(atomic, min_max_word_forms_use_32_bit_comparison) {
    CpuFixture cpu;
    // 0x80000000 is negative when read as a signed 32 bit value.
    CHECK(cpu.space.store(0x80001000, 32, 0x7fffffffu));

    run_amo(cpu, enc::amomin(12, 11, 10, true), 0x80001000, 0x80000000ull);
    u64 value = 0;
    CHECK(cpu.space.load(0x80001000, 32, value));
    CHECK_EQ(value, std::uint64_t{0x80000000ull});

    // The result of an AMO*.W wraps to 32 bits and sign extends into rd.
    CHECK(cpu.space.store(0x80001000, 32, 0x80000000u));
    const u64 old = run_amo(cpu, enc::amomaxu(12, 11, 10, true), 0x80001000, 0x7fffffffull);
    CHECK_EQ(old, std::uint64_t{0xffffffff80000000ull});
    CHECK(cpu.space.load(0x80001000, 32, value));
    CHECK_EQ(value, std::uint64_t{0x80000000ull});
}

TEST(atomic, lr_sc_pair_succeeds) {
    CpuFixture cpu;
    CHECK(cpu.space.store(0x80001000, 64, 7));

    cpu.hart.reset(cpu.ram.base());
    CHECK_EQ(step_with(cpu, enc::lr(12, 10, false), 0x80001000), true);
    CHECK_EQ(cpu.hart.get_reg(12), std::uint64_t{7});
    CHECK(cpu.hart.reservation_valid);
    CHECK_EQ(cpu.hart.reservation_address, std::uint64_t{0x80001000});

    // SC with a live reservation stores and reports success.
    CHECK_EQ(step_with(cpu, enc::sc(12, 11, 10, false), 0x80001000, 99), true);
    CHECK_EQ(cpu.hart.get_reg(12), std::uint64_t{0});
    u64 value = 0;
    CHECK(cpu.space.load(0x80001000, 64, value));
    CHECK_EQ(value, std::uint64_t{99});
    CHECK(!cpu.hart.reservation_valid);
}

TEST(atomic, sc_fails_without_a_reservation) {
    CpuFixture cpu;
    CHECK(cpu.space.store(0x80001000, 64, 7));
    CHECK(!cpu.hart.reservation_valid);

    cpu.hart.reset(cpu.ram.base());
    CHECK_EQ(step_with(cpu, enc::sc(12, 11, 10, false), 0x80001000, 99), true);
    CHECK_EQ(cpu.hart.get_reg(12), std::uint64_t{1});
    u64 value = 0;
    CHECK(cpu.space.load(0x80001000, 64, value));
    CHECK_EQ(value, std::uint64_t{7}); // the store must not happen
}

TEST(atomic, sc_fails_when_the_address_does_not_match) {
    CpuFixture cpu;
    CHECK(cpu.space.store(0x80001000, 64, 7));

    cpu.hart.reset(cpu.ram.base());
    CHECK_EQ(step_with(cpu, enc::lr(12, 10, false), 0x80001000), true);
    CHECK(cpu.hart.reservation_valid);

    // Same hart, different address.
    CHECK_EQ(step_with(cpu, enc::sc(12, 11, 10, false), 0x80001008, 99), true);
    CHECK_EQ(cpu.hart.get_reg(12), std::uint64_t{1});
    CHECK(!cpu.hart.reservation_valid);
    u64 value = 0;
    CHECK(cpu.space.load(0x80001008, 64, value));
    CHECK_EQ(value, std::uint64_t{0});
}

TEST(atomic, an_intervening_store_breaks_the_reservation) {
    CpuFixture cpu;
    CHECK(cpu.space.store(0x80001000, 64, 7));

    cpu.hart.reset(cpu.ram.base());
    CHECK_EQ(step_with(cpu, enc::lr(12, 10, false), 0x80001000), true);
    CHECK(cpu.hart.reservation_valid);

    // A plain store to the reserved granule invalidates the reservation, so the
    // following SC must fail even though the addresses match.
    cpu.hart.set_reg(10, 0x80001000);
    cpu.hart.set_reg(11, 55);
    cpu.place32(0, enc::store(11, 10, 0, 3)); // sd a1, 0(a0)
    cpu.hart.pc = cpu.ram.base();
    CHECK_EQ(cpu.step(), true);
    CHECK(!cpu.hart.reservation_valid);

    CHECK_EQ(step_with(cpu, enc::sc(12, 11, 10, false), 0x80001000, 99), true);
    CHECK_EQ(cpu.hart.get_reg(12), std::uint64_t{1});
    u64 value = 0;
    CHECK(cpu.space.load(0x80001000, 64, value));
    CHECK_EQ(value, std::uint64_t{55}); // the SC store must not happen
}

TEST(atomic, a_store_outside_the_reserved_granule_keeps_it) {
    CpuFixture cpu;
    CHECK(cpu.space.store(0x80001000, 64, 7));

    cpu.hart.reset(cpu.ram.base());
    CHECK_EQ(step_with(cpu, enc::lr(12, 10, false), 0x80001000), true);
    CHECK_EQ(step_with(cpu, enc::store(11, 10, 8, 3), 0x80001000, 55), true);
    CHECK(cpu.hart.reservation_valid);

    CHECK_EQ(step_with(cpu, enc::sc(12, 11, 10, false), 0x80001000, 99), true);
    CHECK_EQ(cpu.hart.get_reg(12), std::uint64_t{0});
}

TEST(atomic, lr_sc_word_pair) {
    CpuFixture cpu;
    CHECK(cpu.space.store(0x80001000, 32, 0x7ffffff0u));

    cpu.hart.reset(cpu.ram.base());
    CHECK_EQ(step_with(cpu, enc::lr(12, 10, true), 0x80001000), true);
    // LR.W sign extends the loaded 32 bit value into rd.
    CHECK_EQ(cpu.hart.get_reg(12), std::uint64_t{0x7ffffff0ull});

    CHECK_EQ(step_with(cpu, enc::sc(12, 11, 10, true), 0x80001000, 0xfffffffffffffff0ull), true);
    CHECK_EQ(cpu.hart.get_reg(12), std::uint64_t{0});
    u64 value = 0;
    CHECK(cpu.space.load(0x80001000, 32, value));
    CHECK_EQ(value, std::uint64_t{0xfffffff0ull});
}

TEST(atomic, lr_with_nonzero_rs2_is_illegal) {
    CpuFixture cpu;
    cpu.hart.reset(cpu.ram.base());
    cpu.hart.set_reg(10, 0x80001000);
    cpu.place32(0, enc::amo(0x02, false, false, 12, /*rs2=*/1, 10, false));
    CHECK_EQ(cpu.step(), false);
    CHECK(cpu.trap.cause == TrapCause::IllegalInstruction);
}

TEST(atomic, misaligned_addresses_trap_before_any_access) {
    CpuFixture cpu;
    // .W access at an odd address.
    run_amo(cpu, enc::amoadd(12, 11, 10, true), 0x80001001, 1, 12, /*expect_trap=*/true,
            TrapCause::StoreAddressMisaligned);
    // .D access at a 2-byte aligned address.
    run_amo(cpu, enc::amoadd(12, 11, 10, false), 0x80001002, 1, 12, /*expect_trap=*/true,
            TrapCause::StoreAddressMisaligned);
    // LR reports a load misalignment.
    run_amo(cpu, enc::lr(12, 10, true), 0x80001001, 0, 12, /*expect_trap=*/true,
            TrapCause::LoadAddressMisaligned);
    run_amo(cpu, enc::lr(12, 10, false), 0x80001002, 0, 12, /*expect_trap=*/true,
            TrapCause::LoadAddressMisaligned);
    run_amo(cpu, enc::sc(12, 11, 10, false), 0x80001002, 0, 12, /*expect_trap=*/true,
            TrapCause::StoreAddressMisaligned);
}

TEST(atomic, access_faults_on_unmapped_memory) {
    CpuFixture cpu(0x1000, 0x80000000); // only 4 KiB of RAM
    cpu.hart.set_reg(10, 0x90000000);
    cpu.hart.set_reg(11, 1);
    cpu.place32(0, enc::amoadd(12, 11, 10, false));
    CHECK_EQ(cpu.step(), false);
    CHECK(cpu.trap.cause == TrapCause::StoreAccessFault);

    cpu.hart.pc = 0x80000000;
    cpu.place32(0, enc::lr(12, 10, false));
    CHECK_EQ(cpu.step(), false);
    CHECK(cpu.trap.cause == TrapCause::LoadAccessFault);
}
TEST(atomic, sc_cannot_write_bytes_outside_the_reserved_width) {
    CpuFixture c;const u64 address=kDramBase+0x1000;
    CHECK(c.space.store(address,64,0x1122334455667788));
    CHECK(step_with(c,enc::lr(12,10,true),address));
    CHECK(step_with(c,enc::sc(12,11,10,false),address,99));
    CHECK_EQ(c.hart.get_reg(12),1ull);CHECK(!c.hart.reservation_valid);
    u64 value;CHECK(c.space.load(address,64,value));CHECK_EQ(value,0x1122334455667788ull);
}
TEST(atomic, failed_sc_checks_unmapped_and_read_only_memory) {
    CpuFixture c;
    CHECK(!step_with(c,enc::sc(12,11,10,false),0x90000000,99));
    CHECK(c.trap.cause==TrapCause::StoreAccessFault);
    CHECK(c.space.add(0x90000000,c.ram.size(),&c.ram,"readonly-alias",{true,false,false}));
    CHECK(!step_with(c,enc::sc(12,11,10,false),0x90000000,99));
    CHECK(c.trap.cause==TrapCause::StoreAccessFault);
}

TEST(atomic, aq_and_rl_bits_are_accepted) {
    CpuFixture cpu;
    CHECK(cpu.space.store(0x80001000, 64, 1));

    cpu.hart.reset(cpu.ram.base());
    CHECK_EQ(step_with(cpu, enc::lr_aqrl(12, 10, false), 0x80001000), true);
    CHECK(cpu.hart.reservation_valid);

    CHECK_EQ(step_with(cpu, enc::amo(0x00, true, true, 12, 11, 10, false), 0x80001000, 5), true);
    CHECK_EQ(cpu.hart.get_reg(12), std::uint64_t{1});
    u64 value = 0;
    CHECK(cpu.space.load(0x80001000, 64, value));
    CHECK_EQ(value, std::uint64_t{6});
}

TEST(atomic, reserved_and_unknown_function_encodings_are_illegal) {
    CpuFixture cpu;
    cpu.hart.set_reg(10, 0x80001000);
    cpu.hart.set_reg(11, 1);

    // funct5 = 0b00101 is not allocated by RV64A.
    cpu.place32(0, enc::amo(0x05, false, false, 12, 11, 10, false));
    CHECK_EQ(cpu.step(), false);
    CHECK(cpu.trap.cause == TrapCause::IllegalInstruction);

    // funct3 = 0b000 / 0b001 do not select a width.
    cpu.hart.pc = 0x80000000;
    cpu.place32(0, enc::opcode_r(0x00, 11, 10, 0, 12, 0x2f));
    CHECK_EQ(cpu.step(), false);
    CHECK(cpu.trap.cause == TrapCause::IllegalInstruction);

    cpu.hart.pc = 0x80000000;
    cpu.place32(0, enc::opcode_r(0x00, 11, 10, 4, 12, 0x2f));
    CHECK_EQ(cpu.step(), false);
    CHECK(cpu.trap.cause == TrapCause::IllegalInstruction);
}

TEST(atomic, mnemonics_cover_the_whole_family) {
    CpuFixture cpu;
    CHECK_EQ(disassemble(enc::amoadd(12, 11, 10, false)), std::string("amoadd.d a2, a1, (a0)"));
    CHECK_EQ(disassemble(enc::amoadd(12, 11, 10, true)), std::string("amoadd.w a2, a1, (a0)"));
    CHECK_EQ(disassemble(enc::lr(12, 10, false)), std::string("lr.d a2, (a0)"));
    CHECK_EQ(disassemble(enc::lr_aqrl(12, 10, true)), std::string("lr.w.aqrl a2, (a0)"));
    CHECK_EQ(disassemble(enc::sc(12, 11, 10, false)), std::string("sc.d a2, a1, (a0)"));
    CHECK_EQ(disassemble(enc::amomaxu(12, 11, 10, true)), std::string("amomaxu.w a2, a1, (a0)"));
    CHECK_EQ(disassemble(enc::mul(12, 10, 11)), std::string("mul a2, a0, a1"));
    CHECK_EQ(disassemble(enc::divw(12, 10, 11)), std::string("divw a2, a0, a1"));
}

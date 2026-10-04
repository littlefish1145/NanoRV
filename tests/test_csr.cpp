// SPDX-License-Identifier: MIT
//
// Zicsr tests: the CSR file itself, the instruction encodings, and the side
// effect rules that make CSR access loops safe.
#include "cpu/csr.hpp"
#include "cpu/disassembler.hpp"
#include "test_framework.hpp"
#include "test_helpers.hpp"

using namespace nanorv;
using namespace nanorv::cpu;
using namespace nanorv::test;

namespace {

constexpr std::uint64_t kArchIdRvvM = 0x5256564dull;

} // namespace

TEST(csr, descriptors_expose_names_and_permissions) {
    CHECK_EQ(std::string(csr_name(0x001)), std::string("fflags"));
    CHECK_EQ(std::string(csr_name(0x003)), std::string("fcsr"));
    CHECK_EQ(std::string(csr_name(0xc00)), std::string("cycle"));
    CHECK_EQ(std::string(csr_name(0xf14)), std::string("mhartid"));
    CHECK_EQ(std::string(csr_name(0x999)), std::string("unknown"));

    const CsrInfo* const fcsr = find_csr(0x003);
    CHECK(fcsr != nullptr);
    CHECK(fcsr->writable);
    CHECK_EQ(fcsr->write_mask, std::uint64_t{0xff});
    CHECK(fcsr->privilege == Privilege::User);

    const CsrInfo* const cycle = find_csr(0xc00);
    CHECK(cycle != nullptr);
    CHECK(!cycle->writable);

    const CsrInfo* const mhartid = find_csr(0xf14);
    CHECK(mhartid != nullptr);
    CHECK(!mhartid->writable);
    CHECK(mhartid->privilege == Privilege::Machine);
}

TEST(csr, fflags_frm_and_fcsr_share_storage) {
    CsrFile csr(3, 1'000'000'000ull);

    csr.set_fcsr(0xe5); // fflags = 0x05, frm = 7
    CHECK_EQ(csr.fflags(), std::uint64_t{0x05});
    CHECK_EQ(csr.frm(), std::uint64_t{7});
    CHECK_EQ(csr.fcsr(), std::uint64_t{0xe5});

    // Writing fflags leaves frm alone and vice versa.
    csr.set_fflags(0x1f);
    CHECK_EQ(csr.fcsr(), std::uint64_t{0xff});
    csr.set_frm(0);
    CHECK_EQ(csr.fflags(), std::uint64_t{0x1f});
    CHECK_EQ(csr.frm(), std::uint64_t{0});

    // Bits above the mask are dropped.
    csr.set_fflags(0xffffffffffffffffull);
    CHECK_EQ(csr.fflags(), std::uint64_t{0x1f});
    csr.set_frm(0xff);
    CHECK_EQ(csr.frm(), std::uint64_t{0x7});
}

TEST(csr, reads_and_writes_respect_the_write_mask) {
    CsrFile csr(0, 1'000'000'000ull);
    u64 value = 0;

    CHECK(csr.write(0x003, Privilege::User, 0x1ff));
    CHECK(csr.read(0x003, Privilege::User, 0, value));
    CHECK_EQ(value, std::uint64_t{0xff});

    CHECK(csr.write(0x001, Privilege::User, 0xffffffffffffffffull));
    CHECK(csr.read(0x001, Privilege::User, 0, value));
    CHECK_EQ(value, std::uint64_t{0x1f});

    CHECK(csr.write(0x002, Privilege::User, 0xffffffffffffffffull));
    CHECK(csr.read(0x002, Privilege::User, 0, value));
    CHECK_EQ(value, std::uint64_t{0x7});
}

TEST(csr, privilege_is_enforced) {
    CsrFile csr(1, 1'000'000'000ull);
    u64 value = 0;

    // mhartid is machine level.
    CHECK(!csr.read(0xf14, Privilege::User, 0, value));
    CHECK(!csr.read(0xf14, Privilege::Supervisor, 0, value));
    CHECK(csr.read(0xf14, Privilege::Machine, 0, value));
    CHECK_EQ(value, std::uint64_t{1});

    // fcsr is user level.
    CHECK(csr.read(0x003, Privilege::User, 0, value));
}

TEST(csr, unknown_addresses_are_rejected) {
    CsrFile csr(0, 1'000'000'000ull);
    u64 value = 0;
    // Only CSR numbers that exist may be touched. Note that 0x001-0x003 are
    // legal addresses even though their low bits are non-zero, because fflags,
    // frm and fcsr live there; there is no blanket alignment rule.
    CHECK(csr.read(0x001, Privilege::User, 0, value));
    CHECK(csr.read(0x002, Privilege::User, 0, value));
    CHECK(csr.read(0x003, Privilege::User, 0, value));

    for (u32 address : {0x000u, 0x004u, 0x101u, 0x307u, 0x7c0u, 0x800u, 0xb03u, 0xc80u, 0xf10u}) {
        CHECK(!csr.read(address, Privilege::Machine, 0, value));
        CHECK(!csr.write(address, Privilege::Machine, 0));
    }

    // On RV64 the high performance counters (0xc80, 0xc81, 0xc82) do not
    // exist; cycle/time/instret are already 64 bits wide.
    CHECK(!csr.read(0xc80, Privilege::Machine, 0, value));
}

TEST(csr, read_only_registers_cannot_be_written) {
    CsrFile csr(7, 1'000'000'000ull);
    CHECK(!csr.write(0xc00, Privilege::Machine, 0));
    CHECK(!csr.write(0xf12, Privilege::Machine, 0));
    CHECK(!csr.write(0xf14, Privilege::Machine, 0));
}

TEST(csr, identity_and_counter_registers_read_back) {
    CsrFile csr(5, 1'000'000'000ull);
    CHECK(csr.write(kCsrMcounteren,Privilege::Machine,7));
    CHECK(csr.write(kCsrScounteren,Privilege::Machine,7));
    u64 value = 0;

    CHECK(csr.read(0xf11, Privilege::Machine, 0, value));
    CHECK_EQ(value, kVendorId);
    CHECK(csr.read(0xf12, Privilege::Machine, 0, value));
    CHECK_EQ(value, kArchIdRvvM);
    CHECK(csr.read(0xf13, Privilege::Machine, 0, value));
    CHECK_EQ(value, std::uint64_t{0x0000000100000000ull});
    CHECK(csr.read(0xf14, Privilege::Machine, 0, value));
    CHECK_EQ(value, std::uint64_t{5});

    // instret reports the caller's retired count, cycle mirrors it for now.
    CHECK(csr.read(0xc02, Privilege::User, 1234, value));
    CHECK_EQ(value, std::uint64_t{1234});
    CHECK(csr.read(0xc00, Privilege::User, 1234, value));
    CHECK_EQ(value, std::uint64_t{1234});

    // time advances.
    CHECK(csr.read(0xc01, Privilege::User, 0, value));
    const u64 first = value;
    CHECK(csr.read(0xc01, Privilege::User, 0, value));
    CHECK(value >= first);
}

// ---------------------------------------------------------------------------
// Instructions
// ---------------------------------------------------------------------------

TEST(zicsr, encodings_match_the_llvm_assembler) {
    // Cross-checked against clang --target=riscv64-unknown-elf -march=rv64i_zicsr
    // using t0 = x5 and t1 = x6.
    CHECK_EQ(enc::csrr(5, 0x003), std::uint32_t{0x003022f3});      // csrr   t0, fcsr
    CHECK_EQ(enc::csrrs(5, 0x003, 6), std::uint32_t{0x003322f3});  // csrrs  t0, fcsr, t1
    CHECK_EQ(enc::csrrc(5, 0x003, 6), std::uint32_t{0x003332f3});  // csrrc  t0, fcsr, t1
    CHECK_EQ(enc::csrrw(5, 0x003, 6), std::uint32_t{0x003312f3});  // csrrw  t0, fcsr, t1
    CHECK_EQ(enc::csrrwi(0x003, 5, 7), std::uint32_t{0x0033d2f3}); // csrrwi t0, fcsr, 7
    CHECK_EQ(enc::csrrsi(0x003, 5, 7), std::uint32_t{0x0033e2f3}); // csrrsi t0, fcsr, 7
    CHECK_EQ(enc::csrrci(0x003, 5, 7), std::uint32_t{0x0033f2f3}); // csrrci t0, fcsr, 7
    CHECK_EQ(enc::csrr(5, 0xc00), std::uint32_t{0xC00022F3u});      // rdcycle t0
    CHECK_EQ(enc::csrw(0x003, 6), std::uint32_t{0x00331073});      // csrw   fcsr, t1
}

TEST(zicsr, csrr_reads_a_csr) {
    CpuFixture cpu;
    cpu.hart.csr.set_fcsr(0xe5);

    cpu.place32(0, enc::csrr(10, 0x003));
    CHECK_EQ(cpu.step(), true);
    CHECK_EQ(cpu.hart.get_reg(10), std::uint64_t{0xe5});

    cpu.hart.pc = 0x80000000;
    cpu.place32(0, enc::csrr(11, 0x001));
    CHECK_EQ(cpu.step(), true);
    CHECK_EQ(cpu.hart.get_reg(11), std::uint64_t{0x05});

    cpu.hart.pc = 0x80000000;
    cpu.place32(0, enc::csrr(12, 0x002));
    CHECK_EQ(cpu.step(), true);
    CHECK_EQ(cpu.hart.get_reg(12), std::uint64_t{7});
}

TEST(zicsr, csrrw_returns_the_old_value_and_writes) {
    CpuFixture cpu;
    cpu.hart.csr.set_fcsr(0x00);
    cpu.hart.set_reg(5, 0x1f);

    cpu.place32(0, enc::csrrw(10, 0x003, 5));
    CHECK_EQ(cpu.step(), true);
    CHECK_EQ(cpu.hart.get_reg(10), std::uint64_t{0}); // old value
    CHECK_EQ(cpu.hart.csr.fcsr(), std::uint64_t{0x1f}); // new value
}

TEST(zicsr, csrrs_and_csrrc_set_and_clear_bits) {
    CpuFixture cpu;
    cpu.hart.set_reg(5, 0b1010);

    cpu.hart.csr.set_fcsr(0x00);
    cpu.place32(0, enc::csrrs(10, 0x003, 5));
    CHECK_EQ(cpu.step(), true);
    CHECK_EQ(cpu.hart.csr.fcsr(), std::uint64_t{0b1010});

    cpu.hart.pc = 0x80000000;
    cpu.place32(0, enc::csrrc(11, 0x003, 5));
    CHECK_EQ(cpu.step(), true);
    CHECK_EQ(cpu.hart.csr.fcsr(), std::uint64_t{0b1010 & ~std::uint64_t{0b1010}});
}

TEST(zicsr, immediate_forms_use_a_zero_extended_five_bit_value) {
    CpuFixture cpu;
    cpu.hart.csr.set_fcsr(0x00);

    // CSRRWI replaces the whole register, so writing frm = 7 sets fcsr = 0xe0.
    cpu.place32(0, enc::csrrwi(0x002, 10, 7));
    CHECK_EQ(cpu.step(), true);
    CHECK_EQ(cpu.hart.csr.frm(), std::uint64_t{7});
    CHECK_EQ(cpu.hart.csr.fcsr(), std::uint64_t{0xe0});

    // CSRRCI clears the bits selected by the immediate. The 5 bit immediate
    // cannot reach frm, which lives in fcsr bits [7:5], so clearing the rounding
    // mode means targeting the frm register itself.
    cpu.hart.pc = 0x80000000;
    cpu.place32(0, enc::csrrci(0x002, 11, 7));
    CHECK_EQ(cpu.step(), true);
    CHECK_EQ(cpu.hart.csr.frm(), std::uint64_t{0});
    CHECK_EQ(cpu.hart.csr.fcsr(), std::uint64_t{0x00});

    // On fcsr the same immediate clears the three low fflags bits instead.
    cpu.hart.pc = 0x80000000;
    cpu.hart.csr.set_fcsr(0xff);
    cpu.place32(0, enc::csrrci(0x003, 11, 7));
    CHECK_EQ(cpu.step(), true);
    CHECK_EQ(cpu.hart.csr.fflags(), std::uint64_t{0x18});

    // CSRRSI sets bits: fflags |= 0x1f.
    cpu.hart.pc = 0x80000000;
    cpu.place32(0, enc::csrrsi(0x001, 12, 31));
    CHECK_EQ(cpu.step(), true);
    CHECK_EQ(cpu.hart.csr.fflags(), std::uint64_t{0x1f});

    // The immediate is zero extended from five bits, so zimm 31 is the largest
    // value that can reach a register.
    cpu.hart.pc = 0x80000000;
    cpu.place32(0, enc::csrrwi(0x001, 13, 31));
    CHECK_EQ(cpu.step(), true);
    CHECK_EQ(cpu.hart.csr.fflags(), std::uint64_t{0x1f});
}

TEST(zicsr, source_x0_must_not_write) {
    CpuFixture cpu;
    cpu.hart.csr.set_fcsr(0x5a);

    // csrrs with rs1 = x0 is a pure read.
    cpu.place32(0, enc::csrr(10, 0x003));
    CHECK_EQ(cpu.step(), true);
    CHECK_EQ(cpu.hart.get_reg(10), std::uint64_t{0x5a});
    CHECK_EQ(cpu.hart.csr.fcsr(), std::uint64_t{0x5a});

    // csrrc with rs1 = x0 must not clear anything.
    cpu.hart.pc = 0x80000000;
    cpu.place32(0, enc::csrrc(11, 0x003, 0));
    CHECK_EQ(cpu.step(), true);
    CHECK_EQ(cpu.hart.get_reg(11), std::uint64_t{0x5a});
    CHECK_EQ(cpu.hart.csr.fcsr(), std::uint64_t{0x5a});

    // csrrsi with a zero immediate must not write either.
    cpu.hart.pc = 0x80000000;
    cpu.place32(0, enc::csrrsi(0x003, 12, 0));
    CHECK_EQ(cpu.step(), true);
    CHECK_EQ(cpu.hart.csr.fcsr(), std::uint64_t{0x5a});
}

TEST(zicsr, csrrw_with_rd_x0_does_not_read) {
    // CSRRW always writes, so it must succeed on a writable CSR even when the
    // destination is x0 (which means "do not read the old value").
    CpuFixture cpu;
    cpu.hart.csr.set_fcsr(0x00);
    cpu.hart.set_reg(5, 0x3f);

    cpu.place32(0, enc::csrw(0x003, 5));
    CHECK_EQ(cpu.step(), true);
    CHECK_EQ(cpu.hart.csr.fcsr(), std::uint64_t{0x3f});
}

TEST(zicsr, writing_a_read_only_csr_traps) {
    CpuFixture cpu;
    cpu.hart.set_reg(5, 1);

    // csrw cycle, t1
    cpu.place32(0, enc::csrw(0xc00, 5));
    CHECK_EQ(cpu.step(), false);
    CHECK(cpu.trap.cause == TrapCause::IllegalInstruction);

    // csrrs t0, cycle, t1 (rs1 != 0 forces a write)
    cpu.hart.pc = 0x80000000;
    cpu.place32(0, enc::csrrs(10, 0xc00, 5));
    CHECK_EQ(cpu.step(), false);
    CHECK(cpu.trap.cause == TrapCause::IllegalInstruction);
}

TEST(zicsr, reading_a_read_only_csr_with_rs1_x0_is_legal) {
    CpuFixture cpu;
    cpu.place32(0, enc::csrr(10, 0xc00)); // csrr t0, cycle
    CHECK_EQ(cpu.step(), true);
    // The CSR read observes the retired count from before this instruction.
    CHECK_EQ(cpu.hart.get_reg(10), std::uint64_t{0});
    CHECK_EQ(cpu.hart.retired, std::uint64_t{1});

    cpu.hart.pc = 0x80000000;
    cpu.place32(0, enc::csrr(11, 0xc00));
    CHECK_EQ(cpu.step(), true);
    CHECK_EQ(cpu.hart.get_reg(11), std::uint64_t{1});
}

TEST(zicsr, privilege_is_enforced_by_the_instruction) {
    CpuFixture cpu;
    cpu.hart.privilege = Privilege::User;

    cpu.place32(0, enc::csrr(10, 0xf14)); // csrr t0, mhartid from U-mode
    CHECK_EQ(cpu.step(), false);
    CHECK(cpu.trap.cause == TrapCause::IllegalInstruction);

    cpu.hart.pc = 0x80000000;
    cpu.hart.privilege = Privilege::Machine;
    cpu.place32(0, enc::csrr(10, 0xf14));
    CHECK_EQ(cpu.step(), true);
    CHECK_EQ(cpu.hart.get_reg(10), std::uint64_t{0}); // fixture hart id
}

TEST(zicsr, unknown_and_misaligned_csr_addresses_trap) {
    CpuFixture cpu;

    cpu.place32(0, enc::csrr(10, 0x107));
    CHECK_EQ(cpu.step(), false);
    CHECK(cpu.trap.cause == TrapCause::IllegalInstruction);

    // Address 0x101 has non-zero bits [1:0].
    cpu.hart.pc = 0x80000000;
    cpu.place32(0, enc::csrr(10, 0x101));
    CHECK_EQ(cpu.step(), false);
    CHECK(cpu.trap.cause == TrapCause::IllegalInstruction);
}

TEST(zicsr, reserved_funct3_traps) {
    CpuFixture cpu;
    // funct3 = 0b100 is not allocated.
    cpu.place32(0, enc::opcode_i(0x003, 0, 4, 10, 0x73));
    CHECK_EQ(cpu.step(), false);
    CHECK(cpu.trap.cause == TrapCause::IllegalInstruction);
}

TEST(zicsr, instret_reports_the_count_before_the_current_instruction) {
    CpuFixture cpu;
    cpu.place32(0, enc::csrr(10, 0xc02)); // csrr a0, instret
    CHECK_EQ(cpu.step(), true);
    CHECK_EQ(cpu.hart.get_reg(10), std::uint64_t{0});
    CHECK_EQ(cpu.hart.retired, std::uint64_t{1});

    // The next read sees one more retired instruction.
    cpu.hart.pc = 0x80000000;
    cpu.place32(0, enc::csrr(11, 0xc02));
    CHECK_EQ(cpu.step(), true);
    CHECK_EQ(cpu.hart.get_reg(11), std::uint64_t{1});
}

TEST(zicsr, mnemonics) {
    CpuFixture cpu;
    CHECK_EQ(disassemble(enc::csrrs(10, 0x003, 0)), std::string("csrrs a0, fcsr, zero"));
    CHECK_EQ(disassemble(enc::csrrs(10, 0x003, 5)), std::string("csrrs a0, fcsr, t0"));
    CHECK_EQ(disassemble(enc::csrrc(10, 0x003, 5)), std::string("csrrc a0, fcsr, t0"));
    CHECK_EQ(disassemble(enc::csrrw(10, 0x003, 5)), std::string("csrrw a0, fcsr, t0"));
    CHECK_EQ(disassemble(enc::csrrwi(0x003, 10, 7)), std::string("csrrwi a0, fcsr, 7"));
    CHECK_EQ(disassemble(enc::csrrsi(0x001, 10, 3)), std::string("csrrsi a0, fflags, 3"));
    CHECK_EQ(disassemble(enc::csrrci(0xf14, 10, 7)), std::string("csrrci a0, mhartid, 7"));
    CHECK_EQ(disassemble(enc::csrrs(10, 0xc00, 0)), std::string("csrrs a0, cycle, zero"));
    CHECK_EQ(disassemble(enc::csrrs(0, 0x100, 0)), std::string("csrrs zero, sstatus, zero"));
}

// SPDX-License-Identifier: MIT
#include "cpu/execute.hpp"
#include "test_framework.hpp"
#include "test_helpers.hpp"

using namespace rvvm;
using namespace rvvm::cpu;
using namespace rvvm::test;

TEST(execute, lui_and_addi) {
    CpuFixture cpu;
    cpu.place32(0, enc::lui(11, 0x12345));
    cpu.place32(1, enc::addi(10, 11, 11));
    CHECK_EQ(cpu.run(2), std::uint64_t{2});
    CHECK_EQ(cpu.hart.get_reg(11), std::uint64_t{0x12345000});
    CHECK_EQ(cpu.hart.get_reg(10), std::uint64_t{0x1234500b});
    CHECK_EQ(cpu.hart.pc, std::uint64_t{0x80000008});
    CHECK_EQ(cpu.hart.retired, std::uint64_t{2});
}

TEST(execute, x0_stays_zero) {
    CpuFixture cpu;
    cpu.place32(0, enc::addi(10, 0, -1));
    CHECK_EQ(cpu.step(), true);
    CHECK_EQ(cpu.hart.get_reg(0), std::uint64_t{0});
    CHECK_EQ(cpu.hart.get_reg(10), std::uint64_t{0xffffffffffffffffull});

    // Writing x0 with a LUI must also leave it at zero.
    cpu.hart.pc = 0x80000000;
    cpu.place32(0, enc::lui(0, 0xfffff));
    CHECK_EQ(cpu.step(), true);
    CHECK_EQ(cpu.hart.get_reg(0), std::uint64_t{0});
}

TEST(execute, arithmetic_edge_cases) {
    CpuFixture cpu;
    cpu.hart.set_reg(10, 0xffffffffffffffffull); // -1
    cpu.hart.set_reg(11, 1);

    cpu.place32(0, enc::add(10, 10, 11));  // add  a0, a0, a1 -> 0 (wraps)
    cpu.place32(1, enc::sub(12, 11, 10));  // sub  a2, a1, a0 -> 1
    cpu.place32(2, enc::slli(10, 10, 1));  // slli a0, a0, 1  -> 0
    cpu.place32(3, enc::slli(11, 11, 63)); // slli a1, a1, 63 -> 0x8000...
    cpu.place32(4, enc::srai(11, 11, 4));  // srai a1, a1, 4  -> 0xf800...
    cpu.place32(5, enc::srli(11, 11, 1));  // srli a1, a1, 1  -> 0x7c00...

    CHECK_EQ(cpu.run(6), std::uint64_t{6});
    CHECK_EQ(cpu.hart.get_reg(10), std::uint64_t{0});
    CHECK_EQ(cpu.hart.get_reg(12), std::uint64_t{1});
    CHECK_EQ(cpu.hart.get_reg(11), std::uint64_t{0x7c00000000000000ull});
    CHECK_EQ(cpu.hart.get_reg(0), std::uint64_t{0});
}

TEST(execute, signed_and_unsigned_comparisons) {
    CpuFixture cpu;
    cpu.hart.set_reg(10, static_cast<u64>(-1));
    cpu.hart.set_reg(11, 1);

    cpu.place32(0, enc::slt(12, 10, 11));   // slt   a2, a0, a1 -> 1
    cpu.place32(1, enc::sltu(13, 10, 11));  // sltu  a3, a0, a1 -> 0
    cpu.place32(2, enc::slt(14, 11, 10));   // slt   a4, a1, a0 -> 0
    cpu.place32(3, enc::sltiu(15, 10, 1));  // sltiu a5, a0, 1   -> 0
    cpu.place32(4, enc::sltiu(16, 11, -1)); // sltiu a6, a1, -1  -> 1

    CHECK_EQ(cpu.run(5), std::uint64_t{5});
    CHECK_EQ(cpu.hart.get_reg(12), std::uint64_t{1});
    CHECK_EQ(cpu.hart.get_reg(13), std::uint64_t{0});
    CHECK_EQ(cpu.hart.get_reg(14), std::uint64_t{0});
    CHECK_EQ(cpu.hart.get_reg(15), std::uint64_t{0});
    CHECK_EQ(cpu.hart.get_reg(16), std::uint64_t{1});
}

TEST(execute, logical_immediates) {
    CpuFixture cpu;
    cpu.hart.set_reg(10, 0x0f0f0f0f0f0f0f0full);

    cpu.place32(0, enc::xori(11, 10, -1));   // xori a1, a0, -1
    cpu.place32(1, enc::andi(12, 10, 0x0ff)); // andi a2, a0, 0xff
    cpu.place32(2, enc::ori(13, 10, 0xf0));   // ori  a3, a0, 0xf0
    cpu.place32(3, enc::and_(14, 10, 12));    // and  a4, a0, a2 (a2 = 0x0f from above)

    CHECK_EQ(cpu.run(4), std::uint64_t{4});
    CHECK_EQ(cpu.hart.get_reg(11), std::uint64_t{0xf0f0f0f0f0f0f0f0ull});
    CHECK_EQ(cpu.hart.get_reg(12), std::uint64_t{0x0full});
    CHECK_EQ(cpu.hart.get_reg(13), std::uint64_t{0x0f0f0f0f0f0f0fffull});
    CHECK_EQ(cpu.hart.get_reg(14), std::uint64_t{0x0full});
}

TEST(execute, word_operations_sign_extend) {
    CpuFixture cpu;
    cpu.hart.set_reg(10, 0x123456789abcdef0ull);

    cpu.place32(0, enc::addi32(10, 10, 1)); // addiw a0, a0, 1
    CHECK_EQ(cpu.step(), true);
    CHECK_EQ(cpu.hart.get_reg(10), std::uint64_t{0xffffffff9abcdef1ull});

    cpu.hart.pc = 0x80000000;
    cpu.place32(0, enc::slliw(10, 10, 4)); // slliw a0, a0, 4
    CHECK_EQ(cpu.step(), true);
    CHECK_EQ(cpu.hart.get_reg(10), std::uint64_t{0xffffffffabcdef10ull});

    cpu.hart.pc = 0x80000000;
    cpu.place32(0, enc::srliw(10, 10, 4)); // srliw a0, a0, 4
    CHECK_EQ(cpu.step(), true);
    CHECK_EQ(cpu.hart.get_reg(10), std::uint64_t{0x0abcdef1});

    cpu.hart.pc = 0x80000000;
    cpu.hart.set_reg(11, 0x80000000ull);
    cpu.place32(0, enc::sraiw(10, 11, 31)); // sraiw a0, a1, 31
    CHECK_EQ(cpu.step(), true);
    CHECK_EQ(cpu.hart.get_reg(10), std::uint64_t{0xffffffffffffffffull});
}

TEST(execute, word_register_operations_mask_the_shift_amount) {
    CpuFixture cpu;
    cpu.hart.set_reg(10, 0x00000001'00000003ull);
    cpu.hart.set_reg(11, 33); // 33 & 0x1f == 1 for *W shifts, 33 for * shifts

    cpu.place32(0, enc::sllw(12, 10, 11)); // sllw a2, a0, a1 -> low 32 bits << 1
    CHECK_EQ(cpu.step(), true);
    CHECK_EQ(cpu.hart.get_reg(12), std::uint64_t{6});

    cpu.hart.pc = 0x80000000;
    cpu.place32(0, enc::sll(13, 10, 11)); // sll a3, a0, a1 -> 64-bit shift by 33
    CHECK_EQ(cpu.step(), true);
    CHECK_EQ(cpu.hart.get_reg(13), std::uint64_t{0x600000000});

    cpu.hart.pc = 0x80000000;
    cpu.place32(0, enc::addw(14, 10, 11)); // addw a4, a0, a1 -> 3 + 33
    CHECK_EQ(cpu.step(), true);
    CHECK_EQ(cpu.hart.get_reg(14), std::uint64_t{36});

    cpu.hart.pc = 0x80000000;
    cpu.place32(0, enc::subw(15, 11, 10)); // subw a5, a1, a0 -> 33 - 3
    CHECK_EQ(cpu.step(), true);
    CHECK_EQ(cpu.hart.get_reg(15), std::uint64_t{30});
}

TEST(execute, load_and_store_round_trip) {
    CpuFixture cpu;
    cpu.hart.set_reg(10, 0x80001000);
    cpu.hart.set_reg(11, 0x0123456789abcdefull);

    cpu.place32(0, enc::store(11, 10, 0, 3)); // sd a1, 0(a0)
    cpu.place32(1, enc::load(11, 10, 0, 3));  // ld a1, 0(a0)
    CHECK_EQ(cpu.run(2), std::uint64_t{2});
    CHECK_EQ(cpu.hart.get_reg(11), std::uint64_t{0x0123456789abcdefull});

    // Narrow accesses must extend exactly as the ISA specifies. Each case rewrites
    // the first word and restarts execution there.
    struct LoadCase {
        u32 funct3;
        i32 offset;
        u64 expected;
    };
    const LoadCase cases[] = {
        {0, 0, 0xffffffffffffffefull}, // lb  a2, 0(a0)
        {4, 0, 0xef},                   // lbu a2, 0(a0)
{1, 1, 0xffffffffffffabcdull}, // lh  a2, 2(a0)
        {5, 1, 0xabcd},                // lhu a2, 2(a0)
        {6, 4, 0x01234567},             // lwu a2, 4(a0)
        {2, 0, 0xffffffff89abcdefull},  // lw  a2, 0(a0)
    };
    for (const LoadCase& test_case : cases) {
        cpu.hart.pc = 0x80000000;
        cpu.place32(0, enc::load(12, 10, test_case.offset, test_case.funct3));
        CHECK_EQ(cpu.step(), true);
        CHECK_EQ(cpu.hart.get_reg(12), test_case.expected);
    }

    // Partial stores leave the neighbouring bytes untouched.
    cpu.hart.pc = 0x80000000;
    cpu.hart.set_reg(12, 0);
    cpu.place32(0, enc::store(12, 10, 0, 1)); // sh a2, 0(a0)
    CHECK_EQ(cpu.step(), true);
    u64 value = 0;
    CHECK(cpu.space.load(0x80001000, 64, value));
    CHECK_EQ(value, std::uint64_t{0x0123456789ab0000ull});
}

TEST(execute, branches_take_and_fall_through) {
    CpuFixture cpu;
    cpu.hart.set_reg(10, 5);
    cpu.hart.set_reg(11, 5);

    // beq a0, a1, +8 -> skips the next instruction
    cpu.place32(0, enc::branch(8, 11, 10, 0));
    cpu.place32(1, enc::addi(10, 10, 1)); // skipped
    cpu.place32(2, enc::addi(10, 10, 4)); // executed
    CHECK_EQ(cpu.run(2), std::uint64_t{2});
    CHECK_EQ(cpu.hart.pc, std::uint64_t{0x8000000c});
    CHECK_EQ(cpu.hart.get_reg(10), std::uint64_t{9});

    // bne a0, a1 -> not taken (a0 == a1)
    cpu.hart.reset(cpu.ram.base());
    cpu.hart.set_reg(10, 5);
    cpu.hart.set_reg(11, 5);
    cpu.place32(0, enc::branch(8, 11, 10, 1));
    CHECK_EQ(cpu.step(), true);
    CHECK_EQ(cpu.hart.pc, std::uint64_t{0x80000004});

    // bne a0, a1 -> taken when they differ
    cpu.hart.reset(cpu.ram.base());
    cpu.hart.set_reg(10, 4);
    cpu.hart.set_reg(11, 5);
    cpu.place32(0, enc::branch(8, 11, 10, 1));
    CHECK_EQ(cpu.step(), true);
    CHECK_EQ(cpu.hart.pc, std::uint64_t{0x80000008});
}

TEST(execute, branch_condition_matrix) {
    struct BranchCase {
        u32 funct3;
        bool taken_when_equal;
        bool taken_when_a0_smaller;
    };
    const BranchCase cases[] = {{0, true, false},  {1, false, true}, {4, false, true},
                                {5, true, false},  {6, false, true}, {7, true, false}};

    CpuFixture cpu;
    for (const BranchCase& test_case : cases) {
        // a0 == a1 == 5
        cpu.hart.reset(cpu.ram.base());
        cpu.hart.set_reg(10, 5);
        cpu.hart.set_reg(11, 5);
        cpu.place32(0, enc::branch(8, 11, 10, test_case.funct3));
        CHECK_EQ(cpu.step(), true);
        CHECK_EQ(cpu.hart.pc, test_case.taken_when_equal ? std::uint64_t{0x80000008}
                                                        : std::uint64_t{0x80000004});

        // a0 = 4 < a1 = 5, both signed and unsigned
        cpu.hart.reset(cpu.ram.base());
        cpu.hart.set_reg(10, 4);
        cpu.hart.set_reg(11, 5);
        cpu.place32(0, enc::branch(8, 11, 10, test_case.funct3));
        CHECK_EQ(cpu.step(), true);
        CHECK_EQ(cpu.hart.pc, test_case.taken_when_a0_smaller ? std::uint64_t{0x80000008}
                                                               : std::uint64_t{0x80000004});
    }
}

TEST(execute, backward_branch_loops) {
    CpuFixture cpu;
    cpu.hart.set_reg(10, 4); // counter

    // addi a0, a0, -1 ; bne a0, zero, -4 -> four iterations, then fall through
    cpu.place32(0, enc::addi(10, 10, -1));
    cpu.place32(1, enc::branch(-4, 0, 10, 1));
    cpu.place32(2, enc::addi(11, 11, 1)); // executed once, after the loop

    CHECK_EQ(cpu.run(9), std::uint64_t{9});
    CHECK_EQ(cpu.hart.get_reg(10), std::uint64_t{0});
    CHECK_EQ(cpu.hart.get_reg(11), std::uint64_t{1});
    CHECK_EQ(cpu.hart.pc, std::uint64_t{0x8000000c});
}

TEST(execute, jal_and_jalr) {
    CpuFixture cpu;
    cpu.place32(0, enc::jal(1, 8)); // jal ra, +8
    cpu.place32(1, enc::nop());     // skipped
    cpu.place32(2, enc::ecall());   // traps once reached

    CHECK_EQ(cpu.step(), true);
    CHECK_EQ(cpu.hart.get_reg(1), std::uint64_t{0x80000004});
    CHECK_EQ(cpu.hart.pc, std::uint64_t{0x80000008});

    // JALR must clear bit 0 of the computed target.
    cpu.hart.set_reg(5, 0x80000009);
    cpu.hart.pc = 0x80000000;
    cpu.place32(0, enc::jalr(0, 5, 0));
    CHECK_EQ(cpu.step(), true);
    CHECK_EQ(cpu.hart.pc, std::uint64_t{0x80000008});

    // The instruction at 0x80000008 is the ECALL, so the next step traps.
    CHECK_EQ(cpu.step(), false);
    CHECK(cpu.trap.cause == TrapCause::EnvironmentCallMachine);
}

TEST(execute, ecall_raises_environment_call_for_the_current_privilege) {
    CpuFixture cpu;
    cpu.place32(0, enc::ecall());

    CHECK_EQ(cpu.step(), false);
    CHECK(cpu.trap.cause == TrapCause::EnvironmentCallMachine);

    cpu.hart.privilege = Privilege::User;
    cpu.hart.pc = 0x80000000;
    CHECK_EQ(cpu.step(), false);
    CHECK(cpu.trap.cause == TrapCause::EnvironmentCallUser);

    cpu.hart.privilege = Privilege::Supervisor;
    cpu.hart.pc = 0x80000000;
    CHECK_EQ(cpu.step(), false);
    CHECK(cpu.trap.cause == TrapCause::EnvironmentCallSupervisor);
}

TEST(execute, ebreak_raises_breakpoint_with_the_pc_in_tval) {
    CpuFixture cpu;
    cpu.place32(0, enc::ebreak());
    CHECK_EQ(cpu.step(), false);
    CHECK(cpu.trap.cause == TrapCause::Breakpoint);
    CHECK_EQ(cpu.trap.tval, std::uint64_t{0x80000000});
}

TEST(execute, unsupported_encodings_raise_illegal_instruction) {
    CpuFixture cpu;

    // funct7 == 0x01 on OP-32 with funct3 = 1..3 has no RV64M meaning.
    cpu.place32(0, enc::opcode_r(0x01, 11, 11, 1, 10, 0x3b));
    CHECK_EQ(cpu.step(), false);
    CHECK(cpu.trap.cause == TrapCause::IllegalInstruction);
    CHECK_EQ(cpu.hart.retired, std::uint64_t{0});

    cpu.hart.pc = 0x80000000;
    cpu.place32(0, enc::amo(0x02, false, false, 10, /*rs2=*/1, 10, false)); // LR with rs2 != 0
    CHECK_EQ(cpu.step(), false);
    CHECK(cpu.trap.cause == TrapCause::IllegalInstruction);

    cpu.hart.pc = 0x80000000;
    cpu.place32(0, enc::opcode_r(0x7c, 11, 12, 3, 10, 0x2f)); // undefined funct5
    CHECK_EQ(cpu.step(), false);
    CHECK(cpu.trap.cause == TrapCause::IllegalInstruction);

    cpu.hart.pc = 0x80000000;
    cpu.place32(0, enc::opcode_r(0x08, 0, 12, 0, 10, 0x2f)); // .D encoding with funct3 = 0
    CHECK_EQ(cpu.step(), false);
    CHECK(cpu.trap.cause == TrapCause::IllegalInstruction);

    cpu.hart.pc = 0x80000000;
    cpu.hart.csr.set_raw(cpu::kCsrMstatus,0);
    cpu.place32(0, enc::fld(10, 10, 0)); // D extension, stage 3
    CHECK_EQ(cpu.step(), false);
    CHECK(cpu.trap.cause == TrapCause::IllegalInstruction);

    cpu.hart.pc = 0x80000000;
    cpu.hart.privilege = cpu::Privilege::User;
    cpu.place32(0, enc::mret());
    CHECK_EQ(cpu.step(), false);
    CHECK(cpu.trap.cause == TrapCause::IllegalInstruction);

    cpu.hart.pc = 0x80000000;
    cpu.place32(0, enc::wfi());
    CHECK_EQ(cpu.step(), false);
    CHECK(cpu.trap.cause == TrapCause::IllegalInstruction);

    cpu.hart.pc = 0x80000000;
    cpu.place32(0, enc::sfence_vma());
    CHECK_EQ(cpu.step(), false);
    CHECK(cpu.trap.cause == TrapCause::IllegalInstruction);

    cpu.hart.pc = 0x80000000;
    cpu.place32(0, enc::csrr(10, 0x300)); // Machine CSR is inaccessible in U mode.
    CHECK_EQ(cpu.step(), false);
    CHECK(cpu.trap.cause == TrapCause::IllegalInstruction);
}

TEST(execute, reserved_bits_in_shift_immediates_are_illegal) {
    CpuFixture cpu;

    // RV64I shift immediates use a 6-bit shamt: bits [31:26] must be zero, while
    // bit [25] is shamt[5].
    cpu.place32(0, enc::opcode_i(0x041, 11, 1, 11, 0x13)); // SLLI with imm[11:6] != 0
    CHECK_EQ(cpu.step(), false);
    CHECK(cpu.trap.cause == TrapCause::IllegalInstruction);

    cpu.hart.pc = 0x80000000;
    cpu.place32(0, enc::opcode_i(0x7e5, 11, 5, 11, 0x13)); // SR funct7 == 0x0f
    CHECK_EQ(cpu.step(), false);
    CHECK(cpu.trap.cause == TrapCause::IllegalInstruction);

    cpu.hart.pc = 0x80000000;
    cpu.place32(0, enc::opcode_i(0x081, 11, 5, 11, 0x13)); // imm[11:6] == 0b000010, unallocated
    CHECK_EQ(cpu.step(), false);
    CHECK(cpu.trap.cause == TrapCause::IllegalInstruction);
}

TEST(execute, rv64_shift_immediates_accept_a_six_bit_shamt) {
    CpuFixture cpu;
    cpu.hart.set_reg(11, 1);

    cpu.place32(0, enc::slli(11, 11, 63)); // slli a1, a1, 63
    CHECK_EQ(cpu.step(), true);
    CHECK_EQ(cpu.hart.get_reg(11), std::uint64_t{0x8000000000000000ull});

    cpu.hart.pc = 0x80000000;
    cpu.place32(0, enc::srai(11, 11, 63)); // srai a1, a1, 63
    CHECK_EQ(cpu.step(), true);
    CHECK_EQ(cpu.hart.get_reg(11), std::uint64_t{0xffffffffffffffffull});
}

TEST(execute, invalid_store_funct3_is_illegal) {
    CpuFixture cpu;
    cpu.place32(0, enc::store(11, 10, 0, 4)); // funct3 = 100 is not a store
    CHECK_EQ(cpu.step(), false);
    CHECK(cpu.trap.cause == TrapCause::IllegalInstruction);
}

TEST(execute, compressed_addi_retires_with_two_byte_pc) {
    CpuFixture cpu;
    cpu.place16(0, 0x0505); // c.addi a0, a0, 1
    CHECK_EQ(cpu.step(), true);
    CHECK_EQ(cpu.hart.get_reg(10),std::uint64_t{1});
    CHECK_EQ(cpu.hart.pc, std::uint64_t{0x80000002});
}

TEST(execute, access_faults_carry_the_faulting_address) {
    CpuFixture cpu(0x1000, 0x80000000); // small RAM so the access is out of range
    cpu.hart.set_reg(10, 0x80001000);
    cpu.hart.set_reg(11, 1);

    cpu.place32(0, enc::load(12, 10, 0, 3)); // ld a2, 0(a0)
    CHECK_EQ(cpu.step(), false);
    CHECK(cpu.trap.cause == TrapCause::LoadAccessFault);
    CHECK_EQ(cpu.trap.tval, std::uint64_t{0x80001000});

    cpu.hart.pc = 0x80000000;
    cpu.place32(0, enc::store(11, 10, 0, 3)); // sd a1, 0(a0)
    CHECK_EQ(cpu.step(), false);
    CHECK(cpu.trap.cause == TrapCause::StoreAccessFault);
    CHECK_EQ(cpu.trap.tval, std::uint64_t{0x80001000});
}

TEST(execute, instruction_access_fault_on_unmapped_pc) {
    CpuFixture cpu(0x1000, 0x80000000);
    cpu.hart.pc = 0x90000000;
    CHECK_EQ(cpu.step(), false);
    CHECK(cpu.trap.cause == TrapCause::InstructionAccessFault);
    CHECK_EQ(cpu.trap.tval, std::uint64_t{0x90000000});
}

TEST(execute, misaligned_pc_faults) {
    CpuFixture cpu;
    cpu.hart.pc = 0x80000001;
    CHECK_EQ(cpu.step(), false);
    CHECK(cpu.trap.cause == TrapCause::InstructionAddressMisaligned);
    CHECK_EQ(cpu.trap.tval, std::uint64_t{0x80000001});
}

TEST(execute, fence_and_fence_i_retire) {
    CpuFixture cpu;
    cpu.place32(0, enc::fence_i());
    cpu.place32(1, enc::fence());
    CHECK_EQ(cpu.run(2), std::uint64_t{2});
    CHECK_EQ(cpu.hart.pc, std::uint64_t{0x80000008});
}

TEST(execute, auipc_uses_the_instruction_pc) {
    CpuFixture cpu;
    cpu.place32(0, enc::auipc(10, 1));
    CHECK_EQ(cpu.step(), true);
    CHECK_EQ(cpu.hart.get_reg(10), std::uint64_t{0x80001000});

    cpu.hart.pc = 0x80000004;
    cpu.place32(1, enc::auipc(10, 0)); // auipc at 0x80000004
    CHECK_EQ(cpu.step(), true);
    CHECK_EQ(cpu.hart.get_reg(10), std::uint64_t{0x80000004});
}

TEST(execute, a_faulting_load_does_not_write_its_destination) {
    CpuFixture cpu(0x1000, 0x80000000);
    cpu.hart.set_reg(10, 0x80001000);
    cpu.place32(0, enc::load(11, 10, 0, 3)); // ld a1, 0(a0) -> fault
    CHECK_EQ(cpu.step(), false);
    CHECK(cpu.trap.cause == TrapCause::LoadAccessFault);
    CHECK_EQ(cpu.hart.get_reg(11), std::uint64_t{0});
}

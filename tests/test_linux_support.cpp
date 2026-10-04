// SPDX-License-Identifier: MIT
#include "cpu/compressed.hpp"
#include "cpu/privileged.hpp"
#include "devices/virt_devices.hpp"
#include "mmu/mmu.hpp"
#include "test_framework.hpp"
#include "test_helpers.hpp"
using namespace nanorv;
using namespace nanorv::cpu;
using namespace nanorv::test;
namespace {
bool exec(CpuFixture &c, u32 raw) {
  c.hart.next_pc = c.hart.pc + 4;
  return execute(c.hart, c.space, decode(raw), c.trap);
}
u32 fp(u32 fn, u32 rm = 0, u32 rd = 3, u32 a = 1, u32 b = 2) {
  return enc::opcode_r(fn, b, a, rm, rd, 0x53);
}
struct Paged : CpuFixture {
  static constexpr u64 va = 0x400000, root = 0x80001000, l1 = 0x80002000,
                       l0 = 0x80003000, data = 0x80004000;
  Paged() {
    hart.csr.set_raw(0x3a0, 0x1f);
    hart.csr.set_raw(0x3b0, (1ull << 54) - 1);
    hart.csr.set_raw(kCsrSatp, (8ull << 60) | (root >> 12));
    hart.privilege = Privilege::Supervisor;
    space.store(root, 64, (l1 >> 12) << 10 | 1);
    space.store(l1 + 16, 64, (l0 >> 12) << 10 | 1);
    leaf(7);
    space.store(data, 64, 0x123456789abcdef0);
  }
  void leaf(u64 flags) { space.store(l0, 64, (data >> 12) << 10 | flags); }
};
} // namespace
TEST(mmu, cached_translation_isolated_by_access_privilege_and_sfence) {
  Paged c;
  c.hart.translation_cache_enabled = true;
  u64 value = 0;
  CHECK(mmu::load(c.hart, c.space, Paged::va, 64, value, c.trap));
  CHECK_EQ(value, 0x123456789abcdef0ull);
  // Reading may set A but cannot skip the first write's D-bit update.
  CHECK(mmu::store(c.hart, c.space, Paged::va, 64, 123, c.trap));
  c.space.load(Paged::l0, 64, value);
  CHECK_EQ(value & 192, 192ull);
  c.hart.privilege = Privilege::User;
  CHECK(!mmu::load(c.hart, c.space, Paged::va, 64, value, c.trap));
  c.hart.privilege = Privilege::Supervisor;
  c.leaf(1); // page-table edits become visible after SFENCE.VMA
  CHECK(exec(c, 0x12000073));
  CHECK(!mmu::load(c.hart, c.space, Paged::va, 64, value, c.trap));
}
TEST(mmu, victim_tlb_recovers_conflicts_and_rejects_stale_permissions) {
  // Two Sv39 mappings collide in the primary cache. Their cached translations
  // must swap without another page walk, but SFENCE and privilege changes must
  // still make an old victim translation unusable.
  Paged c;c.hart.translation_cache_enabled=true;
  const u64 other=Paged::va+Hart::kTlbEntries*4096ull;
  const u64 other_table=kDramBase+0x6000,other_data=kDramBase+0x7000;
  CHECK(c.space.store(Paged::l1+24,64,(other_table>>12)<<10|1));
  CHECK(c.space.store(other_table,64,(other_data>>12)<<10|7));
  CHECK(c.space.store(other_data,64,77));
  u64 value=0;
  CHECK(mmu::load(c.hart,c.space,Paged::va,64,value,c.trap));
  CHECK(mmu::load(c.hart,c.space,other,64,value,c.trap));CHECK_EQ(value,77ull);
  CHECK_EQ(c.hart.victim_tlb[0][0].vpn,Paged::va>>12);
  // Deliberately remove the leaf without SFENCE: the already cached mapping is
  // still eligible, so recovering it cannot depend on a successful page walk.
  c.leaf(0);
  CHECK(mmu::load(c.hart,c.space,Paged::va,64,value,c.trap));
  CHECK_EQ(value,0x123456789abcdef0ull);
  CHECK(mmu::load(c.hart,c.space,other,64,value,c.trap));
  c.hart.privilege=Privilege::User;
  CHECK(!mmu::load(c.hart,c.space,Paged::va,64,value,c.trap));
  c.hart.privilege=Privilege::Supervisor;
  CHECK(exec(c,0x12000073));
  CHECK(!mmu::load(c.hart,c.space,Paged::va,64,value,c.trap));
  CHECK(c.trap.cause==TrapCause::LoadPageFault);
}
TEST(mmu, amo_unreadable_page_reports_store_page_fault_without_modification) {
  Paged c;c.leaf(1);c.hart.set_reg(10,Paged::va);c.hart.set_reg(11,1);
  CHECK(!exec(c,enc::amoadd(12,11,10,false)));
  CHECK(c.trap.cause==TrapCause::StorePageFault);CHECK_EQ(c.trap.tval,Paged::va);
  u64 value;CHECK(c.space.load(Paged::data,64,value));CHECK_EQ(value,0x123456789abcdef0ull);
}
TEST(mmu, native_tlb_context_serial_reuse_clears_unvisited_old_descriptors) {
  CpuFixture c;c.hart.translation_cache_enabled=true;
  const u64 address=kDramBase+16384;
  CHECK(c.space.store(address,64,0x123456789abcdef0ull));
  u64 value=0;CHECK(mmu::load(c.hart,c.space,address,64,value,c.trap));
  const auto index=(address>>12)&(cpu::Hart::kTlbEntries-1);
  const auto original_tag=c.hart.native_tlb[0][index].tag;
  CHECK_EQ(original_tag,(address&~4095ull)|c.hart.native_tlb_tag);
  // Do not visit this page while the context serial cycles. Lazy tags must
  // not resurrect its old addend on serial reuse.
  for(unsigned n=0;n<510;++n) {++c.hart.translation_epoch;mmu::prepare_native_tlb(c.hart,c.space);}
  CHECK_EQ(c.hart.native_tlb_tag,8ull);
  CHECK_EQ(c.hart.native_tlb[0][index].tag,~0ull);
  CHECK(mmu::load(c.hart,c.space,address,64,value,c.trap));
  CHECK_EQ(value,0x123456789abcdef0ull);CHECK_EQ(c.hart.native_tlb[0][index].tag,original_tag);
  CHECK_EQ(c.hart.native_tlb[1][index].tag,~0ull); // Read does not grant write.
}
TEST(mmu, native_tlb_victim_recovery_republishes_the_correct_host_addend) {
  CpuFixture c(4u<<20);c.hart.translation_cache_enabled=true;
  const u64 first=kDramBase+16384,second=first+cpu::Hart::kTlbEntries*4096ull;
  CHECK(c.space.store(first,64,17));CHECK(c.space.store(second,64,91));
  u64 value=0;CHECK(mmu::load(c.hart,c.space,first,64,value,c.trap));
  CHECK(mmu::load(c.hart,c.space,second,64,value,c.trap));CHECK_EQ(value,91ull);
  CHECK(mmu::load(c.hart,c.space,first,64,value,c.trap));CHECK_EQ(value,17ull);
  const auto& entry=c.hart.native_tlb[0][(first>>12)&(cpu::Hart::kTlbEntries-1)];
  CHECK_EQ(entry.tag,(first&~4095ull)|c.hart.native_tlb_tag);
  CHECK_EQ(entry.addend+first,reinterpret_cast<std::uintptr_t>(c.space.direct(first,8,memory::Access::Read)));
}
TEST(mmu, cache_does_not_bypass_subpage_pmp_boundaries) {
  CpuFixture c;
  c.hart.translation_cache_enabled = true;
  c.hart.privilege = Privilege::Supervisor;
  c.hart.csr.set_raw(0x3a0, 0x11); // NA4 read-only, exactly four bytes
  c.hart.csr.set_raw(0x3b0, kDramBase >> 2);
  u64 value;
  CHECK(mmu::load(c.hart, c.space, kDramBase, 32, value, c.trap));
  CHECK(!mmu::load(c.hart, c.space, kDramBase + 4, 32, value, c.trap));
  CHECK(!mmu::store(c.hart, c.space, kDramBase, 32, 0, c.trap));
}
TEST(privileged, delegation_and_sret_restore_faulting_pc_and_status) {
  CpuFixture c;
  c.hart.privilege = Privilege::User;
  c.hart.pc = 0x1234;
  c.hart.csr.set_raw(kCsrMedeleg, 1ull << 8);
  c.hart.csr.set_raw(kCsrStvec, 0x80000100);
  c.hart.csr.set_raw(kCsrMstatus, 2);
  enter_trap(c.hart, {TrapCause::EnvironmentCallUser, 0, false});
  CHECK(c.hart.privilege == Privilege::Supervisor);
  CHECK_EQ(c.hart.pc, 0x80000100ull);
  CHECK_EQ(c.hart.csr.raw(kCsrSepc), 0x1234ull);
  CHECK_EQ(c.hart.csr.raw(kCsrMstatus) & 0x122, 0x20ull);
  CHECK(exec(c, enc::sret()));
  CHECK(c.hart.privilege == Privilege::User);
  CHECK_EQ(c.hart.next_pc, 0x1234ull);
  CHECK_EQ(c.hart.csr.raw(kCsrMstatus) & 0x122, 0x22ull);
}
TEST(privileged, machine_return_and_vectored_interrupt_priority) {
  CpuFixture c;
  c.hart.privilege = Privilege::Supervisor;
  c.hart.pc = 0x9998;
  c.hart.csr.set_raw(kCsrMtvec, 0x80001001);
  c.hart.csr.set_raw(kCsrMip, 0x880);
  c.hart.csr.set_raw(kCsrMie, 0x880);
  Trap t;
  CHECK(pending_interrupt(c.hart, t));
  CHECK_EQ(static_cast<u64>(t.cause), (1ull << 63) | 11);
  enter_trap(c.hart, t);
  CHECK_EQ(c.hart.pc, 0x8000102cull);
  CHECK_EQ(c.hart.csr.raw(kCsrMepc), 0x9998ull);
  CHECK(exec(c, enc::mret()));
  CHECK(c.hart.privilege == Privilege::Supervisor);
  CHECK_EQ(c.hart.next_pc, 0x9998ull);
  c.hart.csr.set_raw(kCsrMideleg, 0x200);
  c.hart.csr.set_raw(kCsrMip, 0x200);
  c.hart.csr.set_raw(kCsrMie, 0x200);
  CHECK(!pending_interrupt(c.hart, t));
  c.hart.csr.set_raw(kCsrMstatus, 2);
  CHECK(pending_interrupt(c.hart, t));
}
TEST(privileged, sfence_accepts_operand_registers_and_tvm_traps) {
  CpuFixture c;
  c.hart.privilege = Privilege::Supervisor;
  CHECK(exec(c, 0x12b50073));
  c.hart.csr.set_raw(kCsrMstatus, 1ull << 20);
  CHECK(!exec(c, 0x12b50073));
  CHECK(!exec(c, enc::csrr(10, kCsrSatp)));
}
TEST(csr, rs1_register_number_controls_write_and_read_cannot_be_suppressed) {
  CpuFixture c;
  c.hart.set_reg(5, 0);
  CHECK(!exec(
      c, enc::csrrs(0, kCsrCycle, 5))); // attempts a write even with value zero
  CHECK(!exec(c, enc::csrrs(0, 0x999, 0))); // CSRRS always reads
  c.hart.privilege = Privilege::User;
  CHECK(!exec(c, enc::csrr(10, kCsrTime)));
  c.hart.csr.set_raw(kCsrMcounteren, 7);
  c.hart.csr.set_raw(kCsrScounteren, 7);
  CHECK(exec(c, enc::csrr(10, kCsrTime)));
}
TEST(csr, counter_writes_override_the_writing_instruction_increment) {
  CpuFixture c;
  c.hart.retired = 100;
  c.hart.set_reg(5, 42);
  CHECK(exec(c, enc::csrrw(0, 0xb02, 5)));
  CHECK(exec(c, enc::csrr(10, kCsrInstret)));
  CHECK_EQ(c.hart.get_reg(10), 42ull);
}
TEST(mmu, sv39_walk_updates_accessed_and_dirty_and_rejects_noncanonical) {
  Paged c;
  u64 v = 0;
  CHECK(mmu::load(c.hart, c.space, Paged::va, 64, v, c.trap));
  CHECK_EQ(v, 0x123456789abcdef0ull);
  CHECK(c.space.load(Paged::l0, 64, v));
  CHECK_EQ(v & 0xc0, 0x40ull);
  CHECK(mmu::store(c.hart, c.space, Paged::va, 64, 9, c.trap));
  CHECK(c.space.load(Paged::l0, 64, v));
  CHECK_EQ(v & 0xc0, 0xc0ull);
  CHECK(!mmu::load(c.hart, c.space, 1ull << 39, 64, v, c.trap));
  CHECK(c.trap.cause == TrapCause::LoadPageFault);
}
TEST(mmu, sum_mxr_user_permissions_and_mprv) {
  Paged c;
  u64 v;
  c.leaf(0x19); // V, X, U
  CHECK(!mmu::load(c.hart, c.space, Paged::va, 64, v, c.trap));
  c.hart.csr.set_raw(kCsrMstatus, (1ull << 18) | (1ull << 19));
  CHECK(mmu::load(c.hart, c.space, Paged::va, 64, v, c.trap));
  CHECK(!mmu::load(c.hart, c.space, Paged::va, 16, v, c.trap, true));
  c.hart.privilege = Privilege::User;
  CHECK(mmu::load(c.hart, c.space, Paged::va, 16, v, c.trap, true));
  c.hart.privilege = Privilege::Machine;
  c.hart.csr.set_raw(kCsrMstatus, (1ull << 17) | (1ull << 19));
  CHECK(mmu::load(c.hart, c.space, Paged::va, 64, v, c.trap));
  CHECK(!mmu::store(c.hart, c.space, Paged::va, 64, 1, c.trap));
}
TEST(mmu, superpage_alignment_and_cross_page_store_fault) {
  Paged c;
  u64 v;
  c.space.store(Paged::l1 + 16, 64, ((Paged::data >> 12) << 10) | 15);
  CHECK(!mmu::load(c.hart, c.space, Paged::va, 64, v, c.trap));
  CHECK(c.trap.cause == TrapCause::LoadPageFault);
  c.space.store(Paged::l1 + 16, 64, ((kDramBase >> 12) << 10) | 15);
  CHECK(mmu::load(c.hart, c.space, Paged::va, 64, v, c.trap));
  c.space.store(Paged::l1 + 16, 64, ((Paged::l0 >> 12) << 10) | 1);
  c.leaf(7);
  c.space.store(Paged::data + 4092, 32, 0xfeedbeef);
  CHECK(!mmu::store(c.hart, c.space, Paged::va + 4092, 64, 0, c.trap));
  CHECK(c.space.load(Paged::data + 4092, 32, v));
  CHECK_EQ(v, 0xfeedbeefull);
}
TEST(pmp, locked_permissions_and_reservation_width) {
  CpuFixture c;
  u64 v;
  c.hart.csr.set_raw(0x3b0, kDramBase >> 2);
  c.hart.csr.set_raw(0x3a0, 0x90); // locked NA4, no access
  CHECK(!mmu::load(c.hart, c.space, kDramBase, 32, v, c.trap));
  CHECK(c.trap.cause == TrapCause::LoadAccessFault);
  CHECK(c.hart.csr.write(0x3b0, Privilege::Machine, 0));
  CHECK_EQ(c.hart.csr.raw(0x3b0), kDramBase >> 2);
  c.hart.set_reservation(kDramBase, 64);
  c.hart.note_store(kDramBase + 4, 8);
  CHECK(!c.hart.reservation_valid);
}
TEST(compressed, reserved_encodings_and_link_length) {
  CpuFixture c;
  CHECK_EQ(expand_compressed(0), 0u);
  CHECK(!exec(c, 0));
  CHECK_EQ(c.trap.tval, 0ull);
  c.hart.set_reg(5, 0x80000200);
  c.hart.next_pc = c.hart.pc + 2;
  CHECK(execute(c.hart, c.space, decode(0x9282), c.trap)); // c.jalr t0
  CHECK_EQ(c.hart.get_reg(1), 0x80000002ull);
  CHECK_EQ(c.hart.next_pc, 0x80000200ull);
}
TEST(fp, arithmetic_rounding_flags_and_nan_boxing) {
  CpuFixture c;
  c.hart.f[1] = 0xffffffff3f800000;
  c.hart.f[2] = 0xffffffff33800000; // 1 + 2^-24
  CHECK(exec(c, fp(0, 0)));
  CHECK_EQ(c.hart.f[3], 0xffffffff3f800000ull);
  CHECK_EQ(c.hart.csr.fflags(), 1ull);
  CHECK(exec(c, fp(0, 4)));
  CHECK_EQ(c.hart.f[3], 0xffffffff3f800001ull);
  c.hart.f[1] = 0x3f800000;
  CHECK(exec(c, fp(0)));
  CHECK_EQ(c.hart.f[3], 0xffffffff7fc00000ull);
  c.hart.f[1] = 0x3ff0000000000000;
  c.hart.f[2] = 0x4000000000000000;
  CHECK(exec(c, fp(1)));
  CHECK_EQ(c.hart.f[3], 0x4008000000000000ull);
  c.hart.f[2] = 0;
  CHECK(exec(c, fp(13)));
  CHECK_EQ(c.hart.f[3], 0x7ff0000000000000ull);
  CHECK(c.hart.csr.fflags() & 8);
}
TEST(fp, min_max_nan_signed_zero_class_and_conversion_saturation) {
  CpuFixture c;
  c.hart.f[1] = 0xffffffff80000000;
  c.hart.f[2] = 0xffffffff00000000;
  CHECK(exec(c, fp(20)));
  CHECK_EQ(c.hart.f[3], 0xffffffff80000000ull);
  CHECK(exec(c, fp(20, 1)));
  CHECK_EQ(c.hart.f[3], 0xffffffff00000000ull);
  c.hart.f[1] = 0xffffffff7f800001;
  c.hart.f[2] = 0xffffffff3f800000;
  CHECK(exec(c, fp(20)));
  CHECK_EQ(c.hart.f[3], c.hart.f[2]);
  CHECK(c.hart.csr.fflags() & 16);
  CHECK(exec(c, fp(112, 1, 3, 1, 0)));
  CHECK_EQ(c.hart.get_reg(3), 256ull);
  CHECK(exec(c, fp(96, 0, 3, 1, 0)));
  CHECK_EQ(c.hart.get_reg(3), 0x7fffffffull);
  c.hart.f[1] = 0xffffffffbf800000;
  CHECK(exec(c, fp(96, 0, 3, 1, 1)));
  CHECK_EQ(c.hart.get_reg(3), 0ull);
  CHECK(!exec(c, fp(0, 5)));
  c.hart.csr.set_raw(kCsrMstatus, 0);
  CHECK(!exec(c, fp(0)));
}
TEST(fp, fused_multiply_add_and_memory_preserve_payload) {
  CpuFixture c;
  c.hart.f[1] = 0xffffffff3fc00000;
  c.hart.f[2] = 0xffffffff40000000;
  c.hart.f[4] = 0xffffffff3f800000;
  CHECK(exec(c, (4u << 27) | (2u << 20) | (1u << 15) | (3u << 7) | 0x43));
  CHECK_EQ(c.hart.f[3], 0xffffffff40800000ull);
  c.hart.set_reg(10, kDramBase + 0x100);
  c.space.store(kDramBase + 0x100, 64, 0x7ff0000000000042);
  CHECK(exec(c, enc::fld(5, 10, 0)));
  CHECK_EQ(c.hart.f[5], 0x7ff0000000000042ull);
}
TEST(devices, clint_timer_and_uart_plic_claim_complete) {
  Hart h;
  h.reset(0);
  devices::Clint clint;
  CHECK(clint.store(0x4000, 64, 100));
  clint.time = 99;
  clint.update(h);
  CHECK(!(h.csr.raw(kCsrMip) & 128));
  clint.time = 100;
  clint.update(h);
  CHECK(h.csr.raw(kCsrMip) & 128);
  devices::Uart uart;
  devices::Plic plic;
  CHECK(uart.store(1, 8, 1));
  uart.inject('x');
  CHECK(uart.interrupt());
  CHECK(plic.store(40, 32, 1));
  CHECK(plic.store(0x2080, 32, 1u << 10));
  plic.update(h, uart.interrupt());
  CHECK(h.csr.raw(kCsrMip) & 512);
  u64 v;
  CHECK(plic.load(0x201004, 32, v));
  CHECK_EQ(v, 10ull);
  CHECK(uart.load(0, 8, v));
  CHECK_EQ(v, static_cast<u64>('x'));
  plic.update(h, uart.interrupt());
  CHECK(plic.store(0x201004, 32, 10));
  plic.update(h, uart.interrupt());
  CHECK(!(h.csr.raw(kCsrMip) & 512));
  CHECK(!uart.load(0, 64, v));
  CHECK(!clint.load(0x8000, 64, v));
}

// SPDX-License-Identifier: MIT
#include "cpu/block_engine.hpp"
#include "test_framework.hpp"
#include "test_helpers.hpp"
using namespace nanorv;
using namespace nanorv::test;
TEST(block_engine, matches_reference_arithmetic_memory_and_branch) {
  CpuFixture reference, cached;
  reference.hart.set_reg(5,kDramBase+0x800);
  cached.hart.set_reg(5,kDramBase+0x800);
  const u32 instructions[] = {enc::addi(1,0,-123),enc::addi(2,0,19),
      enc::opcode_r(0,2,1,0,3,0x33),enc::opcode_r(0x20,1,2,0,4,0x33),
      enc::opcode_r(1,2,1,0,6,0x33),enc::opcode_s(0,6,5,3),
      enc::opcode_i(0,5,3,7,3),enc::opcode_b(8,6,7,0)};
  for (u32 n=0;n<std::size(instructions);++n) {reference.place32(n,instructions[n]);cached.place32(n,instructions[n]);}
  CHECK_EQ(reference.run(std::size(instructions)),std::size(instructions));
  cpu::BlockEngine engine;
  const auto result=engine.run(cached.hart,cached.space,cached.trap);
  CHECK(result.handled);CHECK(!result.trapped);CHECK_EQ(result.retired,8u);
  CHECK_EQ(reference.hart.pc,cached.hart.pc);
  CHECK_EQ(reference.hart.retired,cached.hart.retired);
  for (unsigned n=0;n<32;++n) CHECK_EQ(reference.hart.get_reg(n),cached.hart.get_reg(n));
  u64 a,b;reference.space.load(kDramBase+0x800,64,a);cached.space.load(kDramBase+0x800,64,b);CHECK_EQ(a,b);
}
TEST(block_engine, observes_code_changes_and_precise_fault_after_retirement) {
  CpuFixture c;cpu::BlockEngine engine;
  c.place16(0,0x0085); // c.addi ra,1
  c.place16(1,0x0000); // reserved c.addi4spn
  auto result=engine.run(c.hart,c.space,c.trap);
  CHECK(result.trapped);CHECK_EQ(result.retired,1u);CHECK_EQ(c.hart.pc,kDramBase+2);
  CHECK_EQ(c.trap.tval,0ull);CHECK_EQ(c.hart.get_reg(1),1ull);
  c.hart.pc=kDramBase;c.place16(0,0x0089); // c.addi ra,2, same physical address
  result=engine.run(c.hart,c.space,c.trap,1);
  CHECK(!result.trapped);CHECK_EQ(result.retired,1u);CHECK_EQ(c.hart.get_reg(1),3ull);
}
TEST(block_engine, page_boundary_falls_back_without_fetching_unmapped_bytes) {
  CpuFixture c(4096);cpu::BlockEngine engine;
  c.hart.pc=kDramBase+4094;c.place16(2047,0x0001);
  const auto result=engine.run(c.hart,c.space,c.trap);
  CHECK(!result.handled);CHECK(c.step());CHECK_EQ(c.hart.pc,kDramBase+4096);
}
TEST(block_engine, hot_jit_block_tracks_code_changes_after_compilation) {
  CpuFixture c;cpu::BlockEngine engine;
  c.place32(0,enc::addi(1,1,1));c.place32(1,enc::opcode_b(-4,0,0,0));
  for(unsigned n=0;n<8;++n) {
    auto result=engine.run(c.hart,c.space,c.trap,32,true);
    CHECK(result.handled);CHECK(!result.trapped);CHECK_EQ(result.retired,2u);
  }
  CHECK_EQ(c.hart.get_reg(1),8ull);
  c.place32(0,enc::addi(1,1,7));
  for(unsigned n=0;n<8;++n) {
    auto result=engine.run(c.hart,c.space,c.trap,32,true);
    CHECK(!result.trapped);CHECK_EQ(result.retired,2u);
  }
  CHECK_EQ(c.hart.get_reg(1),64ull);
}

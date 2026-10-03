// SPDX-License-Identifier: MIT
#include "platform/platform.hpp"
#include "cpu/jit_x64.hpp"
#include "test_framework.hpp"
#include "test_helpers.hpp"
#include <array>
#include <cstring>
#include <thread>
#include <set>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
using namespace rvvm;
using namespace rvvm::test;

TEST(platform, wake_event_combines_external_event_notification_and_timeout) {
#if defined(_WIN32)
  struct Event {HANDLE handle=CreateEventW(nullptr,TRUE,FALSE,nullptr);~Event(){if(handle)CloseHandle(handle);}} external;
  CHECK(external.handle);platform::WakeEvent wake;
  CHECK(!wake.wait_ms(0,external.handle));
  CHECK(SetEvent(external.handle));CHECK(wake.wait_ms(0,external.handle));
  CHECK(wake.wait_ms(0,external.handle)); // manual reset belongs to its owner
  CHECK(ResetEvent(external.handle));CHECK(!wake.wait_ms(1,external.handle));
  wake.notify();CHECK(wake.wait_ms(0,external.handle));CHECK(!wake.wait_ms(0,external.handle));
  std::jthread sender([&] {platform::sleep_ms(5);SetEvent(external.handle);});
  CHECK(wake.wait_ms(2000,external.handle));
#endif
}

TEST(platform, code_memory_has_coherent_distinct_rw_and_rx_views) {
#if defined(_M_X64) || defined(__x86_64__)
  platform::CodeMemory memory(4096);
  CHECK(memory.writable_data());CHECK(memory.executable_data());
  CHECK_NE(memory.writable_data(),memory.executable_data());
  const std::array<u8,6> first{0xb8,42,0,0,0,0xc3},second{0xb8,99,0,0,0,0xc3};
  std::memcpy(memory.writable_data(),first.data(),first.size());
  CHECK(memory.publish(0,first.size()));
  using Function=unsigned(*)();
  const auto* code=static_cast<const u8*>(memory.executable_data());
  CHECK_EQ(reinterpret_cast<Function>(code)(),42u);
  std::memcpy(static_cast<u8*>(memory.writable_data())+32,second.data(),second.size());
  CHECK(memory.publish(32,second.size()));
  CHECK_EQ(reinterpret_cast<Function>(code+32)(),99u);
  CHECK_EQ(reinterpret_cast<Function>(code)(),42u);
  CHECK(!memory.publish(4096,1));
#if defined(_WIN32)
  MEMORY_BASIC_INFORMATION writable{},executable{};
  CHECK(VirtualQuery(memory.writable_data(),&writable,sizeof(writable)));
  CHECK(VirtualQuery(memory.executable_data(),&executable,sizeof(executable)));
  CHECK_EQ(writable.Protect,PAGE_READWRITE);CHECK_EQ(executable.Protect,PAGE_EXECUTE_READ);
  CHECK_EQ(executable.Type,MEM_MAPPED);
#endif
#endif
}
TEST(jit, code_pool_append_and_cross_thread_release_preserve_other_entries) {
  std::array<cpu::CachedInstruction,48> instructions;
  std::vector<std::unique_ptr<cpu::NativeBlock>> blocks;
  for(unsigned n=0;n<instructions.size();++n) {
    const auto word=enc::addi(1,0,static_cast<i32>(n+1));instructions[n]={cpu::decode(word),word};
    blocks.push_back(cpu::NativeBlock::compile({&instructions[n],1}));CHECK(blocks.back());
  }
#if defined(_WIN32)
  std::set<void*> allocations;
  for(const auto& block:blocks) {
    MEMORY_BASIC_INFORMATION info{};CHECK(VirtualQuery(block->entry(),&info,sizeof(info)));
    allocations.insert(info.AllocationBase);
  }
  CHECK(allocations.size()<blocks.size()/2); // independent batches must be packed, not page-allocated
#endif
  std::thread release([&] {for(unsigned n=1;n<blocks.size();n+=2)blocks[n].reset();});release.join();
  for(unsigned n=0;n<blocks.size();n+=2) {
    CpuFixture c;CHECK_EQ(blocks[n]->run(c.hart,c.space,c.trap),1u);CHECK_EQ(c.hart.get_reg(1),n+1ull);
  }
  // Appending fresh code after old neighbours were released must not overwrite
  // any still-live instruction range or unmap a shared region early.
  auto appended=cpu::NativeBlock::compile({&instructions[1],1});CHECK(appended);
  CpuFixture c;CHECK_EQ(appended->run(c.hart,c.space,c.trap),1u);CHECK_EQ(c.hart.get_reg(1),2ull);
  CHECK_EQ(blocks[0]->run(c.hart,c.space,c.trap),1u);CHECK_EQ(c.hart.get_reg(1),1ull);
}

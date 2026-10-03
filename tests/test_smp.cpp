// SPDX-License-Identifier: MIT
#include "machine/machine.hpp"
#include "machine/parallel_clock.hpp"
#include "mmu/mmu.hpp"
#include "test_framework.hpp"
#include "test_helpers.hpp"
#include "platform/platform.hpp"
#include <barrier>
#include <thread>
#include <set>
#include <atomic>
using namespace rvvm;
using namespace rvvm::test;
TEST(smp, fast_subword_stores_and_generic_stores_do_not_lose_updates) {
  CpuFixture c;c.space.set_concurrent(true);
  auto* data=c.space.direct(kDramBase+0x800,8,memory::Access::Write);
  std::barrier start(3);
  std::thread first([&]{start.arrive_and_wait();for(unsigned n=0;n<20000;++n)c.space.write_fast<8>(data,0x55);});
  std::thread second([&]{start.arrive_and_wait();for(unsigned n=0;n<20000;++n)c.space.store(kDramBase+0x801,16,0xbeef);});
  start.arrive_and_wait();first.join();second.join();
  CHECK_EQ(c.space.read_fast<64>(data),0xbeef55ull);
}
TEST(smp, ram_publication_preserves_payload_before_flag) {
  CpuFixture c;c.space.set_concurrent(true);std::atomic<bool> failed{false};
  auto* data=c.space.direct(kDramBase+0x800,1024,memory::Access::Write);
  std::thread sender([&]{
    for(u64 sequence=1;sequence<=10000;++sequence) {
      while(c.space.read_fast<64>(data+512)!=sequence-1)std::this_thread::yield();
      c.space.write_fast<64>(data,sequence);
      c.space.write_fast<32>(data+64,static_cast<u32>(~sequence));
      c.space.write_fast<64>(data+256,sequence);
    }
  });
  std::thread receiver([&]{
    for(u64 sequence=1;sequence<=10000;++sequence) {
      while(c.space.read_fast<64>(data+256)!=sequence)std::this_thread::yield();
      if(c.space.read_fast<64>(data)!=sequence || c.space.read_fast<32>(data+64)!=static_cast<u32>(~sequence))failed=true;
      c.space.write_fast<64>(data+512,sequence);
    }
  });
  sender.join();receiver.join();CHECK(!failed);
}
TEST(smp, cache_line_dma_preserves_unaligned_edges_and_adjacent_cpu_bytes) {
  CpuFixture c;c.space.set_concurrent(true);
  std::vector<u8> original(8192,0x33),readback(8192);
  for(u64 offset=0;offset<16;++offset)for(unsigned length:{1u,7u,8u,15u,63u,64u,65u,127u,4097u}) {
    CHECK(c.space.write_bytes(kDramBase+0x1000,original));
    std::vector<u8> input(length);for(unsigned n=0;n<length;++n)input[n]=static_cast<u8>(n*37+5);
    CHECK(c.space.write_bytes(kDramBase+0x1000+offset,input));
    CHECK(c.space.read_bytes(kDramBase+0x1000,readback));
    for(std::size_t n=0;n<readback.size();++n)CHECK_EQ(readback[n],n>=offset && n<offset+length?input[n-offset]:original[n]);
  }
  CHECK(c.space.fill_zero(kDramBase+0x1000,8192));
  std::array<u8,37> input;input.fill(0xa5);std::barrier start(3);
  auto* ptr=c.space.direct(kDramBase+0x1000,64,memory::Access::Write);
  std::thread cpu([&]{start.arrive_and_wait();for(unsigned n=0;n<10000;++n)c.space.write_fast<8>(ptr+12,0x5a);});
  std::thread dma([&]{start.arrive_and_wait();for(unsigned n=0;n<10000;++n)c.space.write_bytes(kDramBase+0x100d,input);});
  start.arrive_and_wait();cpu.join();dma.join();
  CHECK_EQ(c.space.read_fast<8>(ptr+12),0x5aull);
  for(unsigned n=13;n<50;++n)CHECK_EQ(c.space.read_fast<8>(ptr+n),0xa5ull);
  CHECK_EQ(c.space.read_fast<8>(ptr+50),0ull);
}
TEST(smp, long_dma_interval_invalidates_reservations_across_hashed_stripes) {
  CpuFixture c;c.space.set_concurrent(true);
  const auto lr=cpu::decode(enc::opcode_r(2<<2,0,1,3,3,0x2f));
  const auto sc=cpu::decode(enc::opcode_r(3<<2,2,1,3,3,0x2f));
  for(u64 offset:{0x8040ull,0x48040ull,0x80000ull,0xd0000ull}) {
    c.hart.set_reg(1,kDramBase+offset);c.hart.set_reg(2,17);
    CHECK(cpu::execute(c.hart,c.space,lr,c.trap));
    c.space.notify_dma_write(kDramBase+0x8000,0xd0000);
    CHECK(cpu::execute(c.hart,c.space,sc,c.trap));CHECK_EQ(c.hart.get_reg(3),1ull);
  }
}
TEST(smp, guest_fence_and_annotated_amo_order_store_before_load) {
  // Store buffering: after both harts store then synchronize, both reading zero
  // is forbidden. Use actual guest instructions and disjoint RAM/cache stripes.
  for(unsigned ordering=0;ordering<4;++ordering) {
    CpuFixture c;c.space.set_concurrent(true);
    std::barrier phase(3);std::atomic<bool> failed{false};std::array<u64,2> observations{};
    const auto fence=cpu::decode(0x0330000f);
    const auto amo=cpu::decode(enc::opcode_r((1<<2)|ordering,2,1,3,3,0x2f)); // .rl / .aq / .aqrl
    auto worker=[&](unsigned index) {
      cpu::Hart hart;hart.reset(kDramBase);hart.set_reg(1,kDramBase+0xa00+index*64);hart.set_reg(2,1);cpu::Trap trap{};
      for(unsigned iteration=0;iteration<2000;++iteration) {
        phase.arrive_and_wait();
        c.space.store(kDramBase+0x800+index*64,64,1);
        if(!cpu::execute(hart,c.space,ordering?amo:fence,trap))failed=true;
        c.space.load(kDramBase+0x800+(1-index)*64,64,observations[index]);
        phase.arrive_and_wait();
      }
    };
    std::thread first(worker,0),second(worker,1);
    for(unsigned iteration=0;iteration<2000;++iteration) {
      CHECK(c.space.store(kDramBase+0x800,64,0));CHECK(c.space.store(kDramBase+0x840,64,0));
      phase.arrive_and_wait();phase.arrive_and_wait();
      if(observations[0]==0 && observations[1]==0)failed=true;
    }
    first.join();second.join();CHECK(!failed);
  }
}
TEST(smp, virtual_clock_handles_parked_cpus_parallel_work_and_migration) {
  machine::ParallelClock clock(4,123);
  std::array<u64,4> work{};CHECK_EQ(clock.advance(work,false),123ull);
  work[0]=100'000'000;CHECK_EQ(clock.advance(work,false),10'000'123ull);
  work={200'000'000,100'000'000,100'000'000,100'000'000};
  CHECK_EQ(clock.advance(work,false),20'000'123ull);
  work[1]+=100'000'000;CHECK_EQ(clock.advance(work,false),30'000'123ull);
  CHECK_EQ(clock.advance(work,true),30'010'123ull);
  work[2]+=3;CHECK_EQ(clock.advance(work,false),30'010'123ull);
  work[3]+=7;CHECK_EQ(clock.advance(work,false),30'010'124ull);
}
TEST(smp, stores_and_dma_invalidate_other_hart_reservations) {
  machine::MachineConfig config;config.ram_size=1<<20;config.hart_count=2;
  machine::Machine m(config);
  auto& first=m.hart(0);auto& second=m.hart(1);
  first.set_reservation(kDramBase+0x100,64);
  second.translation_cache_enabled=true;
  cpu::Trap trap;
  CHECK(mmu::store(second,m.memory(),kDramBase+0x200,64,1,trap));
  CHECK(first.reservation_valid);
  CHECK(mmu::store(second,m.memory(),kDramBase+0x100,64,2,trap)); // TLB hit
  CHECK(!first.reservation_valid);
  first.set_reservation(kDramBase+0x100,64);
  const std::array<u8,4> bytes{1,2,3,4};
  CHECK(m.memory().write_bytes(kDramBase+0x104,bytes));CHECK(!first.reservation_valid);
}
TEST(smp, clint_routes_software_and_timer_interrupts_per_hart) {
  devices::Clint c;cpu::Hart first,second;second.hart_id=1;
  CHECK(c.store(4,32,1));CHECK(c.store(0x4008,64,100));c.time=150;
  c.update(first);c.update(second);
  CHECK_EQ(first.csr.raw(cpu::kCsrMip)&0x88,0ull);
  CHECK_EQ(second.csr.raw(cpu::kCsrMip)&0x88,0x88ull);
  CHECK(c.store(4,32,0));c.update(second);
  CHECK_EQ(second.csr.raw(cpu::kCsrMip)&0x88,0x80ull);
}
TEST(smp, plic_contexts_and_mhartid_are_independent) {
  machine::MachineConfig config;config.ram_size=1<<20;config.hart_count=4;
  machine::Machine m(config);
  u64 value=0;
  CHECK(m.hart(3).csr.read(cpu::kCsrMhartid,cpu::Privilege::Machine,0,value));CHECK_EQ(value,3ull);
  devices::Plic p;cpu::Hart h;h.hart_id=2;
  CHECK(p.store(4,32,7));CHECK(p.store(0x2280,32,2)); // hart 2 supervisor context 5
  p.set_irq(1,true);p.update(h,false);CHECK(h.csr.raw(cpu::kCsrMip)&0x200);
  CHECK(p.load(0x205004,32,value));CHECK_EQ(value,1ull);
  p.update(m.hart(0),false);CHECK_EQ(m.hart(0).csr.raw(cpu::kCsrMip)&0xa00,0ull);
}
TEST(smp, parallel_workers_are_distinct_and_instruction_budget_is_exact) {
  machine::MachineConfig config;config.ram_size=1<<20;config.hart_count=4;
  config.engine=machine::EngineKind::Jit;config.jit_workers=2;config.max_instructions=4'000'000;
  machine::Machine m(config);
  CHECK(m.memory().store(kDramBase,32,enc::addi(1,1,1)));
  CHECK(m.memory().store(kDramBase+4,32,enc::opcode_j(-4,0)));
  const auto result=m.run();CHECK(!result.trapped);CHECK(result.reason==machine::StopReason::InstructionLimit);
  CHECK_EQ(result.instructions,config.max_instructions);
  CHECK_EQ(std::set<u64>(result.hart_host_ids.begin(),result.hart_host_ids.end()).size(),4u);
  u64 sum=0;for(u32 n=0;n<4;++n) {CHECK(m.hart(n).retired>0);sum+=m.hart(n).retired;CHECK_NE(result.hart_host_ids[n],platform::thread_id());}
  CHECK_EQ(sum,result.instructions);
}
TEST(smp, parallel_mmio_pollers_complete_with_bounded_io_dispatch) {
  machine::MachineConfig config;config.ram_size=1<<20;config.hart_count=4;
  config.engine=machine::EngineKind::Jit;config.jit_workers=0;config.max_instructions=50000;
  machine::Machine m(config);
  CHECK(m.memory().store(kDramBase,32,enc::opcode_u(0x2000,1,0x37))); // CLINT
  CHECK(m.memory().store(kDramBase+4,32,enc::opcode_i(0,1,2,2,3)));
  CHECK(m.memory().store(kDramBase+8,32,enc::addi(3,3,1)));
  CHECK(m.memory().store(kDramBase+12,32,enc::opcode_j(-8,0)));
  const auto result=m.run();CHECK(!result.trapped);CHECK_EQ(result.instructions,50000ull);
  CHECK(result.reason==machine::StopReason::InstructionLimit);
}
TEST(smp, concurrent_mixed_width_stores_preserve_adjacent_bytes) {
  CpuFixture c;c.space.set_concurrent(true);
  std::barrier start(3);
  auto write=[&](u64 offset,u64 value) {start.arrive_and_wait();for(unsigned n=0;n<20000;++n)c.space.store(kDramBase+offset,8,value);};
  std::thread a(write,0,0x55),b(write,1,0xaa);start.arrive_and_wait();a.join();b.join();
  u64 value=0;CHECK(c.space.load(kDramBase,64,value));CHECK_EQ(value,0xaa55ull);
}
TEST(smp, concurrent_amo_add_has_no_lost_updates) {
  machine::MachineConfig config;config.ram_size=1<<20;config.hart_count=4;
  machine::Machine m(config);m.memory().set_concurrent(true);
  const auto instruction=cpu::decode(enc::opcode_r(0,2,1,3,3,0x2f));
  std::barrier start(4);std::atomic<bool> failed{false};std::vector<std::thread> threads;
  for(u32 n=0;n<4;++n)threads.emplace_back([&,n] {
    auto& h=m.hart(n);h.set_reg(1,kDramBase+0x800);h.set_reg(2,1);cpu::Trap trap{};start.arrive_and_wait();
    for(unsigned i=0;i<10000;++i)if(!cpu::execute(h,m.memory(),instruction,trap))failed=true;
  });
  for(auto& thread:threads)thread.join();u64 value=0;CHECK(!failed);CHECK(m.memory().load(kDramBase+0x800,64,value));CHECK_EQ(value,40000ull);
}
TEST(smp, ordinary_subword_and_amo_updates_share_one_nonrecursive_critical_section) {
  CpuFixture c;c.space.set_concurrent(true);
  const auto address=kDramBase+0x800;
  c.hart.set_reg(1,address);c.hart.set_reg(2,1);
  const auto amo=cpu::decode(enc::opcode_r(0,2,1,2,3,0x2f)); // AMOADD.W
  auto* ptr=c.space.direct(address,8,memory::Access::Write);
  std::barrier start(3);std::atomic<bool> failed{false};
  std::thread atomic([&] {
    start.arrive_and_wait();
    for(unsigned n=0;n<20000;++n)if(!cpu::execute(c.hart,c.space,amo,c.trap))failed=true;
  });
  std::thread ordinary([&] {
    start.arrive_and_wait();
    for(unsigned n=0;n<20000;++n)c.space.write_fast<8>(ptr+4,0x55);
  });
  start.arrive_and_wait();atomic.join();ordinary.join();CHECK(!failed);
  CHECK_EQ(c.space.read_fast<64>(ptr),(0x55ull<<32)|20000ull);
}
TEST(smp, locked_store_requires_owning_matching_guard_and_tracks_code_and_reservations) {
  CpuFixture c;c.space.set_concurrent(true);
  const auto address=kDramBase+0x800;
  const auto generation=c.space.code_generation(address);
  memory::AddressSpace::AtomicGuard empty;
  CHECK(!c.space.store_locked(address,64,99,empty));
  auto wrong=c.space.atomic_guard(address+64);
  CHECK(!c.space.store_locked(address,64,99,wrong));wrong.unlock();
  auto guard=c.space.atomic_guard(address);
  const auto version=c.space.reservation_version(address);
  CHECK(!c.space.store_locked(address+1,64,99,guard));
  CHECK(c.space.store_locked(address,64,0x123456789abcdef0ull,guard));
  CHECK_EQ(c.space.reservation_version(address),version+1);
  CHECK(c.space.store_locked(address+2,16,0x4321,guard));
  CHECK_EQ(c.space.reservation_version(address),version+2);guard.unlock();
  u64 value=0;CHECK(c.space.load(address,64,value));CHECK_EQ(value,0x123456784321def0ull);
  CHECK_EQ(c.space.code_generation(address),generation+2);
  const auto before_compare=c.space.code_generation(address);
  u64 expected=17;
  CHECK(c.space.compare_exchange(address,64,expected,91));CHECK_EQ(expected,value);
  CHECK_EQ(c.space.code_generation(address),before_compare);
  CHECK(c.space.compare_exchange(address,64,expected,91));
  CHECK(c.space.load(address,64,value));CHECK_EQ(value,91ull);
  CHECK_EQ(c.space.code_generation(address),before_compare+1);
}
TEST(smp, parallel_lr_sc_rejects_aba_and_dma_stores) {
  CpuFixture c;c.space.set_concurrent(true);c.hart.set_reg(1,kDramBase+0x800);c.hart.set_reg(2,9);
  const auto lr=cpu::decode(enc::opcode_r(2<<2,0,1,3,3,0x2f));
  const auto sc=cpu::decode(enc::opcode_r(3<<2,2,1,3,3,0x2f));
  CHECK(cpu::execute(c.hart,c.space,lr,c.trap));
  std::thread other([&] {c.space.store(kDramBase+0x800,64,1);c.space.store(kDramBase+0x800,64,0);});other.join();
  CHECK(cpu::execute(c.hart,c.space,sc,c.trap));CHECK_EQ(c.hart.get_reg(3),1ull);
  CHECK(cpu::execute(c.hart,c.space,lr,c.trap));const std::array<u8,1> byte{7};CHECK(c.space.write_bytes(kDramBase+0x804,byte));
  CHECK(cpu::execute(c.hart,c.space,sc,c.trap));CHECK_EQ(c.hart.get_reg(3),1ull);
  CHECK(cpu::execute(c.hart,c.space,lr,c.trap));CHECK(cpu::execute(c.hart,c.space,sc,c.trap));CHECK_EQ(c.hart.get_reg(3),0ull);
}
TEST(smp, softfloat_rounding_and_flags_are_thread_local) {
  std::barrier start(2);std::atomic<bool> failed{false};
  auto divide=[&](u32 mode,u64 expected,u64 denominator,u64 flags) {
    CpuFixture c;c.hart.f[1]=0x3ff0000000000000;c.hart.f[2]=denominator;
    const auto instruction=cpu::decode(enc::opcode_r(13,2,1,mode,3,0x53));start.arrive_and_wait();
    for(unsigned i=0;i<20000;++i) {
      c.hart.csr.set_fflags(0);
      if(!cpu::execute(c.hart,c.space,instruction,c.trap) || c.hart.f[3]!=expected || c.hart.csr.fflags()!=flags)failed=true;
    }
  };
  std::thread a(divide,0,0x3fd5555555555555ull,0x4008000000000000ull,1),b(divide,3,0x3ff0000000000000ull,0x3ff0000000000000ull,0);
  a.join();b.join();CHECK(!failed);
}

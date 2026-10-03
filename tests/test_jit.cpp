// SPDX-License-Identifier: MIT
#include "cpu/block_engine.hpp"
#include "test_framework.hpp"
#include "test_helpers.hpp"
#include <random>
#include "cpu/ir.hpp"
#include "platform/platform.hpp"
#include "mmu/mmu.hpp"
using namespace rvvm;
using namespace rvvm::test;
static_assert(offsetof(cpu::NativeChain,sampled_loads)<=127); // Generated disp8 ABI.
TEST(jit, packed_code_pages_keep_other_blocks_alive_after_eviction) {
  const std::array<u32,3> words{enc::addi(1,1,7),enc::addi(1,1,13),enc::addi(1,1,19)};
  std::array<cpu::CachedInstruction,3> instructions;
  std::array<std::span<const cpu::CachedInstruction>,4> spans;
  for(unsigned n=0;n<3;++n) {instructions[n]={cpu::decode(words[n]),words[n]};spans[n]={&instructions[n],1};}
  auto blocks=cpu::NativeBlock::compile_many(spans);CHECK_EQ(blocks.size(),4u);CHECK(!blocks[3]);
  for(unsigned n=0;n<3;++n)CHECK(blocks[n]);
  const auto first=reinterpret_cast<std::uintptr_t>(blocks[0]->entry());
  const auto last=reinterpret_cast<std::uintptr_t>(blocks[2]->entry());
  CHECK(last>first);CHECK(last-first<platform::kPageSize);CHECK_EQ(last&15,0ull);
  CpuFixture c;CHECK_EQ(blocks[1]->run(c.hart,c.space,c.trap),1u);blocks[1].reset();
  CHECK_EQ(blocks[0]->run(c.hart,c.space,c.trap),1u);blocks[0].reset();
  CHECK_EQ(blocks[2]->run(c.hart,c.space,c.trap),1u);CHECK_EQ(c.hart.get_reg(1),39ull);
}
TEST(jit, inline_ram_loads_match_width_signedness_aliasing_and_fallbacks) {
  for(bool concurrent:{false,true})for(u32 funct=0;funct<=6;++funct)for(u32 rd:{0u,1u,2u})
    for(u32 offset:{0u,1u,2u,4u,7u,8u,4095u}) {
      CpuFixture ref,fast;
      const u64 address=kDramBase+16384+offset;
      for(auto* c:{&ref,&fast}) {
        c->space.set_concurrent(concurrent);c->hart.translation_cache_enabled=true;
        c->hart.set_reg(1,address-8);c->hart.set_reg(2,123);
        for(unsigned n=0;n<16;++n)CHECK(c->space.store(address+n,8,0xf8u+n));
        u64 value=0;CHECK(mmu::load(c->hart,c->space,address,8u<<(funct&3),value,c->trap));
      }
      const std::array<u32,3> words{enc::addi(1,1,8),enc::opcode_i(0,1,funct,rd,3),enc::addi(2,2,17)};
      std::vector<cpu::CachedInstruction> instructions;
      for(const auto word:words) {
        instructions.push_back({cpu::decode(word),word});
        ref.hart.next_pc=ref.hart.pc+4;CHECK(cpu::execute(ref.hart,ref.space,instructions.back().decoded,ref.trap));ref.hart.pc=ref.hart.next_pc;
      }
      cpu::NativeChain chain;
      chain.resolve=[](cpu::NativeChain*,cpu::Hart*,const memory::AddressSpace*,cpu::Trap*,u32)->const void* {return nullptr;};
      chain.sampling=true;
      auto native=cpu::NativeBlock::compile(instructions);CHECK(native);CHECK_EQ(native->run(fast.hart,fast.space,fast.trap,&chain),3u);
      const auto width=1u<<(funct&3);
      CHECK_EQ(chain.sampled_loads,(address&(width-1))?0ull:1ull);
      CHECK_EQ(ref.hart.pc,fast.hart.pc);CHECK_EQ(ref.hart.retired,fast.hart.retired);
      for(unsigned reg=0;reg<32;++reg)CHECK_EQ(ref.hart.get_reg(reg),fast.hart.get_reg(reg));
    }
}
TEST(jit, inline_loads_reject_stale_permissions_bus_epoch_and_disabled_cache) {
  for(unsigned condition=0;condition<6;++condition) {
    CpuFixture c,other;c.hart.translation_cache_enabled=true;
    c.hart.set_reg(1,kDramBase+16384);CHECK(c.space.store(kDramBase+16384,64,0x123456789abcdef0));
    u64 value=0;CHECK(mmu::load(c.hart,c.space,c.hart.get_reg(1),64,value,c.trap));
    cpu::NativeChain chain;
    chain.resolve=[](cpu::NativeChain*,cpu::Hart*,const memory::AddressSpace*,cpu::Trap*,u32)->const void* {return nullptr;};
    chain.sampling=true;
    // Change actual execution context after warming the descriptor. Each
    // change must reject the old native hit, then use the full MMU path.
    if(condition==0)++c.hart.translation_epoch;
    if(condition==1)c.hart.csr.set_raw(cpu::kCsrMstatus,1ull<<18);
    if(condition==2)c.hart.csr.set_raw(cpu::kCsrSatp,1);
    const auto& bus=condition==3?other.space:c.space;
    if(condition==4)c.hart.translation_cache_enabled=false;
    if(condition==5)chain.inline_loads=false;
    const auto word=enc::opcode_i(0,1,3,2,3);cpu::CachedInstruction instruction{cpu::decode(word),word};
    auto native=cpu::NativeBlock::compile({&instruction,1});CHECK(native);
    CHECK_EQ(native->run(c.hart,bus,c.trap,&chain),1u);CHECK_EQ(chain.sampled_loads,0ull);
    CHECK_EQ(c.hart.get_reg(2),condition==3?0ull:value);
  }
}
TEST(jit, deferred_inline_load_commits_preserve_mixed_hits_misses_and_faults) {
  for(bool concurrent:{false,true})for(bool fault:{false,true}) {
    CpuFixture ref,fast;
    for(auto* c:{&ref,&fast}) {
      c->space.set_concurrent(concurrent);c->hart.translation_cache_enabled=true;
      c->hart.set_reg(20,kDramBase+16384);c->hart.set_reg(21,kDramBase+20480);
      CHECK(c->space.store(kDramBase+16384,64,0x123456789abcdef0));
      CHECK(c->space.store(kDramBase+20480,64,0xfedcba9876543210));
      u64 ignored=0;CHECK(mmu::load(c->hart,c->space,kDramBase+16384,64,ignored,c->trap));
    }
    std::vector<cpu::CachedInstruction> source;
    for(unsigned n=0;n<3;++n)for(const auto word:{enc::addi(1,1,7),enc::addi(2,2,13),
        enc::opcode_i(0,20,3,4,3),enc::addi(4,4,17),enc::opcode_i(0,21,3,5,3),
        enc::opcode_r(0,5,4,4,1,0x33),enc::opcode_s(8,1,20,3),
        enc::opcode_i(8,20,3,1,3),enc::addi(2,1,-7)})source.push_back({cpu::decode(word),word});
    if(fault) {const auto word=enc::opcode_i(0,31,3,3,3);source.push_back({cpu::decode(word),word});}
    u32 count=0;
    for(const auto& instruction:source) {
      ref.hart.next_pc=ref.hart.pc+4;
      if(!cpu::execute(ref.hart,ref.space,instruction.decoded,ref.trap))break;
      ref.hart.pc=ref.hart.next_pc;++count;
    }
    cpu::NativeChain chain;
    chain.resolve=[](cpu::NativeChain*,cpu::Hart*,const memory::AddressSpace*,cpu::Trap*,u32)->const void* {return nullptr;};
    chain.sampling=true;
    auto native=cpu::NativeBlock::compile(source);CHECK(native);
    CHECK_EQ(native->run(fast.hart,fast.space,fast.trap,&chain),count|(fault?0x80000000u:0u));
    CHECK(chain.sampled_loads>0);CHECK_EQ(ref.hart.pc,fast.hart.pc);CHECK_EQ(ref.hart.retired,fast.hart.retired);
    for(unsigned reg=0;reg<32;++reg)CHECK_EQ(ref.hart.get_reg(reg),fast.hart.get_reg(reg));
    if(fault) {CHECK(fast.trap.cause==ref.trap.cause);CHECK_EQ(fast.trap.tval,ref.trap.tval);}
    u64 a=0,b=0;CHECK(ref.space.load(kDramBase+16392,64,a));CHECK(fast.space.load(kDramBase+16392,64,b));CHECK_EQ(a,b);
  }
}
TEST(jit, compact_tlb_sv39_remap_permissions_and_high_virtual_alias) {
  CpuFixture c;c.hart.translation_cache_enabled=true;
  constexpr u64 root=kDramBase+0x1000,l1=kDramBase+0x2000,l0=kDramBase+0x3000;
  constexpr u64 first=kDramBase+0x4000,second=kDramBase+0x5000;
  c.hart.csr.set_raw(0x3a0,0x1f);c.hart.csr.set_raw(0x3b0,(1ull<<54)-1);
  c.hart.csr.set_raw(cpu::kCsrSatp,(8ull<<60)|(root>>12));
  c.hart.privilege=cpu::Privilege::Supervisor;
  CHECK(c.space.store(root,64,(l1>>12)<<10|1));
  CHECK(c.space.store(root+256*8,64,(l1>>12)<<10|1));
  CHECK(c.space.store(l1+16,64,(l0>>12)<<10|1));
  CHECK(c.space.store(first,64,17));CHECK(c.space.store(second,64,91));
  const auto load_word=enc::opcode_i(0,1,3,2,3),store_word=enc::opcode_s(0,3,1,3);
  cpu::CachedInstruction load{cpu::decode(load_word),load_word},store{cpu::decode(store_word),store_word};
  auto load_code=cpu::NativeBlock::compile({&load,1}),store_code=cpu::NativeBlock::compile({&store,1});
  CHECK(load_code);CHECK(store_code);
  for(const u64 address:{0x400000ull,0xffffffc000400000ull}) {
    c.hart.set_reg(1,address);c.hart.set_reg(3,123);
    CHECK(c.space.store(l0,64,(first>>12)<<10|7));++c.hart.translation_epoch;
    u64 value=0;CHECK(mmu::load(c.hart,c.space,address,64,value,c.trap));
    CHECK(mmu::store(c.hart,c.space,address,64,17,c.trap));
    cpu::NativeChain chain;chain.sampling=true;
    chain.resolve=[](cpu::NativeChain*,cpu::Hart*,const memory::AddressSpace*,cpu::Trap*,u32)->const void* {return nullptr;};
    CHECK_EQ(load_code->run(c.hart,c.space,c.trap,&chain),1u);CHECK_EQ(chain.sampled_loads,1ull);
    CHECK_EQ(c.hart.get_reg(2),17ull);
    CHECK_EQ(store_code->run(c.hart,c.space,c.trap,&chain),1u);
    CHECK(c.space.load(first,64,value));CHECK_EQ(value,123ull);
    // A different PA plus read-only permission after SFENCE must invalidate
    // both old descriptors, including the write permission already cached.
    CHECK(c.space.store(l0,64,(second>>12)<<10|3));++c.hart.translation_epoch;
    chain.sampled_loads=0;
    CHECK_EQ(load_code->run(c.hart,c.space,c.trap,&chain),1u);CHECK_EQ(chain.sampled_loads,0ull);
    CHECK_EQ(c.hart.get_reg(2),91ull);
    const auto pc=c.hart.pc,retired=c.hart.retired;
    CHECK_EQ(store_code->run(c.hart,c.space,c.trap,&chain),0x80000000u);
    CHECK(c.trap.cause==cpu::TrapCause::StorePageFault);CHECK_EQ(c.trap.tval,address);
    CHECK_EQ(c.hart.pc,pc);CHECK_EQ(c.hart.retired,retired);
    CHECK(c.space.load(second,64,value));CHECK_EQ(value,91ull);
    c.trap={};
  }
}
TEST(jit, compact_tlb_context_change_inside_external_span_keeps_helpers) {
  CpuFixture c;c.hart.translation_cache_enabled=true;c.hart.set_reg(1,kDramBase+16384);
  CHECK(c.space.store(c.hart.get_reg(1),64,17));
  u64 value=0;CHECK(mmu::load(c.hart,c.space,c.hart.get_reg(1),64,value,c.trap));
  const std::array<u32,2> words{0x12000073,enc::opcode_i(0,1,3,2,3)};
  std::array<cpu::CachedInstruction,2> source;
  for(unsigned n=0;n<2;++n)source[n]={cpu::decode(words[n]),words[n]};
  auto native=cpu::NativeBlock::compile(source);CHECK(native);
  cpu::NativeChain chain;chain.sampling=true;
  chain.resolve=[](cpu::NativeChain*,cpu::Hart*,const memory::AddressSpace*,cpu::Trap*,u32)->const void* {return nullptr;};
  CHECK_EQ(native->run(c.hart,c.space,c.trap,&chain),2u);
  CHECK_EQ(c.hart.get_reg(2),17ull);CHECK_EQ(chain.sampled_loads,0ull);
}
TEST(jit, compact_tlb_rejects_privilege_sum_mxr_mprv_and_pmp_changes) {
  for(unsigned condition=0;condition<5;++condition) {
    CpuFixture c;c.hart.translation_cache_enabled=true;
    constexpr u64 address=0x400000,root=kDramBase+0x1000,l1=kDramBase+0x2000;
    constexpr u64 l0=kDramBase+0x3000,data=kDramBase+0x4000;
    c.hart.csr.set_raw(0x3a0,0x1f);c.hart.csr.set_raw(0x3b0,(1ull<<54)-1);
    c.hart.csr.set_raw(cpu::kCsrSatp,(8ull<<60)|(root>>12));
    c.hart.privilege=condition==3?cpu::Privilege::Machine:cpu::Privilege::Supervisor;
    const u64 status=condition==1?1ull<<18:condition==2?1ull<<19:condition==3?(1ull<<17)|(1ull<<11):0;
    c.hart.csr.set_raw(cpu::kCsrMstatus,status);
    CHECK(c.space.store(root,64,(l1>>12)<<10|1));CHECK(c.space.store(l1+16,64,(l0>>12)<<10|1));
    CHECK(c.space.store(l0,64,(data>>12)<<10|(condition==1?0x13:condition==2?9:3)));
    CHECK(c.space.store(data,64,17));c.hart.set_reg(1,address);c.hart.set_reg(2,123);
    u64 value=0;CHECK(mmu::load(c.hart,c.space,address,64,value,c.trap));CHECK_EQ(value,17ull);
    if(condition==0)c.hart.privilege=cpu::Privilege::User;
    if(condition>=1 && condition<=3)c.hart.csr.set_raw(cpu::kCsrMstatus,0);
    if(condition==4) {
      c.hart.privilege=cpu::Privilege::Machine;
      c.hart.set_reg(5,0x18);c.hart.next_pc=c.hart.pc+4;
      CHECK(cpu::execute(c.hart,c.space,cpu::decode(enc::csrrw(0,0x3a0,5)),c.trap));
      c.hart.privilege=cpu::Privilege::Supervisor;
    }
    const auto word=enc::opcode_i(0,1,3,2,3);cpu::CachedInstruction source{cpu::decode(word),word};
    auto native=cpu::NativeBlock::compile({&source,1});CHECK(native);
    cpu::NativeChain chain;chain.sampling=true;
    chain.resolve=[](cpu::NativeChain*,cpu::Hart*,const memory::AddressSpace*,cpu::Trap*,u32)->const void* {return nullptr;};
    const auto pc=c.hart.pc,retired=c.hart.retired;
    CHECK_EQ(native->run(c.hart,c.space,c.trap,&chain),0x80000000u);
    CHECK_EQ(chain.sampled_loads,0ull);CHECK_EQ(c.hart.get_reg(2),123ull);
    CHECK_EQ(c.hart.pc,pc);CHECK_EQ(c.hart.retired,retired);CHECK_EQ(c.trap.tval,address);
    CHECK(c.trap.cause==(condition>=3?cpu::TrapCause::LoadAccessFault:cpu::TrapCause::LoadPageFault));
  }
}
TEST(jit, compact_tlb_invalid_tag_rejects_last_page_misaligned_access) {
  CpuFixture c;c.hart.translation_cache_enabled=true;c.hart.set_reg(1,~0ull);
  const auto word=enc::opcode_i(0,1,3,2,3);cpu::CachedInstruction source{cpu::decode(word),word};
  auto native=cpu::NativeBlock::compile({&source,1});CHECK(native);
  cpu::NativeChain chain;chain.sampling=true;
  chain.resolve=[](cpu::NativeChain*,cpu::Hart*,const memory::AddressSpace*,cpu::Trap*,u32)->const void* {return nullptr;};
  // Exercise every serial, including the highest allowed value and wrap.
  for(unsigned n=0;n<1024;++n) {
    ++c.hart.translation_epoch;c.trap={};
    CHECK_EQ(native->run(c.hart,c.space,c.trap,&chain),0x80000000u);
    CHECK(c.trap.cause==cpu::TrapCause::LoadAccessFault);CHECK_EQ(c.trap.tval,~0ull);
    CHECK_EQ(chain.sampled_loads,0ull);
  }
}
TEST(jit, predecoded_cpp_executors_match_reference_and_extension_fallbacks) {
  std::vector<u32> words;
  for(u32 opcode:{0x13u,0x1bu})for(u32 f=0;f<8;++f)for(i32 immediate:{-2048,-1,0,1,31,63,1024,2047})
    words.push_back(enc::opcode_i(immediate,1,f,3,opcode));
  for(u32 opcode:{0x33u,0x3bu})for(u32 f=0;f<8;++f)for(u32 fn:{0u,1u,4u,16u,32u,48u})
    words.push_back(enc::opcode_r(fn,2,1,f,3,opcode));
  for(u32 f=0;f<8;++f)words.push_back(enc::opcode_b(-4096,2,1,f));
  words.push_back(enc::opcode_j(-1048576,1));
  words.push_back(enc::opcode_i(-2048,1,0,1,0x67)); // JALR rd=rs1
  words.push_back(enc::opcode_u(0x80000,3,0x37));
  words.push_back(enc::opcode_u(0xfffff,3,0x17));
  words.push_back(0x00000000); // illegal compressed instruction
  CpuFixture reference,cached;std::mt19937_64 random(0x606);
  for(const auto raw:words)for(unsigned n=0;n<10;++n) {
    reference.hart.reset(kDramBase);cached.hart.reset(kDramBase);
    for(unsigned reg=1;reg<32;++reg)reference.hart.x[reg]=cached.hart.x[reg]=random();
    if(n<5) {
      static constexpr std::array<std::array<u64,2>,5> edges{{{0,0},{0x8000000000000000ull,~0ull},
        {0xffffffff80000000ull,~0ull},{1,0},{~0ull,1}}};
      reference.hart.x[1]=cached.hart.x[1]=edges[n][0];reference.hart.x[2]=cached.hart.x[2]=edges[n][1];
    }
    reference.trap={};cached.trap={};
    auto decoded=cpu::decode(raw);cpu::CachedInstruction instruction{decoded,raw};
    reference.hart.next_pc=reference.hart.pc+decoded.length;
    const bool expected=cpu::execute(reference.hart,reference.space,decoded,reference.trap);
    if(expected)reference.hart.pc=reference.hart.next_pc;
    const bool actual=cpu::NativeBlock::executor_for(decoded)(&cached.hart,&cached.space,&instruction,&cached.trap);
    CHECK_EQ(actual,expected);CHECK_EQ(cached.hart.pc,reference.hart.pc);
    CHECK_EQ(cached.hart.next_pc,reference.hart.next_pc);CHECK_EQ(cached.hart.retired,reference.hart.retired);
    for(unsigned reg=0;reg<32;++reg)CHECK_EQ(cached.hart.get_reg(reg),reference.hart.get_reg(reg));
    if(!expected) {CHECK(cached.trap.cause==reference.trap.cause);CHECK_EQ(cached.trap.tval,reference.trap.tval);}
  }
}
TEST(jit, integer_operations_match_reference_at_signed_boundaries) {
  CpuFixture reference,native;
  std::vector<u32> words;
  for (u32 f=0;f<8;++f) {
    words.push_back(enc::opcode_r(0,2,1,f,3,0x33));
    words.push_back(enc::opcode_i(f==1?63:f==5?63:-17,1,f,3,0x13));
  }
  for (u32 opcode:{0x33u,0x3bu}) {
    words.push_back(enc::opcode_r(32,2,1,0,3,opcode));
    words.push_back(enc::opcode_r(32,2,1,5,3,opcode));
    words.push_back(enc::opcode_r(1,2,1,0,3,opcode));
  }
  for (u32 f:{0u,1u,5u}) {
    words.push_back(enc::opcode_r(0,2,1,f,3,0x3b));
    words.push_back(enc::opcode_i(f==0?-1023:31,1,f,3,0x1b));
  }
  words.push_back(enc::opcode_i(0x43f,1,5,3,0x13));
  words.push_back(enc::opcode_i(0x41f,1,5,3,0x1b));
  words.push_back(enc::opcode_u(0x80001,3,0x37));
  words.push_back(enc::opcode_u(0x80001,3,0x17));
  words.push_back(enc::addi(0,1,123));
  for(u32 f:{0u,1u,4u,5u,6u,7u})words.push_back(enc::opcode_b(-16,2,1,f));
  words.push_back(enc::opcode_j(-32,3));
  words.push_back(enc::opcode_i(17,1,0,1,0x67)); // rd==rs1 must preserve the target
  std::mt19937_64 random(0x12345);
  const u64 values[]={0,1,~0ull,1ull<<63,(1ull<<63)-1,0xffffffff,0x80000000};
  for (const auto word:words) {
    const cpu::CachedInstruction instruction{cpu::decode(word),word};
    auto code=cpu::NativeBlock::compile({&instruction,1});CHECK(code!=nullptr);
    for (unsigned n=0;n<20;++n) {
      const auto a=n<std::size(values)?values[n]:random();
      const auto b=n<std::size(values)?values[std::size(values)-n-1]:random();
      reference.hart.reset(kDramBase);native.hart.reset(kDramBase);
      for(auto* h:{&reference.hart,&native.hart}) {h->set_reg(1,a);h->set_reg(2,b);}
      reference.hart.next_pc=reference.hart.pc+4;
      CHECK(cpu::execute(reference.hart,reference.space,instruction.decoded,reference.trap));
      reference.hart.pc=reference.hart.next_pc;
      CHECK_EQ(code->run(native.hart,native.space,native.trap),1u);
      CHECK_EQ(reference.hart.pc,native.hart.pc);CHECK_EQ(reference.hart.retired,native.hart.retired);
      for(unsigned r=0;r<32;++r) CHECK_EQ(reference.hart.get_reg(r),native.hart.get_reg(r));
    }
  }
}
TEST(jit, native_division_and_bit_operations_match_reference_at_edges) {
  const std::array<u64,8> values{0,1,~0ull,1ull<<63,(1ull<<63)-1,
                                0x80000000ull,0xffffffffull,0x0123456789abcdefull};
  std::vector<u32> words;
  for(u32 op:{0x33u,0x3bu}) {
    for(u32 f=4;f<=7;++f)words.push_back(enc::opcode_r(1,2,1,f,3,op));
    for(u32 f:{1u,5u})words.push_back(enc::opcode_r(48,2,1,f,3,op));
  }
  for(u32 f:{4u,6u,7u})words.push_back(enc::opcode_r(32,2,1,f,3,0x33));
  words.push_back(enc::opcode_i(0x6b8,1,5,3,0x13)); // rev8
  words.push_back(enc::opcode_i(0x604,1,1,3,0x13)); // sext.b
  words.push_back(enc::opcode_i(0x605,1,1,3,0x13)); // sext.h
  words.push_back(enc::opcode_r(4,0,1,4,3,0x3b));  // zext.h
  words.push_back(enc::opcode_i(0x63f,1,5,3,0x13)); // rori 63
  words.push_back(enc::opcode_i(0x61f,1,5,3,0x1b)); // roriw 31
  for(const auto original:words)for(u32 rd:{0u,1u,2u,3u}) {
    const u32 word=(original&~(31u<<7))|(rd<<7);
    cpu::CachedInstruction source{cpu::decode(word),word};
    auto native=cpu::NativeBlock::compile({&source,1});CHECK(native);
    CHECK_EQ(native->helper_calls(1,false),0ull);
    CpuFixture ref,fast;
    for(const auto a:values)for(const auto b:values) {
      for(auto* c:{&ref,&fast}) {
        c->hart.reset(kDramBase);c->hart.set_reg(1,a);c->hart.set_reg(2,b);c->hart.set_reg(3,91);
      }
      ref.hart.next_pc=ref.hart.pc+4;
      CHECK(cpu::execute(ref.hart,ref.space,source.decoded,ref.trap));ref.hart.pc=ref.hart.next_pc;
      CHECK_EQ(native->run(fast.hart,fast.space,fast.trap),1u);
      CHECK_EQ(fast.hart.pc,ref.hart.pc);CHECK_EQ(fast.hart.retired,ref.hart.retired);
      for(unsigned reg=0;reg<32;++reg)CHECK_EQ(fast.hart.get_reg(reg),ref.hart.get_reg(reg));
    }
  }
}
TEST(jit, cached_destinations_match_reference_for_operand_aliases_and_edges) {
  const std::array<u64,8> values{0,1,~0ull,1ull<<63,(1ull<<63)-1,
                                0x80000000ull,0xffffffffull,0x0123456789abcdefull};
  std::vector<u32> operations;
  for(const u32 op:{0x33u,0x3bu}) {
    for(const u32 f:{0u,1u,5u}) {
      operations.push_back(enc::opcode_r(0,2,1,f,3,op));
      if(f==0 || f==5)operations.push_back(enc::opcode_r(32,2,1,f,3,op));
    }
    operations.push_back(enc::opcode_r(1,2,1,0,3,op)); // mul / mulw
    for(const u32 f:{1u,5u})operations.push_back(enc::opcode_r(48,2,1,f,3,op));
  }
  for(const u32 f:{2u,3u,4u,6u,7u})operations.push_back(enc::opcode_r(0,2,1,f,3,0x33));
  for(const i32 imm:{-2048,-129,-128,0,127,128,2047}) {
    for(const u32 f:{0u,2u,3u,4u,6u,7u})operations.push_back(enc::opcode_i(imm,1,f,3,0x13));
    operations.push_back(enc::opcode_i(imm,1,0,3,0x1b));
  }
  operations.push_back(enc::opcode_i(0x6b8,1,5,3,0x13)); // rev8
  operations.push_back(enc::opcode_i(0x604,1,1,3,0x13)); // sext.b
  operations.push_back(enc::opcode_i(0x605,1,1,3,0x13)); // sext.h
  operations.push_back(enc::opcode_r(4,0,1,4,3,0x3b));  // zext.h
  operations.push_back(enc::opcode_i(0x63f,1,5,3,0x13));
  operations.push_back(enc::opcode_i(0x61f,1,5,3,0x1b));
  CpuFixture ref,fast;
  for(const auto original:operations)for(const u32 dst:{0u,1u,2u,3u}) {
    // Use all four registers before a precise trap. They remain observable,
    // qualify for register caching, and must be flushed at the helper boundary.
    const std::array<u32,5> words{(original&~(31u<<7))|(dst<<7),
      enc::opcode_r(0,1,4,4,4,0x33),enc::opcode_r(0,2,4,0,4,0x33),
      enc::opcode_r(0,3,4,4,4,0x33),0x73};
    std::vector<cpu::CachedInstruction> instructions;
    for(const auto word:words)instructions.push_back({cpu::decode(word),word});
    auto native=cpu::NativeBlock::compile(instructions);CHECK(native);
    CHECK_EQ(native->helper_calls(4,false),0ull);
    for(const auto a:values)for(const auto b:values) {
      for(auto* c:{&ref,&fast}) {
        c->hart.reset(kDramBase);c->trap={};c->hart.set_reg(1,a);c->hart.set_reg(2,b);
        c->hart.set_reg(3,0x8123456789abcdefull);c->hart.set_reg(4,0xfedcba9876543210ull);
      }
      for(unsigned n=0;n<4;++n) {
        ref.hart.next_pc=ref.hart.pc+4;CHECK(cpu::execute(ref.hart,ref.space,instructions[n].decoded,ref.trap));
        ref.hart.pc=ref.hart.next_pc;
      }
      ref.hart.next_pc=ref.hart.pc+4;CHECK(!cpu::execute(ref.hart,ref.space,instructions[4].decoded,ref.trap));
      CHECK_EQ(native->run(fast.hart,fast.space,fast.trap),0x80000004u);
      CHECK_EQ(fast.hart.pc,ref.hart.pc);CHECK_EQ(fast.hart.retired,ref.hart.retired);
      CHECK(fast.trap.cause==ref.trap.cause);CHECK_EQ(fast.trap.tval,ref.trap.tval);
      for(unsigned r=0;r<32;++r)CHECK_EQ(fast.hart.get_reg(r),ref.hart.get_reg(r));
    }
  }
}
TEST(jit, helper_memory_trap_and_modified_code_preserve_precise_state) {
  CpuFixture c;cpu::BlockEngine engine;
  c.hart.set_reg(5,kDramBase+0x800);
  c.place32(0,enc::addi(1,0,42));c.place32(1,enc::opcode_s(0,1,5,3));
  c.place32(2,enc::opcode_i(0,5,3,2,3));c.place32(3,0x00000073);
  auto result=engine.run(c.hart,c.space,c.trap,32,true);
  CHECK(result.trapped);CHECK_EQ(result.retired,3u);CHECK_EQ(c.hart.pc,kDramBase+12);
  CHECK_EQ(c.hart.get_reg(2),42ull);CHECK_EQ(c.hart.retired,3ull);
  CHECK(c.trap.cause==cpu::TrapCause::EnvironmentCallMachine);
  c.place32(0,enc::addi(1,0,99));c.hart.pc=kDramBase;
  result=engine.run(c.hart,c.space,c.trap,32,true);
  CHECK(result.trapped);CHECK_EQ(result.retired,3u);CHECK_EQ(c.hart.get_reg(2),99ull);
}
TEST(jit, ir_folds_constants_and_removes_only_unobservable_pure_definitions) {
  const std::array<cpu::CachedInstruction,4> source{{
    {cpu::decode(enc::addi(1,0,7)),enc::addi(1,0,7)},
    {cpu::decode(enc::addi(2,1,3)),enc::addi(2,1,3)},
    {cpu::decode(enc::addi(1,1,1)),enc::addi(1,1,1)},
    {cpu::decode(0x73),0x73}}};
  auto ir=cpu::lower_ir(source);cpu::optimize_ir(ir);
  CHECK_EQ(ir.size(),4u);CHECK(ir[0].op==cpu::IROp::Nop);
  CHECK(ir[1].op==cpu::IROp::Const);CHECK_EQ(ir[1].immediate,10ull);
  CHECK(ir[2].op==cpu::IROp::Const);CHECK_EQ(ir[2].immediate,8ull);
  CHECK(ir[3].op==cpu::IROp::CallHelper);CHECK(ir[3].source==&source[3]);
}
TEST(jit, optimized_register_cache_matches_reference_across_helpers_and_faults) {
  CpuFixture ref,fast;std::mt19937_64 random(0x52674a);
  for(unsigned trial=0;trial<500;++trial) {
    std::vector<cpu::CachedInstruction> instructions;
    for(unsigned n=0;n<24;++n) {
      // Alternate broad register pressure with six hot values, exercising the
      // complete cache across memory helpers, ordinary stores and fault exits.
      const auto range=trial%2?31u:7u;
      const u32 a=static_cast<u32>(random()%range),b=static_cast<u32>(random()%range),dst=static_cast<u32>(random()%range);
      u32 word=0;const auto kind=random()%5;
      if(n==8 || n==16)word=enc::opcode_i(0,31,3,dst,3);
      else if(n==12)word=enc::opcode_s(0,b,31,3);
      else if(n==20 && trial%3==0)word=enc::opcode_i(1,31,3,dst,3); // exact misaligned-load fault
      else if(kind==0 || kind==1) {
        const bool narrow=kind==1;const u32 f=narrow?std::array<u32,3>{0,1,5}[random()%3]:static_cast<u32>(random()%8);
        i32 imm=static_cast<i32>(random()%4096)-2048;
        if(f==1 || f==5)imm=static_cast<i32>(random()%(narrow?32:64))|(f==5 && (random()&1)?0x400:0);
        word=enc::opcode_i(imm,a,f,dst,narrow?0x1b:0x13);
      } else if(kind==2 || kind==3) {
        const bool narrow=kind==3;const u32 f=narrow?std::array<u32,3>{0,1,5}[random()%3]:static_cast<u32>(random()%8);
        const u32 fn=(f==0 && random()%3==0)?1u:((f==0 || f==5) && (random()&1))?32u:0u;
        word=enc::opcode_r(fn,b,a,f,dst,narrow?0x3b:0x33);
      } else word=enc::opcode_u(static_cast<u32>(random()&0xfffff),dst,(random()&1)?0x37:0x17);
      instructions.push_back({cpu::decode(word),word});
    }
    const auto branch=enc::opcode_b(12,2,1,std::array<u32,6>{0,1,4,5,6,7}[trial%6]);instructions.push_back({cpu::decode(branch),branch});
    ref.hart.reset(kDramBase);fast.hart.reset(kDramBase);ref.trap={};fast.trap={};
    for(u32 r=1;r<31;++r) {const auto value=random();ref.hart.set_reg(r,value);fast.hart.set_reg(r,value);}
    for(auto* c:{&ref,&fast}) {c->hart.set_reg(31,kDramBase+0x800);CHECK(c->space.store(kDramBase+0x800,64,trial));}
    bool trapped=false;u32 retired=0;
    for(const auto& i:instructions) {
      ref.hart.next_pc=ref.hart.pc+4;
      if(!cpu::execute(ref.hart,ref.space,i.decoded,ref.trap)) {trapped=true;break;}
      ref.hart.pc=ref.hart.next_pc;++retired;
    }
    auto native=cpu::NativeBlock::compile(instructions);CHECK(native);
    const auto result=native->run(fast.hart,fast.space,fast.trap);
    CHECK_EQ(result,retired|(trapped?0x80000000u:0));CHECK_EQ(ref.hart.pc,fast.hart.pc);CHECK_EQ(ref.hart.retired,fast.hart.retired);
    for(u32 r=0;r<32;++r)CHECK_EQ(ref.hart.get_reg(r),fast.hart.get_reg(r));
    u64 a=0,b=0;CHECK(ref.space.load(kDramBase+0x800,64,a));CHECK(fast.space.load(kDramBase+0x800,64,b));CHECK_EQ(a,b);
    if(trapped) {CHECK(ref.trap.cause==fast.trap.cause);CHECK_EQ(ref.trap.tval,fast.trap.tval);}
  }
}
TEST(jit, native_chaining_checks_budget_generation_and_successor_traps) {
  CpuFixture c;cpu::BlockEngine engine;
  c.place32(0,enc::addi(1,1,1));c.place32(1,enc::opcode_j(4092,0));
  CHECK(c.space.store(kDramBase+4096,32,enc::addi(2,2,1)));
  CHECK(c.space.store(kDramBase+4100,32,enc::opcode_j(-4100,0)));
  for(unsigned n=0;n<10;++n) {const auto r=engine.run_batch(c.hart,c.space,c.trap,64,true);CHECK(!r.trapped);CHECK_EQ(r.retired,64u);}
  CHECK(engine.linked_blocks()>0);CHECK_EQ(c.hart.pc,kDramBase);
  auto r=engine.run_batch(c.hart,c.space,c.trap,5,true);CHECK_EQ(r.retired,5u);CHECK_EQ(c.hart.pc,kDramBase+4);
  r=engine.run_batch(c.hart,c.space,c.trap,3,true);CHECK_EQ(r.retired,3u);CHECK_EQ(c.hart.pc,kDramBase);
  CHECK(c.space.store(kDramBase+4096,32,enc::addi(2,2,7)));
  const auto before=c.hart.get_reg(2);r=engine.run_batch(c.hart,c.space,c.trap,64,true);
  CHECK(!r.trapped);CHECK_EQ(r.retired,64u);CHECK_EQ(c.hart.get_reg(2),before+16*7);
  CHECK(c.space.store(kDramBase+4100,32,0x73));
  r=engine.run_batch(c.hart,c.space,c.trap,64,true);CHECK(r.trapped);CHECK_EQ(r.retired,3u);
  CHECK_EQ(c.hart.pc,kDramBase+4100);CHECK(c.trap.cause==cpu::TrapCause::EnvironmentCallMachine);
}
TEST(jit, native_links_reject_evicted_targets_before_dereferencing) {
  CpuFixture c(1<<20);cpu::BlockEngine engine;
  c.place32(0,enc::addi(1,1,1));c.place32(1,enc::opcode_j(4092,0));
  c.place32(1024,enc::addi(2,2,1));c.place32(1025,enc::opcode_j(-4100,0));
  for(unsigned n=0;n<12;++n) {auto r=engine.run_batch(c.hart,c.space,c.trap,64,true);CHECK(!r.trapped);CHECK_EQ(r.retired,64u);}
  CHECK(engine.linked_blocks()>0);
  const auto hash=[](u64 address) {return ((address>>1)^(address>>13)^(address>>25))&16383;};
  const auto wanted=hash(kDramBase+4096);
  unsigned collisions=0;
  for(u64 page=kDramBase+8192;page<kDramBase+(1<<20) && collisions<6;page+=4096) {
    const u64 low=wanted^hash(page);if(low>2047)continue;
    const auto address=page+low*2;if((address&4095)>4080 || hash(address)!=wanted)continue;
    CHECK(c.space.store(address,32,enc::addi(10,10,1)));
    CHECK(c.space.store(address+4,32,enc::opcode_j(0,0)));
    c.hart.pc=address;const auto r=engine.run_batch(c.hart,c.space,c.trap,8,true);
    CHECK(!r.trapped);CHECK_EQ(r.retired,8u);++collisions;
  }
  CHECK_EQ(collisions,6u);
  const auto a=c.hart.get_reg(1),b=c.hart.get_reg(2);c.hart.pc=kDramBase;
  const auto r=engine.run_batch(c.hart,c.space,c.trap,64,true);
  CHECK(!r.trapped);CHECK_EQ(r.retired,64u);CHECK_EQ(c.hart.get_reg(1),a+16);CHECK_EQ(c.hart.get_reg(2),b+16);
}
TEST(jit, unrelated_cache_set_eviction_preserves_hot_native_links) {
  CpuFixture c;cpu::BlockEngine engine;
  c.place32(0,enc::addi(1,1,1));c.place32(1,enc::opcode_j(4092,0));
  CHECK(c.space.store(kDramBase+4096,32,enc::addi(2,2,1)));
  CHECK(c.space.store(kDramBase+4100,32,enc::opcode_j(-4100,0)));
  c.hart.translation_cache_enabled=true;
  for(unsigned n=0;n<10;++n) {
    const auto r=engine.run_batch(c.hart,c.space,c.trap,64,true);
    CHECK(!r.trapped);CHECK_EQ(r.retired,64u);
  }
  CHECK(engine.statistics().direct_link_hits>0);
  const auto hash=[](u64 pa) {return ((pa>>1)^(pa>>13)^(pa>>25))&16383;};
  const auto wanted=hash(kDramBase+8192);
  CHECK_NE(wanted,hash(kDramBase));CHECK_NE(wanted,hash(kDramBase+4096));
  unsigned collisions=0;
  for(u64 page=kDramBase+8192;page<kDramBase+(1<<20) && collisions<6;page+=4096) {
    const u64 low=wanted^hash(page);
    if(low>2047)continue;
    const auto address=page+low*2;
    if((address&4095)>4080 || hash(address)!=wanted)continue;
    CHECK(c.space.store(address,32,enc::addi(10,10,1)));
    CHECK(c.space.store(address+4,32,enc::opcode_j(0,0)));
    c.hart.pc=address;
    const auto r=engine.run_batch(c.hart,c.space,c.trap,8,true);
    CHECK(!r.trapped);CHECK_EQ(r.retired,8u);++collisions;
  }
  CHECK_EQ(collisions,6u);CHECK(engine.statistics().evicted>0);
  const auto misses=engine.statistics().link_misses;
  const auto direct=engine.statistics().direct_link_hits;
  c.hart.pc=kDramBase;
  const auto r=engine.run_batch(c.hart,c.space,c.trap,64,true);
  CHECK(!r.trapped);CHECK_EQ(r.retired,64u);
  CHECK_EQ(engine.statistics().link_misses,misses);
  CHECK(engine.statistics().direct_link_hits>direct);
}
TEST(jit, same_page_direct_links_survive_context_change_but_cross_page_links_revalidate) {
  for(bool same_page:{false,true}) {
    CpuFixture c;cpu::BlockEngine engine;c.hart.translation_cache_enabled=true;
    const u64 target=kDramBase+(same_page?64:4096);
    CHECK(c.space.store(kDramBase,32,enc::addi(1,1,1)));
    CHECK(c.space.store(kDramBase+4,32,enc::opcode_j(static_cast<i32>(target-kDramBase-4),0)));
    CHECK(c.space.store(target,32,enc::addi(2,2,1)));
    CHECK(c.space.store(target+4,32,enc::opcode_j(-static_cast<i32>(target+4-kDramBase),0)));
    for(unsigned n=0;n<10;++n) {
      const auto r=engine.run_batch(c.hart,c.space,c.trap,64,true);
      CHECK(!r.trapped);CHECK_EQ(r.retired,64u);
    }
    const auto misses=engine.statistics().link_misses;
    const auto x1=c.hart.get_reg(1),x2=c.hart.get_reg(2);
    ++c.hart.translation_epoch; // dispatcher must validate the entry page anew
    const auto r=engine.run_batch(c.hart,c.space,c.trap,64,true);
    CHECK(!r.trapped);CHECK_EQ(r.retired,64u);
    CHECK_EQ(c.hart.get_reg(1),x1+16);CHECK_EQ(c.hart.get_reg(2),x2+16);
    if(same_page)CHECK_EQ(engine.statistics().link_misses,misses);
    else CHECK(engine.statistics().link_misses>misses);
  }
}
TEST(jit, same_page_links_do_not_bypass_remapping_or_execute_permission_changes) {
  CpuFixture c;cpu::BlockEngine engine;
  constexpr u64 va=0x400000,root=kDramBase+4096,l1=root+4096,l0=l1+4096;
  constexpr u64 first=kDramBase+0x10000,second=kDramBase+0x20000;
  c.hart.translation_cache_enabled=true;c.hart.privilege=cpu::Privilege::Supervisor;
  c.hart.csr.set_raw(0x3a0,0x1f);c.hart.csr.set_raw(0x3b0,(1ull<<54)-1);
  c.hart.csr.set_raw(cpu::kCsrSatp,(8ull<<60)|(root>>12));
  CHECK(c.space.store(root,64,(l1>>12)<<10|1));
  CHECK(c.space.store(l1+16,64,(l0>>12)<<10|1));
  CHECK(c.space.store(l0,64,(first>>12)<<10|15));
  for(const auto page:{first,second}) {
    CHECK(c.space.store(page,32,enc::addi(1,1,page==first?1:7)));
    CHECK(c.space.store(page+4,32,enc::opcode_j(60,0)));
    CHECK(c.space.store(page+64,32,enc::addi(2,2,page==first?1:11)));
    CHECK(c.space.store(page+68,32,enc::opcode_j(-68,0)));
  }
  c.hart.pc=va;
  for(unsigned n=0;n<10;++n) {
    const auto r=engine.run_batch(c.hart,c.space,c.trap,64,true);
    CHECK(!r.trapped);CHECK_EQ(r.retired,64u);
  }
  CHECK(engine.statistics().direct_link_hits>0);
  const auto a=c.hart.get_reg(1),b=c.hart.get_reg(2);
  CHECK(c.space.store(l0,64,(second>>12)<<10|15));++c.hart.translation_epoch;
  auto r=engine.run_batch(c.hart,c.space,c.trap,64,true);
  CHECK(!r.trapped);CHECK_EQ(r.retired,64u);
  CHECK_EQ(c.hart.get_reg(1),a+16*7);CHECK_EQ(c.hart.get_reg(2),b+16*11);
  CHECK(c.space.store(l0,64,(second>>12)<<10|7));++c.hart.translation_epoch;
  r=engine.run_batch(c.hart,c.space,c.trap,8,true);
  CHECK(!r.handled);CHECK_EQ(r.retired,0u);
  cpu::Decoded decoded;
  CHECK(!cpu::fetch(c.hart,c.space,decoded,c.trap));
  CHECK(c.trap.cause==cpu::TrapCause::InstructionPageFault);
}
TEST(jit, direct_links_match_reference_at_every_budget_and_context_change) {
  CpuFixture ref,fast;cpu::BlockEngine engine;
  for(auto* c:{&ref,&fast}) {
    c->place32(0,enc::addi(1,1,7));c->place32(1,enc::jal(0,4092));
    c->place32(1024,enc::opcode_i(0,20,3,2,3));
    c->place32(1025,enc::opcode_r(0,2,1,0,1,0x33));
    c->place32(1026,enc::jal(0,-4104));
    c->hart.set_reg(20,kDramBase+8192);CHECK(c->space.store(kDramBase+8192,64,13));
  }
  for(unsigned n=0;n<12;++n) {
    CHECK_EQ(ref.run(255),255ull);CHECK_EQ(engine.run_batch(fast.hart,fast.space,fast.trap,255,true).retired,255u);
  }
  CHECK(engine.statistics().direct_link_hits>0);
  for(u32 budget=1;budget<=513;++budget) {
    // Address translation/permission changes must reject links learned in a
    // previous context, even when the virtual PC and physical bytes coincide.
    if(budget%11==0) {++ref.hart.translation_epoch;++fast.hart.translation_epoch;}
    if(budget%7==0) {
      const auto status=fast.hart.csr.raw(cpu::kCsrMstatus)^(1ull<<18);
      ref.hart.csr.set_raw(cpu::kCsrMstatus,status);fast.hart.csr.set_raw(cpu::kCsrMstatus,status);
    }
    CHECK_EQ(ref.run(budget),static_cast<u64>(budget));
    const auto r=engine.run_batch(fast.hart,fast.space,fast.trap,budget,true);
    CHECK(!r.trapped);CHECK_EQ(r.retired,budget);
    CHECK_EQ(fast.hart.pc,ref.hart.pc);CHECK_EQ(fast.hart.retired,ref.hart.retired);
    for(unsigned reg=0;reg<32;++reg)CHECK_EQ(fast.hart.get_reg(reg),ref.hart.get_reg(reg));
  }
  // A helper fault in a directly linked successor preserves preceding retire
  // counts and dirty registers, without executing its later add/jump.
  ref.hart.pc=fast.hart.pc=kDramBase;
  ref.hart.set_reg(20,0);fast.hart.set_reg(20,0);
  CHECK_EQ(ref.run(256),2ull);
  const auto r=engine.run_batch(fast.hart,fast.space,fast.trap,256,true);
  CHECK(r.trapped);CHECK_EQ(r.retired,2u);CHECK_EQ(fast.hart.pc,ref.hart.pc);
  CHECK(fast.trap.cause==ref.trap.cause);CHECK_EQ(fast.trap.tval,ref.trap.tval);
  for(unsigned reg=0;reg<32;++reg)CHECK_EQ(fast.hart.get_reg(reg),ref.hart.get_reg(reg));
}
TEST(jit, ir_fuses_updates_without_losing_retired_instruction_slots) {
  std::vector<cpu::CachedInstruction> source;
  for(i32 n=1;n<=8;++n) {const auto word=enc::addi(1,1,n);source.push_back({cpu::decode(word),word});}
  auto ir=cpu::lower_ir(source);cpu::optimize_ir(ir);
  for(unsigned n=0;n<7;++n)CHECK(ir[n].op==cpu::IROp::Nop);
  CHECK(ir[7].op==cpu::IROp::Add);CHECK_EQ(ir[7].immediate,36ull);
  CpuFixture c;c.hart.set_reg(1,~0ull);auto native=cpu::NativeBlock::compile(source);CHECK(native);
  CHECK_EQ(native->run(c.hart,c.space,c.trap),8u);CHECK_EQ(c.hart.pc,kDramBase+32);CHECK_EQ(c.hart.retired,8ull);CHECK_EQ(c.hart.get_reg(1),35ull);
}
TEST(jit, constant_branches_and_algebraic_identities_are_folded_like_qemu) {
  // beq x0,x0 keeps its taken displacement; bne x0,x0 folds to a jump by one
  // instruction length. Both update PC exactly like the reference executor.
  for(const u32 condition:{0u,1u}) {
    const u32 branch=enc::opcode_b(12,0,0,condition);
    const std::array<cpu::CachedInstruction,2> source{{
      {cpu::decode(enc::addi(1,1,1)),enc::addi(1,1,1)},{cpu::decode(branch),branch}}};
    auto ir=cpu::lower_ir(source);cpu::optimize_ir(ir);
    CHECK(ir[1].op==cpu::IROp::Branch);
    CHECK(ir[1].dst==0u);CHECK(!ir[1].indirect);
    CHECK_EQ(ir[1].immediate,condition?4ull:12ull);
    CpuFixture c;c.hart.set_reg(1,7);auto native=cpu::NativeBlock::compile(source);CHECK(native);
    CHECK_EQ(native->run(c.hart,c.space,c.trap),2u);
    CHECK_EQ(c.hart.pc,kDramBase+(condition?8:16));CHECK_EQ(c.hart.get_reg(1),8ull);
  }
  // Neutral and annihilating right operands become moves or constants. Word
  // forms sign-extend and must stay arithmetic instead of folding to a plain
  // Move, which the emitter would emit without the sign-extension.
  {
    const std::array<u32,9> words{enc::addi(1,0,1),enc::addi(2,0,-1),enc::add(4,3,0),
      enc::slli(5,3,0),enc::mul(6,3,1),enc::mul(7,3,0),enc::and_(8,3,2),enc::or_(9,3,2),
      enc::addi32(10,3,0)};
    std::vector<cpu::CachedInstruction> source;
    for(const auto word:words)source.push_back({cpu::decode(word),word});
    auto ir=cpu::lower_ir(source);cpu::optimize_ir(ir);
    CHECK(ir[0].op==cpu::IROp::Const);CHECK_EQ(ir[0].immediate,1ull);
    CHECK(ir[1].op==cpu::IROp::Const);CHECK_EQ(ir[1].immediate,~0ull);
    CHECK(ir[2].op==cpu::IROp::Move);
    CHECK(ir[3].op==cpu::IROp::Move);
    CHECK(ir[4].op==cpu::IROp::Move);
    CHECK(ir[5].op==cpu::IROp::Const);CHECK_EQ(ir[5].immediate,0ull);
    CHECK(ir[6].op==cpu::IROp::Move);
    CHECK(ir[7].op==cpu::IROp::Const);CHECK_EQ(ir[7].immediate,~0ull);
    CHECK(ir[8].op==cpu::IROp::Add);CHECK(ir[8].word);
    CpuFixture ref,fast;for(auto* c:{&ref,&fast})c->hart.set_reg(3,0x180000000ull);
    for(const auto& i:source) {ref.hart.next_pc=ref.hart.pc+i.decoded.length;
      CHECK(cpu::execute(ref.hart,ref.space,i.decoded,ref.trap));ref.hart.pc=ref.hart.next_pc;}
    auto native=cpu::NativeBlock::compile(source);CHECK(native);
    CHECK_EQ(native->run(fast.hart,fast.space,fast.trap),9u);
    CHECK_EQ(ref.hart.get_reg(10),0xffffffff80000000ull);
    for(unsigned r=0;r<32;++r)CHECK_EQ(ref.hart.get_reg(r),fast.hart.get_reg(r));
    CHECK_EQ(ref.hart.pc,fast.hart.pc);CHECK_EQ(ref.hart.retired,fast.hart.retired);
  }
}
TEST(jit, fused_nops_preserve_the_live_input_definition) {
  for(u32 opcode:{0x13u,0x1bu})for(u32 funct:{0u,4u,6u,7u}) {
    if(opcode==0x1b && funct!=0)continue;
    const std::array<u32,4> words{
      enc::opcode_r(0,3,2,0,1,0x33),
      enc::opcode_i(-7,1,funct,1,opcode),
      enc::opcode_i(13,1,funct,1,opcode),
      enc::opcode_i(-19,1,funct,1,opcode)};
    std::vector<cpu::CachedInstruction> source;
    for(const auto word:words)source.push_back({cpu::decode(word),word});
    CpuFixture ref,fast;
    for(auto* c:{&ref,&fast}) {c->hart.set_reg(1,999);c->hart.set_reg(2,0x8123456789abcdef);c->hart.set_reg(3,12345);}
    for(const auto& i:source) {ref.hart.next_pc=ref.hart.pc+4;CHECK(cpu::execute(ref.hart,ref.space,i.decoded,ref.trap));ref.hart.pc=ref.hart.next_pc;}
    auto native=cpu::NativeBlock::compile(source);CHECK(native);CHECK_EQ(native->run(fast.hart,fast.space,fast.trap),4u);
    for(u32 r=0;r<32;++r)CHECK_EQ(ref.hart.get_reg(r),fast.hart.get_reg(r));
    CHECK_EQ(ref.hart.pc,fast.hart.pc);CHECK_EQ(ref.hart.retired,fast.hart.retired);
  }
}
TEST(jit, unrelated_writes_keep_hot_code_but_instruction_edits_recompile) {
  CpuFixture c;cpu::BlockEngine engine;
  c.place32(0,enc::addi(1,1,1));c.place32(1,enc::opcode_j(-4,0));
  for(unsigned n=0;n<32;++n) {
    CHECK(c.space.store(kDramBase+128+n,8,n));
    const auto r=engine.run_batch(c.hart,c.space,c.trap,64,true);
    CHECK(!r.trapped);CHECK_EQ(r.retired,64u);
  }
  CHECK_EQ(engine.statistics().decoded,1ull);CHECK_EQ(engine.native_compiled(),1ull);
  CHECK(engine.statistics().revalidated>0);CHECK_EQ(c.hart.get_reg(1),1024ull);
  // DMA changes an instruction, not just the generation: old native code must
  // be rejected, and its existing predecessor link must not resurrect it.
  const auto replacement=enc::addi(1,1,7);
  const std::array<u8,4> bytes{static_cast<u8>(replacement),static_cast<u8>(replacement>>8),
    static_cast<u8>(replacement>>16),static_cast<u8>(replacement>>24)};
  CHECK(c.space.write_bytes(kDramBase,bytes));
  const auto r=engine.run_batch(c.hart,c.space,c.trap,64,true);
  CHECK(!r.trapped);CHECK_EQ(r.retired,64u);CHECK_EQ(c.hart.get_reg(1),1024ull+32*7);
  CHECK_EQ(engine.statistics().invalidated,1ull);
}
TEST(jit, selective_helper_spills_preserve_dirty_registers_at_a_fault) {
  for(bool store:{false,true}) {
    CpuFixture ref,fast;
    std::vector<cpu::CachedInstruction> instructions;
    for(u32 n=0;n<24;++n) {
      const auto word=enc::addi(1+n%4,1+n%4,static_cast<i32>(n+1));
      instructions.push_back({cpu::decode(word),word});
    }
    const auto fault=store?enc::opcode_s(0,2,31,3):enc::opcode_i(0,31,3,1,3);
    instructions.push_back({cpu::decode(fault),fault});
    for(auto* c:{&ref,&fast})c->hart.set_reg(31,kDramBase+(1<<20));
    for(const auto& i:instructions) {
      ref.hart.next_pc=ref.hart.pc+i.decoded.length;
      if(!cpu::execute(ref.hart,ref.space,i.decoded,ref.trap))break;
      ref.hart.pc=ref.hart.next_pc;
    }
    auto code=cpu::NativeBlock::compile(instructions);CHECK(code);
    CHECK_EQ(code->run(fast.hart,fast.space,fast.trap),24u|0x80000000u);
    CHECK_EQ(ref.hart.pc,fast.hart.pc);CHECK_EQ(ref.hart.retired,fast.hart.retired);
    for(u32 n=0;n<32;++n)CHECK_EQ(ref.hart.get_reg(n),fast.hart.get_reg(n));
    CHECK(ref.trap.cause==fast.trap.cause);CHECK_EQ(ref.trap.tval,fast.trap.tval);
  }
}
TEST(jit, floating_point_helpers_preserve_integer_cache_and_precise_faults) {
  for(bool fault:{false,true})for(bool concurrent:{false,true}) {
    CpuFixture ref,fast;
    for(auto* c:{&ref,&fast}) {
      c->space.set_concurrent(concurrent);c->hart.translation_cache_enabled=true;
      c->hart.csr.set_raw(cpu::kCsrMstatus,3ull<<13);
      c->hart.f[5]=0x3ff0000000000000ull;c->hart.f[6]=0x4000000000000000ull;
      c->hart.set_reg(10,kDramBase+16384);
      CHECK(c->space.store(kDramBase+16384,64,0x4008000000000000ull));
    }
    const std::array<u32,13> words{
      enc::addi(1,1,11),enc::addi(2,2,17),enc::addi(3,3,23),
      enc::opcode_r(1,6,5,0,7,0x53), // fadd.d: must not invalidate x1..x3
      enc::addi(1,1,13),enc::addi(10,10,8),
      enc::opcode_s(0,7,10,3)|4u, // fsd observes the updated cached base
      enc::opcode_i(0,10,3,8,7),   // fld changes f8, not x8
      enc::opcode_r(97,2,8,0,2,0x53), // fcvt.l.d changes cached integer x2
      enc::opcode_r(0,2,1,0,3,0x33),
      enc::opcode_r(121,0,3,0,9,0x53), // fmv.d.x reads dirty cached x3
      enc::opcode_r(113,0,9,0,1,0x53), // fmv.x.d writes x1
      enc::addi(2,1,7)};
    std::vector<cpu::CachedInstruction> source;
    for(const auto word:words)source.push_back({cpu::decode(word),word});
    if(fault) {
      const auto word=enc::opcode_r(45,1,9,0,3,0x53); // reserved fsqrt.d rs2!=0
      source.push_back({cpu::decode(word),word});
    }
    u32 retired=0;
    for(const auto& instruction:source) {
      ref.hart.next_pc=ref.hart.pc+4;
      if(!cpu::execute(ref.hart,ref.space,instruction.decoded,ref.trap))break;
      ref.hart.pc=ref.hart.next_pc;++retired;
    }
    auto native=cpu::NativeBlock::compile(source);CHECK(native);
    CHECK_EQ(native->run(fast.hart,fast.space,fast.trap),retired|(fault?0x80000000u:0));
    CHECK_EQ(fast.hart.pc,ref.hart.pc);CHECK_EQ(fast.hart.retired,ref.hart.retired);
    CHECK_EQ(fast.hart.csr.fflags(),ref.hart.csr.fflags());
    CHECK_EQ(fast.hart.csr.raw(cpu::kCsrMstatus),ref.hart.csr.raw(cpu::kCsrMstatus));
    for(unsigned reg=0;reg<32;++reg) {
      CHECK_EQ(fast.hart.get_reg(reg),ref.hart.get_reg(reg));CHECK_EQ(fast.hart.f[reg],ref.hart.f[reg]);
    }
    if(fault) {CHECK(fast.trap.cause==ref.trap.cause);CHECK_EQ(fast.trap.tval,ref.trap.tval);}
    u64 a=0,b=0;CHECK(ref.space.load(kDramBase+16392,64,a));CHECK(fast.space.load(kDramBase+16392,64,b));CHECK_EQ(a,b);
  }
}
TEST(jit, inline_floating_memory_matches_boxing_alignment_fs_and_reservation_rules) {
  for(bool concurrent:{false,true})for(unsigned f:{2u,3u})for(unsigned offset:{0u,1u,4u,4095u})
    for(unsigned rd:{0u,3u})for(bool enabled:{false,true}) {
      CpuFixture ref,fast;
      const u64 address=kDramBase+16384+offset;
      for(auto* c:{&ref,&fast}) {
        c->space.set_concurrent(concurrent);c->hart.translation_cache_enabled=true;
        c->hart.set_reg(1,address-8);c->hart.set_reg(2,17);
        c->hart.csr.set_raw(cpu::kCsrMstatus,enabled?3ull<<13:0);c->hart.csr.set_fflags(5);
        c->hart.f[rd]=0x123456789abcdef0ull;c->hart.f[4]=0xfedcba9876543210ull;
        for(unsigned n=0;n<24;++n)CHECK(c->space.store(address+n,8,0x80+n));
        u64 ignored=0;CHECK(mmu::load(c->hart,c->space,address,8u<<f,ignored,c->trap));
        c->hart.set_reservation(address+8,64);
      }
      const std::array<u32,5> words{enc::addi(1,1,8),enc::opcode_i(0,1,f,rd,7),
        enc::opcode_s(8,rd,1,f)|4u,enc::opcode_i(8,1,f,4,7),enc::addi(2,2,13)};
      std::vector<cpu::CachedInstruction> source;
      u32 retired=0;
      for(const auto word:words)source.push_back({cpu::decode(word),word});
      for(const auto& instruction:source) {
        ref.hart.next_pc=ref.hart.pc+4;
        if(!cpu::execute(ref.hart,ref.space,instruction.decoded,ref.trap))break;
        ref.hart.pc=ref.hart.next_pc;++retired;
      }
      cpu::NativeChain chain;
      chain.resolve=[](cpu::NativeChain*,cpu::Hart*,const memory::AddressSpace*,cpu::Trap*,u32)->const void* {return nullptr;};
      chain.sampling=true;
      auto native=cpu::NativeBlock::compile(source);CHECK(native);
      CHECK_EQ(native->run(fast.hart,fast.space,fast.trap,&chain),retired|(enabled?0:0x80000000u));
      CHECK_EQ(fast.hart.pc,ref.hart.pc);CHECK_EQ(fast.hart.retired,ref.hart.retired);
      CHECK_EQ(fast.hart.csr.raw(cpu::kCsrMstatus),ref.hart.csr.raw(cpu::kCsrMstatus));
      CHECK_EQ(fast.hart.csr.fflags(),5ull);CHECK_EQ(fast.hart.reservation_valid,ref.hart.reservation_valid);
      if(enabled && !(address&((1u<<f)-1)))CHECK(chain.sampled_loads>0);
      else CHECK_EQ(chain.sampled_loads,0ull);
      for(unsigned reg=0;reg<32;++reg) {CHECK_EQ(fast.hart.get_reg(reg),ref.hart.get_reg(reg));CHECK_EQ(fast.hart.f[reg],ref.hart.f[reg]);}
      if(!enabled) {CHECK(fast.trap.cause==ref.trap.cause);CHECK_EQ(fast.trap.tval,ref.trap.tval);}
      std::array<u8,24> a{},b{};CHECK(ref.space.read_bytes(address,a));CHECK(fast.space.read_bytes(address,b));CHECK(a==b);
    }
}
TEST(jit, floating_memory_fault_preserves_previous_fast_load_and_deferred_integer_updates) {
  CpuFixture ref,fast;
  for(auto* c:{&ref,&fast}) {
    c->hart.translation_cache_enabled=true;c->hart.csr.set_raw(cpu::kCsrMstatus,3ull<<13);
    c->hart.set_reg(10,kDramBase+16384);c->hart.f[3]=0x0123456789abcdefull;
    CHECK(c->space.store(kDramBase+16384,64,0x3ff0000000000000ull));
    u64 ignored=0;CHECK(mmu::load(c->hart,c->space,kDramBase+16384,64,ignored,c->trap));
  }
  const std::array<u32,4> words{enc::addi(1,1,5),enc::opcode_i(0,10,3,3,7),
    enc::addi(1,1,17),enc::opcode_i(0,31,3,3,7)};
  std::vector<cpu::CachedInstruction> source;
  for(const auto word:words)source.push_back({cpu::decode(word),word});
  for(const auto& instruction:source) {
    ref.hart.next_pc=ref.hart.pc+4;
    if(!cpu::execute(ref.hart,ref.space,instruction.decoded,ref.trap))break;
    ref.hart.pc=ref.hart.next_pc;
  }
  cpu::NativeChain chain;
  chain.resolve=[](cpu::NativeChain*,cpu::Hart*,const memory::AddressSpace*,cpu::Trap*,u32)->const void* {return nullptr;};
  chain.sampling=true;
  auto native=cpu::NativeBlock::compile(source);CHECK(native);
  CHECK_EQ(native->run(fast.hart,fast.space,fast.trap,&chain),3u|0x80000000u);
  CHECK_EQ(chain.sampled_loads,1ull);
  CHECK_EQ(fast.hart.pc,ref.hart.pc);CHECK_EQ(fast.hart.retired,ref.hart.retired);
  CHECK_EQ(fast.hart.get_reg(1),22ull);CHECK_EQ(fast.hart.f[3],ref.hart.f[3]);
  CHECK(fast.trap.cause==ref.trap.cause);CHECK_EQ(fast.trap.tval,ref.trap.tval);
}
TEST(platform, code_snapshot_simd_compare_handles_all_lengths_and_mismatches) {
  std::array<u8,256> a{},b{};
  for(unsigned n=0;n<a.size();++n)a[n]=b[n]=static_cast<u8>(n*71);
  for(unsigned offset=0;offset<16;++offset)for(unsigned length=0;length<=128;++length) {
    CHECK(platform::code_bytes_equal(a.data()+offset,b.data()+offset,length));
    for(unsigned n=0;n<length;++n) {
      b[offset+n]^=0x80;
      CHECK(!platform::code_bytes_equal(a.data()+offset,b.data()+offset,length));
      b[offset+n]^=0x80;
    }
  }
  CHECK(platform::code_bytes_equal(nullptr,nullptr,0));
  const auto backend=platform::code_compare_backend();
  std::printf("SIMD compare backend: %.*s; CPU/OS AVX2: %s\n",static_cast<int>(backend.size()),
    backend.data(),platform::avx2_available()?"yes":"no");
}
TEST(platform, simd_snapshot_tails_do_not_read_into_guard_pages) {
  struct Guarded {
    u8* base=static_cast<u8*>(platform::allocate(3*platform::kPageSize,platform::Protect::ReadWrite));
    ~Guarded() {if(base)platform::release(base,3*platform::kPageSize);}
  } a,b;
  CHECK(a.base && b.base);
  for(auto* buffer:{a.base,b.base}) {
    CHECK(platform::protect(buffer,platform::kPageSize,platform::Protect::NoAccess));
    CHECK(platform::protect(buffer+2*platform::kPageSize,platform::kPageSize,platform::Protect::NoAccess));
    for(unsigned n=0;n<platform::kPageSize;++n)buffer[platform::kPageSize+n]=static_cast<u8>(n*13);
  }
  for(unsigned length=0;length<=257;++length)for(bool tail:{false,true}) {
    const auto offset=tail?2*platform::kPageSize-length:platform::kPageSize;
    CHECK(platform::code_bytes_equal(a.base+offset,b.base+offset,length));
    if(length) {
      b.base[offset+length-1]^=1;
      CHECK(!platform::code_bytes_equal(a.base+offset,b.base+offset,length));
      b.base[offset+length-1]^=1;
    }
  }
}
TEST(jit, native_self_backedges_respect_every_budget_and_conditional_exit) {
  CpuFixture c;cpu::BlockEngine engine;
  c.place32(0,enc::addi(1,1,1));c.place32(1,enc::opcode_j(-4,0));
  for(unsigned n=0;n<8;++n)CHECK_EQ(engine.run_batch(c.hart,c.space,c.trap,256,true).retired,256u);
  CHECK(engine.statistics().self_loops>0);
  for(u32 limit=1;limit<=513;++limit) {
    c.hart.pc=kDramBase;c.hart.retired=0;c.hart.x[1]=0;
    const auto r=engine.run_batch(c.hart,c.space,c.trap,limit,true);
    CHECK(!r.trapped);CHECK_EQ(r.retired,limit);CHECK_EQ(c.hart.retired,limit);
    CHECK_EQ(c.hart.pc,kDramBase+(limit%2)*4);CHECK_EQ(c.hart.get_reg(1),(limit+1)/2);
  }
  c.place32(1,enc::opcode_b(-4,2,1,6)); // BLTU backedge
  c.place32(2,0x73);
  c.hart.x[2]=1000000;c.hart.pc=kDramBase;
  for(unsigned n=0;n<8;++n)CHECK_EQ(engine.run_batch(c.hart,c.space,c.trap,256,true).retired,256u);
  c.hart.pc=kDramBase;c.hart.x[1]=0;c.hart.x[2]=13;c.hart.retired=0;
  const auto r=engine.run_batch(c.hart,c.space,c.trap,256,true);
  CHECK(r.trapped);CHECK_EQ(r.retired,26u);CHECK_EQ(c.hart.retired,26ull);
  CHECK_EQ(c.hart.get_reg(1),13ull);CHECK_EQ(c.hart.pc,kDramBase+8);
}
TEST(jit, self_backedge_conditions_match_reference_with_partial_budgets_and_pc_relative_inputs) {
  const std::array<u64,6> values{0,1,~0ull,~1ull,1ull<<63,(1ull<<63)-1};
  CpuFixture ref,fast;
  for(const u32 f:{0u,1u,4u,5u,6u,7u})for(unsigned mode=0;mode<3;++mode)for(unsigned operands=0;operands<4;++operands) {
    std::vector<cpu::CachedInstruction> source;
    if(mode==1) {const auto word=enc::opcode_u(0,3,0x17);source.push_back({cpu::decode(word),word});}
    const auto add=enc::addi(1,1,1);source.push_back({cpu::decode(add),add});
    if(mode==2)source.back().decoded.length=2; // expanded compressed ADDI
    const u32 first=operands==2?0:1,second=operands==1?0:operands==3?1:operands==2?1:2;
    const auto branch=enc::opcode_b(mode==1?-8:mode==2?-2:-4,second,first,f);
    source.push_back({cpu::decode(branch),branch});
    auto code=cpu::NativeBlock::compile(source);CHECK(code);
    for(const auto start:values)for(u32 budget=0;budget<=34;++budget) {
      if(budget && budget<source.size())continue;
      for(auto* c:{&ref,&fast}) {
        c->hart.reset(kDramBase);c->trap={};c->hart.set_reg(1,start);
        c->hart.set_reg(2,start+3);c->hart.set_reg(3,0x8123456789abcdefull);
      }
      const auto limit=budget?budget:static_cast<u32>(source.size());
      u32 retired=0;
      do {
        for(const auto& instruction:source) {
          ref.hart.next_pc=ref.hart.pc+instruction.decoded.length;
          CHECK(cpu::execute(ref.hart,ref.space,instruction.decoded,ref.trap));
          ref.hart.pc=ref.hart.next_pc;++retired;
        }
      } while(ref.hart.pc==kDramBase && retired+source.size()<=limit);
      std::atomic<u64> generation{0};u32 resolved=0;
      cpu::NativeChain chain;
      chain.remaining=budget;chain.generation=&generation;chain.state=&resolved;
      chain.resolve=[](cpu::NativeChain* current,cpu::Hart*,const memory::AddressSpace*,cpu::Trap*,u32 count)->const void* {
        *static_cast<u32*>(current->state)=count;return nullptr;
      };
      CHECK_EQ(code->run(fast.hart,fast.space,fast.trap,budget?&chain:nullptr),retired);
      if(budget)CHECK_EQ(resolved,retired);
      CHECK_EQ(fast.hart.pc,ref.hart.pc);CHECK_EQ(fast.hart.retired,ref.hart.retired);
      for(unsigned r=0;r<32;++r)CHECK_EQ(fast.hart.get_reg(r),ref.hart.get_reg(r));
    }
  }
}
TEST(jit, native_self_backedges_preserve_prior_iterations_at_helper_fault) {
  CpuFixture c;cpu::BlockEngine engine;
  c.place32(0,enc::opcode_i(0,31,3,1,3));c.place32(1,enc::addi(31,31,8));
  c.place32(2,enc::opcode_j(-8,0));c.hart.x[31]=kDramBase+4096;
  for(unsigned n=0;n<8;++n)CHECK_EQ(engine.run_batch(c.hart,c.space,c.trap,255,true).retired,255u);
  CHECK(engine.statistics().self_loops>0);
  CHECK(c.space.store(kDramBase+(1<<20)-8,64,0xabcdef));
  c.hart.pc=kDramBase;c.hart.retired=0;c.hart.x[31]=kDramBase+(1<<20)-16;
  const auto r=engine.run_batch(c.hart,c.space,c.trap,256,true);
  CHECK(r.trapped);CHECK_EQ(r.retired,6u);CHECK_EQ(c.hart.retired,6ull);
  CHECK_EQ(c.hart.get_reg(1),0xabcdefull);CHECK_EQ(c.hart.pc,kDramBase);
  CHECK(c.trap.cause==cpu::TrapCause::LoadAccessFault);CHECK_EQ(c.trap.tval,kDramBase+(1<<20));
}
TEST(jit, sampled_helper_metadata_counts_calls_and_failed_instructions) {
  const std::array<u32,5> words{enc::addi(1,1,7),enc::opcode_i(0,2,3,3,3),
    enc::opcode_s(0,3,2,3),enc::addi(1,1,9),0x73};
  std::vector<cpu::CachedInstruction> instructions;
  for(const auto word:words)instructions.push_back({cpu::decode(word),word});
  const auto code=cpu::NativeBlock::compile(instructions);CHECK(code);
  CHECK_EQ(code->helper_calls(0,false),0ull);CHECK_EQ(code->helper_calls(1,false),0ull);
  CHECK_EQ(code->helper_calls(2,false),1ull);CHECK_EQ(code->helper_calls(2,true),2ull);
  CHECK_EQ(code->helper_calls(4,true),3ull);CHECK_EQ(code->helper_calls(5,false),3ull);
  CHECK_EQ(code->helper_calls(12,false),7ull);
}

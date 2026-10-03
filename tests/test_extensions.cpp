// SPDX-License-Identifier: MIT
#include "cpu/block_engine.hpp"
#include "mmu/mmu.hpp"
#include "cpu/execute_fp.hpp"
#include "cpu/host_fp.hpp"
#include "platform/platform.hpp"
#if defined(_M_X64) || defined(__x86_64__)
#include <emmintrin.h>
#endif
#include "test_helpers.hpp"
#include "test_framework.hpp"
#include <bit>
#include <random>
#include "cpu/execute_crypto.hpp"
using namespace rvvm;using namespace rvvm::cpu;using namespace rvvm::test;
TEST(extensions, scalar_crypto_known_aes128_vector_key_schedule_and_reserved_round) {
  CpuFixture c;
  auto instruction=[&](u32 raw,u64 a,u64 b,bool fast=false) {
    c.hart.reset(kDramBase);c.hart.native_fp=fast;c.hart.set_reg(1,a);c.hart.set_reg(2,b);c.hart.next_pc+=4;
    CHECK(execute(c.hart,c.space,decode(raw),c.trap));return c.hart.get_reg(3);
  };
  const auto ks2=enc::opcode_r(63,2,1,0,3,0x33);
  CHECK_EQ(instruction(enc::opcode_r(4,2,1,4,3,0x33),0xabcdef0011223344,0x5566778899aabbcc),0x99aabbcc11223344ull);
  CHECK_EQ(instruction(enc::opcode_r(4,2,1,7,3,0x33),0xff34,0xfeab),0xab34ull);
  CHECK_EQ(instruction(enc::opcode_r(4,2,1,4,3,0x3b),0x1234,0x9876),0xffffffff98761234ull);
  CHECK_EQ(instruction(enc::opcode_i(0x687,1,5,3,0x13),0x0123456789abcdef,0),0x80c4a2e691d5b3f7ull);
  for(bool fast:{false,true}) {
    u64 ka=0x0706050403020100,kb=0x0f0e0d0c0b0a0908;
    u64 a=0x7766554433221100^ka,b=0xffeeddccbbaa9988^kb;
    for(unsigned n=0;n<10;++n) {
      const auto t=instruction(enc::opcode_i(static_cast<i32>(0x310+n),1,1,3,0x13),kb,0,fast);
      ka=instruction(ks2,t,ka,fast);kb=instruction(ks2,ka,kb,fast);
      const auto nexta=aes_round(n==9?25:27,a,b,fast)^ka,nextb=aes_round(n==9?25:27,b,a,fast)^kb;
      a=nexta;b=nextb;
    }
    CHECK_EQ(a,0x30047b6ad8e0c469ull);CHECK_EQ(b,0x5ac5b47080b7cdd8ull);
    CHECK_EQ(instruction(enc::opcode_i(0x31a,1,1,3,0x13),0,0,fast),0x6363636363636363ull);
  }
  for(u32 number=11;number<16;++number) {
    c.hart.next_pc+=4;CHECK(!execute(c.hart,c.space,decode(enc::opcode_i(static_cast<i32>(0x310+number),1,1,3,0x13)),c.trap));CHECK(c.trap.cause==TrapCause::IllegalInstruction);
  }
  for(u32 immediate=0x100;immediate<=0x107;++immediate)CHECK_EQ(instruction(enc::opcode_i(static_cast<i32>(immediate),1,1,3,0x13),~0ull,0),immediate<=0x101?~0ull:immediate==0x102?0x1fffffffull:immediate==0x103?0x3fffffull:immediate<=0x105?~0ull:immediate==0x106?0x1ffffffffffffffull:0x3ffffffffffffffull);
}
TEST(extensions, crypto_native_matches_fixed_iteration_reference_and_jit) {
  std::mt19937_64 random(0x414553);
  for(unsigned n=0;n<1000;++n) {
    const auto a=random(),b=random();
    for(unsigned fn:{24u,25u,27u,29u,31u})CHECK_EQ(aes_round(fn,a,b,true),aes_round(fn,a,b,false));
    for(unsigned fn:{1u,2u,3u})CHECK_EQ(carryless(fn,a,b,true),carryless(fn,a,b,false));
  }
  CHECK_EQ(carryless(1,0x8000000000000001ull,2,false),2ull);CHECK_EQ(carryless(3,1ull<<63,2,false),1ull);
  CHECK_EQ(carryless(2,1ull<<63,1ull<<63,false),1ull<<63);
  CpuFixture c,reference;
  for(u32 raw:{enc::opcode_r(27,2,1,0,3,0x33),enc::opcode_r(5,2,1,3,3,0x33),enc::opcode_i(0x100,1,1,3,0x13)}) {
    CachedInstruction instruction{decode(raw),raw};auto code=NativeBlock::compile({&instruction,1});CHECK(code!=nullptr);
    c.hart.reset(kDramBase);reference.hart.reset(kDramBase);for(auto* h:{&c.hart,&reference.hart}) {h->set_reg(1,0x123456789abcdef0);h->set_reg(2,0x9876543210fedcba);}
    reference.hart.next_pc+=4;CHECK(execute(reference.hart,reference.space,instruction.decoded,reference.trap));CHECK_EQ(code->run(c.hart,c.space,c.trap),1u);CHECK_EQ(c.hart.get_reg(3),reference.hart.get_reg(3));
  }
}
TEST(extensions, bitmanip_known_results_and_jit_match_reference) {
  struct Case {u32 word;u64 a,b,value;};
  std::vector<Case> cases;
  auto r=[&](u32 fn,u32 f,u32 op,u64 a,u64 b,u64 value) {cases.push_back({enc::opcode_r(fn,2,1,f,3,op),a,b,value});};
  auto i=[&](u32 imm,u32 f,u32 op,u64 a,u64 value) {cases.push_back({enc::opcode_i(static_cast<i32>(imm),1,f,3,op),a,0,value});};
  for(u32 shift=1;shift<=3;++shift) {
    r(16,shift*2,0x33,~0ull,9,9+(~0ull<<shift));r(16,shift*2,0x3b,~0ull,9,9+(0xffffffffull<<shift));
  }
  r(4,0,0x3b,~0ull,2,0x100000001ull);i(0x0bf,1,0x1b,~0ull,1ull<<63);
  r(32,7,0x33,0x1234,0xff,0x1200);r(32,6,0x33,0x1234,0xff,0xffffffffffffff34ull);r(32,4,0x33,0x1234,0xff,~0x12cbull);
  r(5,4,0x33,1ull<<63,3,1ull<<63);r(5,5,0x33,1ull<<63,3,3);r(5,6,0x33,1ull<<63,3,3);r(5,7,0x33,1ull<<63,3,1ull<<63);
  for(u32 op:{0x13u,0x1bu}) {
    const unsigned width=op==0x13?64:32;
    i(0x600,1,op,0,width);i(0x601,1,op,0,width);i(0x602,1,op,~0ull,width);
    i(0x600,1,op,1,width-1);i(0x601,1,op,8,3);i(0x602,1,op,0x80808080,4);
  }
  i(0x604,1,0x13,0x80,0xffffffffffffff80ull);i(0x605,1,0x13,0x8000,0xffffffffffff8000ull);
  cases.push_back({enc::opcode_r(4,0,1,4,3,0x3b),~0ull,0,65535});
  i(0x287,5,0x13,0x0100000042000080,0xff000000ff0000ffull);i(0x6b8,5,0x13,0x0123456789abcdef,0xefcdab8967452301);
  r(48,1,0x33,1,63,1ull<<63);r(48,5,0x33,1,65,1ull<<63);
  r(48,1,0x3b,1,31,0xffffffff80000000ull);r(48,5,0x3b,1,33,0xffffffff80000000ull);
  i(0x63f,5,0x13,1,2);i(0x61f,5,0x1b,1,2);
  for(u32 fn:{20u,36u,52u}) {
    r(fn,1,0x33,1,63,fn==36?1:(1ull<<63)|1);
    i((fn<<5)|63,1,0x13,1,fn==36?1:(1ull<<63)|1);
  }
  r(36,5,0x33,1ull<<63,127,1);i(0x4bf,5,0x13,1ull<<63,1);
  CpuFixture c,native;
  for(const auto& test:cases) {
    const CachedInstruction instruction{decode(test.word),test.word};auto code=NativeBlock::compile({&instruction,1});CHECK(code!=nullptr);
    c.hart.reset(kDramBase);native.hart.reset(kDramBase);
    for(auto* h:{&c.hart,&native.hart}) {h->set_reg(1,test.a);h->set_reg(2,test.b);}
    c.hart.next_pc+=4;CHECK(execute(c.hart,c.space,instruction.decoded,c.trap));CHECK_EQ(c.hart.get_reg(3),test.value);
    CHECK_EQ(code->run(native.hart,native.space,native.trap),1u);CHECK_EQ(native.hart.get_reg(3),test.value);
  }
}
TEST(extensions, reserved_bitmanip_encodings_trap) {
  CpuFixture c;
  for(u32 word:{enc::opcode_i(0x603,1,1,3,0x13),enc::opcode_r(4,2,1,5,3,0x3b),enc::opcode_r(16,2,1,3,3,0x33),enc::opcode_i(0x63f,1,5,3,0x1b)}) {
    CHECK(!execute(c.hart,c.space,decode(word),c.trap));CHECK(c.trap.cause==TrapCause::IllegalInstruction);
  }
}
TEST(mmu, sv48_four_level_canonical_translation_and_dirty_updates) {
  CpuFixture c;auto& h=c.hart;
  constexpr u64 va=0xffff800000000000ull,data=kDramBase+0x5000;
  h.csr.set_raw(0x3a0,0x1f);h.csr.set_raw(0x3b0,(1ull<<54)-1);h.privilege=Privilege::Supervisor;
  h.csr.set_raw(kCsrSatp,(9ull<<60)|((kDramBase+0x1000)>>12));h.translation_cache_enabled=true;
  for(int level=3;level>0;--level) {
    const u64 table=kDramBase+static_cast<u64>(4-level)*4096,next=table+4096;
    CHECK(c.space.store(table+((va>>(12+9*level))&511)*8,64,(next>>12)<<10|1));
  }
  CHECK(c.space.store(kDramBase+0x4000,64,(data>>12)<<10|7));
  CHECK(mmu::store(h,c.space,va+8,64,0x1234,c.trap));u64 value=0;
  CHECK(mmu::load(h,c.space,va+8,64,value,c.trap));CHECK_EQ(value,0x1234ull);
  CHECK(c.space.load(kDramBase+0x4000,64,value));CHECK_EQ(value&192,192ull);
  CHECK(!mmu::load(h,c.space,0x800000000000ull,64,value,c.trap));CHECK(c.trap.cause==TrapCause::LoadPageFault);
}
TEST(jit, batch_budget_precise_trap_and_pc_relative_commit) {
  CpuFixture c;BlockEngine engine;
  for(u32 n=0;n<40;++n)c.place32(n,enc::addi(1,1,1));
  c.place32(40,enc::opcode_u(0,2,0x17));c.place32(41,0x00000073);
  for(unsigned round=0;round<6;++round) {
    c.hart.reset(kDramBase);auto part=engine.run_batch(c.hart,c.space,c.trap,35,true);
    CHECK_EQ(part.retired,35u);CHECK_EQ(c.hart.pc,kDramBase+140);CHECK(!part.trapped);
    part=engine.run_batch(c.hart,c.space,c.trap,256,true);
    CHECK_EQ(part.retired,6u);CHECK(part.trapped);CHECK_EQ(c.hart.retired,41ull);CHECK_EQ(c.hart.get_reg(2),kDramBase+160);
  }
}
TEST(extensions, native_fp_results_and_exception_flags_match_softfloat) {
  CpuFixture ref,fast;fast.hart.native_fp=true;
  std::mt19937_64 random(0x414650);
  const u64 special[]={0,1,~0ull,0x8000000000000000ull,0x7ff0000000000000ull,0x7ff8000000000000ull,0x7ff0000000000001ull,0x0010000000000000ull,0x000fffffffffffffull,0x3ff0000000000000ull,0x7f800000,0x7f800001,0x7fc00000,0x00800000,0x007fffff,0x3f800000};
  for(bool dbl:{false,true})for(u32 fn:{0u,4u,8u,12u,44u})for(u32 rm=0;rm<=4;++rm) {
    const auto d=decode(enc::opcode_r(fn+(dbl?1:0),fn==44?0:2,1,rm,3,0x53));
    for(unsigned n=0;n<1200;++n) {
      u64 a=n<std::size(special)?special[n]:random(),b=n<std::size(special)?special[std::size(special)-1-n]:random();
      if(!dbl) {a=0xffffffff00000000|static_cast<u32>(a);b=0xffffffff00000000|static_cast<u32>(b);}
      for(auto* h:{&ref.hart,&fast.hart}) {h->f[1]=a;h->f[2]=b;h->csr.set_raw(kCsrMstatus,3ull<<13);h->csr.set_fflags(0);}
      CHECK(execute_fp(ref.hart,ref.space,d,ref.trap));
      {HostFpScope scope;CHECK(execute_fp(fast.hart,fast.space,d,fast.trap));}
      CHECK_EQ(fast.hart.f[3],ref.hart.f[3]);CHECK_EQ(fast.hart.csr.fflags(),ref.hart.csr.fflags());
    }
  }
}
TEST(extensions, native_fp_restores_host_rounding_and_denormal_settings) {
#if defined(_M_X64) || defined(__x86_64__)
  const auto original=_mm_getcsr();const unsigned unusual=0xffe1;_mm_setcsr(unusual);
  CpuFixture c;c.hart.native_fp=true;c.hart.csr.set_raw(kCsrMstatus,3ull<<13);c.hart.f[1]=0xffffffff00000001;c.hart.f[2]=0xffffffff00000001;
  const bool ok=execute_fp(c.hart,c.space,decode(enc::opcode_r(0,2,1,0,3,0x53)),c.trap);
  const auto restored=_mm_getcsr();_mm_setcsr(original);
  CHECK(ok);CHECK_EQ(restored,unusual);CHECK_EQ(c.hart.f[3],0xffffffff00000002ull);CHECK_EQ(c.hart.csr.fflags(),0ull);
#endif
}
TEST(extensions, native_fp_scope_handles_guest_flag_clear_rounding_and_tininess_fallback) {
#if defined(_M_X64) || defined(__x86_64__)
  const auto saved=_mm_getcsr();_mm_setcsr(0xffe1);
  CpuFixture ref,fast;fast.hart.native_fp=true;
  for(auto* h:{&ref.hart,&fast.hart})h->csr.set_raw(kCsrMstatus,3ull<<13);
  bool matched=true,lazy=false;
  {
    HostFpScope scope;lazy=_mm_getcsr()==0xffe1;
    const auto op=[&](u32 fn,u32 rm,u64 a,u64 b) {
      const auto instruction=decode(enc::opcode_r(fn,2,1,rm,3,0x53));
      for(auto* h:{&ref.hart,&fast.hart}) {h->f[1]=a;h->f[2]=b;}
      if(!execute_fp(ref.hart,ref.space,instruction,ref.trap) ||
         !execute_fp(fast.hart,fast.space,instruction,fast.trap) ||
         ref.hart.f[3]!=fast.hart.f[3] || ref.hart.csr.fflags()!=fast.hart.csr.fflags())matched=false;
    };
    op(12,0,0xffffffff3f800000ull,0xffffffff00000000ull); // divide by zero
    const auto clear=decode(enc::csrrw(0,kCsrFflags,5));
    for(auto* c:{&ref,&fast}) {
      c->hart.set_reg(5,0);
      if(!execute(c->hart,c->space,clear,c->trap))matched=false;
    }
    op(0,0,0xffffffff3f800000ull,0xffffffff40000000ull); // exact after clearing DZ
    op(8,0,0xffffffff00800000ull,0xffffffff3f7fffffull); // rounded min-normal
    op(0,0,0xffffffff3f800000ull,0xffffffff40000000ull); // must not inherit x86 UF
    for(u32 rm:{3u,1u,2u,0u,4u,0u}) {
      op(0,rm,0xffffffff3f800000ull,0xffffffff33800000ull);
      op(1,rm,0x3ff0000000000000ull,0x3ca0000000000000ull);
    }
  }
  const auto restored=_mm_getcsr();_mm_setcsr(saved);
  CHECK(lazy);CHECK(matched);CHECK_EQ(restored,0xffe1u);
#endif
}
TEST(jit, parallel_compilation_keeps_modified_block_helper_storage_alive) {
  CpuFixture c;BlockEngine engine(2);
  c.place32(0,enc::addi(1,0,19));c.place32(1,enc::opcode_i(0,5,3,2,3));c.place32(2,0x00000073);
  c.hart.set_reg(5,kDramBase+0x800);CHECK(c.space.store(kDramBase+0x800,64,42));
  for(unsigned n=0;n<150;++n) {
    const u32 value=n%2?19:27;c.place32(0,enc::addi(1,0,static_cast<i32>(value)));
    for(unsigned hot=0;hot<70;++hot) {c.hart.pc=kDramBase;const auto r=engine.run(c.hart,c.space,c.trap,32,true);CHECK(r.trapped);CHECK_EQ(r.retired,2u);CHECK_EQ(c.hart.get_reg(1),static_cast<u64>(value));CHECK_EQ(c.hart.get_reg(2),42ull);}
    platform::sleep_ms(1);
  }
  CHECK(engine.native_compiled()>0);
}

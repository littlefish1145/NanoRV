// SPDX-License-Identifier: MIT
#include "cpu/jit_x64.hpp"
#include "platform/platform.hpp"
#include "mmu/mmu.hpp"
#include "cpu/ir.hpp"
#include "cpu/softint.hpp"
#include "cpu/host_fp.hpp"
#include <cstddef>
#include <cstring>
#include <vector>
#include <algorithm>
#include <utility>
namespace rvvm::cpu {
namespace {
template<class T> std::size_t hart_offset(T Hart::* member) {
  // Hart contains non-standard-layout CSR storage. Compute offsets from a
  // real object instead of conditionally supported offsetof expressions.
  static const Hart layout{};
  return static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(&(layout.*member))-
      reinterpret_cast<std::uintptr_t>(&layout));
}
std::size_t mstatus_offset() {
  static const Hart layout{};
  return reinterpret_cast<std::uintptr_t>(layout.csr.raw_address(kCsrMstatus))-
    reinterpret_cast<std::uintptr_t>(&layout);
}
bool execute_one(Hart* hart, const memory::AddressSpace* bus,
                 const CachedInstruction* instruction, Trap* trap) noexcept {
  hart->next_pc=hart->pc+instruction->decoded.length;
  const auto& d=instruction->decoded;
  if (!d.compressed && (d.opcode==3 || d.opcode==0x23)) {
    const bool store=d.opcode==0x23;
    if ((!store && d.funct3<=6) || (store && d.funct3<=3)) {
      const unsigned width=8u<<(d.funct3&3);
      const auto immediate=store ? sign_extend(((d.raw>>25)<<5)|((d.raw>>7)&31),12)
          : static_cast<i64>(static_cast<i32>(d.raw)>>20);
      const u64 address=hart->get_reg(d.rs1)+static_cast<u64>(immediate);
      u64 value=0;
      if (store) {
        if (!mmu::store(*hart,*bus,address,width,hart->get_reg(d.rs2),*trap)) return false;
        if(hart->reservation_valid) {
          u64 physical;if(mmu::translate(*hart,*bus,address,width,memory::Access::Write,physical,*trap))hart->note_store(physical,width);
        }
      } else {
        if (!mmu::load(*hart,*bus,address,width,value,*trap)) return false;
        hart->set_reg(d.rd,d.funct3<3?static_cast<u64>(sign_extend(value,width)):value);
      }
      ++hart->retired;hart->pc=hart->next_pc;return true;
    }
  }
  if (!execute(*hart,*bus,instruction->decoded,*trap)) {
    if (trap->cause==TrapCause::IllegalInstruction) trap->tval=instruction->original;
    return false;
  }
  hart->pc=hart->next_pc;return true;
}
template<unsigned F,bool Store> bool memory_one(Hart* hart,const memory::AddressSpace* bus,
    const CachedInstruction* instruction,Trap* trap) noexcept {
  constexpr unsigned width=8u<<(F&3);
  const auto& d=instruction->decoded;
  const i64 immediate=Store?sign_extend(((d.raw>>25)<<5)|((d.raw>>7)&31),12):static_cast<i64>(static_cast<i32>(d.raw)>>20);
  const u64 address=hart->get_reg(d.rs1)+static_cast<u64>(immediate);
  hart->next_pc=hart->pc+d.length;
  auto* entry=mmu::cached_ram(*hart,*bus,address,width,Store?memory::Access::Write:memory::Access::Read);
  if constexpr(Store) {
    const auto value=hart->get_reg(d.rs2);
    if(entry)bus->write_fast<width>(entry->host+(address&4095),value);
    else if(!mmu::store(*hart,*bus,address,width,value,*trap))return false;
    if(hart->reservation_valid) {
      u64 physical=0;
      if(entry)physical=entry->ppn|(address&4095);
      else if(!mmu::translate(*hart,*bus,address,width,memory::Access::Write,physical,*trap))return false;
      hart->note_store(physical,width);
    }
  } else {
    u64 value=0;
    if(entry)value=bus->read_fast<width>(entry->host+(address&4095));
    else if(!mmu::load(*hart,*bus,address,width,value,*trap))return false;
    hart->set_reg(d.rd,F<3?static_cast<u64>(sign_extend(value,width)):value);
  }
  ++hart->retired;hart->pc=hart->next_pc;return true;
}
using Helper=bool(*)(Hart*,const memory::AddressSpace*,const CachedInstruction*,Trap*) noexcept;
template<unsigned Width,bool Store> bool fp_memory_one(Hart* hart,const memory::AddressSpace* bus,
    const CachedInstruction* instruction,Trap* trap) noexcept {
  if(!(hart->csr.raw(kCsrMstatus)&(3ull<<13)))return execute_one(hart,bus,instruction,trap);
  const auto& d=instruction->decoded;
  const i64 immediate=Store?sign_extend(((d.raw>>25)<<5)|((d.raw>>7)&31),12):static_cast<i64>(static_cast<i32>(d.raw)>>20);
  const u64 address=hart->get_reg(d.rs1)+static_cast<u64>(immediate);
  hart->next_pc=hart->pc+d.length;
  auto* entry=mmu::cached_ram(*hart,*bus,address,Width,Store?memory::Access::Write:memory::Access::Read);
  if constexpr(Store) {
    const auto value=hart->f[d.rs2];
    if(entry)bus->write_fast<Width>(entry->host+(address&4095),value);
    else if(!mmu::store(*hart,*bus,address,Width,value,*trap))return false;
    if(hart->reservation_valid) {
      u64 physical=0;
      if(entry)physical=entry->ppn|(address&4095);
      else if(!mmu::translate(*hart,*bus,address,Width,memory::Access::Write,physical,*trap))return false;
      hart->note_store(physical,Width);
    }
  } else {
    u64 value=0;
    if(entry)value=bus->read_fast<Width>(entry->host+(address&4095));
    else if(!mmu::load(*hart,*bus,address,Width,value,*trap))return false;
    hart->f[d.rd]=Width==32?0xffffffff00000000ull|static_cast<u32>(value):value;
    hart->csr.set_raw(kCsrMstatus,hart->csr.raw(kCsrMstatus)|(3ull<<13)|(1ull<<63));
  }
  ++hart->retired;hart->pc=hart->next_pc;return true;
}
template<unsigned Width> void native_store_ram(Hart* hart,const memory::AddressSpace* bus,u8* ptr,u64 value) noexcept {
  bus->write_fast<Width>(ptr,value);
  if(hart->reservation_valid)hart->note_store(bus->host_physical(ptr),Width);
}

template<unsigned Fn,bool Double> bool fp_arithmetic_one(Hart* hart,const memory::AddressSpace* bus,
    const CachedInstruction* instruction,Trap* trap) noexcept {
  if(!hart->native_fp || !(hart->csr.raw(kCsrMstatus)&(3ull<<13)))
    return execute_one(hart,bus,instruction,trap);
  const auto& d=instruction->decoded;
  const auto rounding=d.funct3==7?static_cast<unsigned>(hart->csr.frm()):d.funct3;
  auto operand=[&](unsigned reg) {
    const auto value=hart->f[reg];
    if constexpr(Double)return value;
    else return value>>32==0xffffffffull?static_cast<u64>(static_cast<u32>(value)):0x7fc00000ull;
  };
  u64 value=0;u8 flags=0;
  if(!host_fp(Fn,Double,rounding,operand(d.rs1),operand(d.rs2),value,flags))
    return execute_one(hart,bus,instruction,trap);
  hart->f[d.rd]=Double?value:0xffffffff00000000ull|static_cast<u32>(value);
  hart->csr.set_fflags(hart->csr.fflags()|flags);
  hart->csr.set_raw(kCsrMstatus,hart->csr.raw(kCsrMstatus)|(3ull<<13)|(1ull<<63));
  hart->next_pc=hart->pc+d.length;hart->pc=hart->next_pc;++hart->retired;return true;
}
template<unsigned F,bool Word,bool Immediate,bool Alt=false>
bool integer_one(Hart* hart,const memory::AddressSpace*,const CachedInstruction* instruction,Trap*) noexcept {
  const auto& d=instruction->decoded;
  u64 a=hart->get_reg(d.rs1);
  const u64 b=Immediate?static_cast<u64>(static_cast<i64>(static_cast<i32>(d.raw)>>20)):hart->get_reg(d.rs2);
  if constexpr(Word)a=static_cast<u32>(a);
  const auto shift=static_cast<unsigned>(b)&(Word?31:63);
  u64 value=0;
  if constexpr(F==0)value=Alt?a-b:a+b;
  if constexpr(F==1)value=a<<shift;
  if constexpr(F==2)value=static_cast<i64>(a)<static_cast<i64>(b);
  if constexpr(F==3)value=a<b;
  if constexpr(F==4)value=a^b;
  if constexpr(F==5) {
    if constexpr(Alt)value=Word?static_cast<u64>(static_cast<i32>(a)>>shift):static_cast<u64>(static_cast<i64>(a)>>shift);
    else value=a>>shift;
  }
  if constexpr(F==6)value=a|b;
  if constexpr(F==7)value=a&b;
  if constexpr(Word)value=static_cast<u64>(static_cast<i64>(static_cast<i32>(value)));
  hart->set_reg(d.rd,value);hart->next_pc=hart->pc+d.length;
  hart->pc=hart->next_pc;++hart->retired;return true;
}
template<unsigned F,bool Word> bool muldiv_one(Hart* hart,const memory::AddressSpace*,const CachedInstruction* instruction,Trap*) noexcept {
  const auto& d=instruction->decoded;
  u64 a=hart->get_reg(d.rs1),b=hart->get_reg(d.rs2),value=0;
  if constexpr(Word) {a=static_cast<u32>(a);b=static_cast<u32>(b);}
  if constexpr(F==0)value=a*b;
  if constexpr(F>=1 && F<=3) {
    value=platform::multiply_high_unsigned(a,b);
    if constexpr(F==1 || F==2)if(static_cast<i64>(a)<0)value-=b;
    if constexpr(F==1)if(static_cast<i64>(b)<0)value-=a;
  }
  const i64 signed_a=Word?static_cast<i32>(a):static_cast<i64>(a);
  const i64 signed_b=Word?static_cast<i32>(b):static_cast<i64>(b);
  if constexpr(F==4)value=div_signed(signed_a,signed_b);
  if constexpr(F==5)value=div_unsigned(a,b);
  if constexpr(F==6)value=rem_signed(signed_a,signed_b);
  if constexpr(F==7)value=rem_unsigned(a,b);
  if constexpr(Word)value=static_cast<u64>(static_cast<i64>(static_cast<i32>(value)));
  hart->set_reg(d.rd,value);hart->next_pc=hart->pc+d.length;
  hart->pc=hart->next_pc;++hart->retired;return true;
}
template<unsigned F> bool branch_one(Hart* hart,const memory::AddressSpace*,const CachedInstruction* instruction,Trap*) noexcept {
  const auto& d=instruction->decoded;const auto a=hart->get_reg(d.rs1),b=hart->get_reg(d.rs2);
  bool taken=false;
  if constexpr(F==0)taken=a==b;
  if constexpr(F==1)taken=a!=b;
  if constexpr(F==4)taken=static_cast<i64>(a)<static_cast<i64>(b);
  if constexpr(F==5)taken=static_cast<i64>(a)>=static_cast<i64>(b);
  if constexpr(F==6)taken=a<b;
  if constexpr(F==7)taken=a>=b;
  const auto raw=d.raw;
  const auto offset=sign_extend(((raw>>31)<<12)|(((raw>>7)&1)<<11)|(((raw>>25)&63)<<5)|(((raw>>8)&15)<<1),13);
  hart->next_pc=hart->pc+(taken?static_cast<u64>(offset):d.length);
  hart->pc=hart->next_pc;++hart->retired;return true;
}
template<unsigned Opcode> bool address_one(Hart* hart,const memory::AddressSpace*,const CachedInstruction* instruction,Trap*) noexcept {
  const auto& d=instruction->decoded;u64 next=hart->pc+d.length,value=0;
  if constexpr(Opcode==0x37 || Opcode==0x17) {
    value=static_cast<u64>(sign_extend(d.raw&0xfffff000u,32));
    if constexpr(Opcode==0x17)value+=hart->pc;
  } else {
    value=next;
    if constexpr(Opcode==0x67)next=(hart->get_reg(d.rs1)+static_cast<u64>(sign_extend(d.raw>>20,12)))&~1ull;
    else next=hart->pc+static_cast<u64>(sign_extend(((d.raw>>31)<<20)|(((d.raw>>12)&255)<<12)|(((d.raw>>20)&1)<<11)|(((d.raw>>21)&1023)<<1),21));
  }
  hart->set_reg(d.rd,value);hart->next_pc=next;hart->pc=next;++hart->retired;return true;
}
template<bool Word,bool Immediate> Helper integer_helper(const Decoded& d) noexcept {
  const auto f=d.funct3,fn=d.funct7;
  if constexpr(Word)if(f!=0 && f!=1 && f!=5)return execute_one;
  if constexpr(Immediate) {
    if(f==1 && (Word?fn!=0:(d.raw>>26)!=0))return execute_one;
    if(f==5 && (Word?(fn!=0 && fn!=32):((d.raw>>26)!=0 && (d.raw>>26)!=16)))return execute_one;
  } else {
    if(fn==1) {
      if constexpr(Word)if(f>=1 && f<=3)return execute_one;
      static constexpr Helper multiply[]={muldiv_one<0,Word>,muldiv_one<1,Word>,muldiv_one<2,Word>,muldiv_one<3,Word>,
        muldiv_one<4,Word>,muldiv_one<5,Word>,muldiv_one<6,Word>,muldiv_one<7,Word>};
      return multiply[f];
    }
    if(fn!=0 && !(fn==32 && (f==0 || f==5)))return execute_one;
    if(fn==32 && f==0)return integer_one<0,Word,false,true>;
  }
  static constexpr Helper helpers[]={integer_one<0,Word,Immediate>,integer_one<1,Word,Immediate>,
    integer_one<2,Word,Immediate>,integer_one<3,Word,Immediate>,integer_one<4,Word,Immediate>,
    integer_one<5,Word,Immediate>,integer_one<6,Word,Immediate>,integer_one<7,Word,Immediate>};
  if(f==5 && (d.raw&(1u<<30)))return integer_one<5,Word,Immediate,true>;
  return helpers[f];
}
Helper select_helper(const Decoded& d) noexcept {
  if(d.funct3>7)return execute_one;
  static constexpr Helper loads[]={memory_one<0,false>,memory_one<1,false>,memory_one<2,false>,memory_one<3,false>,memory_one<4,false>,memory_one<5,false>,memory_one<6,false>};
  static constexpr Helper stores[]={memory_one<0,true>,memory_one<1,true>,memory_one<2,true>,memory_one<3,true>};
  if(!d.compressed && d.opcode==3 && d.funct3<std::size(loads))return loads[d.funct3];
  if(!d.compressed && d.opcode==0x23 && d.funct3<std::size(stores))return stores[d.funct3];
  if(!d.compressed) {
    if(d.opcode==7 && (d.funct3==2 || d.funct3==3))return d.funct3==2?fp_memory_one<32,false>:fp_memory_one<64,false>;
    if(d.opcode==0x27 && (d.funct3==2 || d.funct3==3))return d.funct3==2?fp_memory_one<32,true>:fp_memory_one<64,true>;
    if(d.opcode==0x53) {
      const bool dbl=(d.funct7&1)!=0;
      switch(d.funct7&~1u) {
      case 0:return dbl?fp_arithmetic_one<0,true>:fp_arithmetic_one<0,false>;
      case 4:return dbl?fp_arithmetic_one<4,true>:fp_arithmetic_one<4,false>;
      case 8:return dbl?fp_arithmetic_one<8,true>:fp_arithmetic_one<8,false>;
      case 12:return dbl?fp_arithmetic_one<12,true>:fp_arithmetic_one<12,false>;
      case 44:if(!d.rs2)return dbl?fp_arithmetic_one<44,true>:fp_arithmetic_one<44,false>;break;
      }
    }
    switch(d.opcode) {
    case 0x13:return integer_helper<false,true>(d);
    case 0x1b:return integer_helper<true,true>(d);
    case 0x33:return integer_helper<false,false>(d);
    case 0x3b:return integer_helper<true,false>(d);
    case 0x37:return address_one<0x37>;
    case 0x17:return address_one<0x17>;
    case 0x6f:return address_one<0x6f>;
    case 0x67:if(d.funct3==0)return address_one<0x67>;break;
    case 0x63: {
      static constexpr Helper branches[]={branch_one<0>,branch_one<1>,execute_one,execute_one,
        branch_one<4>,branch_one<5>,branch_one<6>,branch_one<7>};
      return branches[d.funct3];
    }
    }
  }
  return execute_one;
}
struct Emitter {
  static constexpr u8 kFrameBytes=0x48,kBusStack=0x38,kTrapStack=0x40;
  std::vector<u8> bytes;
  unsigned cached_rax=32;
  std::array<int,32> mapping=[] {std::array<int,32> a{};a.fill(-1);return a;}();
  std::array<bool,32> valid{},dirty{};
  bool self_backedge=false;
  bool conditional_backedge=false;
  bool persistent_loop_registers=false;
  bool deferred_backedge=false;
  bool linkable=true;
  bool fp_state_dirty=false;
  u32 helpers=0;
  std::size_t body_start=0;
  void b(std::initializer_list<u8> values) { bytes.insert(bytes.end(),values); }
  void d(u32 value) { for(unsigned n=0;n<4;++n) bytes.push_back(static_cast<u8>(value>>(8*n))); }
  void q(u64 value) { for(unsigned n=0;n<8;++n) bytes.push_back(static_cast<u8>(value>>(8*n))); }
  void move_register(unsigned dest,unsigned source) {
    if(dest==source)return;
    b({static_cast<u8>(0x48|(source>=8?4:0)|(dest>=8?1:0)),0x89,static_cast<u8>(0xc0|((source&7)<<3)|(dest&7))});
  }
  void shr_reg_imm8(unsigned host,u8 amount) { // shr r64,imm8 (/5)
    b({static_cast<u8>(0x48|(host>=8?1:0)),0xc1,static_cast<u8>(0xe8|(host&7))});b({amount});
  }
  void cmp_reg_reg(unsigned first,unsigned second) { // cmp r/m64,r64: first is r/m, second is reg
    b({static_cast<u8>(0x48|(second>=8?4:0)|(first>=8?1:0)),0x39,static_cast<u8>(0xc0|((second&7)<<3)|(first&7))});
  }
  void and_reg_imm8(unsigned host,u8 value) { // and r64,imm8 (/4)
    b({static_cast<u8>(0x48|(host>=8?1:0)),0x83,static_cast<u8>(0xe0|(host&7))});b({value});
  }
  void bt_reg(unsigned base,unsigned index) { // bt r/m64,r64: base is r/m, index is reg
    b({static_cast<u8>(0x48|(index>=8?4:0)|(base>=8?1:0)),0x0f,0xa3,static_cast<u8>(0xc0|((index&7)<<3)|(base&7))});
  }
  void test_byte_r12(u32 disp) { // test byte [r12+disp32],0 (F6 /0 with SIB base)
    b({0x41,0xf6,0x84,0x24});d(disp);b({0x00});
  }
  void cmp_byte_r12(u32 disp) { // cmp byte [r12+disp32],0 (80 /7 with SIB base)
    b({0x41,0x80,0xbc,0x24});d(disp);b({0x00});
  }
  void load_stack(unsigned host,u8 offset) {
    b({static_cast<u8>(0x48|(host>=8?4:0)),0x8b,static_cast<u8>(0x44|((host&7)<<3)),0x24,offset});
  }
  void prefix(bool word,unsigned dest,unsigned source=0,unsigned index=0) {
    const auto rex=static_cast<u8>(0x40|(!word?8:0)|(dest>=8?4:0)|(source>=8?1:0)|(index>=8?2:0));
    if(rex!=0x40)b({rex});
  }
  void lea(unsigned dest,unsigned base,unsigned index,i32 displacement,bool word=false) {
    const bool indexed=index<16,sib=indexed || (base&7)==4;
    const unsigned mod=displacement==0 && (base&7)!=5?0:
      displacement>=-128 && displacement<=127?1:2;
    prefix(word,dest,base,indexed?index:0);
    b({0x8d,static_cast<u8>((mod<<6)|((dest&7)<<3)|(sib?4:(base&7)))});
    if(sib)b({static_cast<u8>(((indexed?index&7:4)<<3)|(base&7))});
    if(mod==1)b({static_cast<u8>(displacement)});
    if(mod==2)d(static_cast<u32>(displacement));
  }
  unsigned operand_register(unsigned guest,bool second=false) {
    if(guest && mapping[guest]>=0) {
      const auto host=static_cast<unsigned>(mapping[guest]);
      if(!valid[guest]) {load_hart(guest,host);valid[guest]=true;}
      return host;
    }
    load(guest,second);return second?2u:0u;
  }
  void define_register(unsigned guest) {
    valid[guest]=dirty[guest]=true;
    if(cached_rax==guest)cached_rax=32;
  }
  void sign_word(unsigned host) {
    prefix(false,host,host);b({0x63,static_cast<u8>(0xc0|((host&7)<<3)|(host&7))});
  }
  u8 branch_condition(const IRInstruction& i) {
    const auto f=i.condition;
    const auto condition=static_cast<u8>(f==0?0x84:f==1?0x85:f==4?0x8c:f==5?0x8d:f==6?0x82:0x83);
    if(!i.b || !i.a) {
      const auto host=operand_register(i.b?i.b:i.a);
      prefix(false,host,host);b({0x85,static_cast<u8>(0xc0|((host&7)<<3)|(host&7))});
      // TEST clears CF/OF. For 0 < x and 0 >= x, reverse signed
      // comparisons and use nonzero/zero for the unsigned conditions.
      if(!i.a && i.b)return static_cast<u8>(f==4?0x8f:f==5?0x8e:f==6?0x85:f==7?0x84:condition);
      return condition;
    }
    const auto first=operand_register(i.a),second=operand_register(i.b,true);
    // CMP equal nonzero operands must set ZF, unlike TEST.
    prefix(false,second,first);b({0x39,static_cast<u8>(0xc0|((second&7)<<3)|(first&7))});
    return condition;
  }
  bool destination_arithmetic(const IRInstruction& i) {
    if(!i.dst || mapping[i.dst]<0)return false;
    const bool binary=i.op==IROp::Add || i.op==IROp::Sub || i.op==IROp::And || i.op==IROp::Or || i.op==IROp::Xor;
    const bool shift=i.op==IROp::Shl || i.op==IROp::Shr || i.op==IROp::Sar ||
      i.op==IROp::RotateLeft || i.op==IROp::RotateRight;
    const bool unary=i.op==IROp::ByteSwap || i.op==IROp::SignByte || i.op==IROp::SignHalf || i.op==IROp::ZeroHalf;
    if(!binary && !shift && !unary && i.op!=IROp::Move && i.op!=IROp::Compare && i.op!=IROp::Mul)return false;
    const auto host=static_cast<unsigned>(mapping[i.dst]);
    const auto first=operand_register(i.a);
    if(i.op==IROp::Move)move_register(host,first);
    else if(i.op==IROp::Compare) {
      if(i.immediate_rhs) {
        const bool small=static_cast<i64>(i.immediate)>=-128 && static_cast<i64>(i.immediate)<=127;
        prefix(false,0,first);b({static_cast<u8>(small?0x83:0x81),static_cast<u8>(0xf8|(first&7))});
        if(small)b({static_cast<u8>(i.immediate)});else d(static_cast<u32>(i.immediate));
      } else {
        const auto second=operand_register(i.b,true);
        prefix(false,second,first);b({0x39,static_cast<u8>(0xc0|((second&7)<<3)|(first&7))});
      }
      // REX also selects BPL/SIL/DIL, rather than legacy high-byte registers.
      b({static_cast<u8>(0x40|(host>=8?1:0)),0x0f,static_cast<u8>(i.unsigned_compare?0x92:0x9c),static_cast<u8>(0xc0|(host&7))});
      b({static_cast<u8>(0x40|(host>=8?5:0)),0x0f,0xb6,static_cast<u8>(0xc0|((host&7)<<3)|(host&7))});
    } else if(unary) {
      if(i.op==IROp::ByteSwap) {move_register(host,first);prefix(false,0,host);b({0x0f,static_cast<u8>(0xc8|(host&7))});}
      else {
        prefix(i.op==IROp::ZeroHalf,host,first);
        b({0x0f,static_cast<u8>(i.op==IROp::SignByte?0xbe:i.op==IROp::SignHalf?0xbf:0xb7),
          static_cast<u8>(0xc0|((host&7)<<3)|(first&7))});
      }
    } else if(i.op==IROp::Mul) {
      const auto second=operand_register(i.b,true),rhs=host==second?first:second;
      if(host!=second)move_register(host,first);
      prefix(i.word,host,rhs);b({0x0f,0xaf,static_cast<u8>(0xc0|((host&7)<<3)|(rhs&7))});
    }
    else if(shift) {
      // Capture CL before overwriting a destination that also supplies count.
      if(!i.immediate_rhs)move_register(1,operand_register(i.b,true));
      move_register(host,first);
      const auto group=static_cast<u8>(i.op==IROp::Shl?4:i.op==IROp::Shr?5:i.op==IROp::Sar?7:i.op==IROp::RotateLeft?0:1);
      prefix(i.word,0,host);b({static_cast<u8>(i.immediate_rhs?0xc1:0xd3),static_cast<u8>(0xc0|(group<<3)|(host&7))});
      if(i.immediate_rhs)b({static_cast<u8>(i.immediate)});
    } else if(i.immediate_rhs) {
      const auto displacement=i.op==IROp::Sub?0-i.immediate:i.immediate;
      if((i.op==IROp::Add || i.op==IROp::Sub) && host!=first &&
          static_cast<u64>(static_cast<i64>(static_cast<i32>(displacement)))==displacement)
        lea(host,first,16,static_cast<i32>(displacement),i.word);
      else {
        move_register(host,first);prefix(i.word,0,host);
        const auto group=static_cast<u8>(i.op==IROp::Add?0:i.op==IROp::Sub?5:i.op==IROp::And?4:i.op==IROp::Or?1:6);
        const bool small=static_cast<i64>(i.immediate)>=-128 && static_cast<i64>(i.immediate)<=127;
        b({static_cast<u8>(small?0x83:0x81),static_cast<u8>(0xc0|(group<<3)|(host&7))});
        if(small)b({static_cast<u8>(i.immediate)});else d(static_cast<u32>(i.immediate));
      }
    } else {
      const auto second=operand_register(i.b,true);
      if(i.op==IROp::Add && host!=first && host!=second)lea(host,first,second,0,i.word);
      else if(i.op==IROp::Sub && host==second && first!=second) {
        prefix(i.word,0,host);b({0xf7,static_cast<u8>(0xd8|(host&7))});
        prefix(i.word,first,host);b({0x01,static_cast<u8>(0xc0|((first&7)<<3)|(host&7))});
      } else {
        // Commutative operations can keep an existing RHS in the destination.
        const auto rhs=host==second && i.op!=IROp::Sub?first:second;
        if(host!=second || i.op==IROp::Sub)move_register(host,first);
        prefix(i.word,rhs,host);
        b({static_cast<u8>(i.op==IROp::Add?0x01:i.op==IROp::Sub?0x29:i.op==IROp::And?0x21:i.op==IROp::Or?0x09:0x31),
          static_cast<u8>(0xc0|((rhs&7)<<3)|(host&7))});
      }
    }
    if(i.word)sign_word(host);
    define_register(i.dst);return true;
  }
  void load_hart(unsigned guest,unsigned host) {
    b({static_cast<u8>(0x49|(host>=8?4:0)),0x8b,static_cast<u8>(0x84|((host&7)<<3)),0x24});d(static_cast<u32>(hart_offset(&Hart::x)+guest*8));
  }
  void allocate_registers(std::span<const IRInstruction> ir) {
    std::array<unsigned,32> score{};
    for(const auto& i:ir)if(i.op==IROp::Load || i.op==IROp::Store || i.op==IROp::FloatLoad || i.op==IROp::FloatStore) {
      const auto& d=i.source->decoded;
      if(d.rs1)score[d.rs1]+=3;
      if(i.op==IROp::Store) {if(d.rs2)score[d.rs2]+=2;}
      else if(d.rd)score[d.rd]+=2;
    } else if(pure_ir(i.op) || i.op==IROp::Branch || i.op==IROp::BranchCond) {
      if(i.op!=IROp::Nop && i.dst)++score[i.dst];
      if(i.op!=IROp::Const && i.op!=IROp::PCRelative && i.op!=IROp::Nop) {
        if(i.a)score[i.a]+=2;
        if(!i.immediate_rhs && i.b)score[i.b]+=2;
        if(i.op==IROp::Select && i.c)score[i.c]+=2;
      }
    }
    for(int host:{3,5,6,7,13,14}) {
      const auto it=std::max_element(score.begin()+1,score.end());if(*it<2)break;
      mapping[static_cast<unsigned>(it-score.begin())]=host;*it=0;
    }
  }
  void flush_register(unsigned guest) {
    if(guest && dirty[guest]) {
      const auto host=static_cast<unsigned>(mapping[guest]);
      b({static_cast<u8>(0x49|(host>=8?4:0)),0x89,static_cast<u8>(0x84|((host&7)<<3)),0x24});d(static_cast<u32>(hart_offset(&Hart::x)+guest*8));dirty[guest]=false;
    }
  }
  void flush() {for(unsigned guest=1;guest<32;++guest)flush_register(guest);}
  void load(unsigned reg,bool second=false) {
    if(!second && cached_rax==reg)return;
    if(!second)cached_rax=reg;
    if (!reg) {b(second?std::initializer_list<u8>{0x31,0xd2}:std::initializer_list<u8>{0x31,0xc0});return;}
    if(mapping[reg]>=0) {
      if(!valid[reg]) {load_hart(reg,static_cast<unsigned>(mapping[reg]));valid[reg]=true;}
      move_register(second?2:0,static_cast<unsigned>(mapping[reg]));
    } else load_hart(reg,second?2:0);
  }
  void store(unsigned reg) {
    cached_rax=reg?reg:32;if(!reg)return;
    if(mapping[reg]>=0) {move_register(static_cast<unsigned>(mapping[reg]),0);valid[reg]=dirty[reg]=true;}
    else {b({0x49,0x89,0x84,0x24});d(static_cast<u32>(hart_offset(&Hart::x)+reg*8));}
  }
  void store_float(unsigned reg) {
    b({0x49,0x89,0x84,0x24});d(static_cast<u32>(hart_offset(&Hart::f)+reg*8));cached_rax=32;
    if(!fp_state_dirty) {
      b({0x49,0x81,0x8c,0x24});d(static_cast<u32>(mstatus_offset()));d(3u<<13);
      b({0x49,0x0f,0xba,0xac,0x24});d(static_cast<u32>(mstatus_offset()));b({0x3f}); // set SD
      fp_state_dirty=true;
    }
  }
  void add_field(std::size_t offset,u32 value) {b({0x49,0x81,0x84,0x24});d(static_cast<u32>(offset));d(value);}
  void imm(u64 value,bool second=false) {
    if(!second)cached_rax=32;
    if(value<=0xffffffffull) {
      // Writing a 32-bit register zero-extends on x64.
      b({static_cast<u8>(second?0xba:0xb8)});d(static_cast<u32>(value));
    } else if(static_cast<u64>(static_cast<i64>(static_cast<i32>(value)))==value) {
      b({0x48,0xc7,static_cast<u8>(second?0xc2:0xc0)});d(static_cast<u32>(value));
    } else {b({0x48,static_cast<u8>(second?0xba:0xb8)});q(value);}
  }
  void comparison(bool unsign) {b({0x0f,static_cast<u8>(unsign?0x92:0x9c),0xc0,0x0f,0xb6,0xc0});}
  std::size_t conditional_jump(u8 condition) {b({0x0f,condition});const auto patch=bytes.size();d(0);return patch;}
  void patch_jump(std::size_t patch,std::size_t target) {
    const auto displacement=static_cast<u32>(target-patch-4);
    std::memcpy(bytes.data()+patch,&displacement,4);
  }
  void direct_links(u32 count) {
    // All operands below use signed disp8 encodings. A size check against 256
    // would incorrectly allow offsets 128..255 to become negative addresses.
    static_assert(sizeof(NativeChain)<=128 && sizeof(NativeLinkNode)<=128);
    std::vector<std::size_t> exits;
    b({0x4d,0x85,0xff});exits.push_back(conditional_jump(0x84));
    b({0x41,0x80,0x7f,static_cast<u8>(offsetof(NativeChain,direct_enabled)),0x00});exits.push_back(conditional_jump(0x84));
    b({0x4d,0x8b,0x5f,static_cast<u8>(offsetof(NativeChain,current)),0x4d,0x85,0xdb});
    exits.push_back(conditional_jump(0x84));
    b({0x49,0x8b,0x94,0x24});d(static_cast<u32>(hart_offset(&Hart::pc)));
    b({0x4d,0x8b,0x47,static_cast<u8>(offsetof(NativeChain,context))});
    for(unsigned slot=0;slot<2;++slot) {
      std::vector<std::size_t> misses;
      const auto offset=offsetof(NativeLinkNode,next)+slot*sizeof(NativeLinkNode::Link);
      // First reject PC, eviction epoch and translation context; only then
      // dereference the target. Descriptors are private to the owning hart.
      b({0x49,0x39,0x53,static_cast<u8>(offset+offsetof(NativeLinkNode::Link,pc))});misses.push_back(conditional_jump(0x85));
      // The counter belongs to the target's stable cache set, not the target
      // block object. Check it before touching a potentially freed target.
      b({0x4d,0x8b,0x53,static_cast<u8>(offset+offsetof(NativeLinkNode::Link,epoch_counter)),0x4d,0x85,0xd2});
      misses.push_back(conditional_jump(0x84));
      b({0x4d,0x8b,0x12});
      b({0x4d,0x39,0x53,static_cast<u8>(offset+offsetof(NativeLinkNode::Link,epoch))});misses.push_back(conditional_jump(0x85));
      // Fast exact context match, with the high bit reserved for a validated
      // direct same-page link. A context change can reuse only that page: a
      // cross-page or indirect successor still requires the exact context.
      b({0x49,0x8b,0x4b,static_cast<u8>(offset+offsetof(NativeLinkNode::Link,context)),
         0x48,0x0f,0xba,0xf1,0x3f,0x4c,0x39,0xc1});
      const auto context_ok=conditional_jump(0x84);
      b({0x41,0xf6,0x43,static_cast<u8>(offset+offsetof(NativeLinkNode::Link,context)+7),0x80});
      misses.push_back(conditional_jump(0x84));
      b({0x49,0x8b,0x4f,static_cast<u8>(offsetof(NativeChain,current_pc)),
         0x48,0x31,0xd1,0x48,0xc1,0xe9,0x0c});
      misses.push_back(conditional_jump(0x85));
      patch_jump(context_ok,bytes.size());
      b({0x4d,0x8b,0x4b,static_cast<u8>(offset+offsetof(NativeLinkNode::Link,target)),0x4d,0x85,0xc9});misses.push_back(conditional_jump(0x84));
      b({0x49,0x8b,0x41,static_cast<u8>(offsetof(NativeLinkNode,entry)),0x48,0x85,0xc0});misses.push_back(conditional_jump(0x84));
      b({0x41,0x8b,0x49,static_cast<u8>(offsetof(NativeLinkNode,count)),0x81,0xc1});d(count);
      b({0x41,0x39,0x4f,static_cast<u8>(offsetof(NativeChain,remaining))});misses.push_back(conditional_jump(0x82));
      b({0x49,0x8b,0x49,static_cast<u8>(offsetof(NativeLinkNode,generation)),0x48,0x85,0xc9});misses.push_back(conditional_jump(0x84));
      b({0x48,0x8b,0x09,0x49,0x3b,0x49,static_cast<u8>(offsetof(NativeLinkNode,expected_generation))});misses.push_back(conditional_jump(0x85));
      b({0x41,0x81,0x47,static_cast<u8>(offsetof(NativeChain,retired))});d(count);
      b({0x41,0x81,0x6f,static_cast<u8>(offsetof(NativeChain,remaining))});d(count);
      b({0x49,0x83,0x47,static_cast<u8>(offsetof(NativeChain,direct_links)),0x01});
      b({0x41,0x80,0x7f,static_cast<u8>(offsetof(NativeChain,sampling)),0x00});const auto no_sample=conditional_jump(0x84);
      b({0x49,0x81,0x47,static_cast<u8>(offsetof(NativeChain,sampled_instructions))});d(count);
      if(helpers) {b({0x49,0x81,0x47,static_cast<u8>(offsetof(NativeChain,sampled_helpers))});d(helpers);}
      patch_jump(no_sample,bytes.size());
      b({0x4d,0x89,0x4f,static_cast<u8>(offsetof(NativeChain,current))});
      b({0x49,0x89,0x57,static_cast<u8>(offsetof(NativeChain,current_pc))});
      b({0x49,0x8b,0x49,static_cast<u8>(offsetof(NativeLinkNode,generation)),0x49,0x89,0x4f,static_cast<u8>(offsetof(NativeChain,generation))});
      b({0x49,0x8b,0x49,static_cast<u8>(offsetof(NativeLinkNode,expected_generation)),0x49,0x89,0x4f,static_cast<u8>(offsetof(NativeChain,expected_generation))});
      b({0xff,0xe0}); // same frame, preserved hart/bus/trap/chain registers
      for(const auto miss:misses)patch_jump(miss,bytes.size());
    }
    for(const auto exit:exits)patch_jump(exit,bytes.size());
  }
  std::vector<std::size_t> inline_address(const Decoded& decoded,bool store,bool floating=false) {
    std::vector<std::size_t> misses;
    if(floating && !fp_state_dirty) {
      b({0x49,0xf7,0x84,0x24});d(static_cast<u32>(mstatus_offset()));d(3u<<13);
      misses.push_back(conditional_jump(0x84)); // FS=Off must fault before memory access
    }
    const auto immediate=store?sign_extend(((decoded.raw>>25)<<5)|((decoded.raw>>7)&31),12):static_cast<i64>(static_cast<i32>(decoded.raw)>>20);
    if(decoded.rs1 && mapping[decoded.rs1]>=0 && immediate) {
      lea(0,operand_register(decoded.rs1),16,static_cast<i32>(immediate));cached_rax=32;
    } else {load(decoded.rs1);if(immediate) {b({0x48,0x05});d(static_cast<u32>(immediate));}}
    b({0x4d,0x85,0xff});misses.push_back(conditional_jump(0x84));
    b({0x4d,0x8b,0x47,static_cast<u8>(store?offsetof(NativeChain,memory_stores):offsetof(NativeChain,memory_loads))});
    b({0x4d,0x85,0xc0});misses.push_back(conditional_jump(0x84));
    const auto width=1u<<(decoded.funct3&3);
    // One exact tag checks VA, validated context and natural alignment.
    // MMIO, misaligned and cross-page accesses retain the full helper path.
    // Fold descriptor scaling into the shift/mask, as in TCG's soft-MMU
    // indexing: ((VA >> 12) & 511) * 16 == (VA >> 8) & 0x1ff0.
    static_assert(sizeof(Hart::NativeTranslation)==16);
    move_register(1,0);b({0x48,0xc1,0xe9,0x08,0x81,0xe1});d((Hart::kTlbEntries-1)<<4);
    b({0x49,0x01,0xc8}); // r8 += byte offset of the descriptor
    move_register(2,0);b({0x48,0x81,0xe2});d(static_cast<u32>(~4095u|(width-1)));
    b({0x49,0x0b,0x57,static_cast<u8>(offsetof(NativeChain,memory_tag))});
    b({0x49,0x39,0x10});misses.push_back(conditional_jump(0x85));
    move_register(1,0);b({0x49,0x03,0x48,0x08}); // rcx = VA + host addend
    return misses;
  }
  std::vector<std::size_t> inline_store(const Decoded& decoded,bool floating=false) {
    auto misses=inline_address(decoded,true,floating);
    // RCX=validated host address. Obtain rs2 directly from the
    // register cache; the non-faulting RAM helper needs no architectural commit.
    move_register(8,1);
    cached_rax=32;
    if(floating) {b({0x49,0x8b,0x84,0x24});d(static_cast<u32>(hart_offset(&Hart::f)+decoded.rs2*8));}
    else load(decoded.rs2);
    move_register(9,0);
    move_register(1,12);load_stack(2,kBusStack);
    if(!floating) {
      // Inline the common RAM store instead of calling native_store_ram: the
      // address is validated, aligned and single-page, so the data write is one
      // MOV. Slow cases rejoin the shared helper path via misses (which repeats
      // the full write_fast/notify_write/note_store sequence exactly once; the
      // inline MOV below is skipped there, so nothing runs twice).
      // r8=host, r9=value, rdx=bus. rcx/rax/r10 are scratch here: the slow path
      // reloads rcx/rdx and recomputes operands from Hart.
      const unsigned width_bytes=1u<<(decoded.funct3&3);
      move_register(0,8); // rax = host
      b({0x48,0x2b,0x82});d(static_cast<u32>(memory::AddressSpace::offset_ram_data())); // sub rax,[rdx+ram_data]: offset
      // A descriptor hit implies mapped RAM: code_pages_ is allocated with the
      // RAM region, the offset is in range, and the tag mask already proved
      // natural alignment (hence single-page). Only the code-mark check below
      // can still divert to the slow path.
      // code_summary_ bit for the start page; a marked page may hold code.
      move_register(10,0); // r10 = offset
      shr_reg_imm8(10,0x0c); // shr r10,12: start page
      b({0x48,0x8b,0x8a});d(static_cast<u32>(memory::AddressSpace::offset_code_summary())); // mov rcx,[rdx+code_summary]
      move_register(0,10); // rax = page
      b({0x48,0xc1,0xe8,0x06}); // shr rax,6: summary index
      b({0x48,0x8b,0x0c,0xc1}); // mov rcx,[rcx+rax*8]
      and_reg_imm8(10,0x3f); // and r10,63: bit index
      bt_reg(1,10); // bt rcx,r10
      misses.push_back(conditional_jump(0x82)); // jc slow: possibly a code page
      b({0x80,0xba});d(static_cast<u32>(memory::AddressSpace::offset_concurrent()));b({0x00}); // cmp byte [rdx+concurrent],0
      misses.push_back(conditional_jump(0x85)); // jne slow: locked SMP path
      // Own-hart reservation: a valid reservation diverts to the helper, which
      // performs the write and notes (possibly clears) the reservation. With no
      // other hart present there is nothing else to clear; SMP uses the slow
      // locked path via concurrent_ above.
      cmp_byte_r12(static_cast<u32>(hart_offset(&Hart::reservation_valid))); // cmp byte [r12+reservation_valid],0
      misses.push_back(conditional_jump(0x85)); // jne slow: helper notes the store
      switch(width_bytes) {
      case 1:b({0x45,0x88,0x08});break; // mov [r8],r9b
      case 2:b({0x66,0x45,0x89,0x08});break; // mov [r8],r9w
      case 4:b({0x45,0x89,0x08});break; // mov [r8],r9d
      default:b({0x4d,0x89,0x08});break; // mov [r8],r9
      }
      // Compensate the prefix helper metadata like inline loads do; the sampled
      // counter test below is already runtime-gated, so no new config split.
      b({0x41,0x80,0x7f,static_cast<u8>(offsetof(NativeChain,sampling)),0x00});const auto no_store_sample=conditional_jump(0x84);
      b({0x49,0x83,0x6f,static_cast<u8>(offsetof(NativeChain,sampled_helpers)),0x01});
      patch_jump(no_store_sample,bytes.size());
      cached_rax=32;
      return misses;
    }
    // Floating-point stores keep the helper call: the value lives in an FP
    // register file slot (r8/r9/rcx/rdx above already hold host/value/hart/bus).
    const std::array<void(*)(Hart*,const memory::AddressSpace*,u8*,u64),4> stores{
      native_store_ram<8>,native_store_ram<16>,native_store_ram<32>,native_store_ram<64>};
    imm(reinterpret_cast<u64>(stores[decoded.funct3]));b({0xff,0xd0});cached_rax=32;
    return misses;
  }
  std::vector<std::size_t> inline_load(const Decoded& decoded,bool floating=false) {
    auto misses=inline_address(decoded,false,floating);
    const auto width=1u<<(decoded.funct3&3);
    if(width<8) {
      move_register(2,0);b({0x83,0xe2,0x07,0x48,0x83,0xe1,0xf8});
    }
    // An aligned MOV implements the shared atomic-u64 acquire read on x64.
    b({0x48,0x8b,0x01});
    if(width<8)b({0x89,0xd1,0xc1,0xe1,0x03,0x48,0xd3,0xe8});
    if(floating && decoded.funct3==2) {
      b({0x89,0xc0});imm(0xffffffff00000000ull,true);b({0x48,0x09,0xd0});
    } else if(!floating)switch(decoded.funct3) {
    case 0:b({0x48,0x0f,0xbe,0xc0});break;
    case 1:b({0x48,0x0f,0xbf,0xc0});break;
    case 2:b({0x48,0x63,0xc0});break;
    case 4:b({0x0f,0xb6,0xc0});break;
    case 5:b({0x0f,0xb7,0xc0});break;
    case 6:b({0x89,0xc0});break;
    }
    cached_rax=32;
    b({0x41,0x80,0x7f,static_cast<u8>(offsetof(NativeChain,sampling)),0x00});const auto no_sample=conditional_jump(0x84);
    // Prefix metadata conservatively includes the fallback call. Subtract the
    // calls avoided by actual TLB hits, only during opt-in sampled chains.
    b({0x49,0x83,0x6f,static_cast<u8>(offsetof(NativeChain,sampled_helpers)),0x01});
    b({0x49,0x83,0x47,static_cast<u8>(offsetof(NativeChain,sampled_loads)),0x01});
    patch_jump(no_sample,bytes.size());
    return misses;
  }
  void finish(u32 count,bool allow_backedge=false) {
    // Keep loop-carried values in callee-saved registers on the backedge.
    // All exits (including helper faults) still materialize architectural state.
    if(!persistent_loop_registers || !allow_backedge)flush();
    if(self_backedge && allow_backedge) {
      std::vector<std::size_t> exits;
      b({0x4d,0x85,0xff});exits.push_back(conditional_jump(0x84)); // no chain
      b({0x41,0x81,0x7f,static_cast<u8>(offsetof(NativeChain,remaining))});d(count*2);
      exits.push_back(conditional_jump(0x82)); // no budget for another block
      if(conditional_backedge && !deferred_backedge) {
        b({0x49,0x8b,0x84,0x24});d(static_cast<u32>(hart_offset(&Hart::pc)));
        b({0x48,0x3b,0x44,0x24,0x30});exits.push_back(conditional_jump(0x85)); // branch not taken
      }
      b({0x49,0x8b,0x47,static_cast<u8>(offsetof(NativeChain,generation)),0x48,0x85,0xc0});
      exits.push_back(conditional_jump(0x84));
      b({0x48,0x8b,0x00,0x49,0x3b,0x47,static_cast<u8>(offsetof(NativeChain,expected_generation))});exits.push_back(conditional_jump(0x85)); // modified code: ordinary resolver
      b({0x41,0x81,0x6f,static_cast<u8>(offsetof(NativeChain,remaining))});d(count);
      b({0x81,0x44,0x24,0x28});d(count);
      b({0xe9});const auto back=bytes.size();d(0);patch_jump(back,body_start);
      for(const auto exit:exits)patch_jump(exit,bytes.size());
    }
    if(persistent_loop_registers && allow_backedge)flush();
    if(allow_backedge && linkable && !self_backedge)direct_links(count);
    const auto restore=[&] {b({0x48,0x83,0xc4,kFrameBytes,0x5f,0x5e,0x5d,0x5b,0x41,0x5f,0x41,0x5e,0x41,0x5d,0x41,0x5c});};
    b({0x4d,0x85,0xff,0x0f,0x84});const auto no_chain=bytes.size();d(0);
    move_register(1,15);move_register(2,12);load_stack(8,kBusStack);load_stack(9,kTrapStack);b({0xb8});d(count);
    if(self_backedge)b({0x03,0x44,0x24,0x28});
    b({0x89,0x44,0x24,0x20});
    b({0x49,0x8b,0x07,0xff,0xd0,0x48,0x85,0xc0,0x0f,0x84});const auto stop_chain=bytes.size();d(0);
    move_register(1,12);load_stack(2,kBusStack);load_stack(8,kTrapStack);move_register(9,15);restore();b({0xff,0xe0});
    for(const auto patch:{no_chain,stop_chain}) {const auto delta=static_cast<u32>(bytes.size()-patch-4);std::memcpy(bytes.data()+patch,&delta,4);}
    b({0xb8});d(count);if(self_backedge)b({0x03,0x44,0x24,0x28});restore();b({0xc3});
  }
  // 0: helper, 1: sequential arithmetic, 2: control flow updated pc.
  int arithmetic(const IRInstruction& i) {
    if(i.op==IROp::Nop)return 1;
    if(i.op==IROp::Const || i.op==IROp::PCRelative) {
      if(i.op==IROp::Const && i.dst && mapping[i.dst]>=0) {
        const auto host=static_cast<unsigned>(mapping[i.dst]);
        if(i.immediate<=0xffffffffull) {if(host>=8)b({0x41});b({static_cast<u8>(0xb8|(host&7))});d(static_cast<u32>(i.immediate));}
        else if(static_cast<u64>(static_cast<i64>(static_cast<i32>(i.immediate)))==i.immediate) {
          prefix(false,0,host);b({0xc7,static_cast<u8>(0xc0|(host&7))});d(static_cast<u32>(i.immediate));
        } else {prefix(false,0,host);b({static_cast<u8>(0xb8|(host&7))});q(i.immediate);}
        define_register(i.dst);return 1;
      }
      imm(i.immediate);
      if(i.op==IROp::PCRelative) {b({0x49,0x03,0x84,0x24});d(static_cast<u32>(hart_offset(&Hart::pc)));}
      store(i.dst);return 1;
    }
    if(i.op==IROp::BranchCond) {
      b({0x0f,branch_condition(i)});
      d(17);
      add_field(hart_offset(&Hart::pc),i.source->decoded.length);b({0xe9});d(12);
      add_field(hart_offset(&Hart::pc),static_cast<u32>(i.immediate));return 2;
    }
    if(i.op==IROp::Branch) {
      if(i.indirect) {
        load(i.a);b({0x48,0x81,0xc0});d(static_cast<u32>(i.immediate));
        b({0x48,0x83,0xe0,0xfe,0x48,0x89,0xc2});
      }
      // Jumps/returns with rd=x0 discard the return address. Do not read PC
      // into a scratch register only to throw that value away.
      if(i.dst) {
        b({0x49,0x8b,0x84,0x24});d(static_cast<u32>(hart_offset(&Hart::pc)));
        b({0x48,0x83,0xc0,i.source->decoded.length});store(i.dst);
      }
      if(i.indirect) {b({0x49,0x89,0x94,0x24});d(static_cast<u32>(hart_offset(&Hart::pc)));}
      else add_field(hart_offset(&Hart::pc),static_cast<u32>(i.immediate));
      cached_rax=32;
      return 2;
    }
    if(!pure_ir(i.op))return 0;
    if(destination_arithmetic(i))return 1;
    load(i.a);
    if(i.op==IROp::Move) {store(i.dst);return 1;}
    if(i.op==IROp::Select) {
      move_register(10,0);cached_rax=32;load(i.c);load(i.b,true);
      b({0x4d,0x85,0xd2,0x48,0x0f,0x45,0xc2});
    } else if(i.op==IROp::Div || i.op==IROp::Rem) {
      load(i.b,true);move_register(1,2); // divisor in RCX; RDX is the high dividend
      if(!i.word)b({0x48});b({0x85,0xc9});const auto zero=conditional_jump(0x84);
      std::vector<std::size_t> divide,done;
      if(!i.unsigned_compare) {
        if(!i.word)b({0x48});b({0x83,0xf9,0xff});divide.push_back(conditional_jump(0x85));
        if(i.word) {b({0x3d});d(0x80000000u);}
        else {imm(1ull<<63,true);b({0x48,0x39,0xd0});}
        divide.push_back(conditional_jump(0x85));
        // RISC-V defines signed MIN / -1 and division by zero. Guard IDIV so
        // neither case raises a host exception; the quotient is already MIN.
        if(i.op==IROp::Rem)b({0x31,0xc0});
        b({0xe9});done.push_back(bytes.size());d(0);
      }
      for(const auto patch:divide)patch_jump(patch,bytes.size());
      if(i.unsigned_compare)b({0x31,0xd2});
      else {if(!i.word)b({0x48});b({0x99});} // CQO / CDQ
      if(!i.word)b({0x48});b({0xf7,static_cast<u8>(i.unsigned_compare?0xf1:0xf9)});
      if(i.op==IROp::Rem)move_register(0,2);
      b({0xe9});done.push_back(bytes.size());d(0);
      patch_jump(zero,bytes.size());
      if(i.op==IROp::Div)imm(~0ull);
      for(const auto patch:done)patch_jump(patch,bytes.size());
      cached_rax=32;
    } else if(i.op==IROp::ByteSwap)b({0x48,0x0f,0xc8});
    else if(i.op==IROp::SignByte)b({0x48,0x0f,0xbe,0xc0});
    else if(i.op==IROp::SignHalf)b({0x48,0x0f,0xbf,0xc0});
    else if(i.op==IROp::ZeroHalf)b({0x0f,0xb7,0xc0});
    else if(i.op==IROp::AndNot || i.op==IROp::OrNot || i.op==IROp::Xnor) {
      load(i.b,true);
      if(i.op==IROp::Xnor)b({0x48,0x31,0xd0,0x48,0xf7,0xd0});
      else b({0x48,0xf7,0xd2,0x48,static_cast<u8>(i.op==IROp::AndNot?0x21:0x09),0xd0});
    } else if(i.op==IROp::Shl || i.op==IROp::Shr || i.op==IROp::Sar ||
              i.op==IROp::RotateLeft || i.op==IROp::RotateRight) {
      const auto mod=static_cast<u8>(i.op==IROp::Shl?0xe0:i.op==IROp::Sar?0xf8:
                                    i.op==IROp::RotateLeft?0xc0:i.op==IROp::RotateRight?0xc8:0xe8);
      if(i.immediate_rhs) {
        if(!i.word)b({0x48});b({0xc1,mod,static_cast<u8>(i.immediate)});
      } else {
        load(i.b,true);b({0x89,0xd1});if(!i.word)b({0x48});b({0xd3,mod});
      }
    } else if(i.op==IROp::Compare) {
      if(i.immediate_rhs) {b({0x48,0x81,0xf8});d(static_cast<u32>(i.immediate));}
      else {load(i.b,true);b({0x48,0x39,0xd0});}
      comparison(i.unsigned_compare);
    } else if(i.op==IROp::Mul) {
      load(i.b,true);if(!i.word)b({0x48});b({0x0f,0xaf,0xc2});
    } else {
      if(!i.immediate_rhs)load(i.b,true);
      if(!i.word)b({0x48});
      if(i.immediate_rhs) {
        b({0x81,static_cast<u8>(i.op==IROp::Add?0xc0:i.op==IROp::Sub?0xe8:i.op==IROp::Xor?0xf0:i.op==IROp::Or?0xc8:0xe0)});
        d(static_cast<u32>(i.immediate));
      } else b({static_cast<u8>(i.op==IROp::Add?0x01:i.op==IROp::Sub?0x29:i.op==IROp::Xor?0x31:i.op==IROp::Or?0x09:0x21),0xd0});
    }
    if(i.word)b({0x48,0x63,0xc0});
    store(i.dst);return 1;
  }
};
}
CachedInstruction::Executor NativeBlock::executor_for(const Decoded& decoded) noexcept {return select_helper(decoded);}
struct NativeBlock::CodeArena {
  platform::CodeMemory memory;
  std::size_t used=0;
  explicit CodeArena(std::size_t bytes):memory(bytes) {}
  static std::shared_ptr<CodeArena> claim(std::size_t bytes,std::size_t& offset) {
    // Each compiler owns its append cursor. CPU threads only hold references
    // and execute sealed byte ranges, so new blocks need no CPU-side lock.
    // Small regions bound retention when an old block outlives its neighbours.
    constexpr std::size_t pooled_bytes=16*1024;
    static thread_local std::weak_ptr<CodeArena> current;
    auto arena=bytes<=pooled_bytes?current.lock():std::shared_ptr<CodeArena>{};
    if(!arena || bytes>arena->memory.size()-arena->used) {
      arena=std::make_shared<CodeArena>(std::max(pooled_bytes,platform::align_up_pages(bytes)));
      if(!arena->memory.executable_data())return {};
      if(bytes<=pooled_bytes)current=arena;
    }
    offset=arena->used;arena->used+=bytes;
    return arena;
  }
};
struct NativeBlock::Prepared {
  std::unique_ptr<NativeBlock> block;std::vector<u8> bytes;
};
NativeBlock::Prepared NativeBlock::prepare(std::span<const CachedInstruction> instructions) {
#if defined(_M_X64) || defined(__x86_64__)
  Emitter e;
  auto ir=lower_ir(instructions);optimize_ir(ir);e.allocate_registers(ir);
  if(!ir.empty()) {
    const auto& last=ir.back();u64 prefix_bytes=0;
    for(std::size_t n=0;n+1<instructions.size();++n)prefix_bytes+=instructions[n].decoded.length;
    e.self_backedge=(last.op==IROp::BranchCond || (last.op==IROp::Branch && !last.indirect)) &&
      last.immediate==0-prefix_bytes;
    e.conditional_backedge=last.op==IROp::BranchCond;
    // SYSTEM/FENCE inside externally supplied instruction spans can change
    // context. BlockEngine normally terminates there; reject these spans too.
    for(const auto& i:instructions)if(i.decoded.opcode==0x73 || i.decoded.opcode==0x0f) {e.self_backedge=false;e.linkable=false;}
    e.persistent_loop_registers=e.self_backedge && std::all_of(ir.begin(),ir.end(),[](const auto& i) {
      return pure_ir(i.op) || i.op==IROp::Branch || i.op==IROp::BranchCond ||
        i.op==IROp::Load || i.op==IROp::Store || i.op==IROp::FloatLoad || i.op==IROp::FloatStore;
    });
    // RAM fallback paths restore the deferred PC after a successful access.
    // In these loops the final backedge therefore already has the correct PC:
    // only a not-taken conditional exit needs to materialize the fallthrough.
    // PC-relative operations, earlier branches and generic helpers are excluded.
    e.deferred_backedge=e.persistent_loop_registers &&
      (last.op==IROp::BranchCond || (last.op==IROp::Branch && !last.dst)) &&
      std::all_of(ir.begin(),ir.end()-1,[](const auto& i) {
        return (pure_ir(i.op) && i.op!=IROp::PCRelative) ||
          (!i.source->decoded.compressed && (i.op==IROp::Load || i.op==IROp::Store ||
            i.op==IROp::FloatLoad || i.op==IROp::FloatStore));
      });
  }
  // r12=hart and r15=chain. Bus/trap pointers occupy private stack slots,
  // freeing r13/r14 for two more callee-saved guest registers. The frame also
  // keeps Win64 shadow space, a fifth argument and loop bookkeeping separate.
  e.b({0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x53,0x55,0x56,0x57,0x48,0x83,0xec,Emitter::kFrameBytes,
       0x49,0x89,0xcc,0x48,0x89,0x54,0x24,Emitter::kBusStack,
       0x4c,0x89,0x44,0x24,Emitter::kTrapStack,0x4d,0x89,0xcf});
  const auto linked_offset=e.bytes.size();
  if(e.self_backedge) {
    e.b({0xc7,0x44,0x24,0x28});e.d(0);
    if(!e.deferred_backedge) {
      e.b({0x49,0x8b,0x84,0x24});e.d(static_cast<u32>(hart_offset(&Hart::pc)));
      e.b({0x48,0x89,0x44,0x24,0x30});
    }
  }
  if(e.persistent_loop_registers)for(unsigned guest=1;guest<32;++guest)if(e.mapping[guest]>=0) {
    e.load_hart(guest,static_cast<unsigned>(e.mapping[guest]));
    // A fault early in a later iteration must also commit values produced by
    // the previous iteration, even if this iteration has not written them yet.
    e.valid[guest]=e.dirty[guest]=true;
  }
  // Entry executes padding once; the repeated loop starts on a fetch boundary.
  if(e.persistent_loop_registers)while(e.bytes.size()&15)e.b({0x90});
  e.body_start=e.bytes.size();
  u32 n=0, pending_pc=0, pending_retired=0;
  std::size_t conditional_loop_exit=0;
  std::vector<u8> helper_prefix{0};
  auto commit=[&] {
    if(pending_pc)e.add_field(hart_offset(&Hart::pc),pending_pc);
    if(pending_retired)e.add_field(hart_offset(&Hart::retired),pending_retired);
    pending_pc=pending_retired=0;
  };
  for (const auto& operation:ir) {
    const auto& instruction=*operation.source;
    // Helpers and PC-relative instructions observe the precise architectural
    // state. Ordinary ALU runs can commit once instead of twice per instruction.
    const bool alu=pure_ir(operation.op) && operation.op!=IROp::PCRelative;
    const auto& decoded=instruction.decoded;
    if(e.deferred_backedge && &operation==&ir.back()) {
      e.add_field(hart_offset(&Hart::retired),pending_retired+1);
      pending_retired=pending_pc=0;
      if(e.conditional_backedge) {
        const auto taken=e.branch_condition(operation);
        conditional_loop_exit=e.conditional_jump(taken^1u);
      }
      helper_prefix.push_back(helper_prefix.back());++n;continue;
    }
    const bool floating=operation.op==IROp::FloatLoad || operation.op==IROp::FloatStore;
    const bool native_load=e.linkable && !decoded.compressed &&
      ((operation.op==IROp::Load && decoded.funct3<=6) || operation.op==IROp::FloatLoad);
    const bool native_store=e.linkable && !decoded.compressed &&
      ((operation.op==IROp::Store && decoded.funct3<=3) || operation.op==IROp::FloatStore);
    const bool native_memory=native_load || native_store;
    if(!alu && !native_memory)commit();
    if (const auto direct=e.arithmetic(operation)) {
      helper_prefix.push_back(helper_prefix.back());
      if(direct==1)pending_pc+=instruction.decoded.length;
      ++pending_retired;
    } else {
      ++e.helpers;
      helper_prefix.push_back(static_cast<u8>(helper_prefix.back()+1));
      if(!native_memory)commit();
      const bool memory_helper=!decoded.compressed &&
        ((operation.op==IROp::Load && decoded.funct3<=6) ||
          (operation.op==IROp::Store && decoded.funct3<=3) || floating);
      const bool fp_helper=!decoded.compressed &&
        (decoded.opcode==7 || decoded.opcode==0x27 || decoded.opcode==0x53 ||
         decoded.opcode==0x43 || decoded.opcode==0x47 || decoded.opcode==0x4b || decoded.opcode==0x4f);
      const auto fp_fn=decoded.funct7&~1u;
      const bool fp_integer_result=fp_helper && decoded.opcode==0x53 &&
        (fp_fn==80 || fp_fn==96 || fp_fn==112);
      if(memory_helper && !native_memory) {
        // Memory helpers observe only their operands and modify only the load
        // destination. Preserve other dirty callee-saved guest registers over
        // the call; the fault exit below still materializes the entire state.
        e.flush_register(decoded.rs1);
        if(!floating)e.flush_register(operation.op==IROp::Store?decoded.rs2:decoded.rd);
      } else if(fp_helper && !native_memory) {
        // FP arithmetic does not touch integer registers. Loads/stores and
        // integer-to-FP moves need only rs1; FP-to-integer operations modify rd.
        if(decoded.opcode==7 || decoded.opcode==0x27 ||
            (decoded.opcode==0x53 && (fp_fn==104 || fp_fn==120)))e.flush_register(decoded.rs1);
        if(fp_integer_result)e.flush_register(decoded.rd);
      } else if(!memory_helper)e.flush();
      e.cached_rax=32;
      std::size_t memory_success=0;
      std::array<bool,32> memory_valid{},memory_dirty{};
      if(native_memory) {
        const auto fallback_valid=e.valid,fallback_dirty=e.dirty;
        const auto misses=native_load?e.inline_load(decoded,floating):e.inline_store(decoded,floating);
        if(native_load) {if(floating)e.store_float(decoded.rd);else e.store(decoded.rd);}
        memory_valid=e.valid;memory_dirty=e.dirty;
        e.b({0xe9});memory_success=e.bytes.size();e.d(0);
        for(const auto miss:misses)e.patch_jump(miss,e.bytes.size());
        e.valid=fallback_valid;e.dirty=fallback_dirty;
        // Validated RAM paths use the register cache directly. Only helpers
        // need operands and the old load destination materialized in Hart.
        e.flush_register(decoded.rs1);
        if(!floating)e.flush_register(operation.op==IROp::Store?decoded.rs2:decoded.rd);
        // Only a fallback observes architectural PC/retirement. Keep validated
        // RAM loads/stores in the deferred group with neighboring ALU operations.
        if(pending_pc)e.add_field(hart_offset(&Hart::pc),pending_pc);
        if(pending_retired)e.add_field(hart_offset(&Hart::retired),pending_retired);
      }
      if(fp_helper) {if(fp_integer_result)e.valid[decoded.rd]=false;}
      else if(operation.op==IROp::Load)e.valid[instruction.decoded.rd]=false;
      else if(operation.op!=IROp::Store)e.valid.fill(false);
      e.move_register(1,12);e.load_stack(2,Emitter::kBusStack);
      e.b({0x49,0xb8});e.q(reinterpret_cast<u64>(&instruction));
      e.load_stack(9,Emitter::kTrapStack);e.imm(reinterpret_cast<u64>(select_helper(instruction.decoded)));
      e.b({0xff,0xd0,0x84,0xc0,0x0f,0x85});
      const auto patch=e.bytes.size();e.d(0);
      const auto successful_dirty=e.dirty;
      e.finish(n|0x80000000u);
      e.dirty=successful_dirty;
      const auto delta=static_cast<u32>(e.bytes.size()-patch-4);
      std::memcpy(e.bytes.data()+patch,&delta,4);
      if(native_memory) {
        // Both paths join with the same register-cache state. On a slow-path
        // fault the destination still has its old, already materialized value.
        for(unsigned guest=1;guest<32;++guest)
          if(memory_valid[guest] && !e.valid[guest] && e.mapping[guest]>=0)
            e.load_hart(guest,static_cast<unsigned>(e.mapping[guest]));
        // A successful helper committed these instructions already; restore
        // the deferred base so both paths join with one later common commit.
        // Faults return above with the precise, fully materialized state.
        e.add_field(hart_offset(&Hart::pc),0u-pending_pc-decoded.length);
        e.add_field(hart_offset(&Hart::retired),0u-pending_retired-1);
        e.patch_jump(memory_success,e.bytes.size());
        e.valid=memory_valid;e.dirty=memory_dirty;
        pending_pc+=decoded.length;++pending_retired;
      }
      e.cached_rax=32;
      // All successful FP instructions except stores mark FS/SD dirty. There
      // is no SYSTEM/FENCE inside a linkable block, so later native FP memory
      // accesses need not repeat the enable check and dirty-state update.
      if(fp_helper && decoded.opcode!=0x27)e.fp_state_dirty=true;
    }
    ++n;
  }
  commit();
  const auto body_end=e.bytes.size();
  const auto loop_dirty=e.dirty;
  e.finish(n,true);
  if(conditional_loop_exit) {
    e.patch_jump(conditional_loop_exit,e.bytes.size());e.dirty=loop_dirty;
    u32 bytes=0;for(const auto& instruction:instructions)bytes+=instruction.decoded.length;
    e.add_field(hart_offset(&Hart::pc),bytes);
    e.finish(n,false);
  }
  auto block=std::make_unique<NativeBlock>();
  block->linked_offset_=linked_offset;
  block->body_offset_=e.body_start;block->body_size_=body_end-e.body_start;
  block->helper_prefix_=std::move(helper_prefix);
  return {std::move(block),std::move(e.bytes)};
#else
  (void)instructions;return {};
#endif
}
std::vector<std::unique_ptr<NativeBlock>> NativeBlock::compile_many(std::span<const std::span<const CachedInstruction>> sources) {
  std::vector<Prepared> prepared;prepared.reserve(sources.size());
  std::vector<std::unique_ptr<NativeBlock>> result(sources.size());
  std::size_t total=0;
  for(const auto source:sources) {
    prepared.push_back(source.empty()?Prepared{}:prepare(source));
    total+=(prepared.back().bytes.size()+15)&~std::size_t{15};
  }
  if(!total)return result;
  std::size_t offset=0;
  auto arena=CodeArena::claim(total,offset);
  if(!arena)return result;
  const auto start=offset;
  for(auto& item:prepared)if(item.block) {
    item.block->code_=const_cast<u8*>(static_cast<const u8*>(arena->memory.executable_data()))+offset;
    item.block->code_size_=item.bytes.size();
    item.block->arena_=arena;
    std::memcpy(static_cast<u8*>(arena->memory.writable_data())+offset,item.bytes.data(),item.bytes.size());
    offset+=(item.bytes.size()+15)&~std::size_t{15};
  }
  if(!arena->memory.publish(start,total))return result;
  for(std::size_t n=0;n<prepared.size();++n)result[n]=std::move(prepared[n].block);
  return result;
}
std::unique_ptr<NativeBlock> NativeBlock::compile(std::span<const CachedInstruction> source) {
  const std::array<std::span<const CachedInstruction>,1> spans{source};
  auto code=compile_many(spans);return std::move(code.front());
}
NativeBlock::~NativeBlock()=default;
u32 NativeBlock::run(Hart& hart,const memory::AddressSpace& bus,Trap& trap,NativeChain* chain) const {
  HostFpScope fp_environment;
  if(chain) {
    mmu::prepare_native_tlb(hart,bus);
    chain->memory_tag=hart.native_tlb_tag;
    chain->memory_loads=hart.translation_cache_enabled && chain->inline_loads?hart.native_tlb[0]:nullptr;
    chain->memory_stores=hart.translation_cache_enabled && chain->inline_stores?hart.native_tlb[1]:nullptr;
  }
  using Function=u32(*)(Hart*,const memory::AddressSpace*,Trap*,NativeChain*);
  const bool old=hart.native_fp;hart.native_fp=true;
  const auto result=reinterpret_cast<Function>(code_)(&hart,&bus,&trap,chain);
  hart.native_fp=old;return result;
}
}


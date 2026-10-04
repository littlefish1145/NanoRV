// SPDX-License-Identifier: MIT
#include "cpu/ir.hpp"
#include "cpu/softint.hpp"
#include <array>
#include <bit>
namespace nanorv::cpu {
bool pure_ir(IROp op) noexcept {return op<=IROp::PCRelative;}
std::vector<IRInstruction> lower_ir(std::span<const CachedInstruction> instructions) {
  std::vector<IRInstruction> output;output.reserve(instructions.size());
  for(const auto& source:instructions) {
    const auto& d=source.decoded;IRInstruction i;
    i.source=&source;i.dst=d.rd;i.a=d.rs1;i.b=d.rs2;
    const auto op=d.opcode,f=d.funct3,fn=d.funct7;
    if(!d.compressed) {
      if(op==0x37 || op==0x17) {i.op=op==0x37?IROp::Const:IROp::PCRelative;i.immediate=static_cast<u64>(sign_extend(d.raw&0xfffff000u,32));}
      else if(op==0x13 || op==0x1b || op==0x33 || op==0x3b) {
        i.word=op==0x1b || op==0x3b;i.immediate_rhs=op==0x13 || op==0x1b;
        i.immediate=static_cast<u64>(static_cast<i64>(static_cast<i32>(d.raw)>>20));
        bool valid=true;
        if(i.word && f!=0 && f!=1 && f!=5)valid=false;
        if(i.immediate_rhs) {
          if(f==1 && (i.word?fn!=0:(d.raw>>26)!=0))valid=false;
          if(f==5 && (i.word?(fn!=0 && fn!=32):((d.raw>>26)!=0 && (d.raw>>26)!=16)))valid=false;
          if(f==1 || f==5)i.immediate=(d.raw>>20)&(i.word?31:63);
        } else if(fn!=0 && !(fn==32 && (f==0 || f==5)) && !(fn==1 && f==0))valid=false;
        if(valid) {
          if(!i.immediate_rhs && fn==1)i.op=IROp::Mul;
          else if(f==0)i.op=!i.immediate_rhs && fn==32?IROp::Sub:IROp::Add;
          else if(f==1)i.op=IROp::Shl;
          else if(f==2 || f==3) {i.op=IROp::Compare;i.unsigned_compare=f==3;}
          else if(f==4)i.op=IROp::Xor;
          else if(f==5)i.op=(d.raw>>30)&1?IROp::Sar:IROp::Shr;
          else if(f==6)i.op=IROp::Or;
          else if(f==7)i.op=IROp::And;
          if(i.op==IROp::Add && i.immediate_rhs && !i.immediate && !i.word)i.op=IROp::Move;
        }
      } else if(op==3)i.op=IROp::Load;
      else if(op==0x23)i.op=IROp::Store;
      else if(op==7 && (f==2 || f==3))i.op=IROp::FloatLoad;
      else if(op==0x27 && (f==2 || f==3))i.op=IROp::FloatStore;
      else if(op==0x63 && f!=2 && f!=3) {
        i.op=IROp::BranchCond;i.condition=static_cast<u8>(f);
        const u32 immediate=((d.raw>>31)<<12)|(((d.raw>>7)&1)<<11)|(((d.raw>>25)&63)<<5)|(((d.raw>>8)&15)<<1);
        i.immediate=static_cast<u64>(sign_extend(immediate,13));
      } else if(op==0x6f || (op==0x67 && f==0)) {
        i.op=IROp::Branch;i.indirect=op==0x67;
        const u32 immediate=((d.raw>>31)<<20)|(((d.raw>>12)&255)<<12)|(((d.raw>>20)&1)<<11)|(((d.raw>>21)&1023)<<1);
        i.immediate=static_cast<u64>(i.indirect?sign_extend(d.raw>>20,12):sign_extend(immediate,21));
      }
      // These instructions have non-trapping native equivalents. Keep them in
      // the ALU/deferred-commit group instead of spilling all registers for a
      // reference executor call. Reserved encodings still fall back unchanged.
      if((op==0x33 || op==0x3b) && fn==1 && f>=4) {
        i.op=f<6?IROp::Div:IROp::Rem;i.word=op==0x3b;
        i.immediate_rhs=false;i.unsigned_compare=(f&1)!=0;
      } else if((op==0x33 || op==0x3b) && fn==48 && (f==1 || f==5)) {
        i.op=f==1?IROp::RotateLeft:IROp::RotateRight;i.word=op==0x3b;i.immediate_rhs=false;
      } else if(f==5 && ((op==0x13 && (d.raw>>26)==24) || (op==0x1b && fn==48))) {
        i.op=IROp::RotateRight;i.word=op==0x1b;i.immediate_rhs=true;
        i.immediate=(d.raw>>20)&(i.word?31:63);
      } else if(op==0x33 && fn==32 && (f==4 || f==6 || f==7)) {
        i.op=f==4?IROp::Xnor:f==6?IROp::OrNot:IROp::AndNot;i.immediate_rhs=false;
      } else if(op==0x13 && f==5 && (d.raw>>20)==0x6b8) {
        i.op=IROp::ByteSwap;
      } else if(op==0x13 && f==1 && ((d.raw>>20)==0x604 || (d.raw>>20)==0x605)) {
        i.op=(d.raw>>20)==0x604?IROp::SignByte:IROp::SignHalf;
      } else if(op==0x3b && fn==4 && f==4 && !d.rs2) {
        i.op=IROp::ZeroHalf;i.word=false;
      }
    }
    output.push_back(i);
  }
  return output;
}
namespace {
u64 evaluate(const IRInstruction& i,u64 a,u64 b) {
  const unsigned shift=static_cast<unsigned>(b)&(i.word?31:63);u64 value=0;
  if(i.word)a=static_cast<u32>(a);
  switch(i.op) {
  case IROp::Move:value=a;break;
  case IROp::Add:value=a+b;break;
  case IROp::Sub:value=a-b;break;
  case IROp::And:value=a&b;break;
  case IROp::Or:value=a|b;break;
  case IROp::Xor:value=a^b;break;
  case IROp::Shl:value=a<<shift;break;
  case IROp::Shr:value=a>>shift;break;
  case IROp::Sar:value=i.word?static_cast<u64>(static_cast<i32>(a)>>shift):static_cast<u64>(static_cast<i64>(a)>>shift);break;
  case IROp::Mul:value=a*b;break;
  case IROp::Div:case IROp::Rem: {
    if(i.word)b=static_cast<u32>(b);
    if(i.unsigned_compare)value=i.op==IROp::Div?div_unsigned(a,b):rem_unsigned(a,b);
    else {
      const auto signed_a=i.word?static_cast<i64>(static_cast<i32>(a)):static_cast<i64>(a);
      const auto signed_b=i.word?static_cast<i64>(static_cast<i32>(b)):static_cast<i64>(b);
      value=i.op==IROp::Div?div_signed(signed_a,signed_b):rem_signed(signed_a,signed_b);
    }
    break;
  }
  case IROp::RotateLeft:value=i.word?std::rotl(static_cast<u32>(a),static_cast<int>(shift)):std::rotl(a,static_cast<int>(shift));break;
  case IROp::RotateRight:value=i.word?std::rotr(static_cast<u32>(a),static_cast<int>(shift)):std::rotr(a,static_cast<int>(shift));break;
  case IROp::AndNot:value=a&~b;break;
  case IROp::OrNot:value=a|~b;break;
  case IROp::Xnor:value=~(a^b);break;
  case IROp::ByteSwap:value=std::byteswap(a);break;
  case IROp::SignByte:value=static_cast<u64>(sign_extend(a,8));break;
  case IROp::SignHalf:value=static_cast<u64>(sign_extend(a,16));break;
  case IROp::ZeroHalf:value=a&65535;break;
  case IROp::Compare:value=i.unsigned_compare?a<b:static_cast<i64>(a)<static_cast<i64>(b);break;
  default:break;
  }
  return i.word?static_cast<u64>(sign_extend(value,32)):value;
}
}
void optimize_ir(std::span<IRInstruction> instructions) {
  std::array<bool,32> known{};std::array<u64,32> values{};known[0]=true;
  for(auto& i:instructions) {
    // Fold a conditional branch with two known operands, as QEMU does. A taken
    // condition keeps its displacement; a never-taken one becomes a jump to the
    // fallthrough so PC still advances exactly one instruction.
    if(i.op==IROp::BranchCond && known[i.a] && known[i.b]) {
      const u64 lhs=values[i.a],rhs=values[i.b];
      const auto left=static_cast<i64>(lhs),right=static_cast<i64>(rhs);
      bool taken=false;
      switch(i.condition) {
      case 0:taken=lhs==rhs;break;
      case 1:taken=lhs!=rhs;break;
      case 4:taken=left<right;break;
      case 5:taken=left>=right;break;
      case 6:taken=lhs<rhs;break;
      default:taken=lhs>=rhs;break;
      }
      i.op=IROp::Branch;i.indirect=false;i.dst=0;i.a=0;i.b=0;
      i.immediate=taken?i.immediate:static_cast<u64>(i.source->decoded.length);
      continue;
    }
    if(!pure_ir(i.op)) {known.fill(false);known[0]=true;continue;}
    if(i.op!=IROp::Const&&i.dst&&i.op!=IROp::Move&&i.op!=IROp::Select) {
      // QEMU folds algebraic identities before propagating values, so an
      // operation with a neutral or annihilating right operand disappears even
      // when the other operand is only known at run time. -O2 code emits
      // several of these per function, mostly against x0.
      u64 right=0;bool have=false;
      if(i.immediate_rhs) {right=i.immediate;have=true;}
      else if(known[i.b]) {right=values[i.b];have=true;}
      if(have) {
        const auto ones=i.word?0xffffffffull:~0ull;
        const auto move=[&i] {i.op=IROp::Move;i.immediate_rhs=false;i.b=0;};
        const auto constant=[&i](u64 value) {i.op=IROp::Const;i.immediate=value;i.immediate_rhs=false;i.b=0;};
        // Word forms sign-extend their result and Move is emitted as a plain
        // register copy, so only constant results may fold when i.word is set.
        const auto identity=[&] {if(!i.word)move();};
        if(right==0) switch(i.op) {
        case IROp::Add:case IROp::Sub:case IROp::Xor:case IROp::Or:
        case IROp::Shl:case IROp::Shr:case IROp::Sar:identity();break;
        case IROp::And:case IROp::Mul:constant(0);break;
        default:break;
        } else if(i.op==IROp::Mul&&right==1) identity();
        else if(right==ones&&!i.word) {
          if(i.op==IROp::And)move();
          else if(i.op==IROp::Or)constant(ones);
        }
      }
    }
    if(i.op==IROp::Const) {if(i.dst) {known[i.dst]=true;values[i.dst]=i.immediate;}continue;}
    if(i.op!=IROp::PCRelative && i.op!=IROp::Nop && i.op!=IROp::Select && known[i.a] && (i.op==IROp::Move || i.immediate_rhs || known[i.b])) {
      i.immediate=evaluate(i,values[i.a],i.immediate_rhs?i.immediate:values[i.b]);i.op=IROp::Const;
      if(i.dst) {known[i.dst]=true;values[i.dst]=i.immediate;}
    } else if(i.dst)known[i.dst]=false;
  }
  // Fuse adjacent immediate updates of the same value. Their original slots
  // remain NOPs so PC and retired counts remain exact; no helper/fault boundary
  // or intermediate observable register read can be crossed.
  std::size_t previous=instructions.size();
  for(std::size_t n=0;n<instructions.size();++n) {
    auto& current=instructions[n];
    const bool fusible=current.immediate_rhs && current.dst==current.a && current.dst &&
        (current.op==IROp::Add || current.op==IROp::Xor || current.op==IROp::And || current.op==IROp::Or);
    if(fusible && previous<instructions.size()) {
      auto& prior=instructions[previous];
      if(prior.op==current.op && prior.dst==current.dst && prior.word==current.word) {
        if(current.op==IROp::Add)current.immediate+=prior.immediate;
        if(current.op==IROp::Xor)current.immediate^=prior.immediate;
        if(current.op==IROp::And)current.immediate&=prior.immediate;
        if(current.op==IROp::Or)current.immediate|=prior.immediate;
        prior.op=IROp::Nop;
      }
    }
    previous=fusible?n:instructions.size();
  }
  // Preserve architectural state at every helper/fault/branch boundary. Only
  // overwritten pure definitions inside a straight segment are dead stores.
  u32 live=~0u;
  for(auto it=instructions.rbegin();it!=instructions.rend();++it) {
    auto& i=*it;
    if(i.op==IROp::Nop)continue; // Removed slots neither define nor consume a value.
    if(!pure_ir(i.op)) {live=~0u;continue;}
    if(!i.dst || !(live&(1u<<i.dst))) {i.op=IROp::Nop;continue;}
    live&=~(1u<<i.dst);
    if(i.op!=IROp::Const && i.op!=IROp::PCRelative && i.op!=IROp::Nop) {
      live|=1u<<i.a;
      if(i.op!=IROp::Move && !i.immediate_rhs)live|=1u<<i.b;
      if(i.op==IROp::Select)live|=1u<<i.c;
    }
  }
}
}

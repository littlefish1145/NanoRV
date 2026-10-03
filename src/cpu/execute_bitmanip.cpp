// SPDX-License-Identifier: MIT
#include "cpu/execute_bitmanip.hpp"
#include <bit>
#include <algorithm>
namespace rvvm::cpu {
bool execute_bitmanip(Hart& h,const Decoded& d) noexcept {
  const auto op=d.opcode,f=d.funct3,fn=d.funct7;
  const u64 a=h.get_reg(d.rs1),b=h.get_reg(d.rs2);
  const u32 imm=d.raw>>20;
  u64 v=0;
  if((op==0x33 || op==0x3b) && fn==16 && (f==2 || f==4 || f==6)) {
    v=((op==0x3b?static_cast<u64>(static_cast<u32>(a)):a)<<(f/2))+b;
  } else if(op==0x3b && fn==4 && f==0) v=static_cast<u32>(a)+b;
  else if(op==0x1b && (d.raw>>26)==2 && f==1) v=static_cast<u64>(static_cast<u32>(a))<<((d.raw>>20)&63);
  else if(op==0x33 && fn==32 && (f==4 || f==6 || f==7)) v=f==4?~(a^b):f==6?a|~b:a&~b;
  else if(op==0x33 && fn==5 && f>=4) {
    if(f==4)v=static_cast<i64>(a)<static_cast<i64>(b)?a:b;
    if(f==5)v=std::min(a,b);
    if(f==6)v=static_cast<i64>(a)>static_cast<i64>(b)?a:b;
    if(f==7)v=std::max(a,b);
  } else if((op==0x13 || op==0x1b) && f==1 && imm>=0x600 && imm<=0x602) {
    if(op==0x13)v=imm==0x600?std::countl_zero(a):imm==0x601?std::countr_zero(a):std::popcount(a);
    else {const auto w=static_cast<u32>(a);v=imm==0x600?std::countl_zero(w):imm==0x601?std::countr_zero(w):std::popcount(w);}
  } else if(op==0x13 && f==1 && (imm==0x604 || imm==0x605)) v=static_cast<u64>(sign_extend(a,imm==0x604?8:16));
  else if(op==0x3b && fn==4 && f==4 && !d.rs2)v=a&65535;
  else if(op==0x13 && f==5 && imm==0x287) {
    for(unsigned n=0;n<8;++n)if((a>>(n*8))&255)v|=255ull<<(n*8);
  } else if(op==0x13 && f==5 && imm==0x6b8)v=std::byteswap(a);
  else if((op==0x33 || op==0x3b) && fn==48 && (f==1 || f==5)) {
    if(op==0x33)v=f==1?std::rotl(a,static_cast<int>(b&63)):std::rotr(a,static_cast<int>(b&63));
    else {const auto w=static_cast<u32>(a);v=static_cast<u64>(sign_extend(f==1?std::rotl(w,static_cast<int>(b&31)):std::rotr(w,static_cast<int>(b&31)),32));}
  } else if(f==5 && ((op==0x13 && (d.raw>>26)==24) || (op==0x1b && fn==48))) {
    if(op==0x13)v=std::rotr(a,static_cast<int>(imm&63));
    else v=static_cast<u64>(sign_extend(std::rotr(static_cast<u32>(a),static_cast<int>(imm&31)),32));
  } else if(op==0x33 && ((f==1 && (fn==20 || fn==36 || fn==52)) || (f==5 && fn==36))) {
    const auto mask=1ull<<(b&63);
    v=f==5?(a>>(b&63))&1:fn==20?a|mask:fn==36?a&~mask:a^mask;
  } else if(op==0x13 && ((f==1 && ((d.raw>>26)==10 || (d.raw>>26)==18 || (d.raw>>26)==26)) || (f==5 && (d.raw>>26)==18))) {
    const auto mask=1ull<<(imm&63);const auto fun=d.raw>>26;
    v=f==5?(a>>(imm&63))&1:fun==10?a|mask:fun==18?a&~mask:a^mask;
  } else return false;
  h.set_reg(d.rd,v);return true;
}
}

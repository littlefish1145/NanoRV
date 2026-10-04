// SPDX-License-Identifier: MIT
#include "cpu/execute_crypto.hpp"
#include <bit>
#include <array>
#if defined(_M_X64)
#include <intrin.h>
#include <wmmintrin.h>
#define NanoRV_CRYPTO_X64 1
#else
#define NanoRV_CRYPTO_X64 0
#endif
namespace nanorv::cpu {
namespace {
u8 multiply(u8 a,u8 b) {
  u8 result=0;
  // Fixed iteration count and masks: no data-dependent table lookup/branch.
  for(unsigned n=0;n<8;++n) {
    result^=static_cast<u8>(a & (0u-(b&1u)));
    a=static_cast<u8>((static_cast<unsigned>(a)<<1) ^ ((0u-(a>>7))&0x1bu));b>>=1;
  }
  return result;
}
u8 inverse(u8 value) {
  u8 out=1,power=value;
  for(unsigned n=0;n<8;++n) {if(n)out=multiply(out,power);power=multiply(power,power);}
  return out;
}
u8 sbox(u8 v,bool decrypt) {
  if(decrypt)return inverse(static_cast<u8>(std::rotl(v,1)^std::rotl(v,3)^std::rotl(v,6)^5));
  v=inverse(v);return static_cast<u8>(v^std::rotl(v,1)^std::rotl(v,2)^std::rotl(v,3)^std::rotl(v,4)^0x63);
}
u32 mix(u32 v,bool decrypt) {
  std::array<u8,4> a{};for(unsigned n=0;n<4;++n)a[n]=static_cast<u8>(v>>(n*8));
  u32 out=0;
  for(unsigned n=0;n<4;++n) {
    const u8 value=decrypt?static_cast<u8>(multiply(a[n],14)^multiply(a[(n+1)%4],11)^multiply(a[(n+2)%4],13)^multiply(a[(n+3)%4],9)):
      static_cast<u8>(multiply(a[n],2)^multiply(a[(n+1)%4],3)^a[(n+2)%4]^a[(n+3)%4]);
    out|=static_cast<u32>(value)<<(n*8);
  }
  return out;
}
#if NanoRV_CRYPTO_X64
unsigned host_features() {static const unsigned flags=[] {int values[4]{};__cpuid(values,1);return static_cast<unsigned>(values[2]);}();return flags;}
#if defined(__clang__)
#define TARGET_CRYPTO __attribute__((target("aes,pclmul")))
#else
#define TARGET_CRYPTO
#endif
TARGET_CRYPTO u64 accelerated_aes(unsigned fn,u64 a,u64 b) {
  const auto input=_mm_set_epi64x(static_cast<i64>(b),static_cast<i64>(a)),zero=_mm_setzero_si128();
  const auto value=fn==25?_mm_aesenclast_si128(input,zero):fn==27?_mm_aesenc_si128(input,zero):
    fn==29?_mm_aesdeclast_si128(input,zero):fn==31?_mm_aesdec_si128(input,zero):_mm_aesimc_si128(input);
  return static_cast<u64>(_mm_cvtsi128_si64(value));
}
TARGET_CRYPTO u32 accelerated_subword(u32 value) {return static_cast<u32>(_mm_cvtsi128_si32(_mm_aesenclast_si128(_mm_set1_epi32(static_cast<i32>(value)),_mm_setzero_si128())));}
TARGET_CRYPTO u64 accelerated_carryless(unsigned fn,u64 a,u64 b) {
  const auto product=_mm_clmulepi64_si128(_mm_cvtsi64_si128(static_cast<i64>(a)),_mm_cvtsi64_si128(static_cast<i64>(b)),0);
  const u64 lo=static_cast<u64>(_mm_cvtsi128_si64(product)),hi=static_cast<u64>(_mm_cvtsi128_si64(_mm_srli_si128(product,8)));
  return fn==1?lo:fn==3?hi:(lo>>63)|(hi<<1);
}
#endif
u32 subword(u32 value,bool native) {
#if NanoRV_CRYPTO_X64
  if(native && (host_features()&(1u<<25)))return accelerated_subword(value);
#else
  (void)native;
#endif
  u32 result=0;for(unsigned n=0;n<4;++n)result|=static_cast<u32>(sbox(static_cast<u8>(value>>(n*8)),false))<<(n*8);return result;
}
}
u64 aes_round(unsigned fn,u64 a,u64 b,bool native) noexcept {
#if NanoRV_CRYPTO_X64
  if(native && (host_features()&(1u<<25)))return accelerated_aes(fn,a,b);
#else
  (void)native;
#endif
  if(fn==24)return static_cast<u64>(mix(static_cast<u32>(a),true))|(static_cast<u64>(mix(static_cast<u32>(a>>32),true))<<32);
  const bool decrypt=fn==29 || fn==31;
  std::array<u8,16> source{};for(unsigned n=0;n<8;++n) {source[n]=static_cast<u8>(a>>(n*8));source[n+8]=static_cast<u8>(b>>(n*8));}
  u64 value=0;
  for(unsigned n=0;n<8;++n) {
    const auto column=(n/4+(decrypt?4-n%4:n%4))%4;
    value|=static_cast<u64>(sbox(source[column*4+n%4],decrypt))<<(n*8);
  }
  if(fn==27 || fn==31)value=static_cast<u64>(mix(static_cast<u32>(value),decrypt))|(static_cast<u64>(mix(static_cast<u32>(value>>32),decrypt))<<32);
  return value;
}
u64 carryless(unsigned fn,u64 a,u64 b,bool native) noexcept {
#if NanoRV_CRYPTO_X64
  if(native && (host_features()&2))return accelerated_carryless(fn,a,b);
#else
  (void)native;
#endif
  u64 lo=0,hi=0;
  for(unsigned n=0;n<64;++n) {const u64 mask=0ull-((b>>n)&1);lo^=(a<<n)&mask;if(n)hi^=(a>>(64-n))&mask;}
  return fn==1?lo:fn==3?hi:(lo>>63)|(hi<<1);
}
bool execute_crypto(Hart& h,const Decoded& d) noexcept {
  const u32 op=d.opcode,f=d.funct3,fn=d.funct7,imm=d.raw>>20;
  const auto a=h.get_reg(d.rs1),b=h.get_reg(d.rs2);u64 result=0;
  if(op==0x33 && fn==4 && f==4)result=static_cast<u32>(a)|(static_cast<u64>(static_cast<u32>(b))<<32);
  else if(op==0x33 && fn==4 && f==7)result=(a&255)|((b&255)<<8);
  else if(op==0x3b && fn==4 && f==4)result=static_cast<u64>(sign_extend((a&65535)|((b&65535)<<16),32));
  else if(op==0x13 && f==5 && imm==0x687) {
    result=((a>>1)&0x5555555555555555ull)|((a&0x5555555555555555ull)<<1);
    result=((result>>2)&0x3333333333333333ull)|((result&0x3333333333333333ull)<<2);
    result=((result>>4)&0x0f0f0f0f0f0f0f0full)|((result&0x0f0f0f0f0f0f0f0full)<<4);
  }
  else if(op==0x33 && fn==5 && f>=1 && f<=3)result=carryless(f,a,b,h.native_fp);
  else if(op==0x33 && f==0 && (fn==25 || fn==27 || fn==29 || fn==31))result=aes_round(fn,a,b,h.native_fp);
  else if(op==0x13 && f==1 && imm==0x300)result=aes_round(24,a,0,h.native_fp);
  else if(op==0x33 && fn==63 && f==0) {const u32 lo=static_cast<u32>(a>>32)^static_cast<u32>(b);result=lo|(static_cast<u64>(lo^static_cast<u32>(b>>32))<<32);}
  else if(op==0x13 && f==1 && imm>=0x310 && imm<=0x31a) {
    const unsigned number=imm&15;u32 value=static_cast<u32>(a>>32);if(number!=10)value=std::rotr(value,8);
    static constexpr std::array<u32,11> rcon{1,2,4,8,16,32,64,128,27,54,0};value=subword(value,h.native_fp)^rcon[number];result=value|(static_cast<u64>(value)<<32);
  } else if(op==0x13 && f==1 && imm>=0x100 && imm<=0x107) {
    const u32 w=static_cast<u32>(a);
    if(imm==0x100)result=static_cast<u64>(sign_extend(std::rotr(w,2)^std::rotr(w,13)^std::rotr(w,22),32));
    if(imm==0x101)result=static_cast<u64>(sign_extend(std::rotr(w,6)^std::rotr(w,11)^std::rotr(w,25),32));
    if(imm==0x102)result=static_cast<u64>(sign_extend(std::rotr(w,7)^std::rotr(w,18)^(w>>3),32));
    if(imm==0x103)result=static_cast<u64>(sign_extend(std::rotr(w,17)^std::rotr(w,19)^(w>>10),32));
    if(imm==0x104)result=std::rotr(a,28)^std::rotr(a,34)^std::rotr(a,39);
    if(imm==0x105)result=std::rotr(a,14)^std::rotr(a,18)^std::rotr(a,41);
    if(imm==0x106)result=std::rotr(a,1)^std::rotr(a,8)^(a>>7);
    if(imm==0x107)result=std::rotr(a,19)^std::rotr(a,61)^(a>>6);
  } else return false;
  h.set_reg(d.rd,result);return true;
}
}

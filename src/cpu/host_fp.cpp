// SPDX-License-Identifier: MIT
#include "cpu/host_fp.hpp"
#if defined(_M_X64) || defined(__x86_64__)
#include <emmintrin.h>
#endif
namespace nanorv::cpu {
namespace {thread_local HostFpScope* current_scope=nullptr;}
HostFpScope::HostFpScope() noexcept:previous_(current_scope) {current_scope=this;}
HostFpScope::~HostFpScope() {
#if defined(_M_X64) || defined(__x86_64__)
  if(initialized_)_mm_setcsr(saved_);
#endif
  current_scope=previous_;
}
void host_fp_clear_exceptions() noexcept {
#if defined(_M_X64) || defined(__x86_64__)
  if(current_scope && current_scope->initialized_)_mm_setcsr(current_scope->control_);
#endif
}
bool host_fp(unsigned fn,bool dbl,unsigned rounding,u64 a,u64 b,u64& result,u8& flags) noexcept {
#if defined(_M_X64) || defined(__x86_64__)
  if(rounding>3 || (fn!=0 && fn!=4 && fn!=8 && fn!=12 && fn!=44))return false;
  const unsigned rc=rounding==0?0:rounding==1?3:rounding==2?1:2;
  const unsigned wanted=0x1f80|(rc<<13);
  unsigned old=0;
  if(current_scope) {
    if(!current_scope->initialized_) {
      current_scope->saved_=_mm_getcsr();current_scope->initialized_=true;
      current_scope->control_=wanted;_mm_setcsr(wanted);
    } else if(current_scope->control_!=wanted) {
      const auto accumulated=_mm_getcsr()&63;
      current_scope->control_=wanted;_mm_setcsr(wanted|accumulated);
    }
  } else {old=_mm_getcsr();_mm_setcsr(wanted);}
  if(dbl) {
    const auto av=_mm_castsi128_pd(_mm_cvtsi64_si128(static_cast<i64>(a))),bv=_mm_castsi128_pd(_mm_cvtsi64_si128(static_cast<i64>(b)));
    const auto value=fn==0?_mm_add_sd(av,bv):fn==4?_mm_sub_sd(av,bv):fn==8?_mm_mul_sd(av,bv):fn==12?_mm_div_sd(av,bv):_mm_sqrt_sd(av,av);
    result=static_cast<u64>(_mm_cvtsi128_si64(_mm_castpd_si128(value)));
  } else {
    const auto av=_mm_castsi128_ps(_mm_cvtsi64_si128(static_cast<i64>(a))),bv=_mm_castsi128_ps(_mm_cvtsi64_si128(static_cast<i64>(b)));
    const auto value=fn==0?_mm_add_ss(av,bv):fn==4?_mm_sub_ss(av,bv):fn==8?_mm_mul_ss(av,bv):fn==12?_mm_div_ss(av,bv):_mm_sqrt_ss(av);
    result=static_cast<u32>(_mm_cvtsi128_si64(_mm_castps_si128(value)));
  }
  const auto exceptions=_mm_getcsr();if(!current_scope)_mm_setcsr(old);
  flags=static_cast<u8>(((exceptions&1)?16:0)|((exceptions&4)?8:0)|((exceptions&8)?4:0)|((exceptions&16)?2:0)|((exceptions&32)?1:0));
  // x86 and RISC-V differ at the rounded minimum-normal tininess boundary.
  // Keep SoftFloat authoritative there and for the RMM rounding mode.
  if((dbl?((result>>52)&2047):((result>>23)&255))==1) {
    // The result/flags will come from SoftFloat. Do not let an x86-specific
    // tininess flag from this rejected operation leak into a later operation.
    host_fp_clear_exceptions();return false;
  }
  if(dbl && (result&0x7fffffffffffffffull)>0x7ff0000000000000ull)result=0x7ff8000000000000ull;
  if(!dbl && (result&0x7fffffffull)>0x7f800000ull)result=0x7fc00000;
  return true;
#else
  (void)fn;(void)dbl;(void)rounding;(void)a;(void)b;(void)result;(void)flags;return false;
#endif
}
}

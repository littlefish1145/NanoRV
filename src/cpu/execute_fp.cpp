// SPDX-License-Identifier: MIT
#include "cpu/execute_fp.hpp"
#include "cpu/exec_util.hpp"
#include "cpu/host_fp.hpp"
extern "C" {
#include "softfloat.h"
}
namespace nanorv::cpu {
namespace {
constexpr u64 nan32 = 0x7fc00000, nan64 = 0x7ff8000000000000;
u64 operand(const Hart &h, unsigned n, bool dbl) {
  u64 v = h.f[n];
  return !dbl && (v >> 32) != 0xffffffff ? nan32
         : dbl                           ? v
                                         : static_cast<u32>(v);
}
bool isnan(u64 v, bool d) {
  return d ? (v & 0x7fffffffffffffff) > 0x7ff0000000000000
           : (v & 0x7fffffff) > 0x7f800000;
}
bool signaling(u64 v, bool d) {
  return isnan(v, d) && !(v & (d ? 1ull << 51 : 1ull << 22));
}
u64 classify(u64 v, bool d) {
  u64 sign = v >> (d ? 63 : 31), exp = (v >> (d ? 52 : 23)) & (d ? 2047 : 255),
      frac = v & (d ? ((1ull << 52) - 1) : ((1ull << 23) - 1));
  if (exp == (d ? 2047u : 255u))
    return frac ? (signaling(v, d) ? 256 : 512) : (sign ? 1 : 128);
  if (!exp)
    return frac ? (sign ? 4 : 32) : (sign ? 8 : 16);
  return sign ? 2 : 64;
}
void dirty(Hart &h) {
  h.csr.set_raw(kCsrMstatus,
                h.csr.raw(kCsrMstatus) | (3ull << 13) | (1ull << 63));
}
} // namespace
bool execute_fp(Hart &h, const memory::AddressSpace &bus, const Decoded &d,
                Trap &trap) noexcept {
  if (!(h.csr.raw(kCsrMstatus) & (3ull << 13)))
    return detail::illegal_insn(d, trap);
  bool dbl = (d.funct7 & 1) != 0;
  if (d.opcode == 7 || d.opcode == 0x27) {
    if (d.funct3 != 2 && d.funct3 != 3)
      return detail::illegal_insn(d, trap);
    dbl = d.funct3 == 3;
    unsigned width = dbl ? 64 : 32;
    i64 imm = d.opcode == 7
                  ? sign_extend(d.raw >> 20, 12)
                  : sign_extend((d.raw >> 25) << 5 | (d.raw >> 7 & 31), 12);
    u64 addr = h.get_reg(d.rs1) + static_cast<u64>(imm), v = 0;
    if (d.opcode == 7) {
      if (!detail::do_load(h, bus, addr, width, v, trap))
        return false;
      h.f[d.rd] = dbl ? v : 0xffffffff00000000 | v;
      dirty(h);
    } else if (!detail::do_store(h, bus, addr, width, h.f[d.rs2], trap))
      return false;
    return true;
  }
  if (d.opcode != 0x53) {
    unsigned fmt = (d.raw >> 25) & 3;
    if (fmt > 1)
      return detail::illegal_insn(d, trap);
    dbl = fmt == 1;
  } else if (d.funct7 & 2)
    return detail::illegal_insn(d, trap);
  unsigned fn = d.funct7 & ~1u, rm = d.funct3;
  bool rounding = d.opcode != 0x53 || fn == 0 || fn == 4 || fn == 8 ||
                  fn == 12 || fn == 44 || fn == 32 || fn == 96 || fn == 104;
  if (rounding) {
    if (rm == 7)
      rm = static_cast<unsigned>(h.csr.frm());
    if (rm > 4)
      return detail::illegal_insn(d, trap);
  }
  softfloat_roundingMode = static_cast<uint_fast8_t>(rm);
  softfloat_exceptionFlags = 0;
  softfloat_detectTininess = softfloat_tininess_afterRounding;
  u64 av = operand(h, d.rs1, dbl), bv = operand(h, d.rs2, dbl),
      cv = operand(h, d.rs3, dbl), v = 0;
  float32_t a32{static_cast<u32>(av)}, b32{static_cast<u32>(bv)};
  float64_t a64{av}, b64{bv};
  bool integer = false;
  u8 native_flags=0;
  if(h.native_fp && d.opcode==0x53 && !(fn==44 && d.rs2) && host_fp(fn,dbl,rm,av,bv,v,native_flags)) {
    h.f[d.rd]=dbl?v:0xffffffff00000000|static_cast<u32>(v);
    h.csr.set_fflags(h.csr.fflags()|native_flags);dirty(h);return true;
  }
  if (d.opcode != 0x53) {
    u64 sign = dbl ? 1ull << 63 : 1ull << 31;
    if (d.opcode == 0x4b || d.opcode == 0x4f)
      av ^= sign;
    if (d.opcode == 0x47 || d.opcode == 0x4f)
      cv ^= sign;
    v = dbl ? f64_mulAdd(float64_t{av}, b64, float64_t{cv}).v
            : f32_mulAdd(float32_t{static_cast<u32>(av)}, b32,
                         float32_t{static_cast<u32>(cv)})
                  .v;
  } else
    switch (fn) {
    case 0:
      v = dbl ? f64_add(a64, b64).v : f32_add(a32, b32).v;
      break;
    case 4:
      v = dbl ? f64_sub(a64, b64).v : f32_sub(a32, b32).v;
      break;
    case 8:
      v = dbl ? f64_mul(a64, b64).v : f32_mul(a32, b32).v;
      break;
    case 12:
      v = dbl ? f64_div(a64, b64).v : f32_div(a32, b32).v;
      break;
    case 44:
      if (d.rs2)
        return detail::illegal_insn(d, trap);
      v = dbl ? f64_sqrt(a64).v : f32_sqrt(a32).v;
      break;
    case 16: {
      if (d.funct3 > 2)
        return detail::illegal_insn(d, trap);
      u64 sign = dbl ? 1ull << 63 : 1ull << 31;
      u64 s = d.funct3 == 0 ? bv : d.funct3 == 1 ? ~bv : av ^ bv;
      v = (av & ~sign) | (s & sign);
      break;
    }
    case 20: {
      if (d.funct3 > 1)
        return detail::illegal_insn(d, trap);
      bool na = isnan(av, dbl), nb = isnan(bv, dbl);
      if (signaling(av, dbl) || signaling(bv, dbl))
        softfloat_exceptionFlags |= 16;
      if (na && nb)
        v = dbl ? nan64 : nan32;
      else if (na)
        v = bv;
      else if (nb)
        v = av;
      else if ((av & ~(dbl ? 1ull << 63 : 1ull << 31)) == 0 &&
               (bv & ~(dbl ? 1ull << 63 : 1ull << 31)) == 0)
        v = d.funct3 ? av & bv : av | bv;
      else {
        bool less = dbl ? f64_lt_quiet(a64, b64) : f32_lt_quiet(a32, b32);
        v = (less != (d.funct3 == 1)) ? av : bv;
      }
      break;
    }
    case 32:
      if (dbl) {
        if (d.rs2 != 0)
          return detail::illegal_insn(d, trap);
        v = f32_to_f64(float32_t{static_cast<u32>(operand(h, d.rs1, false))}).v;
      } else {
        if (d.rs2 != 1)
          return detail::illegal_insn(d, trap);
        v = f64_to_f32(float64_t{h.f[d.rs1]}).v;
      }
      break;
    case 80:
      integer = true;
      if (d.funct3 == 2)
        v = dbl ? f64_eq(a64, b64) : f32_eq(a32, b32);
      else if (d.funct3 == 1)
        v = dbl ? f64_lt(a64, b64) : f32_lt(a32, b32);
      else if (d.funct3 == 0)
        v = dbl ? f64_le(a64, b64) : f32_le(a32, b32);
      else
        return detail::illegal_insn(d, trap);
      break;
    case 96:
      integer = true;
      switch (d.rs2) {
      case 0:
        v = static_cast<u64>(sign_extend(
            static_cast<u32>(
                dbl ? f64_to_i32(a64, static_cast<uint_fast8_t>(rm), true)
                    : f32_to_i32(a32, static_cast<uint_fast8_t>(rm), true)),
            32));
        break;
      case 1:
        v = static_cast<u64>(sign_extend(
            dbl ? f64_to_ui32(a64, static_cast<uint_fast8_t>(rm), true)
                : f32_to_ui32(a32, static_cast<uint_fast8_t>(rm), true),
            32));
        break;
      case 2:
        v = static_cast<u64>(
            dbl ? f64_to_i64(a64, static_cast<uint_fast8_t>(rm), true)
                : f32_to_i64(a32, static_cast<uint_fast8_t>(rm), true));
        break;
      case 3:
        v = dbl ? f64_to_ui64(a64, static_cast<uint_fast8_t>(rm), true)
                : f32_to_ui64(a32, static_cast<uint_fast8_t>(rm), true);
        break;
      default:
        return detail::illegal_insn(d, trap);
      }
      break;
    case 104: {
      u64 x = h.get_reg(d.rs1);
      switch (d.rs2) {
      case 0:
        v = dbl ? i32_to_f64(static_cast<i32>(x)).v
                : i32_to_f32(static_cast<i32>(x)).v;
        break;
      case 1:
        v = dbl ? ui32_to_f64(static_cast<u32>(x)).v
                : ui32_to_f32(static_cast<u32>(x)).v;
        break;
      case 2:
        v = dbl ? i64_to_f64(static_cast<i64>(x)).v
                : i64_to_f32(static_cast<i64>(x)).v;
        break;
      case 3:
        v = dbl ? ui64_to_f64(x).v : ui64_to_f32(x).v;
        break;
      default:
        return detail::illegal_insn(d, trap);
      }
      break;
    }
    case 112:
      if (d.rs2)
        return detail::illegal_insn(d, trap);
      integer = true;
      if (d.funct3 == 0)
        v = dbl ? h.f[d.rs1] : static_cast<u64>(sign_extend(h.f[d.rs1], 32));
      else if (d.funct3 == 1)
        v = classify(av, dbl);
      else
        return detail::illegal_insn(d, trap);
      break;
    case 120:
      if (d.rs2 || d.funct3)
        return detail::illegal_insn(d, trap);
      v = h.get_reg(d.rs1);
      break;
    default:
      return detail::illegal_insn(d, trap);
    }
  if (integer)
    h.set_reg(d.rd, v);
  else
    h.f[d.rd] = dbl ? v : 0xffffffff00000000 | static_cast<u32>(v);
  h.csr.set_fflags(h.csr.fflags() | softfloat_exceptionFlags);
  dirty(h);
  return true;
}
} // namespace nanorv::cpu

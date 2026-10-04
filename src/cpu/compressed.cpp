// SPDX-License-Identifier: MIT
#include "cpu/compressed.hpp"
#include "cpu/decoder.hpp"
namespace nanorv::cpu {
namespace {
u32 i(u32 op, u32 rd, u32 f, u32 rs, i32 imm) {
  return (static_cast<u32>(imm) & 4095) << 20 | rs << 15 | f << 12 | rd << 7 |
         op;
}
u32 r(u32 op, u32 rd, u32 f, u32 rs, u32 rt, u32 top = 0) {
  return top << 25 | rt << 20 | rs << 15 | f << 12 | rd << 7 | op;
}
u32 s(u32 op, u32 f, u32 rs, u32 rt, u32 imm) {
  return (imm >> 5) << 25 | rt << 20 | rs << 15 | f << 12 | (imm & 31) << 7 |
         op;
}
u32 b(u32 f, u32 rs, i32 imm) {
  u32 n = static_cast<u32>(imm);
  return (n >> 12 & 1) << 31 | (n >> 5 & 63) << 25 | rs << 15 | f << 12 |
         (n >> 1 & 15) << 8 | (n >> 11 & 1) << 7 | 0x63;
}
u32 j(i32 imm) {
  u32 n = static_cast<u32>(imm);
  return (n >> 20 & 1) << 31 | (n >> 1 & 1023) << 21 | (n >> 11 & 1) << 20 |
         (n >> 12 & 255) << 12 | 0x6f;
}
} // namespace
u32 expand_compressed(u16 raw) noexcept {
  const u32 c = raw, q = c & 3, f = c >> 13, rd = c >> 7 & 31, rt = c >> 2 & 31;
  const u32 a = 8 + (c >> 7 & 7), d = 8 + (c >> 2 & 7);
  const i32 imm = sign_extend_32((c >> 2 & 31) | (c >> 12 & 1) << 5, 6);
  if (q == 0) {
    if (f == 0) {
      u32 n = (c >> 7 & 15) << 6 | (c >> 11 & 3) << 4 | (c >> 5 & 1) << 3 |
              (c >> 6 & 1) << 2;
      return n ? i(0x13, d, 0, 2, static_cast<i32>(n)) : 0;
    }
    const u32 w = (c >> 10 & 7) << 3 | (c >> 6 & 1) << 2 | (c >> 5 & 1) << 6;
    const u32 x = (c >> 10 & 7) << 3 | (c >> 5 & 3) << 6;
    if (f == 1)
      return i(7, d, 3, a, static_cast<i32>(x));
    if (f == 2)
      return i(3, d, 2, a, static_cast<i32>(w));
    if (f == 3)
      return i(3, d, 3, a, static_cast<i32>(x));
    if (f == 5)
      return s(0x27, 3, a, d, x);
    if (f == 6)
      return s(0x23, 2, a, d, w);
    if (f == 7)
      return s(0x23, 3, a, d, x);
  } else if (q == 1) {
    if (f == 0)
      return i(0x13, rd, 0, rd, imm);
    if (f == 1)
      return rd ? i(0x1b, rd, 0, rd, imm) : 0;
    if (f == 2)
      return i(0x13, rd, 0, 0, imm);
    if (f == 3) {
      if (rd == 2) {
        i32 n = sign_extend_32((c >> 12 & 1) << 9 | (c >> 6 & 1) << 4 |
                                   (c >> 5 & 1) << 6 | (c >> 3 & 3) << 7 |
                                   (c >> 2 & 1) << 5,
                               10);
        return n ? i(0x13, 2, 0, 2, n) : 0;
      }
      return imm ? (static_cast<u32>(imm) & 0xfffff) << 12 | rd << 7 | 0x37 : 0;
    }
    if (f == 4) {
      u32 sel = c >> 10 & 3, sh = (c >> 2 & 31) | (c >> 12 & 1) << 5;
      if (sel == 0)
        return i(0x13, a, 5, a, static_cast<i32>(sh));
      if (sel == 1)
        return i(0x13, a, 5, a, static_cast<i32>(0x400 | sh));
      if (sel == 2)
        return i(0x13, a, 7, a, imm);
      u32 sub = c >> 5 & 3;
      if (c & 0x1000) {
        if (sub > 1)
          return 0;
        return r(0x3b, a, 0, a, d, sub == 0 ? 0x20 : 0);
      }
      if (sub == 0)
        return r(0x33, a, 0, a, d, 0x20);
      return r(0x33, a, sub == 1 ? 4 : sub == 2 ? 6 : 7, a, d);
    }
    if (f == 5)
      return j(sign_extend_32((c >> 12 & 1) << 11 | (c >> 11 & 1) << 4 |
                                  (c >> 9 & 3) << 8 | (c >> 8 & 1) << 10 |
                                  (c >> 7 & 1) << 6 | (c >> 6 & 1) << 7 |
                                  (c >> 3 & 7) << 1 | (c >> 2 & 1) << 5,
                              12));
    if (f >= 6)
      return b(f == 6 ? 0 : 1, a,
               sign_extend_32((c >> 12 & 1) << 8 | (c >> 10 & 3) << 3 |
                                  (c >> 5 & 3) << 6 | (c >> 3 & 3) << 1 |
                                  (c >> 2 & 1) << 5,
                              9));
  } else if (q == 2) {
    if (f == 0)
      return i(0x13, rd, 1, rd,
               static_cast<i32>((c >> 2 & 31) | (c >> 12 & 1) << 5));
    if (f == 1 || f == 3) {
      u32 n = (c >> 12 & 1) << 5 | (c >> 5 & 3) << 3 | (c >> 2 & 7) << 6;
      return (f == 1 || rd) ? i(f == 1 ? 7 : 3, rd, 3, 2, static_cast<i32>(n))
                            : 0;
    }
    if (f == 2) {
      u32 n = (c >> 12 & 1) << 5 | (c >> 4 & 7) << 2 | (c >> 2 & 3) << 6;
      return rd ? i(3, rd, 2, 2, static_cast<i32>(n)) : 0;
    }
    if (f == 4) {
      if (!(c & 0x1000))
        return rt ? r(0x33, rd, 0, 0, rt) : (rd ? i(0x67, 0, 0, rd, 0) : 0);
      if (!rt)
        return rd ? i(0x67, 1, 0, rd, 0) : 0x00100073;
      return r(0x33, rd, 0, rd, rt);
    }
    if (f == 5 || f == 7)
      return s(f == 5 ? 0x27 : 0x23, 3, 2, rt,
               (c >> 10 & 7) << 3 | (c >> 7 & 7) << 6);
    if (f == 6)
      return s(0x23, 2, 2, rt, (c >> 9 & 15) << 2 | (c >> 7 & 3) << 6);
  }
  return 0;
}
} // namespace nanorv::cpu

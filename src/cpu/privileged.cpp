// SPDX-License-Identifier: MIT
#include "cpu/privileged.hpp"
#include "cpu/exec_util.hpp"
namespace nanorv::cpu {
bool execute_privileged(Hart &h, const Decoded &d, Trap &trap) noexcept {
  auto &c = h.csr;
  u64 m = c.raw(kCsrMstatus);
  if ((d.raw & 0xfe007fff) == 0x12000073) {
    if (h.privilege == Privilege::User ||
        (h.privilege == Privilege::Supervisor && (m & (1ull << 20))))
      return detail::illegal_insn(d, trap);
    ++h.translation_epoch;
    return true;
  }
  if (d.raw == 0x10500073) {
    if (h.privilege == Privilege::User ||
        (h.privilege == Privilege::Supervisor && (m & (1ull << 21))))
      return detail::illegal_insn(d, trap);
    h.waiting = true;
    return true;
  }
  if (d.raw == 0x30200073 && h.privilege == Privilege::Machine) {
    auto p = static_cast<Privilege>((m >> 11) & 3);
    m = (m & ~((3ull << 11) | (1ull << 3))) | ((m >> 7 & 1) << 3) | (1ull << 7);
    if (p != Privilege::Machine)
      m &= ~(1ull << 17);
    c.set_raw(kCsrMstatus, m);
    h.privilege = p;
    h.next_pc = c.raw(kCsrMepc);
    h.waiting = false;
    return true;
  }
  if (d.raw == 0x10200073 && h.privilege != Privilege::User &&
      !(h.privilege == Privilege::Supervisor && (m & (1ull << 22)))) {
    auto p = (m & (1ull << 8)) ? Privilege::Supervisor : Privilege::User;
    m = (m & ~((1ull << 8) | (1ull << 1) | (1ull << 17))) |
        ((m >> 5 & 1) << 1) | (1ull << 5);
    c.set_raw(kCsrMstatus, m);
    h.privilege = p;
    h.next_pc = c.raw(kCsrSepc);
    h.waiting = false;
    return true;
  }
  return detail::illegal_insn(d, trap);
}
void enter_trap(Hart &h, const Trap &t) noexcept {
  auto &c = h.csr;
  u64 cause = static_cast<u64>(t.cause), n = cause & 63;
  bool irq = (cause >> 63) != 0;
  bool supervisor = h.privilege != Privilege::Machine &&
                    ((c.raw(irq ? kCsrMideleg : kCsrMedeleg) >> n) & 1);
  u64 m = c.raw(kCsrMstatus), vector;
  if (supervisor) {
    c.set_raw(kCsrSepc, h.pc & ~1ull);
    c.set_raw(kCsrScause, cause);
    c.set_raw(kCsrStval, t.has_tval ? t.tval : 0);
    m = (m & ~((1ull << 1) | (1ull << 5) | (1ull << 8))) | ((m >> 1 & 1) << 5) |
        (h.privilege == Privilege::Supervisor ? 1ull << 8 : 0);
    h.privilege = Privilege::Supervisor;
    vector = c.raw(kCsrStvec);
  } else {
    c.set_raw(kCsrMepc, h.pc & ~1ull);
    c.set_raw(kCsrMcause, cause);
    c.set_raw(kCsrMtval, t.has_tval ? t.tval : 0);
    m = (m & ~((1ull << 3) | (1ull << 7) | (3ull << 11))) |
        ((m >> 3 & 1) << 7) | (static_cast<u64>(h.privilege) << 11);
    h.privilege = Privilege::Machine;
    vector = c.raw(kCsrMtvec);
  }
  c.set_raw(kCsrMstatus, m);
  h.pc = (vector & ~3ull) + (irq && (vector & 3) == 1 ? 4 * n : 0);
  h.next_pc = h.pc;
  h.clear_reservation();
  h.waiting = false;
}
bool pending_interrupt(const Hart &h, Trap &trap) noexcept {
  const auto &c = h.csr;
  const u64 p = c.raw(kCsrMip) & c.raw(kCsrMie), m = c.raw(kCsrMstatus);
  if (!p) return false;
  for (unsigned n : {11u, 3u, 7u, 9u, 1u, 5u}) {
    const u64 mask = 1ull << n;
    if (!(p & mask))
      continue;
    bool s = (c.raw(kCsrMideleg) & mask) != 0;
    bool enabled = s ? (h.privilege == Privilege::User ||
                        (h.privilege == Privilege::Supervisor && (m & 2)))
                     : (h.privilege != Privilege::Machine || (m & 8));
    if (enabled) {
      trap = {static_cast<TrapCause>((1ull << 63) | n), 0, false};
      return true;
    }
  }
  return false;
}
} // namespace nanorv::cpu

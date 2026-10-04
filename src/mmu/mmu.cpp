// SPDX-License-Identifier: MIT
#include "mmu/mmu.hpp"
#include <bit>
#include <cstring>
namespace nanorv::mmu {
using namespace cpu;
namespace {
Privilege effective(const Hart &h, memory::Access a) {
  u64 m = h.csr.raw(kCsrMstatus);
  return h.privilege == Privilege::Machine && a != memory::Access::Fetch &&
                 (m & (1ull << 17))
             ? static_cast<Privilege>((m >> 11) & 3)
             : h.privilege;
}
bool pmp(const Hart &h, u64 pa, unsigned width, memory::Access a,
         Privilege priv) {
  for (unsigned n = 0; n < 16; ++n) {
    u64 cfg = (h.csr.raw(n < 8 ? 0x3a0 : 0x3a2) >> ((n % 8) * 8)) & 255;
    unsigned mode = static_cast<unsigned>((cfg >> 3) & 3);
    if (!mode)
      continue;
    u64 lo = 0, hi = 0, addr = h.csr.raw(0x3b0 + n);
    if (mode == 1) {
      lo = n ? h.csr.raw(0x3af + n) << 2 : 0;
      hi = addr << 2;
    } else if (mode == 2) {
      lo = addr << 2;
      hi = lo + 4;
    } else {
      unsigned ones = static_cast<unsigned>(std::countr_one(addr));
      u64 mask = (1ull << (ones + 3)) - 1;
      lo = (addr << 2) & ~mask;
      hi = lo + mask + 1;
    }
    u64 end = pa + width / 8;
    if (pa < hi && end > lo) {
      if (pa < lo || end > hi)
        return false;
      if (priv == Privilege::Machine && !(cfg & 128))
        return true;
      return (cfg & (a == memory::Access::Fetch   ? 4
                     : a == memory::Access::Write ? 2
                                                  : 1)) != 0;
    }
  }
  return priv == Privilege::Machine;
}
TrapCause access_fault(memory::Access a) {
  return a == memory::Access::Fetch   ? TrapCause::InstructionAccessFault
         : a == memory::Access::Write ? TrapCause::StoreAccessFault
                                      : TrapCause::LoadAccessFault;
}
TrapCause page_fault(memory::Access a) {
  return a == memory::Access::Fetch   ? TrapCause::InstructionPageFault
         : a == memory::Access::Write ? TrapCause::StorePageFault
                                      : TrapCause::LoadPageFault;
}
cpu::Hart::Translation* lookup(Hart& h, const memory::AddressSpace& bus, u64 va,
                              unsigned width, memory::Access a) {
  if (!h.translation_cache_enabled || (width != 8 && width != 16 && width != 32 && width != 64) ||
      (va & 4095) + width / 8 > 4096) return nullptr;
  return cached_ram(h,bus,va,width,a);
}
} // namespace
bool translate(Hart &h, const memory::AddressSpace &bus, u64 va, unsigned width,
               memory::Access a, u64 &pa, Trap &trap) noexcept {
  const Privilege priv = effective(h, a);
  const u64 satp = h.csr.raw(kCsrSatp), m = h.csr.raw(kCsrMstatus);
  const u64 context = static_cast<u64>(priv) | (m & ((1ull << 18) | (1ull << 19)));
  if (const auto* cached=cached_ram(h,bus,va,width,a)) {
    pa = cached->ppn | (va & 4095);
    return true;
  }
retry_page_walk:
  pa = va;
  if (priv != Privilege::Machine && ((satp >> 60) == 8 || (satp >> 60) == 9)) {
    auto fail = [&] {
      trap = {page_fault(a), va, true};
      return false;
    };
    const int levels=(satp >> 60)==9?4:3;
    if (static_cast<u64>(sign_extend(va, static_cast<unsigned>(12+levels*9))) != va)
      return fail();
    u64 table = (satp & ((1ull << 44) - 1)) << 12;
    bool found = false;
    for (int level = levels-1; level >= 0; --level) {
      u64 pte = 0, slot = table + ((va >> (12 + level * 9)) & 511) * 8;
      if (!pmp(h, slot, 64, memory::Access::Read, Privilege::Supervisor) ||
          !bus.load(slot, 64, pte)) {
        trap = {access_fault(a), va, true};
        return false;
      }
      if (!(pte & 1) || ((pte & 6) == 4) || (pte >> 54))
        return fail();
      if (pte & 10) {
        bool user = (pte & 16) != 0;
        if (priv == Privilege::User && !user)
          return fail();
        if (priv == Privilege::Supervisor && user &&
            (a == memory::Access::Fetch || !(m & (1ull << 18))))
          return fail();
        if (a == memory::Access::Fetch && !(pte & 8))
          return fail();
        if (a == memory::Access::Write && !(pte & 4))
          return fail();
        if (a == memory::Access::Read && !(pte & 2) &&
            !((m & (1ull << 19)) && (pte & 8)))
          return fail();
        unsigned low = static_cast<unsigned>(level) * 9;
        u64 ppn = pte >> 10;
        if (ppn & ((1ull << low) - 1))
          return fail();
        u64 updated = pte | 64 | (a == memory::Access::Write ? 128 : 0);
        if (updated != pte) {
          u64 expected=pte;
          if(!pmp(h,slot,64,memory::Access::Write,Privilege::Supervisor) || !bus.compare_exchange(slot,64,expected,updated)) {
            trap={access_fault(a),va,true};return false;
          }
          // Another hart may have replaced the PTE or set its dirty bit.
          // Rewalk rather than overwriting a concurrently changed mapping.
          if(expected!=pte)goto retry_page_walk;
        }
        pa = (ppn << 12) | (va & ((1ull << (12 + low)) - 1));
        found = true;
        break;
      }
      if (pte & (16 | 64 | 128))
        return fail();
      table = (pte >> 10) << 12;
    }
    if (!found)
      return fail();
  }
  if (!pmp(h, pa, width, a, priv)) {
    trap = {access_fault(a), va, true};
    return false;
  }
  // Cache only a whole RAM page whose PMP permissions have been checked.
  // Sub-page PMP boundaries and MMIO continue through the reference path.
  auto* host=bus.direct(pa & ~4095ull,4096,a);
  if (h.translation_cache_enabled && host && pmp(h, pa & ~4095ull, 4096 * 8, a, priv)) {
    const auto access_index=static_cast<unsigned>(a);
    auto& cached=h.tlb[access_index][(va>>12)&(Hart::kTlbEntries-1)];
    if(cached.vpn!=va>>12 && cached.epoch==h.translation_epoch && cached.bus==&bus) {
      auto& replacement=h.victim_replacement[access_index];
      h.victim_tlb[access_index][replacement++%Hart::kVictimTlbEntries]=cached;
    }
    cached = {va >> 12, pa & ~4095ull, satp, context, h.translation_epoch, &bus,
              host};
    h.publish_native_translation(access_index,cached);
  }
  return true;
}
bool load(Hart &h, const memory::AddressSpace &bus, u64 va, unsigned width,
          u64 &value, Trap &trap, bool fetch) noexcept {
  auto a = fetch ? memory::Access::Fetch : memory::Access::Read;
  if (auto* entry = lookup(h,bus,va,width,a)) {
    const auto* ptr=entry->host+(va&4095);
    switch(width) {
    case 8:value=bus.read_fast<8>(ptr);break;
    case 16:value=bus.read_fast<16>(ptr);break;
    case 32:value=bus.read_fast<32>(ptr);break;
    case 64:value=bus.read_fast<64>(ptr);break;
    default:value=bus.read_host(ptr,width);break;
    }
    return true;
  }
  u64 pa;
  if ((va & 4095) + width / 8 > 4096) {
    value = 0;
    for (unsigned n = 0; n < width / 8; ++n) {
      u64 byte;
      if (!load(h, bus, va + n, 8, byte, trap, fetch))
        return false;
      value |= byte << (n * 8);
    }
    return true;
  }
  if (!translate(h, bus, va, width, a, pa, trap))
    return false;
  if (!bus.load(pa, width, value, fetch)) {
    trap = {access_fault(a), va, true};
    return false;
  }
  return true;
}
bool store(Hart &h, const memory::AddressSpace &bus, u64 va, unsigned width,
           u64 value, Trap &trap) noexcept {
  if (auto* entry = lookup(h,bus,va,width,memory::Access::Write)) {
    auto* ptr=entry->host+(va&4095);
    switch(width) {
    case 8:bus.write_fast<8>(ptr,value);break;
    case 16:bus.write_fast<16>(ptr,value);break;
    case 32:bus.write_fast<32>(ptr,value);break;
    case 64:bus.write_fast<64>(ptr,value);break;
    default:bus.write_host(ptr,width,value);break;
    }
    return true;
  }
  u64 pa;
  if ((va & 4095) + width / 8 > 4096) {
    u64 physical[8]{};
    for (unsigned n = 0; n < width / 8; ++n)
      if (!translate(h, bus, va + n, 8, memory::Access::Write, physical[n],
                     trap))
        return false;
    for (unsigned n = 0; n < width / 8; ++n)
      if (!bus.store(physical[n], 8, (value >> (8 * n)) & 255)) {
        trap = {TrapCause::StoreAccessFault, va + n, true};
        return false;
      }
    return true;
  }
  if (!translate(h, bus, va, width, memory::Access::Write, pa, trap))
    return false;
  if (!bus.store(pa, width, value)) {
    trap = {TrapCause::StoreAccessFault, va, true};
    return false;
  }
  return true;
}
} // namespace nanorv::mmu

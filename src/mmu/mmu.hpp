// SPDX-License-Identifier: MIT
#pragma once
#include "cpu/execute.hpp"
#include <utility>
namespace rvvm::mmu {
// Native chains exit at SYSTEM/FENCE/trap boundaries. Validate all data
// translation context once before entering a chain, including MPRV/SUM/MXR.
inline void prepare_native_tlb(cpu::Hart& h,const memory::AddressSpace& bus) noexcept {
  const auto status=h.csr.raw(cpu::kCsrMstatus);
  auto privilege=h.privilege;
  if(privilege==cpu::Privilege::Machine && (status&(1ull<<17)))
    privilege=static_cast<cpu::Privilege>((status>>11)&3);
  h.prepare_native_tlb(&bus,h.csr.raw(cpu::kCsrSatp),
    static_cast<u64>(privilege)|(status&((1ull<<18)|(1ull<<19))));
}
// Native C++ memory helpers inline the validated TLB-hit path. Misses retain the
// shared page walker/PMP/fault implementation, including cross-page accesses.
inline cpu::Hart::Translation* cached_ram(cpu::Hart& h,const memory::AddressSpace& bus,u64 address,unsigned width,memory::Access access) noexcept {
  if(!h.translation_cache_enabled || (address&4095)+width/8>4096)return nullptr;
  const auto status=h.csr.raw(cpu::kCsrMstatus);
  auto privilege=h.privilege;
  if(privilege==cpu::Privilege::Machine && access!=memory::Access::Fetch && (status&(1ull<<17)))privilege=static_cast<cpu::Privilege>((status>>11)&3);
  const auto context=static_cast<u64>(privilege)|(status&((1ull<<18)|(1ull<<19)));
  const auto access_index=static_cast<unsigned>(access);
  const auto satp=h.csr.raw(cpu::kCsrSatp);
  auto matches=[&](const cpu::Hart::Translation& entry) {
    return entry.vpn==address>>12 && entry.epoch==h.translation_epoch && entry.satp==satp
      && entry.context==context && entry.bus==&bus;
  };
  auto& entry=h.tlb[access_index][(address>>12)&(cpu::Hart::kTlbEntries-1)];
  if(matches(entry)) {h.publish_native_translation(access_index,entry);return &entry;}
  for(auto& victim:h.victim_tlb[access_index])if(matches(victim)) {
    std::swap(entry,victim);h.publish_native_translation(access_index,entry);return &entry;
  }
  return nullptr;
}
bool translate(cpu::Hart &, const memory::AddressSpace &, u64, unsigned,
               memory::Access, u64 &, cpu::Trap &) noexcept;
bool load(cpu::Hart &, const memory::AddressSpace &, u64, unsigned, u64 &,
          cpu::Trap &, bool fetch = false) noexcept;
bool store(cpu::Hart &, const memory::AddressSpace &, u64, unsigned, u64,
           cpu::Trap &) noexcept;
} // namespace rvvm::mmu

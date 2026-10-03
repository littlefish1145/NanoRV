// SPDX-License-Identifier: MIT
#include "devices/virt_devices.hpp"
#include "platform/platform.hpp"
#include <chrono>
#include <bit>
namespace rvvm::devices {
bool Uart::interrupt() const noexcept {
  std::lock_guard lock(mutex_);
  return (!(regs_[3] & 128)) &&
         (((regs_[1] & 1) && !rx_.empty()) || ((regs_[1] & 2) && tx_irq_));
}
void Uart::poll() {
  flush();
  while (auto byte = platform::read_input_byte()) {
    std::lock_guard lock(mutex_);
    if (rx_.size() < 4096)
      rx_.push_back(*byte);
  }
}
void Uart::flush_locked() {
  if(tx_size_) {platform::write_out({tx_.data(),tx_size_});tx_size_=0;}
}
void Uart::flush() {std::lock_guard lock(mutex_);flush_locked();}
void Uart::reset() {
  std::lock_guard lock(mutex_);flush_locked();
  regs_={};divisor_={};rx_.clear();tx_irq_=true;
}
bool Uart::load(u64 offset, unsigned width, u64 &v) noexcept {
  std::lock_guard lock(mutex_);
  if (offset >= 8 || width != 8)
    return false;
  if (regs_[3] & 128) {
    if (offset < 2) {
      v = divisor_[offset];
      return true;
    }
  }
  switch (offset) {
  case 0:
    v = rx_.empty() ? 0 : rx_.front();
    if (!rx_.empty())
      rx_.pop_front();
    break;
  case 1:
    v = regs_[1];
    break;
  case 2:
    v = 1;
    if ((regs_[1] & 1) && !rx_.empty())
      v = 4;
    else if ((regs_[1] & 2) && tx_irq_) {
      v = 2;
      tx_irq_ = false;
    }
    if (regs_[2] & 1)
      v |= 0xc0;
    break;
  case 5:
    v = 0x60 | (!rx_.empty() ? 1 : 0);
    break;
  case 6:
    v = 0xb0;
    break;
  default:
    v = regs_[offset];
    break;
  }
  return true;
}
bool Uart::store(u64 offset, unsigned width, u64 v) noexcept {
  std::lock_guard lock(mutex_);
  if (offset >= 8 || width != 8)
    return false;
  if ((regs_[3] & 128) && offset < 2) {
    divisor_[offset] = static_cast<u8>(v);
    return true;
  }
  if (offset == 0) {
    if(tx_size_==tx_.size())flush_locked();
    tx_[tx_size_++]=static_cast<char>(v);
    // Complete serial lines promptly, including fixed-workload timing marks.
    if(static_cast<u8>(v)=='\n')flush_locked();
    tx_irq_ = true;
  } else if (offset == 2) {
    regs_[2] = static_cast<u8>(v);
    if (v & 2)
      rx_.clear();
    if (v & 4)
      tx_irq_ = true;
  } else {
    regs_[offset] = static_cast<u8>(v);
    if (offset == 1 && (v & 2))
      tx_irq_ = true;
  }
  return true;
}
bool Clint::load(u64 offset, unsigned width, u64 &v) noexcept {
  u64 base = offset & ~7ull, source;
  if (offset < 32 && width == 32 && !(offset&3)) {
    v = msip[offset/4];
    return true;
  }
  if (base >= 0x4000 && base < 0x4040)
    source = compare[(base-0x4000)/8];
  else if (base == 0xbff8)
    source = time;
  else
    return false;
  if ((width != 32 && width != 64) || (offset & (width/8-1)))
    return false;
  v = source >> ((offset & 7) * 8);
  if (width == 32)
    v &= 0xffffffff;
  return true;
}
bool Clint::store(u64 offset, unsigned width, u64 v) noexcept {
  if (offset < 32 && width == 32 && !(offset&3)) {
    msip[offset/4] = static_cast<u32>(v) & 1;
    return true;
  }
  u64 base = offset & ~7ull;
  u64 *target = base >= 0x4000 && base < 0x4040 ? &compare[(base-0x4000)/8] : base == 0xbff8 ? &time : nullptr;
  if (!target || (width != 32 && width != 64) || (offset & (width/8-1)))
    return false;
  if (width == 64)
    *target = v;
  else {
    unsigned sh = static_cast<unsigned>((offset & 7) * 8);
    *target = (*target & ~(0xffffffffull << sh)) | ((v & 0xffffffff) << sh);
  }
  return true;
}
void Clint::update(cpu::Hart &h) {
  h.csr.ticks = time;
  u64 mip = h.csr.raw(cpu::kCsrMip) & ~0x88ull;
  if (msip[h.hart_id])
    mip |= 8;
  if (time >= compare[h.hart_id])
    mip |= 128;
  h.csr.set_raw(cpu::kCsrMip, mip);
}
u32 Plic::claim(unsigned ctx) {
  u32 best = 0, priority = threshold_[ctx];
  for(unsigned word=0;word<3;++word) {
    u32 bits=pending_bits_[word]&enable_[ctx][word];
    while(bits) {
      const auto bit=static_cast<unsigned>(std::countr_zero(bits));bits&=bits-1;
      const unsigned n=word*32+bit;
      if(priority_[n]>priority) {best=n;priority=priority_[n];}
    }
  }
  return best;
}
bool Plic::load(u64 o, unsigned width, u64 &v) noexcept {
  if (width != 32 || (o & 3))
    return false;
  v = 0;
  if (o < 96 * 4)
    v = priority_[o / 4];
  else if (o >= 0x1000 && o < 0x100c) {
    v=pending_bits_[(o-0x1000)/4];
  }
  else if (o >= 0x2000 && o < 0x2800) {
    unsigned ctx = static_cast<unsigned>((o - 0x2000) / 0x80),
             w = static_cast<unsigned>((o % 0x80) / 4);
    if (ctx < 16 && w < 3)
      v = enable_[ctx][w];
  } else if (o >= 0x200000 && o < 0x210000) {
    unsigned ctx = static_cast<unsigned>((o - 0x200000) / 0x1000);
    if ((o & 4095) == 0)
      v = threshold_[ctx];
    else if ((o & 4095) == 4) {
      v = claim(ctx);
      if (v) {
        pending_[v] = false;
        pending_bits_[v/32]&=~(1u<<(v%32));
        claimed_[v] = true;
      }
    }
  } else
    return false;
  return true;
}
bool Plic::store(u64 o, unsigned width, u64 v) noexcept {
  if (width != 32 || (o & 3))
    return false;
  if (o < 96 * 4)
    priority_[o / 4] = static_cast<u32>(v) & 7;
  else if (o >= 0x2000 && o < 0x2800) {
    unsigned ctx = static_cast<unsigned>((o - 0x2000) / 0x80),
             w = static_cast<unsigned>((o % 0x80) / 4);
    if (ctx < 16 && w < 3)
      enable_[ctx][w] = static_cast<u32>(v);
  } else if (o >= 0x200000 && o < 0x210000) {
    unsigned ctx = static_cast<unsigned>((o - 0x200000) / 0x1000);
    if ((o & 4095) == 0)
      threshold_[ctx] = static_cast<u32>(v) & 7;
    else if ((o & 4095) == 4 && v > 0 && v < 96 && claimed_[v]) {
      claimed_[v] = false;
      pending_[v] = level_[v];
      if(pending_[v])pending_bits_[v/32]|=1u<<(v%32);
    }
  } else
    return false;
  return true;
}
void Plic::update(cpu::Hart &h, bool uart_irq) {
  set_irq(10, uart_irq);
  u64 mip = h.csr.raw(cpu::kCsrMip) & ~0xa00ull;
  if (claim(h.hart_id*2))
    mip |= 0x800;
  if (claim(h.hart_id*2+1))
    mip |= 0x200;
  h.csr.set_raw(cpu::kCsrMip, mip);
}
void Plic::set_irq(unsigned id, bool level) {
  if (!id || id >= 96) return;
  level_[id] = level;
  if (level && !claimed_[id]) {pending_[id] = true;pending_bits_[id/32]|=1u<<(id%32);}
}
namespace {
u64 realtime_ns() { return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count()); }
}
bool GoldfishRtc::interrupt() const noexcept { return enabled_ && armed_ && realtime_ns() >= alarm_; }
bool GoldfishRtc::load(u64 o, unsigned w, u64& v) noexcept {
  if (w != 32 || (o & 3)) return false;
  v = 0;
  switch (o) {
  case 0: latched_ = realtime_ns(); v = static_cast<u32>(latched_); break;
  case 4: v = latched_ >> 32; break;
  case 8: v = static_cast<u32>(alarm_); break;
  case 12: v = alarm_ >> 32; break;
  case 16: v = enabled_; break;
  case 24: v = armed_; break;
  case 28: v = interrupt(); break;
  default: break;
  }
  return true;
}
bool GoldfishRtc::store(u64 o, unsigned w, u64 v) noexcept {
  if (w != 32 || (o & 3)) return false;
  switch (o) {
  case 8: alarm_ = (alarm_ & ~0xffffffffull) | static_cast<u32>(v); armed_ = true; break;
  case 12: alarm_ = (alarm_ & 0xffffffff) | (v << 32); break;
  case 16: enabled_ = (v & 1) != 0; break;
  case 20: case 28: armed_ = false; break;
  default: break;
  }
  return true;
}
} // namespace rvvm::devices

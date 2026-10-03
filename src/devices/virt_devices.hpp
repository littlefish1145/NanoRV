// SPDX-License-Identifier: MIT
#pragma once
#include "cpu/hart.hpp"
#include "memory/memory_region.hpp"
#include <array>
#include <deque>
#include <mutex>
namespace rvvm::devices {
class Uart final : public memory::MemoryRegion {
public:
  const char *name() const noexcept override { return "uart"; }
  u64 size() const noexcept override { return 0x1000; }
  bool load(u64, unsigned, u64 &) noexcept override;
  bool store(u64, unsigned, u64) noexcept override;
  void poll();
  void flush();
  void reset();
  bool interrupt() const noexcept;
  void inject(u8 b) {std::lock_guard lock(mutex_);rx_.push_back(b);}

private:
  mutable std::mutex mutex_;
  std::array<char,4096> tx_{};
  std::size_t tx_size_=0;
  void flush_locked();
  std::array<u8, 8> regs_{};
  std::array<u8, 2> divisor_{};
  std::deque<u8> rx_;
  bool tx_irq_ = true;
};
class Clint final : public memory::MemoryRegion {
public:
  const char *name() const noexcept override { return "clint"; }
  u64 size() const noexcept override { return 0x10000; }
  bool load(u64, unsigned, u64 &) noexcept override;
  bool store(u64, unsigned, u64) noexcept override;
  void update(cpu::Hart &);
  u64 pending(u32 hart) const noexcept {return (msip[hart]?8ull:0ull)|(time>=compare[hart]?128ull:0ull);}
  u64 time = 0;
  std::array<u64,8> compare = []{std::array<u64,8> values{};values.fill(~0ull);return values;}();
  std::array<u32,8> msip{};
};
class Plic final : public memory::MemoryRegion {
public:
  const char *name() const noexcept override { return "plic"; }
  u64 size() const noexcept override { return 0x600000; }
  bool load(u64, unsigned, u64 &) noexcept override;
  bool store(u64, unsigned, u64) noexcept override;
  void update(cpu::Hart &, bool uart_irq);
  void set_irq(unsigned id, bool level);
  u64 pending(u32 hart) {return (claim(hart*2)?0x800ull:0ull)|(claim(hart*2+1)?0x200ull:0ull);}

private:
  u32 claim(unsigned);
  std::array<u32, 96> priority_{};
  std::array<std::array<u32, 3>, 16> enable_{};
  std::array<u32, 16> threshold_{};
  std::array<bool, 96> level_{}, pending_{}, claimed_{};
  std::array<u32,3> pending_bits_{};
};
class TestDevice final : public memory::MemoryRegion {
public:
  const char *name() const noexcept override { return "test"; }
  u64 size() const noexcept override { return 0x1000; }
  bool load(u64, unsigned, u64 &value) noexcept override {
    value = 0;
    return true;
  }
  bool store(u64 offset, unsigned width, u64 value) noexcept override {
    if (offset || width != 32)
      return false;
    if ((value & 0xffff) == 0x5555 || (value & 0xffff) == 0x3333 ||
        (value & 0xffff) == 0x7777) {
      stopped = true;
      code = static_cast<u32>(value);
      return true;
    }
    return false;
  }
  bool stopped = false;
  u32 code = 0;
};
class GoldfishRtc final : public memory::MemoryRegion {
public:
  const char* name() const noexcept override { return "rtc"; }
  u64 size() const noexcept override { return 0x1000; }
  bool load(u64, unsigned, u64&) noexcept override;
  bool store(u64, unsigned, u64) noexcept override;
  bool interrupt() const noexcept;
private:
  u64 latched_ = 0, alarm_ = ~0ull;
  bool enabled_ = false, armed_ = false;
};
} // namespace rvvm::devices

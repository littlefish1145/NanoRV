// SPDX-License-Identifier: MIT
#pragma once
#include "memory/address_space.hpp"
#include "platform/network.hpp"
#include "platform/disk.hpp"
#include <array>
#include <deque>
#include <string>
#include <vector>
namespace nanorv::devices {
// VirtIO 1.x MMIO transport with split rings. DMA is restricted to guest RAM.
class VirtioMmio : public memory::MemoryRegion {
public:
  struct Descriptor { u64 address; u32 length; u16 flags, next; };
  struct Queue {
    u32 num = 0, ready = 0;
    u64 generation = 0;
    u64 desc = 0, avail = 0, used = 0;
    u16 next_avail = 0, used_index = 0;
  };
  VirtioMmio(memory::AddressSpace& bus, u32 id, unsigned queues, u64 features);
  u64 size() const noexcept override { return 0x1000; }
  bool load(u64, unsigned, u64&) noexcept override;
  bool store(u64, unsigned, u64) noexcept override;
  bool interrupt() const noexcept { return interrupt_status_ != 0; }
  bool ready() const noexcept { return (status_ & 15) == 15 && !(status_ & (64 | 128)); }
protected:
  bool take(unsigned queue, u16& head, std::vector<Descriptor>& chain);
  bool negotiated(u64 feature) const noexcept { return (driver_features_&feature)!=0; }
  void device_features(u64 features) noexcept {features_=(1ull<<32)|features;}
  void complete(unsigned queue, u16 head, u32 length);
  void fail();
  virtual void notify(unsigned) = 0;
  virtual bool config_read(u64 offset, unsigned width, u64& value) = 0;
  virtual bool config_write(u64, unsigned, u64) { return false; }
  virtual void reset_device() {}
  memory::AddressSpace& bus_;
  std::array<Queue, 2> queues_{};
private:
  u32 id_, queue_count_, device_selector_ = 0, driver_selector_ = 0, queue_selector_ = 0;
  u32 status_ = 0, interrupt_status_ = 0;
  u64 features_, driver_features_ = 0;
};
class VirtioBlock final : public VirtioMmio {
public:
  explicit VirtioBlock(memory::AddressSpace& bus);
  ~VirtioBlock() override;
  const char* name() const noexcept override { return "virtio-blk"; }
  bool open(const std::string& path, std::string& error);
  u64 capacity() const noexcept { return capacity_; }
  // Queue and DMA publication stay on the device owner; only host file I/O
  // runs on the worker. Standalone device callers retain synchronous operation.
  void set_async(bool enabled);
  void set_io_wake(std::function<void()> callback);
  void poll();
private:
  struct Request {
    unsigned queue=0;u16 head=0;u64 epoch=0,queue_generation=0,type=0,position=0;
    u8 status=0;std::vector<Descriptor> chain;std::vector<u8> data;
  };
  struct AsyncIo;
  void perform(Request&);
  void perform_batch(std::vector<Request>&,std::vector<u8>&);
  void finish(Request&);
  void reset_device() override;
  void notify(unsigned) override;
  bool config_read(u64, unsigned, u64&) override;
  platform::DiskFile disk_;
  u64 capacity_ = 0;
  std::unique_ptr<AsyncIo> async_;
  u64 io_epoch_=0;
};
class VirtioNet final : public VirtioMmio {
public:
  explicit VirtioNet(memory::AddressSpace& bus);
  const char* name() const noexcept override { return "virtio-net"; }
  bool open(std::string& error,std::span<const platform::PortForward> forwards={}) { packets_.clear();return network_.open(error,forwards); }
  void set_receive_wake(std::function<void()> callback) {network_.set_receive_wake(std::move(callback));}
  void poll();
private:
  void notify(unsigned) override;
  bool config_read(u64, unsigned, u64&) override;
  void deliver();
  platform::NatNetwork network_;
  std::deque<std::vector<u8>> packets_;
};
} // namespace nanorv::devices

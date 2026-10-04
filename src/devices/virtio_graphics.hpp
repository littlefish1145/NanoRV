// SPDX-License-Identifier: MIT
#pragma once
#include "devices/virtio.hpp"
#include "platform/display.hpp"
#include "platform/renderer.hpp"
#include <unordered_map>
#include <unordered_set>
namespace nanorv::devices {
class VirtioGpu final : public VirtioMmio {
public:
  VirtioGpu(memory::AddressSpace&,platform::Display&);
  const char* name() const noexcept override {return "virtio-gpu";}
  u64 frames() const noexcept {return frames_;}
  bool open_renderer(std::string& error,platform::HostGpu gpu=platform::HostGpu::Auto);
private:
  struct Rect {u32 x=0,y=0,w=0,h=0;};
  struct Backing {u64 address;u32 length;};
  struct Resource {u32 format,width,height;std::vector<u32> pixels;std::vector<Backing> backing;u64 bytes=0;bool rendered=false;u32 depth=1,array_size=1,last_level=0;u64 allocation=0;std::vector<std::vector<u8>> host_backing;};
  void notify(unsigned) override;
  bool config_read(u64,unsigned,u64&) override;
  bool config_write(u64,unsigned,u64) override;
  void reset_device() override;
  std::vector<u8> command(std::span<const u8>);
  bool backing_read(const Resource&,u64,std::span<u8>);
  bool sync_backing(Resource&,bool to_guest,const std::vector<std::vector<u8>>* snapshot=nullptr);
  bool submit_3d(u32 context,std::span<const u8> commands);
  bool rect_valid(const Resource&,Rect) const;
  void publish();
  platform::Display& display_;
  platform::Renderer renderer_;
  std::unordered_set<u32> contexts_;
  std::unordered_map<u32,Resource> resources_;
  std::vector<u32> scan_pixels_;
  u32 scanout_=0,cursor_=0,cursor_x_=0,cursor_y_=0,hot_x_=0,hot_y_=0;
  Rect scan_{};u64 allocated_=0,frames_=0;
};
class VirtioInput final : public VirtioMmio {
public:
  explicit VirtioInput(memory::AddressSpace&);
  const char* name() const noexcept override {return "virtio-input";}
  void push(std::span<const platform::InputEvent>);
private:
  void notify(unsigned) override;
  bool config_read(u64,unsigned,u64&) override;
  bool config_write(u64,unsigned,u64) override;
  void reset_device() override {events_.clear();select_=subsel_=0;}
  void deliver();
  u8 select_=0,subsel_=0;
  std::deque<platform::InputEvent> events_;
};
}

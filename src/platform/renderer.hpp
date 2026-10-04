// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>
#include "platform/vulkan.hpp"
namespace nanorv::platform {
struct RenderBacking {void* data;std::size_t size;};
class Renderer {
public:
  Renderer();~Renderer();
  bool open(std::string& error,HostGpu gpu=HostGpu::Auto);
  bool ready() const;
  std::string description() const;
  void reset();
  std::array<std::uint32_t,2> cap_info(std::uint32_t id);
  std::vector<std::uint8_t> caps(std::uint32_t id,std::uint32_t version);
  int create(std::array<std::uint32_t,11> args);
  void unref(std::uint32_t id);
  int attach(std::uint32_t id,std::span<const RenderBacking>);
  void detach(std::uint32_t id);
  int context(std::uint32_t id,std::string name);
  void destroy_context(std::uint32_t id);
  void context_resource(std::uint32_t ctx,std::uint32_t id,bool attach);
  int submit(std::uint32_t ctx,std::span<const std::uint8_t>);
  int transfer(std::uint32_t id,std::uint32_t ctx,std::array<std::uint32_t,6> box,std::uint64_t offset,std::uint32_t level,std::uint32_t stride,std::uint32_t layer,bool read);
  bool pixels(std::uint32_t id,unsigned width,unsigned height,std::vector<std::uint32_t>& output);
  void finish();
private:
  struct Impl;std::unique_ptr<Impl> impl_;
};
}

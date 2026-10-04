// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>
#include "platform/vulkan.hpp"
namespace nanorv::platform {
enum class DisplayBackend { Auto, Gdi, Vulkan };
struct InputEvent {std::uint16_t type=0,code=0;std::int32_t value=0;};
class Display {
public:
  Display();~Display();
  bool open(bool visible,std::string& error,DisplayBackend backend=DisplayBackend::Auto,HostGpu gpu=HostGpu::Auto);
  std::string description() const;
  void present(unsigned width,unsigned height,std::span<const std::uint32_t> pixels);
  std::vector<InputEvent> events();
  bool save(const std::string& path,std::string& error);
private:
  struct Impl;std::unique_ptr<Impl> impl_;
};
}

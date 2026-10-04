// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace nanorv::platform {
enum class HostGpu { Auto, Nvidia, Intel, Amd };
struct VulkanDevice {
  std::string name;
  std::uint32_t vendor=0,device=0,api=0;
  bool discrete=false,software=false;
};
// Windows Vulkan presentation of the guest framebuffer. This does not expose
// a Vulkan ICD or a Venus capset to the guest.
class VulkanPresenter {
public:
  VulkanPresenter();
  ~VulkanPresenter();
  bool open(void* hwnd,HostGpu,std::string& error);
  bool present(unsigned width,unsigned height,std::span<const std::uint32_t>,std::string& error);
  std::string description() const;
  static bool enumerate(std::vector<VulkanDevice>&,std::string& error);
  // Exercises host buffer/image uploads and GPU readback, checking every pixel.
  static bool self_test(HostGpu,std::string& description,std::string& error);
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}

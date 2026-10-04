// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>
#include <functional>
namespace nanorv::platform {
struct PortForward {std::uint16_t host=0,guest=0;};
class NatNetwork {
public:
  NatNetwork();
  ~NatNetwork();
  bool open(std::string& error,std::span<const PortForward> forwards={});
  void send(std::span<const std::uint8_t> frame);
  void poll();
  bool receive(std::vector<std::uint8_t>& frame);
  // Replacing/clearing this callback waits for any in-flight invocation.
  void set_receive_wake(std::function<void()> callback);
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}

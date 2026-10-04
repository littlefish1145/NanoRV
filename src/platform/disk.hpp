// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <memory>
#include <span>
#include <string>
namespace nanorv::platform {
class DiskFile {
public:
  DiskFile();
  ~DiskFile();
  bool open(const std::string&,std::string& error);
  std::uint64_t size() const noexcept;
  bool read(std::uint64_t,std::span<std::uint8_t>);
  bool write(std::uint64_t,std::span<const std::uint8_t>);
  bool flush();
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}

// SPDX-License-Identifier: MIT
#pragma once
#include "core/types.hpp"
namespace rvvm::cpu {
bool host_fp(unsigned fn,bool dbl,unsigned rounding,u64 a,u64 b,u64& result,u8& flags) noexcept;
void host_fp_clear_exceptions() noexcept;
// Lazy per-native-chain floating point environment. Integer-only chains never
// touch MXCSR; floating point chains restore the caller's exact environment.
class HostFpScope {
public:
  HostFpScope() noexcept;
  ~HostFpScope();
  HostFpScope(const HostFpScope&)=delete;
  HostFpScope& operator=(const HostFpScope&)=delete;
private:
  HostFpScope* previous_=nullptr;
  unsigned saved_=0,control_=0;
  bool initialized_=false;
  friend bool host_fp(unsigned,bool,unsigned,u64,u64,u64&,u8&) noexcept;
  friend void host_fp_clear_exceptions() noexcept;
};
}

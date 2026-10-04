// SPDX-License-Identifier: MIT
#pragma once
#include "core/types.hpp"
#include <algorithm>
#include <span>
#include <vector>

namespace nanorv::machine {
// Parallel CPUs share one clock. Parked CPUs must not slow a busy CPU's time;
// sample deltas also avoid stalling when execution migrates to another hart.
class ParallelClock {
  std::vector<u64> previous_;
  u64 ticks_,fraction_=0;
public:
  ParallelClock(u32 harts,u64 ticks):previous_(harts),ticks_(ticks) {}
  u64 advance(std::span<const u64> attempts,bool all_idle) {
    u64 busiest=0;
    for(std::size_t n=0;n<previous_.size();++n) {
      busiest=std::max(busiest,attempts[n]-previous_[n]);previous_[n]=attempts[n];
    }
    const u64 progress=busiest+fraction_;
    ticks_+=progress/10;fraction_=progress%10;
    if(all_idle)ticks_+=10000; // one 1 ms idle wait at CLINT's 10 MHz
    return ticks_;
  }
};
}

// SPDX-License-Identifier: MIT
#pragma once
#include <atomic>
#include <thread>
#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#endif
namespace nanorv::memory {
// Bounded RAM critical sections. Atomic CPU operations call the already-locked
// write primitive, so ordinary stores need neither TLS ownership nor recursion.
class SpinMutex {
  std::atomic<unsigned> locked_{0};
public:
  void lock() noexcept {
    unsigned spins=0;
    for(;;) {
      unsigned empty=0;
      if(locked_.load(std::memory_order_relaxed)==0 &&
         locked_.compare_exchange_weak(empty,1,std::memory_order_acquire,std::memory_order_relaxed))return;
#if defined(_M_X64) || defined(__x86_64__)
      _mm_pause();
#endif
      if(++spins==64) {spins=0;std::this_thread::yield();}
    }
  }
  void unlock() noexcept {locked_.store(0,std::memory_order_release);}
};
}

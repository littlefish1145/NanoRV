// RVVM configuration for Berkeley SoftFloat 3e (see COPYING.txt).
#pragma once
#define LITTLEENDIAN 1
#define INLINE static inline
#ifndef THREAD_LOCAL
#define THREAD_LOCAL __declspec(thread)
#endif
#define SOFTFLOAT_FAST_INT64
#define SOFTFLOAT_ROUND_ODD
#define INLINE_LEVEL 5
#define SOFTFLOAT_FAST_DIV32TO16
#define SOFTFLOAT_FAST_DIV64TO32

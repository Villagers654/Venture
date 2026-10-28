#pragma once
#include <atomic>
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#endif

namespace venture::test {
// Only the synthetic test's partner polls; guest waits use the OS primitive.
inline void wait_pause() noexcept {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
  _mm_pause();
#elif defined(__aarch64__)
  asm volatile("yield");
#else
  std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}
}

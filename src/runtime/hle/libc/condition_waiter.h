#pragma once
#include <atomic>
#include <cstdint>

namespace venture::hle::libc::detail {
// A64 condition registration. The optional callback is a compile-time seam for
// the register/sequence-load interleaving test; normal callers instantiate an
// empty callback, with no runtime hook or additional branch.
class ConditionWaiter {
  std::uint32_t& waiters_;
 public:
  std::uint32_t sequence;
  template<class Registered = decltype([]() noexcept {})>
  ConditionWaiter(std::uint32_t& word, std::uint32_t& waiters, Registered registered = {}) noexcept
      : waiters_(waiters) {
    static_assert(noexcept(registered()));
    std::atomic_ref(waiters_).fetch_add(1);
    registered();
    sequence = std::atomic_ref(word).load(std::memory_order_acquire);
  }
  ~ConditionWaiter() { std::atomic_ref(waiters_).fetch_sub(1); }
  ConditionWaiter(const ConditionWaiter&) = delete;
  ConditionWaiter& operator=(const ConditionWaiter&) = delete;
};
} // namespace venture::hle::libc::detail

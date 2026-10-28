#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include "core/guest.h"
#include "common/startup_progress.h"

namespace venture { class GuestThread; class Cpu; }
namespace venture::startup_diagnostics {
// Positive seconds enables the probe; unset/zero leaves all recording off.
struct ThreadSample {
  GuestAddr pc = 0, lr = 0, sp = 0;
  std::uint32_t hle = 0;
  std::int64_t sampled_ns = 0;
  bool pending = false;
};
struct ThreadState { std::mutex mutex; ThreadSample sample; };
void poll(); // host monitor only; emits exactly one snapshot
void set_sources(std::function<void()>); // set before app monitor, clear after it joins
void sample(GuestThread&, const Cpu&);
std::uint32_t enter_hle(GuestThread&, const Cpu&, std::uint32_t);
void leave_hle(GuestThread&, const Cpu&, std::uint32_t);
class HleScope {
  GuestThread* thread_ = nullptr;
  const Cpu* cpu_ = nullptr;
  std::uint32_t previous_ = 0;
 public:
  HleScope(GuestThread& thread, const Cpu& cpu, std::uint32_t id) {
    if (enabled) { thread_ = &thread; cpu_ = &cpu; previous_ = enter_hle(thread, cpu, id); }
  }
  ~HleScope() { if (thread_) leave_hle(*thread_, *cpu_, previous_); }
};
}

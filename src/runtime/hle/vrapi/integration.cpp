#include "hle/vrapi/integration.h"
#include <algorithm>
#include <map>
#include <mutex>
#include <thread>
namespace venture::hle::vrapi::integration {
namespace {
std::mutex mutex;
bool initialized = false;
std::map<std::uint32_t, std::int32_t> performance;
} // namespace
std::int32_t initialize(const Init *i) {
  std::lock_guard lock(mutex);
  // First Contact's 1.21 loader compares minor=21 at Thumb VA 0x9f8;
  // Vader uses 1.16. Both have the same four version words + ovrJava ABI.
  if (!i || i->product != 1 || i->major != 1 || (i->minor != 16 && i->minor != 21) || i->patch != 0 || !i->java.vm ||
      !i->java.env || !i->java.activity)
    return abi::invalid;
  initialized = true;
  return 0;
}
void shutdown() {
  std::lock_guard lock(mutex);
  performance.clear();
  initialized = false;
}
bool has_initialized() {
  std::lock_guard lock(mutex);
  return initialized;
}
std::int32_t clear_state(std::uint32_t) {
  std::lock_guard lock(mutex);
  if (!initialized)
    return abi::not_initialized;
  performance.clear();
  return 0;
}
std::int32_t set_thread_performance(std::uint32_t tid, std::int32_t level) {
  std::lock_guard lock(mutex);
  if (!initialized)
    return abi::not_initialized;
  if (!tid || level < 0 || level > 2)
    return abi::invalid;
  performance[tid] = level;
  return 0; // advisory, host scheduler owns actual priority
}
std::int32_t auto_thread_scheduling(std::uint32_t count, const std::uint32_t *tids,
                                    const std::int32_t *levels, std::int32_t mode) {
  std::lock_guard lock(mutex);
  if (!initialized)
    return abi::not_initialized;
  if (count > 4096 || (count && (!tids || !levels)) || mode < 0)
    return abi::invalid;
  for (std::uint32_t i = 0; i < count; ++i)
    if (!tids[i] || levels[i] < 0 || levels[i] > 2)
      return abi::invalid;
  for (std::uint32_t i = 0; i < count; ++i)
    performance[tids[i]] = levels[i];
  return 0;
}
std::int32_t usable_cores() {
  return std::int32_t(std::max(1u, std::thread::hardware_concurrency()));
}
} // namespace venture::hle::vrapi::integration

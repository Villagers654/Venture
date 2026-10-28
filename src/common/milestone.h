// Startup milestones raised by code shared with the launcher (Platform
// services). The runtime profiler installs the hook when profiling is enabled;
// everywhere else marks are no-ops and need no link dependency.
#pragma once
#include <atomic>
#include <string_view>

namespace venture::milestone {
using Hook = void (*)(std::string_view);
inline std::atomic<Hook> hook{nullptr};
inline void mark(std::string_view name) {
  if (auto h = hook.load(std::memory_order_acquire)) h(name);
}
}  // namespace venture::milestone

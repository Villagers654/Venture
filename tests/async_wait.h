#pragma once

#include <chrono>
#include <cstdio>
#include <source_location>
#include <thread>

namespace venture::test {
// These waits guard test hangs, not product latency. Keep semantic timeout and
// performance assertions at their call sites, independent of this watchdog.
inline constexpr auto hang_timeout = std::chrono::seconds(120);
inline constexpr auto hang_timeout_ns =
    std::chrono::duration_cast<std::chrono::nanoseconds>(hang_timeout).count();
inline constexpr auto hang_timeout_ms =
    std::chrono::duration_cast<std::chrono::milliseconds>(hang_timeout).count();

template <typename Predicate>
bool wait_until(Predicate ready,
                std::source_location location = std::source_location::current()) {
  const auto started = std::chrono::steady_clock::now();
  const auto deadline = started + hang_timeout;
  bool completed = true;
  while (!ready()) {
    if (std::chrono::steady_clock::now() >= deadline) { completed = false; break; }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  std::fprintf(stderr, "[fixture-wait] %s:%u: %.6fs %s\n", location.file_name(),
               location.line(), elapsed, completed ? "ready" : "no progress");
  return completed;
}
} // namespace venture::test

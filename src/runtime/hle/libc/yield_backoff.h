#pragma once
#include <cstdio>
#include <utility>

namespace venture::hle::libc {
// VENTURE_YIELD_BACKOFF=N:US: a thread's yields after N back-to-back ones (< 200 us apart) sleep US microseconds.
// Default 16:50; "0" (or any value without a nonzero US) disables it; an unparsable value keeps the default.
inline std::pair<unsigned, unsigned> yield_backoff_setting(const char* value) {
  constexpr std::pair<unsigned, unsigned> standard{16, 50};
  if (!value || !*value) return standard;
  unsigned streak = 0, micros = 0;
  const int fields = std::sscanf(value, "%u:%u", &streak, &micros);
  if (fields == 2) return micros && streak ? std::pair{streak, micros} : std::pair{0u, 0u};
  if (fields == 1 && !streak) return {0, 0};
  return standard;
}
}  // namespace venture::hle::libc

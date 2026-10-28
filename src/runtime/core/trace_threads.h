#pragma once
#include "common/environment.h"
#include <charconv>
#include <cstdint>
#include <string_view>

namespace venture {
// Developer traces only. Call after the trace-enable branch so an ordinary
// runtime does not even consult this optional filter. IDs live in run configs.
inline bool trace_guest_tid(std::int32_t tid) {
  static const char* filter = environment::getenv("VENTURE_TRACE_GUEST_TIDS");
  if (!filter) return true;
  std::string_view remaining(filter);
  while (!remaining.empty()) {
    auto comma = remaining.find(',');
    auto token = remaining.substr(0, comma);
    std::int32_t selected{};
    auto result = std::from_chars(token.data(), token.data() + token.size(), selected);
    if (result.ec == std::errc{} && result.ptr == token.data() + token.size() && selected == tid) return true;
    if (comma == remaining.npos) break;
    remaining.remove_prefix(comma + 1);
  }
  return false;
}
} // namespace venture

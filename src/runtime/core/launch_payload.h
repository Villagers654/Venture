#pragma once
// The cold launch intent the supervisor hands a restarted worker
// (com.venture.runtime.AppMain.takePendingRelaunchPayload): flags as a Java
// int, the activity's class name and the action as Java modified UTF-8 strings
// (empty for the launch activity and no action), then the string extras. The
// worker reads it once; the process setup looks at the activity to choose the
// framework (2D or VR) before Java takes the whole intent.
#include "platform/runtime_child.h"
#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace venture::launch_payload {
namespace detail {
inline std::mutex mutex;
inline std::optional<std::string> payload;
inline std::string& read() {
  if (!payload) payload = platform::take_runtime_launch_payload();
  return *payload;
}
} // namespace detail

/** The payload, read on first use and kept until take. */
inline std::string peek() {
  std::lock_guard lock(detail::mutex);
  return detail::read();
}

/** The payload, once: later calls see none. */
inline std::string take() {
  std::lock_guard lock(detail::mutex);
  return std::exchange(detail::read(), std::string());
}

/** The activity class a payload names; empty for the launch activity or a malformed payload. */
inline std::string activity(std::string_view payload) {
  if (payload.size() < 6) return {};
  const auto length = static_cast<std::size_t>(static_cast<unsigned char>(payload[4])) << 8 |
                      static_cast<unsigned char>(payload[5]);
  if (payload.size() - 6 < length) return {};
  return std::string(payload.substr(6, length));
}
} // namespace venture::launch_payload

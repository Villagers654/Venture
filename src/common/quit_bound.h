#pragma once
#include <charconv>
#include <chrono>
#include <string_view>
#include "common/environment.h"

namespace venture {
// A configurable host backstop, not a claimed Horizon OS deadline. Keep a
// finite positive range and share parsing between worker and supervisor.
inline std::chrono::milliseconds quit_backstop(const char* value) {
  if (value) {
    const std::string_view text(value);
    unsigned ms{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), ms);
    if (error == std::errc{} && end == text.data() + text.size() && ms >= 100 && ms <= 300000)
      return std::chrono::milliseconds(ms);
  }
  return std::chrono::seconds(5);
}
inline std::chrono::milliseconds menu_quit_backstop(const char* menu, const char* value) {
  return quit_backstop(!menu || std::string_view(menu) != "0" ? value : nullptr);
}
// While a file save is an application's only remaining work (a slow disk's
// fsync), the quit window extends to this bound after posting, so the save
// completes rather than being cut off.
inline constexpr std::chrono::seconds save_quit_bound{30};
// Once destroy is delivered the worker allows one more 8 s settle bound.
inline constexpr std::chrono::seconds destroy_settle_bound{8};
inline std::chrono::milliseconds configured_quit_backstop() {
  return menu_quit_backstop(environment::getenv("VENTURE_MENU_QUIT"), environment::getenv("VENTURE_QUIT_BACKSTOP_MS"));
}
}

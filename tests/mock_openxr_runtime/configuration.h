#pragma once
#include <charconv>
#include <cstdint>
#include <stdexcept>
#include <string_view>

namespace mock {
struct ViewConfiguration {
  std::string_view profile = "default";
  std::uint32_t width = 1024, height = 1024;
  std::int64_t period_ns = 11'111'111;
  double budget_ms = 1000.0 / 90;
};
inline ViewConfiguration view_configuration(std::string_view profile = {},
    std::string_view square = {}, std::string_view width = {}, std::string_view height = {},
    std::string_view refresh_hz = {}) {
  ViewConfiguration result;
  if (profile == "pcvr") result = {"pcvr", 2800, 2856, 6'944'444, 7.0};
  else if (profile == "legacy512") result = {"legacy512", 512, 512, 11'111'111, 7.0};
  else if (!profile.empty() && profile != "default")
    throw std::invalid_argument("unknown VENTURE_MOCK_PROFILE");
  const auto dimension = [](std::string_view text) {
    std::uint32_t value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value < 64 || value > 4096)
      throw std::invalid_argument("mock view dimension must be an integer in [64, 4096]");
    return value;
  };
  if (!square.empty()) result.width = result.height = dimension(square);
  if (!width.empty()) result.width = dimension(width);
  if (!height.empty()) result.height = dimension(height);
  // A display rate other than the profile's, as a host runtime reports (Monado: 120 Hz).
  if (!refresh_hz.empty()) {
    std::uint32_t hz{};
    const auto parsed = std::from_chars(refresh_hz.data(), refresh_hz.data() + refresh_hz.size(), hz);
    if (parsed.ec != std::errc{} || parsed.ptr != refresh_hz.data() + refresh_hz.size() || hz < 30 || hz > 240)
      throw std::invalid_argument("VENTURE_MOCK_REFRESH_HZ must be an integer in [30, 240]");
    result.period_ns = (1'000'000'000 + hz / 2) / hz;
    result.budget_ms = 1000.0 / hz;
  }
  return result;
}
}

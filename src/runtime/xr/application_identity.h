#pragma once
#include <openxr/openxr.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>
namespace venture::xr {
template <std::size_t N> inline void identity_name(char (&out)[N], std::string_view text) {
  auto count = std::min(N - 1, text.find('\0'));
  count = std::min(count, text.size());
  // Never cut a UTF-8 code point in the fixed-size OpenXR string.
  if (count < text.size())
    while (count && (static_cast<unsigned char>(text[count]) & 0xc0) == 0x80) --count;
  if (count) std::memcpy(out, text.data(), count);
  out[count] = 0;
}
inline XrApplicationInfo application_identity(std::string_view label, std::string_view package,
                                              std::int64_t version) {
  XrApplicationInfo info{};
  identity_name(info.applicationName, !label.empty() ? label : (!package.empty() ? package : "Venture"));
  if (!info.applicationName[0]) identity_name(info.applicationName, !package.empty() ? package : "Venture");
  identity_name(info.engineName, !package.empty() ? package : "Venture");
  info.applicationVersion = static_cast<std::uint32_t>(std::clamp<std::int64_t>(version, 0,
                                  std::numeric_limits<std::uint32_t>::max()));
  info.apiVersion = XR_MAKE_VERSION(1, 0, 0);
  return info;
}
} // namespace venture::xr

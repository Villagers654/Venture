#pragma once
#include <optional>
#include <string>
#include <string_view>
namespace venture::xr {
// Empty result means the profile has no corresponding physical input. Never
// steal runtime system/menu input or pretend advanced Touch sensors exist.
std::optional<std::string> touch_binding(std::string_view path, std::string_view profile);
bool is_touch_profile(std::string_view profile);
} // namespace venture::xr

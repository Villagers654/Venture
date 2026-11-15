// Vulkan's two-call enumerations (count, then fill) when the count changes
// between the calls.
//
// A hybrid laptop's loader can report one physical device count and then a
// different one: a driver adds or drops a device (discrete GPU power state,
// the loader's DXGI adapter sorting, a driver answering differently between
// calls). The fill call then returns VK_INCOMPLETE, which is not an error: the
// array still holds valid handles. ANGLE and Venture's adapter probe treated it
// as failure, and a Radeon Vega 8 + GTX 1650 laptop under SteamVR failed every
// launch with "A return array was too small for the result" and "no adapters".
//
// enumerate_vulkan repeats count+fill while the fill returns VK_INCOMPLETE, a
// bounded number of times, and then accepts the entries the last fill wrote.
#pragma once

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace venture::gfx {

inline constexpr int vulkan_enumeration_attempts = 4;

// query(std::uint32_t* count, T* out) -> VkResult wraps one vkEnumerate* call.
// Returns VK_SUCCESS with the complete list, VK_INCOMPLETE with the entries the
// last attempt returned (usable: the list kept changing), or a negative error
// with out empty. blank initializes each entry (sType for output structures).
template <class T, class Query>
VkResult enumerate_vulkan(std::vector<T>& out, Query&& query, const T& blank = T{}) {
  for (int attempt = 0; attempt < vulkan_enumeration_attempts; ++attempt) {
    std::uint32_t count = 0;
    auto result = query(&count, static_cast<T*>(nullptr));
    if (result < 0) {
      out.clear();
      return result;
    }
    out.assign(count, blank);
    if (!count) return VK_SUCCESS;
    result = query(&count, out.data());
    if (result < 0) {
      out.clear();
      return result;
    }
    out.resize(std::min<std::size_t>(count, out.size()));
    if (result != VK_INCOMPLETE) return VK_SUCCESS;
  }
  return VK_INCOMPLETE;
}

// The callee half of a two-call enumeration over a list already gathered.
template <class T>
VkResult fill_vulkan_enumeration(const std::vector<T>& values, std::uint32_t* count, T* out) {
  if (!count) return VK_ERROR_UNKNOWN;
  if (!out) {
    *count = static_cast<std::uint32_t>(values.size());
    return VK_SUCCESS;
  }
  const auto n = std::min(*count, static_cast<std::uint32_t>(values.size()));
  std::copy_n(values.begin(), n, out);
  *count = n;
  return n < values.size() ? VK_INCOMPLETE : VK_SUCCESS;
}

}  // namespace venture::gfx

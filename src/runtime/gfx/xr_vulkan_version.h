// Which Vulkan apiVersion the shared instance asks for under an XR runtime's
// XR_KHR_vulkan_enable2 graphics requirements.
//
// The spec makes only the minimum binding: maxApiVersionSupported is the
// newest version the runtime "has been tested on", and newer versions might
// work. Refusing a request above it (VK_ERROR_INCOMPATIBLE_DRIVER before the
// runtime is even asked) stopped every launch on a runtime reporting a low or
// malformed maximum, while engines that ignore the maximum ran. A request above
// the maximum is tried as asked; if the runtime then refuses the instance as
// incompatible, one retry at the tested maximum follows when that still meets
// the minimum and the caller's floor.
#pragma once

#include <algorithm>
#include <cstdint>
#include <format>
#include <string>

namespace venture::gfx {

struct XrVulkanVersionPlan {
  std::uint32_t api_version = 0;   // VkApplicationInfo::apiVersion to request first
  std::uint32_t retry_version = 0; // nonzero: retry with it after VK_ERROR_INCOMPATIBLE_DRIVER
  std::uint32_t min_version = 0;   // runtime minimum as a Vulkan version, 0 when unusable
  std::uint32_t max_version = 0;   // runtime tested maximum as a Vulkan version, 0 when unusable
  bool above_tested_max = false;   // api_version is newer than the runtime's tested maximum
};

// Vulkan major.minor of an XrVersion (major << 48 | minor << 32 | patch); 0 when
// it names no Vulkan 1.x+ version (major 0, e.g. a Vulkan-packed value).
constexpr std::uint32_t xr_to_vulkan_version(std::uint64_t xr) {
  const auto major = static_cast<std::uint32_t>(xr >> 48);
  const auto minor = static_cast<std::uint32_t>((xr >> 32) & 0xffffu);
  if (major == 0 || major > 0x7fu || minor > 0x3ffu) return 0;
  return (major << 22) | (minor << 12);
}

// Major.minor only: patch levels never decide compatibility.
constexpr std::uint32_t vulkan_major_minor(std::uint32_t v) { return v & ~0xfffu & ~(0x7u << 29); }

inline XrVulkanVersionPlan plan_xr_vulkan_version(std::uint32_t requested, std::uint64_t xr_min,
                                                  std::uint64_t xr_max, std::uint32_t floor) {
  constexpr std::uint32_t vulkan_1_0 = 1u << 22;
  XrVulkanVersionPlan plan;
  plan.min_version = xr_to_vulkan_version(xr_min);
  plan.max_version = xr_to_vulkan_version(xr_max);
  // A maximum below the minimum is inconsistent; only the minimum is binding.
  if (plan.max_version && plan.min_version && plan.max_version < plan.min_version) plan.max_version = 0;
  plan.api_version = requested ? requested : vulkan_1_0;
  if (plan.min_version && vulkan_major_minor(plan.api_version) < plan.min_version) plan.api_version = plan.min_version;
  if (plan.max_version && vulkan_major_minor(plan.api_version) > plan.max_version) {
    plan.above_tested_max = true;
    const std::uint32_t lowest = std::max(vulkan_major_minor(floor), plan.min_version);
    if (plan.max_version >= lowest && plan.max_version >= vulkan_1_0) plan.retry_version = plan.max_version;
  }
  return plan;
}

inline std::string vulkan_version_text(std::uint32_t v) {
  if (!v) return "unknown";
  return std::format("{}.{}", (v >> 22) & 0x7fu, (v >> 12) & 0x3ffu);
}

// "1.1.0", or the raw value when it is no XrVersion of a Vulkan 1.x+ version.
inline std::string xr_version_text(std::uint64_t xr) {
  if (!xr_to_vulkan_version(xr)) return std::format("0x{:x}", xr);
  return std::format("{}.{}.{}", xr >> 48, (xr >> 32) & 0xffffu, xr & 0xffffffffu);
}

}  // namespace venture::gfx

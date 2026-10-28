#pragma once
#include "xr/api.h"
#include "common/log.h"
#include <cmath>

namespace venture::xr {
// A64 Android wire layout of XrSwapchainStateSamplerOpenGLESFB. Keep EGL
// native handle declarations out of the Vulkan-only shared XR headers.
struct SamplerOpenGLES {
  XrStructureType type = XR_TYPE_SWAPCHAIN_STATE_SAMPLER_OPENGL_ES_FB;
  void* next = nullptr;
  std::uint32_t minFilter = 0x2601, magFilter = 0x2601;
  std::uint32_t wrapModeS = 0x812f, wrapModeT = 0x812f;
  std::uint32_t swizzleRed = 0x1903, swizzleGreen = 0x1904,
                swizzleBlue = 0x1905, swizzleAlpha = 0x1906;
  float maxAnisotropy = 1;
  XrColor4f borderColor{};
};
static_assert(sizeof(SamplerOpenGLES) == 72 && offsetof(SamplerOpenGLES, borderColor) == 52);
inline XrSwapchainStateSamplerVulkanFB default_sampler() {
  XrSwapchainStateSamplerVulkanFB s{XR_TYPE_SWAPCHAIN_STATE_SAMPLER_VULKAN_FB};
  s.minFilter = s.magFilter = VK_FILTER_LINEAR;
  s.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  s.wrapModeS = s.wrapModeT = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  s.maxAnisotropy = 1;
  return s;
}
inline bool valid_sampler(const XrSwapchainStateSamplerVulkanFB& s) {
  auto filter = [](VkFilter f) { return f == VK_FILTER_NEAREST || f == VK_FILTER_LINEAR || f == VK_FILTER_CUBIC_EXT; };
  auto wrap = [](VkSamplerAddressMode w) { return w >= VK_SAMPLER_ADDRESS_MODE_REPEAT && w <= VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE; };
  auto swizzle = [](VkComponentSwizzle c) { return c >= VK_COMPONENT_SWIZZLE_IDENTITY && c <= VK_COMPONENT_SWIZZLE_A; };
  return !s.next && filter(s.minFilter) && filter(s.magFilter) &&
      (s.mipmapMode == VK_SAMPLER_MIPMAP_MODE_NEAREST || s.mipmapMode == VK_SAMPLER_MIPMAP_MODE_LINEAR) &&
      wrap(s.wrapModeS) && wrap(s.wrapModeT) && swizzle(s.swizzleRed) && swizzle(s.swizzleGreen) &&
      swizzle(s.swizzleBlue) && swizzle(s.swizzleAlpha) &&
      std::isfinite(s.maxAnisotropy) && s.maxAnisotropy >= 1 &&
      std::isfinite(s.borderColor.r) && std::isfinite(s.borderColor.g) &&
      std::isfinite(s.borderColor.b) && std::isfinite(s.borderColor.a);
}
inline bool gles_sampler(const SamplerOpenGLES& g, XrSwapchainStateSamplerVulkanFB& s) {
  s = default_sampler();
  switch (g.minFilter) {
  case 0x2600: case 0x2700: case 0x2702: s.minFilter = VK_FILTER_NEAREST; break;
  case 0x2601: case 0x2701: case 0x2703: s.minFilter = VK_FILTER_LINEAR; break;
  case 0x9139: case 0x913a: case 0x913b: s.minFilter = VK_FILTER_CUBIC_EXT; break;
  default: return false;
  }
  if (g.magFilter != 0x2600 && g.magFilter != 0x2601 && g.magFilter != 0x9139) return false;
  s.magFilter = g.magFilter == 0x9139 ? VK_FILTER_CUBIC_EXT :
      (g.magFilter == 0x2600 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
  s.mipmapMode = g.minFilter == 0x2702 || g.minFilter == 0x2703 || g.minFilter == 0x913b ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
  auto wrap = [](std::uint32_t gl, VkSamplerAddressMode& vk) {
    switch (gl) {
    case 0x2901: vk = VK_SAMPLER_ADDRESS_MODE_REPEAT; return true;
    case 0x8370: vk = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT; return true;
    case 0x812f: vk = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; return true;
    case 0x812d: vk = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER; return true;
    default: return false;
    }
  };
  auto swizzle = [](std::uint32_t gl, VkComponentSwizzle& vk) {
    switch (gl) {
    case 0: vk = VK_COMPONENT_SWIZZLE_ZERO; return true;
    case 1: vk = VK_COMPONENT_SWIZZLE_ONE; return true;
    case 0x1903: vk = VK_COMPONENT_SWIZZLE_R; return true;
    case 0x1904: vk = VK_COMPONENT_SWIZZLE_G; return true;
    case 0x1905: vk = VK_COMPONENT_SWIZZLE_B; return true;
    case 0x1906: vk = VK_COMPONENT_SWIZZLE_A; return true;
    default: return false;
    }
  };
  s.maxAnisotropy = g.maxAnisotropy;
  s.borderColor = g.borderColor;
  return !g.next && wrap(g.wrapModeS, s.wrapModeS) && wrap(g.wrapModeT, s.wrapModeT) &&
      swizzle(g.swizzleRed, s.swizzleRed) && swizzle(g.swizzleGreen, s.swizzleGreen) &&
      swizzle(g.swizzleBlue, s.swizzleBlue) && swizzle(g.swizzleAlpha, s.swizzleAlpha) && valid_sampler(s);
}
inline XrResult update_host_sampler(XrSwapchain chain, const XrSwapchainStateSamplerVulkanFB& s) {
  const auto& extensions = host_xr().enabled_extensions;
  if (!extensions.contains("XR_FB_swapchain_update_state") ||
      !extensions.contains("XR_FB_swapchain_update_state_vulkan")) {
    static std::once_flag warning;
    std::call_once(warning, [] {
      log::warn("xr", "Host compositor uses its own filtering; swapchain sampler hints are retained");
    });
    return XR_SUCCESS;
  }
  return host_call<PFN_xrUpdateSwapchainFB, "xrUpdateSwapchainFB">(chain,
      reinterpret_cast<const XrSwapchainStateBaseHeaderFB*>(&s));
}
} // namespace venture::xr

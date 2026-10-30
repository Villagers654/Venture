#pragma once
#include "hle/openxr/state.h"
#include <array>
#include <functional>

namespace venture::test::passthrough {
inline bool configured = true, grant = false, requested = false;
inline unsigned requests = 0, native_creates = 0, native_destroys = 0;
inline XrResult native_result = XR_SUCCESS;
inline XrSession target = reinterpret_cast<XrSession>(0x100);
inline XrSpace space = reinterpret_cast<XrSpace>(0x200);
struct Fixture {
  XrPassthroughFB feature{};
  XrPassthroughLayerFB layer{};
  Fixture();
  ~Fixture();
  void create(bool native = false);
  XrSwapchain chain(std::uint32_t layers = 2, VkFormat format = VK_FORMAT_R8G8B8A8_UNORM);
};
void commands(const std::function<void(VkCommandBuffer)>&);
void fill(XrSwapchain, std::array<std::array<float, 4>, 2>);
std::array<unsigned char, 4> pixel(XrSwapchain, std::uint32_t layer = 0, std::uint32_t row = 0);
} // namespace venture::test::passthrough

// Swapchain entry points every host shares (hle/vrapi/submit.h): argument
// validation and the older creators, which forward to the host's
// vrapi_CreateTextureSwapChain4.
#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <iterator>

#include "common/log.h"
#include "hle/vrapi/submit.h"

namespace venture::hle::vrapi {
bool valid_sampler(const TextureSamplerState &s) {
  return s.min_filter >= TextureFilter::nearest && s.min_filter <= TextureFilter::cubic_mipmap_linear &&
      (s.mag_filter == TextureFilter::nearest || s.mag_filter == TextureFilter::linear ||
       s.mag_filter == TextureFilter::cubic) &&
      s.wrap_s >= TextureWrap::repeat && s.wrap_s <= TextureWrap::clamp_border &&
      s.wrap_t >= TextureWrap::repeat && s.wrap_t <= TextureWrap::clamp_border &&
      std::isfinite(s.max_anisotropy) && s.max_anisotropy >= 1 &&
      std::all_of(std::begin(s.border_color), std::end(s.border_color),
                  [](float f) { return std::isfinite(f); });
}
// VrApi_Types.h's VRAPI_TEXTURE_SWAPCHAIN_FULL_MIP_CHAIN.
constexpr std::int32_t kFullMipChain = -1;
bool valid_swapchain_create(const SwapChainCreate &ci) {
  // Protected (0x1) and subsampled (0x2) creation are Quest compositor
  // features with nothing to protect or subsample here: ignored. Usage flags
  // only narrow what the images are for, and every ring image can be sampled
  // and rendered to.
  const bool ok = !(ci.width <= 0 || ci.height <= 0 || (ci.levels <= 0 && ci.levels != kFullMipChain) ||
                    ci.count <= 0 || ci.count > 16 || ci.array_size < 1 || ci.array_size > 2 ||
                    (ci.faces != 1 && ci.faces != 6) || (ci.create_flags & ~std::uint64_t(3)));
  static std::atomic<int> reported{0};
  if (!ok && reported.fetch_add(1) < 4)
    log::warn("vrapi", "Swapchain refused: format={:#x} {}x{} levels={} faces={} layers={} count={} create={:#x} usage={:#x}",
              ci.format, ci.width, ci.height, ci.levels, ci.faces, ci.array_size, ci.count, ci.create_flags,
              ci.usage_flags);
  return ok;
}
SwapChainCreate normalized_swapchain_create(const SwapChainCreate &ci) {
  auto out = ci;
  if (out.levels == kFullMipChain)
    out.levels = std::int32_t(std::bit_width(std::uint32_t(std::max(ci.width, ci.height))));
  return out;
}
namespace guest {
TextureSwapChain *vrapi_CreateTextureSwapChain3(std::int32_t type, std::int64_t format,
                                                std::int32_t width, std::int32_t height,
                                                std::int32_t levels, std::int32_t count) {
  if (type != 0 && type != 2 && type != 3) {
    failure("CreateTextureSwapChain3 type", unsupported);
    return nullptr;
  }
  SwapChainCreate ci{format, width, height, levels, type == 3 ? 6 : 1, type == 2 ? 2 : 1,
                     count,  0,     1};
  return vrapi_CreateTextureSwapChain4(&ci);
}
// VrApi 1.0 to 1.1 creators take ovrTextureFormat (VRAPI_TEXTURE_FORMAT_*), not GL formats.
TextureSwapChain *vrapi_CreateTextureSwapChain2(std::int32_t type, std::int32_t format,
                                                std::int32_t width, std::int32_t height,
                                                std::int32_t levels, std::int32_t count) {
  constexpr std::int64_t formats[]{0,      0x8d62, 0x8057, 0x8056, 0x8058, 0x8c43,
                                   0x881a, 0x81a5, 0x81a6, 0x88f0, 0x822c};
  if (format < 0 || std::size_t(format) >= std::size(formats))
    return nullptr;
  return vrapi_CreateTextureSwapChain3(type, formats[format], width, height, levels, count);
}
TextureSwapChain *vrapi_CreateTextureSwapChain(std::int32_t type, std::int32_t format,
                                               std::int32_t width, std::int32_t height,
                                               std::int32_t levels, bool buffered) {
  return vrapi_CreateTextureSwapChain2(type, format, width, height, levels, buffered ? 3 : 1);
}
}  // namespace guest
}  // namespace venture::hle::vrapi

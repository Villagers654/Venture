#pragma once
// Anisotropic filtering for games' textures: Quest games turn it off or keep
// it low because it costs memory bandwidth on a mobile GPU, so their textures
// blur a few metres away. On a PC GPU it costs almost nothing, so Venture
// raises the anisotropy of the game's mipmapped samplers: guest Vulkan
// samplers (hle/vulkan/hardware_buffer.cpp) and the ones ANGLE creates for
// GLES games (gfx/pipeline_trace.cpp).
//
//   VENTURE_ANISOTROPY = auto | off | 2 | 4 | 8 | 16
//     auto (the default; the launcher's "Anisotropic filtering" Automatic) is
//     16x on a discrete GPU and the game's own filtering on an integrated one,
//     which shares memory bandwidth with the CPU (laptops' integrated GPUs,
//     standalone headsets, where no measurement shows the cost is negligible).
//     A number is that level on any GPU; off keeps the game's own. Levels are
//     capped at the GPU's maxSamplerAnisotropy.
#include <vulkan/vulkan.h>
#include <string_view>

namespace venture::gfx {
// The level VENTURE_ANISOTROPY asks for: kAutomaticAnisotropy, 0 (off) or
// 2, 4, 8 or 16. Anything else is automatic.
inline constexpr float kAutomaticAnisotropy = -1;
float parse_anisotropy(std::string_view value);
// The level Venture gives samplers on this GPU for `requested`, or 0 to keep
// the game's.
float anisotropy_level(float requested, const VkPhysicalDeviceProperties& properties,
                       const VkPhysicalDeviceFeatures& features);
// The same from VENTURE_ANISOTROPY, logged once per process.
float anisotropy_level(const VkPhysicalDeviceProperties& properties, const VkPhysicalDeviceFeatures& features);
// Raises `info` to `level` when it filters linearly between mip levels and
// asked for less; true when it changed. Samplers it cannot apply to stay as
// they are: unnormalized coordinates, min/max reduction, YCbCr conversion,
// fewer than two mip levels (maxLod under 1; ANGLE gives unmipmapped GL
// textures 0.25) or a point-filtered minification.
bool raise_anisotropy(VkSamplerCreateInfo& info, float level);
}  // namespace venture::gfx

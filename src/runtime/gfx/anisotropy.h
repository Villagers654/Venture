#pragma once
// Anisotropic filtering for games' textures: Quest games turn it off or keep
// it low because it costs memory bandwidth on a mobile GPU, so their textures
// blur a few metres away. On a PC GPU it costs almost nothing, so Venture
// raises the anisotropy of the game's mipmapped samplers: guest Vulkan
// samplers (hle/vulkan/hardware_buffer.cpp) and the ones ANGLE creates for
// GLES games (gfx/pipeline_trace.cpp).
//
//   VENTURE_ANISOTROPY = off | 2 | 4 | 8 | 16
//     A number is that level on any GPU, capped at its maxSamplerAnisotropy;
//     off keeps the game's own. Unset or anything else is 16, the launcher's
//     default "Anisotropic filtering" on every machine.
#include <vulkan/vulkan.h>
#include <string_view>

namespace venture::gfx {
// The level VENTURE_ANISOTROPY asks for: 0 (the game's own) for off, else 2,
// 4, 8 or 16; anything else is 16.
float parse_anisotropy(std::string_view value);
// The level Venture gives samplers on this GPU for `requested`, or 0 to keep
// the game's.
float anisotropy_level(float requested, const VkPhysicalDeviceProperties& properties,
                       const VkPhysicalDeviceFeatures& features);
// The same from VENTURE_ANISOTROPY, logged once per process.
float anisotropy_level(const VkPhysicalDeviceProperties& properties, const VkPhysicalDeviceFeatures& features);
// Raises `info` to `level` when it is trilinear (linear minification and
// linear between mip levels) and asked for less; true when it changed. Other
// samplers stay as they are: nearest mip selection, depth comparison,
// unnormalized coordinates, min/max reduction, YCbCr conversion, fewer than
// two mip levels (maxLod under 1; ANGLE gives unmipmapped GL textures 0.25)
// or a point-filtered minification.
bool raise_anisotropy(VkSamplerCreateInfo& info, float level);
}  // namespace venture::gfx

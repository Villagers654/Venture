#include "gfx/anisotropy.h"

#include "common/environment.h"
#include "common/log.h"

#include <algorithm>
#include <mutex>
#include <string>

namespace venture::gfx {
float parse_anisotropy(std::string_view value) {
  if (value == "off") return 0;
  for (const float level : {2.0f, 4.0f, 8.0f})
    if (value == std::to_string(int(level))) return level;
  return 16;
}

float anisotropy_level(float requested, const VkPhysicalDeviceProperties& properties,
                       const VkPhysicalDeviceFeatures& features) {
  if (requested <= 1 || !features.samplerAnisotropy) return 0;
  const float level = std::min(requested, properties.limits.maxSamplerAnisotropy);
  return level > 1 ? level : 0;
}

float anisotropy_level(const VkPhysicalDeviceProperties& properties, const VkPhysicalDeviceFeatures& features) {
  const char* value = environment::getenv("VENTURE_ANISOTROPY");
  const float requested = parse_anisotropy(value ? value : "");
  const float level = anisotropy_level(requested, properties, features);
  static std::once_flag reported;
  std::call_once(reported, [&] {
    if (level > 0)
      log::info("gfx", "Anisotropic filtering: {:.0f}x for the game's mipmapped textures", level);
    else
      log::info("gfx", "Anisotropic filtering: the game's own ({})",
                requested == 0 ? "off" : "the GPU has none");
  });
  return level;
}

bool raise_anisotropy(VkSamplerCreateInfo& info, float level) {
  if (level <= 1 || info.unnormalizedCoordinates || info.minFilter != VK_FILTER_LINEAR || info.maxLod < 1)
    return false;
  if (info.anisotropyEnable && info.maxAnisotropy >= level) return false;
  for (auto* next = static_cast<const VkBaseInStructure*>(info.pNext); next; next = next->pNext) {
    if (next->sType == VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO) return false;
    if (next->sType == VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO &&
        reinterpret_cast<const VkSamplerReductionModeCreateInfo*>(next)->reductionMode !=
            VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE)
      return false;
  }
  info.anisotropyEnable = VK_TRUE;
  info.maxAnisotropy = level;
  return true;
}
}  // namespace venture::gfx

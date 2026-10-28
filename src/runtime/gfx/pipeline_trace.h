#pragma once
#include <vulkan/vulkan.h>
#include <string_view>
#include "core/pipeline_stats.h"

namespace venture::gfx {
// Only pipeline creation is interposed. GIPA/GDPA must also forward queries to
// the two wrappers so ANGLE's volk device table cannot bypass them.
PFN_vkGetInstanceProcAddr wrap_pipeline_loader(PFN_vkGetInstanceProcAddr loader);
PFN_vkGetInstanceProcAddr unwrap_pipeline_loader(PFN_vkGetInstanceProcAddr loader);
void register_pipeline_device(VkDevice device, PFN_vkGetDeviceProcAddr loader,
                              bool feedback, profile::PipelineSource source);
void release_pipeline_device(VkDevice device);
PFN_vkVoidFunction pipeline_proc(std::string_view name);
VkResult create_decode_pipeline(VkDevice, VkPipelineCache, std::uint32_t,
                                const VkComputePipelineCreateInfo*, const VkAllocationCallbacks*, VkPipeline*);
}

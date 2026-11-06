#pragma once
#include <vulkan/vulkan.h>
#include <set>
#include <span>
#include <string_view>
#include "core/pipeline_stats.h"

namespace venture::gfx {
// Only pipeline creation is interposed. GIPA/GDPA must also forward queries to
// the two wrappers so ANGLE's volk device table cannot bypass them.
PFN_vkGetInstanceProcAddr wrap_pipeline_loader(PFN_vkGetInstanceProcAddr loader);
PFN_vkGetInstanceProcAddr unwrap_pipeline_loader(PFN_vkGetInstanceProcAddr loader);
// `async`: the guest's graphics pipelines may be created in the background
// (VENTURE_VK_ASYNC_PIPELINES; see pipeline_trace.cpp).
void register_pipeline_device(VkDevice device, PFN_vkGetDeviceProcAddr loader,
                              bool feedback, profile::PipelineSource source, bool async = false);
// False when an enabled device extension takes pipelines in commands that
// background creation does not translate.
bool async_pipeline_extensions_allowed(const std::set<std::string_view>& enabled);
void release_pipeline_device(VkDevice device);
// True while a guest pipeline created in the background is not ready yet.
bool pipeline_pending(VkPipeline pipeline) noexcept;
// A command buffer binds it: workers create it before pipelines nothing
// waits for yet.
void prioritize_pipeline(VkPipeline pipeline);
// Returns once every listed pipeline is ready (or failed), creating those no
// worker has started on this thread.
void await_pipelines(std::span<const VkPipeline> pipelines);
PFN_vkVoidFunction pipeline_proc(std::string_view name);
VkResult create_decode_pipeline(VkDevice, VkPipelineCache, std::uint32_t,
                                const VkComputePipelineCreateInfo*, const VkAllocationCallbacks*, VkPipeline*);
}

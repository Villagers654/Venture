#pragma once
#include <cstdint>
#include <string>
#include <vulkan/vulkan.h>
namespace venture::xr {
PFN_vkGetInstanceProcAddr native_vulkan_loader();
std::int64_t native_counter_to_ns(std::int64_t counter);
std::int64_t native_now_ns();
bool set_runtime_manifest_for_process(const std::string &path);
// Before the first Vulkan instance: Steam's pipeline recording layer serves
// only Steam's own titles and sits inside every pipeline creation (The 7th
// Guest VR crashed in NVIDIA's shader compiler beneath it).
void disable_unused_vulkan_layers_for_process();
} // namespace venture::xr

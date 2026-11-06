#pragma once
#include <cstdint>
#include <string>
#include <vulkan/vulkan.h>
namespace venture::xr {
PFN_vkGetInstanceProcAddr native_vulkan_loader();
std::int64_t native_counter_to_ns(std::int64_t counter);
std::int64_t native_now_ns();
bool set_runtime_manifest_for_process(const std::string &path);
} // namespace venture::xr

#pragma once
#include "hle/vrapi/vrapi.h"
#include <mutex>
namespace venture::hle::vrapi {
XrResult bind_vulkan_queue(const abi::ModeVulkan &, XrGraphicsBindingVulkanKHR &);
XrResult prepare_vulkan_submission();
std::unique_lock<std::recursive_mutex> vulkan_submission_lock();
}

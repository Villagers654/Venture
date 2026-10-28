#pragma once
#include <vulkan/vulkan_core.h>

#include <memory>

#include "core/guest.h"

namespace venture::vulkan {

// Vulkan copies callbacks into long-lived objects, including driver-created
// threads. Stable records are interned for the process lifetime. No host stack
// address, guest VkAllocationCallbacks address, or transient pNext is retained.
const VkAllocationCallbacks* translate_allocator(const VkAllocationCallbacks* guest);
struct DebugCallback {
  GuestAddr function;
  GuestAddr user;
};
std::shared_ptr<DebugCallback> debug_callback(GuestAddr function, GuestAddr user);
VKAPI_ATTR VkBool32 VKAPI_CALL debug_utils_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                    VkDebugUtilsMessageTypeFlagsEXT type,
                                                    const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                    void* user);
VKAPI_ATTR VkBool32 VKAPI_CALL debug_report_callback(VkDebugReportFlagsEXT flags,
                                                     VkDebugReportObjectTypeEXT type, std::uint64_t object,
                                                     std::size_t location, std::int32_t code,
                                                     const char* layer, const char* message, void* user);

}  // namespace venture::vulkan

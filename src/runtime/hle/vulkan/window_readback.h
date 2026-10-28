#pragma once
#include "vulkan.h"
#include <span>
namespace venture::android { struct NativeWindow; }
namespace venture::vulkan {
bool window_readback_format(VkFormat);
// Caller holds this queue's external synchronization mutex. The image is in
// GENERAL, the layout the guest's PRESENT_SRC_KHR becomes (wsi.cpp). Wait
// semaphores are consumed once; on success the image is back in GENERAL and
// its completed pixels have reached the window's single consumer.
VkResult publish_window_image(VkDevice, VkQueue, std::uint32_t family, VkImage,
    VkFormat, VkExtent2D, android::NativeWindow*, std::span<const VkSemaphore> waits);
}

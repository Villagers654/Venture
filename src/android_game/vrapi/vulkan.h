// SPDX-License-Identifier: GPL-3.0-or-later
// The game's Vulkan device inside libvrapi.so (docs/design/native-headsets.md
// 5.4): what vrapi_CreateSystemVulkan hands over, the queue family the
// runtime is told about, and short command buffers on the game's queue for
// the image layout changes, copies and flips the swapchains need.
#pragma once
#include "android_game/vrapi/platform_xr.h"

#include <cstdint>
#include <functional>

#include "hle/vrapi/abi.h"

namespace venture::vrapi_native::vulkan {
// vrapi_CreateSystemVulkan: the game's instance, physical device and device.
// False when they cannot be used (no Vulkan loader, or the runtime wants
// another physical device).
std::int32_t bind_system(const hle::vrapi::abi::SystemVulkan &);
void unbind_system();
bool bound();
// Makes the session on the game's device. `queue` is the game's
// SynchronizationQueue (EnterVrMode), or null before it is known (a swapchain
// created first): the first queue of the device's first graphics family is
// then the runtime's queue, the one games create.
XrResult create_session(VkQueue queue);
// Whether `queue` is the queue the session was made with.
bool session_queue(VkQueue queue);
// Records commands and submits them on the session's queue, after the game's
// work already submitted there.
XrResult submit(const std::function<void(VkCommandBuffer)> &record);
// Waits for every submitted command buffer.
void wait_idle();

// Inside submit's record only: a layout barrier for every layer and level of a
// color image.
void barrier(VkCommandBuffer, VkImage, std::uint32_t layers, VkImageLayout from, VkImageLayout to);
// Copies (or, with flip, mirrors vertically) level 0 of every layer.
void copy(VkCommandBuffer, VkImage from, VkImage to, std::uint32_t width, std::uint32_t height,
          std::uint32_t layers, bool flip);
}  // namespace venture::vrapi_native::vulkan

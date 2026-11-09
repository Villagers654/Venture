// An image produced on one VkDevice and consumed on another device of the same
// physical GPU, through exported memory: no copy through system memory.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace venture::gfx {

// The exporter creates a 2D, single-mip, single-layer, optimally tiled,
// exclusive image with no create flags and a dedicated allocation, and the
// importer recreates it with the same parameters (VK_KHR_external_memory).
// Contents are complete when the image is handed over: the producer waited for
// its writes and released the image to VK_QUEUE_FAMILY_EXTERNAL in
// VK_IMAGE_LAYOUT_GENERAL. The producer reuses the memory only after the last
// reference to this description is gone, and owns the handle.
struct SharedImage {
  std::uint64_t id = 0;  // unique per allocation; importers may cache by it
  std::uint32_t width = 0, height = 0;
  std::uint32_t format = 0;       // VkFormat
  std::uint32_t usage = 0;        // VkImageUsageFlags
  std::uint32_t handle_type = 0;  // VkExternalMemoryHandleTypeFlagBits (opaque Win32 / FD)
  // Windows: an NT handle (importing does not transfer ownership). POSIX: a
  // file descriptor, which an importer duplicates, since importing an FD
  // transfers its ownership.
  std::intptr_t handle = -1;
  // Opaque handles must be imported with the exporter's size and memory type.
  std::uint64_t allocation_size = 0;
  std::uint32_t memory_type = 0;
  std::array<std::uint8_t, 16> device_uuid{}, driver_uuid{};
  // Expires when the producer frees the allocation; importers then drop
  // their import of it rather than keep its memory alive.
  std::weak_ptr<const void> allocation;
};

// The device extension that imports SharedImage handles.
#ifdef _WIN32
inline constexpr const char* shared_image_extension = "VK_KHR_external_memory_win32";
#else
inline constexpr const char* shared_image_extension = "VK_KHR_external_memory_fd";
#endif

// The physical GPU Venture renders with (VkPhysicalDeviceIDProperties), once
// the host device exists. Producers place shared images on the same GPU.
struct HostGpu {
  std::array<std::uint8_t, 16> device_uuid{}, driver_uuid{};
};
namespace detail {
inline std::mutex host_gpu_mutex;
inline std::optional<HostGpu> host_gpu_value;
inline std::vector<const void*> importing_devices;
}  // namespace detail
// Host VkDevices created with shared_image_extension enabled (Venture's and
// those made for guests); consumers check theirs before importing.
inline void note_shared_image_device(const void* device, bool imports) {
  std::lock_guard lock(detail::host_gpu_mutex);
  std::erase(detail::importing_devices, device);
  if (imports) detail::importing_devices.push_back(device);
}
inline bool shared_image_device(const void* device) {
  std::lock_guard lock(detail::host_gpu_mutex);
  return std::ranges::find(detail::importing_devices, device) != detail::importing_devices.end();
}
inline void set_host_gpu(const HostGpu& gpu) {
  std::lock_guard lock(detail::host_gpu_mutex);
  detail::host_gpu_value = gpu;
}
inline std::optional<HostGpu> host_gpu() {
  std::lock_guard lock(detail::host_gpu_mutex);
  return detail::host_gpu_value;
}

}  // namespace venture::gfx

#pragma once
#include <memory>
#include <string_view>
#include <vector>
#include <vulkan/vulkan_core.h>

namespace venture::vulkan {
// Platform external handles are owned by the guest descriptor table. Only
// advertise handle types whose payload and ownership semantics are translated.
bool external_extension(std::string_view name);
void external_extensions(std::vector<VkExtensionProperties> &properties);
const char *external_host_extension(const char *name);
VkResult external_chain(VkBaseOutStructure *structure);
void external_image_properties(VkImageFormatProperties2 *properties);
PFN_vkVoidFunction external_proc(std::string_view name);
void release_external_device(VkDevice);

// A platform handle that holds a reference to an exported device-memory
// payload (Windows: OPAQUE_WIN32 NT handle). Android hardware buffers keep
// their GPU memory alive through it, independent of any VkDevice.
struct MemoryHandle {
  virtual ~MemoryHandle() = default;
};
// The host handle type used for such payloads, or 0 when unavailable, and the
// host device extension that provides it.
VkExternalMemoryHandleTypeFlagBits memory_handle_type();
const char *memory_handle_extension();
std::shared_ptr<MemoryHandle> export_memory_handle(PFN_vkGetDeviceProcAddr gdpa, VkDevice device,
                                                   VkDeviceMemory memory);
// The host import node for vkAllocateMemory; valid while `handle` lives.
// Null if the platform cannot import it. Call memory_handle_imported after a
// successful allocation: handle types whose import transfers ownership (FDs)
// must not release what the driver now owns.
std::shared_ptr<VkBaseInStructure> import_memory_handle(const MemoryHandle &handle);
void memory_handle_imported(VkBaseInStructure &node);
} // namespace venture::vulkan

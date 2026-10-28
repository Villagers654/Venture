#include "hle/libc/vfs.h"
#include "hle/vulkan/external.h"
#include "hle/vulkan/internal.h"
#include <algorithm>
#include <map>
#include <mutex>
#include <vulkan/vulkan.h>
#include <windows.h>

#include <vulkan/vulkan_win32.h>

namespace venture::vulkan {
namespace {
using hle::libc::FdTable;
enum class Payload { memory, semaphore };
struct ExternalHandle final : hle::libc::File {
  HANDLE handle{};
  Payload kind;
  ExternalHandle(HANDLE h, Payload k) : handle(h), kind(k) {}
  ~ExternalHandle() override {
    if (handle)
      CloseHandle(handle);
  }
  short ready(short) override { return hle::libc::poll_err; }
};
std::mutex memory_exports_mutex;
std::map<std::pair<VkDevice, VkDeviceMemory>, std::shared_ptr<ExternalHandle>> memory_exports;
template <class T> T device_proc(VkDevice d, const char *name) {
  if (auto f = next_proc(Layer::external, name))
    return reinterpret_cast<T>(f);
  return reinterpret_cast<T>(raw_device_proc(d, name));
}
template <class T> T physical_proc(VkPhysicalDevice d, const char *name) {
  if (auto f = next_proc(Layer::external, name))
    return reinterpret_cast<T>(f);
  return reinterpret_cast<T>(raw_physical_proc(d, name));
}
std::shared_ptr<ExternalHandle> descriptor(int fd, Payload kind) {
  auto value =
      std::dynamic_pointer_cast<ExternalHandle>(FdTable::get().file(fd));
  return value && value->kind == kind ? value : nullptr;
}
VkResult publish(HANDLE h, Payload kind, int *out) {
  std::shared_ptr<ExternalHandle> file;
  try {
    file = std::make_shared<ExternalHandle>(h, kind);
  } catch (...) {
    CloseHandle(h);
    throw;
  }
  int fd = FdTable::get().insert(std::move(file), 0, true);
  if (fd < 0)
    return VK_ERROR_TOO_MANY_OBJECTS;
  *out = fd;
  return VK_SUCCESS;
}
// OPAQUE_FD and OPAQUE_WIN32 both retain a reference to the same GPU payload.
// SYNC_FD has copy/transference semantics and is deliberately not advertised.
constexpr auto memory_fd = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
constexpr auto memory_nt = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
constexpr auto semaphore_fd = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
constexpr auto semaphore_nt =
    VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
constexpr auto memory_ahb =
    VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
// Android hardware buffer bits stay for the AHB translation (hardware_buffer.cpp),
// which replaces them before the host sees images or allocations.
VkResult memory_types(VkExternalMemoryHandleTypeFlags &flags,
                      VkExternalMemoryHandleTypeFlags ahb = 0) {
  if (flags & ~VkExternalMemoryHandleTypeFlags(memory_fd | ahb))
    return VK_ERROR_INVALID_EXTERNAL_HANDLE;
  flags = ((flags & memory_fd) ? memory_nt : 0) | (flags & ahb);
  return VK_SUCCESS;
}
void memory_properties(VkExternalMemoryProperties &p) {
  p.compatibleHandleTypes =
      (p.compatibleHandleTypes & memory_nt) ? memory_fd : 0;
  p.exportFromImportedHandleTypes =
      (p.exportFromImportedHandleTypes & memory_nt) ? memory_fd : 0;
}
void VKAPI_CALL buffer_properties(
    VkPhysicalDevice p, const VkPhysicalDeviceExternalBufferInfo *info,
    VkExternalBufferProperties *out) {
  if (info->handleType != memory_fd) {
    out->externalMemoryProperties = {};
    return;
  }
  auto copy = *info;
  copy.handleType = memory_nt;
  physical_proc<PFN_vkGetPhysicalDeviceExternalBufferProperties>(
      p, "vkGetPhysicalDeviceExternalBufferProperties")(p, &copy, out);
  memory_properties(out->externalMemoryProperties);
}
void VKAPI_CALL semaphore_properties(
    VkPhysicalDevice p, const VkPhysicalDeviceExternalSemaphoreInfo *info,
    VkExternalSemaphoreProperties *out) {
  out->exportFromImportedHandleTypes = out->compatibleHandleTypes =
      out->externalSemaphoreFeatures = 0;
  if (info->handleType != semaphore_fd)
    return;
  auto copy = *info;
  copy.handleType = semaphore_nt;
  physical_proc<PFN_vkGetPhysicalDeviceExternalSemaphoreProperties>(
      p, "vkGetPhysicalDeviceExternalSemaphoreProperties")(p, &copy, out);
  out->compatibleHandleTypes =
      (out->compatibleHandleTypes & semaphore_nt) ? semaphore_fd : 0;
  out->exportFromImportedHandleTypes =
      (out->exportFromImportedHandleTypes & semaphore_nt) ? semaphore_fd : 0;
}
VkResult VKAPI_CALL get_memory_fd(VkDevice d, const VkMemoryGetFdInfoKHR *info,
                                  int *out) {
  if (!out || info->handleType != memory_fd)
    return VK_ERROR_INVALID_EXTERNAL_HANDLE;
  *out = -1;
  std::lock_guard lock(memory_exports_mutex);
  auto [entry, inserted] = memory_exports.try_emplace({d, info->memory});
  if (inserted || !entry->second) {
    // Allocate ownership before the Vulkan export, which may only be called
    // once per allocation/handle type (VkMemoryGetWin32HandleInfoKHR VUID 00663).
    entry->second = std::make_shared<ExternalHandle>(nullptr, Payload::memory);
  }
  auto& file = entry->second;
  if (file->handle) {
    const auto fd = FdTable::get().insert(file, 0, true); // dup: shared NT payload reference
    if (fd < 0) return VK_ERROR_TOO_MANY_OBJECTS;
    *out = fd;
    return VK_SUCCESS;
  }
  auto fn = device_proc<PFN_vkGetMemoryWin32HandleKHR>(
      d, "vkGetMemoryWin32HandleKHR");
  if (!fn)
    return VK_ERROR_EXTENSION_NOT_PRESENT;
  VkMemoryGetWin32HandleInfoKHR native{
      VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR, info->pNext,
      info->memory, memory_nt};
  HANDLE handle{};
  auto result = fn(d, &native, &handle);
  if (result != VK_SUCCESS) { memory_exports.erase(entry); return result; }
  file->handle = handle;
  const auto fd = FdTable::get().insert(file, 0, true);
  if (fd < 0) return VK_ERROR_TOO_MANY_OBJECTS;
  *out = fd;
  return VK_SUCCESS;
}
void VKAPI_CALL free_memory(VkDevice d, VkDeviceMemory memory, const VkAllocationCallbacks* alloc) {
  {
    std::lock_guard lock(memory_exports_mutex);
    memory_exports.erase({d, memory}); // Exported FDs retain independent shared ownership.
  }
  device_proc<PFN_vkFreeMemory>(d, "vkFreeMemory")(d, memory, alloc);
}
VkResult VKAPI_CALL memory_fd_properties(VkDevice,
                                         VkExternalMemoryHandleTypeFlagBits,
                                         int, VkMemoryFdPropertiesKHR *) {
  // vkGetMemoryFdPropertiesKHR excludes OPAQUE_FD (VUID-handleType-00674).
  // No other memory handle type is exposed by this backend.
  return VK_ERROR_INVALID_EXTERNAL_HANDLE;
}
VkResult VKAPI_CALL get_semaphore_fd(VkDevice d,
                                     const VkSemaphoreGetFdInfoKHR *info,
                                     int *out) {
  if (!out || info->handleType != semaphore_fd)
    return VK_ERROR_INVALID_EXTERNAL_HANDLE;
  *out = -1;
  auto fn = device_proc<PFN_vkGetSemaphoreWin32HandleKHR>(
      d, "vkGetSemaphoreWin32HandleKHR");
  if (!fn)
    return VK_ERROR_EXTENSION_NOT_PRESENT;
  VkSemaphoreGetWin32HandleInfoKHR native{
      VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR, info->pNext,
      info->semaphore, semaphore_nt};
  HANDLE handle{};
  auto result = fn(d, &native, &handle);
  return result == VK_SUCCESS ? publish(handle, Payload::semaphore, out)
                              : result;
}
VkResult VKAPI_CALL
import_semaphore_fd(VkDevice d, const VkImportSemaphoreFdInfoKHR *info) {
  if (info->handleType != semaphore_fd)
    return VK_ERROR_INVALID_EXTERNAL_HANDLE;
  auto file = descriptor(info->fd, Payload::semaphore);
  if (!file)
    return VK_ERROR_INVALID_EXTERNAL_HANDLE;
  auto fn = device_proc<PFN_vkImportSemaphoreWin32HandleKHR>(
      d, "vkImportSemaphoreWin32HandleKHR");
  if (!fn)
    return VK_ERROR_EXTENSION_NOT_PRESENT;
  VkImportSemaphoreWin32HandleInfoKHR native{
      VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR,
      info->pNext,
      info->semaphore,
      info->flags,
      semaphore_nt,
      file->handle,
      nullptr};
  auto result = fn(d, &native);
  // The FD extension transfers ownership only on success; NT imports retain
  // caller ownership (VK_KHR_external_semaphore_win32). Closing our reference
  // after import implements the FD rule, including dup-shared descriptions.
  if (result == VK_SUCCESS)
    FdTable::get().close(info->fd);
  return result;
}
VkResult VKAPI_CALL allocate_memory(VkDevice d,
                                    const VkMemoryAllocateInfo *info,
                                    const VkAllocationCallbacks *alloc,
                                    VkDeviceMemory *out) {
  auto copy = *info;
  auto *previous = reinterpret_cast<VkBaseOutStructure *>(&copy);
  auto *node = static_cast<const VkBaseInStructure *>(info->pNext);
  while (node && node->sType != VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR) {
    previous = const_cast<VkBaseOutStructure *>(
        reinterpret_cast<const VkBaseOutStructure *>(node));
    node = node->pNext;
  }
  auto fn = device_proc<PFN_vkAllocateMemory>(d, "vkAllocateMemory");
  if (!node)
    return fn(d, info, alloc, out);
  auto *imported = reinterpret_cast<const VkImportMemoryFdInfoKHR *>(node);
  // Vulkan memory import rules: handleType 0 ignores the import and fd.
  // Remove the Linux-only node before forwarding to the Windows driver.
  if (!imported->handleType) {
    auto* saved = previous->pNext;
    previous->pNext = const_cast<VkBaseOutStructure*>(reinterpret_cast<const VkBaseOutStructure*>(imported->pNext));
    auto result = fn(d, &copy, alloc, out);
    previous->pNext = saved;
    return result;
  }
  if (imported->handleType != memory_fd)
    return VK_ERROR_INVALID_EXTERNAL_HANDLE;
  auto file = descriptor(imported->fd, Payload::memory);
  if (!file)
    return VK_ERROR_INVALID_EXTERNAL_HANDLE;
  VkImportMemoryWin32HandleInfoKHR native{
      VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR, imported->pNext,
      memory_nt, file->handle, nullptr};
  // Invocation/Chain owns these copies; guest pNext pointers are untouched.
  auto *saved = previous->pNext;
  previous->pNext = reinterpret_cast<VkBaseOutStructure *>(&native);
  auto result = fn(d, &copy, alloc, out);
  previous->pNext = saved;
  if (result == VK_SUCCESS)
    FdTable::get().close(imported->fd);
  return result;
}
struct NtMemory final : MemoryHandle {
  HANDLE handle;
  explicit NtMemory(HANDLE h) : handle(h) {}
  ~NtMemory() override { CloseHandle(handle); }
};
} // namespace
VkExternalMemoryHandleTypeFlagBits memory_handle_type() { return memory_nt; }
const char *memory_handle_extension() { return "VK_KHR_external_memory_win32"; }
std::shared_ptr<MemoryHandle> export_memory_handle(PFN_vkGetDeviceProcAddr gdpa,
                                                   VkDevice device,
                                                   VkDeviceMemory memory) {
  auto fn = reinterpret_cast<PFN_vkGetMemoryWin32HandleKHR>(
      gdpa(device, "vkGetMemoryWin32HandleKHR"));
  if (!fn)
    return nullptr;
  VkMemoryGetWin32HandleInfoKHR info{
      VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR, nullptr, memory,
      memory_nt};
  HANDLE handle{};
  if (fn(device, &info, &handle) != VK_SUCCESS || !handle)
    return nullptr;
  return std::make_shared<NtMemory>(handle);
}
std::shared_ptr<VkBaseInStructure> import_memory_handle(const MemoryHandle &handle) {
  // VK_KHR_external_memory_win32: importing an NT handle does not transfer
  // its ownership, so the payload reference stays with `handle`.
  auto node = std::make_shared<VkImportMemoryWin32HandleInfoKHR>();
  *node = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR, nullptr,
           memory_nt, static_cast<const NtMemory &>(handle).handle, nullptr};
  return std::shared_ptr<VkBaseInStructure>(
      node, reinterpret_cast<VkBaseInStructure *>(node.get()));
}
void memory_handle_imported(VkBaseInStructure &) {} // NT imports keep no ownership
void release_external_device(VkDevice device) {
  std::lock_guard lock(memory_exports_mutex);
  std::erase_if(memory_exports, [&](const auto& entry) { return entry.first.first == device; });
}
bool external_extension(std::string_view name) {
  return name == "VK_KHR_external_memory_fd" ||
         name == "VK_KHR_external_semaphore_fd";
}
void external_extensions(std::vector<VkExtensionProperties> &values) {
  const bool memory = std::ranges::any_of(values, [](auto &v) {
    return std::string_view(v.extensionName) == "VK_KHR_external_memory_win32";
  });
  const bool semaphore = std::ranges::any_of(values, [](auto &v) {
    return std::string_view(v.extensionName) ==
           "VK_KHR_external_semaphore_win32";
  });
  if (memory)
    values.push_back(
        {"VK_KHR_external_memory_fd", VK_KHR_EXTERNAL_MEMORY_FD_SPEC_VERSION});
  if (semaphore)
    values.push_back({"VK_KHR_external_semaphore_fd",
                      VK_KHR_EXTERNAL_SEMAPHORE_FD_SPEC_VERSION});
}
const char *external_host_extension(const char *name) {
  if (std::string_view(name) == "VK_KHR_external_memory_fd")
    return "VK_KHR_external_memory_win32";
  if (std::string_view(name) == "VK_KHR_external_semaphore_fd")
    return "VK_KHR_external_semaphore_win32";
  return name;
}
VkResult external_chain(VkBaseOutStructure *p) {
  switch (p->sType) {
  case VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO:
    return memory_types(
        reinterpret_cast<VkExportMemoryAllocateInfo *>(p)->handleTypes,
        memory_ahb);
  case VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO:
    return memory_types(
        reinterpret_cast<VkExternalMemoryBufferCreateInfo *>(p)->handleTypes);
  case VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO:
    return memory_types(
        reinterpret_cast<VkExternalMemoryImageCreateInfo *>(p)->handleTypes,
        memory_ahb);
  case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO: {
    auto &type = reinterpret_cast<VkPhysicalDeviceExternalImageFormatInfo *>(p)
                     ->handleType;
    if (!type || type == memory_ahb) break; // no handle, or AHB translation
    if (type != memory_fd)
      return VK_ERROR_FORMAT_NOT_SUPPORTED;
    type = memory_nt;
    break;
  }
  case VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO: {
    auto &types =
        reinterpret_cast<VkExportSemaphoreCreateInfo *>(p)->handleTypes;
    if (types & ~VkExternalSemaphoreHandleTypeFlags(semaphore_fd))
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    types = types ? semaphore_nt : 0;
    break;
  }
  default:
    break;
  }
  return VK_SUCCESS;
}
void external_image_properties(VkImageFormatProperties2 *out) {
  for (auto *p = static_cast<VkBaseOutStructure *>(out->pNext); p; p = p->pNext)
    if (p->sType == VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES)
      memory_properties(reinterpret_cast<VkExternalImageFormatProperties *>(p)
                            ->externalMemoryProperties);
}
PFN_vkVoidFunction external_proc(std::string_view name) {
#define FN(vk, fn)                                                             \
  if (name == #vk)                                                             \
  return reinterpret_cast<PFN_vkVoidFunction>(fn)
  FN(vkGetMemoryFdKHR, get_memory_fd);
  FN(vkGetMemoryFdPropertiesKHR, memory_fd_properties);
  FN(vkGetSemaphoreFdKHR, get_semaphore_fd);
  FN(vkImportSemaphoreFdKHR, import_semaphore_fd);
  FN(vkGetPhysicalDeviceExternalBufferProperties, buffer_properties);
  FN(vkGetPhysicalDeviceExternalSemaphoreProperties, semaphore_properties);
  FN(vkAllocateMemory, allocate_memory);
  FN(vkFreeMemory, free_memory);
#undef FN
  return nullptr;
}
} // namespace venture::vulkan

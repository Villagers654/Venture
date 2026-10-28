#include "hle/libc/vfs.h"
#include "hle/vulkan/external.h"
#include "hle/vulkan/internal.h"
#include <fcntl.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

namespace venture::vulkan {
namespace {
using hle::libc::FdTable;
// The host speaks the same FD handle types as the guest. A guest descriptor
// owns one host descriptor; guest dup() shares it. Imports hand the driver a
// duplicate, so its ownership transfer (on success only) never invalidates a
// descriptor the guest still holds.
enum class Payload { memory, semaphore };
struct ExternalFd final : hle::libc::File {
  int fd;
  Payload kind;
  ExternalFd(int f, Payload k) : fd(f), kind(k) {}
  ~ExternalFd() override { ::close(fd); }
  short ready(short) override { return hle::libc::poll_err; }
};
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
// A host duplicate of a guest descriptor's payload, or -1.
int duplicate(int guest_fd, Payload kind) {
  auto file = std::dynamic_pointer_cast<ExternalFd>(FdTable::get().file(guest_fd));
  return file && file->kind == kind ? fcntl(file->fd, F_DUPFD_CLOEXEC, 0) : -1;
}
VkResult publish(int fd, Payload kind, int *out) {
  std::shared_ptr<ExternalFd> file;
  try {
    file = std::make_shared<ExternalFd>(fd, kind);
  } catch (...) {
    ::close(fd);
    throw;
  }
  const int guest = FdTable::get().insert(std::move(file), 0, true); // close-on-exec, per vkGet*FdKHR
  if (guest < 0)
    return VK_ERROR_TOO_MANY_OBJECTS;
  *out = guest;
  return VK_SUCCESS;
}
// OPAQUE_FD retains a reference to the payload. DMA_BUF and host-pointer
// imports need contracts this backend does not provide and stay hidden.
// SYNC_FD (copy transference) is the sync_fd layer's.
constexpr auto memory_fd = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
constexpr auto semaphore_fd = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
constexpr auto memory_ahb =
    VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
// Android hardware buffer bits stay for the AHB translation (hardware_buffer.cpp),
// which replaces them before the host sees images or allocations.
VkResult memory_types(VkExternalMemoryHandleTypeFlags flags,
                      VkExternalMemoryHandleTypeFlags ahb = 0) {
  return flags & ~VkExternalMemoryHandleTypeFlags(memory_fd | ahb)
             ? VK_ERROR_INVALID_EXTERNAL_HANDLE
             : VK_SUCCESS;
}
void memory_properties(VkExternalMemoryProperties &p) {
  p.compatibleHandleTypes &= memory_fd;
  p.exportFromImportedHandleTypes &= memory_fd;
}
void VKAPI_CALL buffer_properties(
    VkPhysicalDevice p, const VkPhysicalDeviceExternalBufferInfo *info,
    VkExternalBufferProperties *out) {
  if (info->handleType != memory_fd) {
    out->externalMemoryProperties = {};
    return;
  }
  physical_proc<PFN_vkGetPhysicalDeviceExternalBufferProperties>(
      p, "vkGetPhysicalDeviceExternalBufferProperties")(p, info, out);
  memory_properties(out->externalMemoryProperties);
}
void VKAPI_CALL semaphore_properties(
    VkPhysicalDevice p, const VkPhysicalDeviceExternalSemaphoreInfo *info,
    VkExternalSemaphoreProperties *out) {
  out->exportFromImportedHandleTypes = out->compatibleHandleTypes =
      out->externalSemaphoreFeatures = 0;
  if (info->handleType != semaphore_fd)
    return;
  physical_proc<PFN_vkGetPhysicalDeviceExternalSemaphoreProperties>(
      p, "vkGetPhysicalDeviceExternalSemaphoreProperties")(p, info, out);
  out->compatibleHandleTypes &= semaphore_fd;
  out->exportFromImportedHandleTypes &= semaphore_fd;
}
VkResult VKAPI_CALL get_memory_fd(VkDevice d, const VkMemoryGetFdInfoKHR *info,
                                  int *out) {
  if (!out || info->handleType != memory_fd)
    return VK_ERROR_INVALID_EXTERNAL_HANDLE;
  *out = -1;
  auto fn = device_proc<PFN_vkGetMemoryFdKHR>(d, "vkGetMemoryFdKHR");
  if (!fn)
    return VK_ERROR_EXTENSION_NOT_PRESENT;
  int fd = -1;
  auto result = fn(d, info, &fd); // each export is a new reference
  return result == VK_SUCCESS ? publish(fd, Payload::memory, out) : result;
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
  auto fn = device_proc<PFN_vkGetSemaphoreFdKHR>(d, "vkGetSemaphoreFdKHR");
  if (!fn)
    return VK_ERROR_EXTENSION_NOT_PRESENT;
  int fd = -1;
  auto result = fn(d, info, &fd);
  return result == VK_SUCCESS ? publish(fd, Payload::semaphore, out) : result;
}
VkResult VKAPI_CALL
import_semaphore_fd(VkDevice d, const VkImportSemaphoreFdInfoKHR *info) {
  if (info->handleType != semaphore_fd)
    return VK_ERROR_INVALID_EXTERNAL_HANDLE;
  auto fn = device_proc<PFN_vkImportSemaphoreFdKHR>(d, "vkImportSemaphoreFdKHR");
  if (!fn)
    return VK_ERROR_EXTENSION_NOT_PRESENT;
  auto native = *info;
  native.fd = duplicate(info->fd, Payload::semaphore);
  if (native.fd < 0)
    return VK_ERROR_INVALID_EXTERNAL_HANDLE;
  auto result = fn(d, &native);
  // Ownership transfers only on success: the driver now owns the duplicate,
  // and the guest's descriptor is consumed.
  if (result == VK_SUCCESS)
    FdTable::get().close(info->fd);
  else
    ::close(native.fd);
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
  // Remove the node: the host device need not enable the FD extension.
  if (!imported->handleType) {
    auto *saved = previous->pNext;
    previous->pNext = const_cast<VkBaseOutStructure *>(
        reinterpret_cast<const VkBaseOutStructure *>(imported->pNext));
    auto result = fn(d, &copy, alloc, out);
    previous->pNext = saved;
    return result;
  }
  if (imported->handleType != memory_fd)
    return VK_ERROR_INVALID_EXTERNAL_HANDLE;
  auto native = *imported;
  native.fd = duplicate(imported->fd, Payload::memory);
  if (native.fd < 0)
    return VK_ERROR_INVALID_EXTERNAL_HANDLE;
  // Invocation/Chain owns these copies; guest pNext pointers are untouched.
  auto *saved = previous->pNext;
  previous->pNext = reinterpret_cast<VkBaseOutStructure *>(&native);
  auto result = fn(d, &copy, alloc, out);
  previous->pNext = saved;
  if (result == VK_SUCCESS)
    FdTable::get().close(imported->fd);
  else
    ::close(native.fd);
  return result;
}
struct FdMemory final : MemoryHandle {
  int fd;
  explicit FdMemory(int f) : fd(f) {}
  ~FdMemory() override { ::close(fd); }
};
// An import node that owns its duplicate until the driver takes it.
struct ImportNode {
  VkImportMemoryFdInfoKHR info;
  ~ImportNode() {
    if (info.fd >= 0)
      ::close(info.fd);
  }
};
} // namespace
VkExternalMemoryHandleTypeFlagBits memory_handle_type() { return memory_fd; }
const char *memory_handle_extension() { return "VK_KHR_external_memory_fd"; }
std::shared_ptr<MemoryHandle> export_memory_handle(PFN_vkGetDeviceProcAddr gdpa,
                                                   VkDevice device,
                                                   VkDeviceMemory memory) {
  auto fn = reinterpret_cast<PFN_vkGetMemoryFdKHR>(
      gdpa(device, "vkGetMemoryFdKHR"));
  if (!fn)
    return nullptr;
  VkMemoryGetFdInfoKHR info{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, nullptr,
                            memory, memory_fd};
  int fd = -1;
  if (fn(device, &info, &fd) != VK_SUCCESS || fd < 0)
    return nullptr;
  return std::make_shared<FdMemory>(fd);
}
std::shared_ptr<VkBaseInStructure> import_memory_handle(const MemoryHandle &handle) {
  // Importing an FD transfers its ownership on success, so each import gets
  // its own duplicate; `handle` keeps the payload reference.
  const int fd = fcntl(static_cast<const FdMemory &>(handle).fd, F_DUPFD_CLOEXEC, 0);
  if (fd < 0)
    return nullptr;
  auto node = std::make_shared<ImportNode>();
  node->info = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, nullptr, memory_fd, fd};
  return std::shared_ptr<VkBaseInStructure>(
      node, reinterpret_cast<VkBaseInStructure *>(&node->info));
}
void memory_handle_imported(VkBaseInStructure &node) {
  reinterpret_cast<VkImportMemoryFdInfoKHR &>(node).fd = -1; // the driver owns it now
}
void release_external_device(VkDevice) {}
bool external_extension(std::string_view name) {
  return name == "VK_KHR_external_memory_fd" ||
         name == "VK_KHR_external_semaphore_fd";
}
void external_extensions(std::vector<VkExtensionProperties> &) {} // native on the host
const char *external_host_extension(const char *name) { return name; }
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
    auto type = reinterpret_cast<VkPhysicalDeviceExternalImageFormatInfo *>(p)
                    ->handleType;
    if (type && type != memory_ahb && type != memory_fd)
      return VK_ERROR_FORMAT_NOT_SUPPORTED;
    break;
  }
  case VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO:
    if (reinterpret_cast<VkExportSemaphoreCreateInfo *>(p)->handleTypes &
        ~VkExternalSemaphoreHandleTypeFlags(semaphore_fd))
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    break;
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
#undef FN
  return nullptr;
}
} // namespace venture::vulkan

// A consumer device's images over SharedImage memory (gfx/shared_image.h),
// cached by allocation. Header-only: XR, GL and test fixtures share it.
#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include <vulkan/vulkan.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <vulkan/vulkan_win32.h>
#else
#include <unistd.h>
#endif

#include "gfx/shared_image.h"

namespace venture::gfx {

class SharedImageImports {
 public:
  using Resolve = std::function<PFN_vkVoidFunction(const char*)>;
  SharedImageImports() = default;
  SharedImageImports(VkDevice device, const Resolve& resolve) { bind(device, resolve); }
  SharedImageImports(const SharedImageImports&) = delete;
  SharedImageImports& operator=(const SharedImageImports&) = delete;
  ~SharedImageImports() { clear(); }
  void bind(VkDevice device, const Resolve& resolve) {
    clear();
    device_ = device;
    create_image_ = reinterpret_cast<PFN_vkCreateImage>(resolve("vkCreateImage"));
    destroy_image_ = reinterpret_cast<PFN_vkDestroyImage>(resolve("vkDestroyImage"));
    allocate_ = reinterpret_cast<PFN_vkAllocateMemory>(resolve("vkAllocateMemory"));
    free_ = reinterpret_cast<PFN_vkFreeMemory>(resolve("vkFreeMemory"));
    bind_ = reinterpret_cast<PFN_vkBindImageMemory>(resolve("vkBindImageMemory"));
  }
  // The image (UNDEFINED layout on first import), or VK_NULL_HANDLE. Imports
  // of allocations the producer has freed are released here.
  VkImage import(const SharedImage& shared, VkResult* failure = nullptr) {
    if (!device_ || !create_image_ || !destroy_image_ || !allocate_ || !free_ || !bind_) return VK_NULL_HANDLE;
    ++clock_;
    std::erase_if(entries_, [this](Entry& entry) {
      if (!entry.allocation.expired()) return false;
      destroy(entry);
      return true;
    });
    for (auto& entry : entries_)
      if (entry.id == shared.id) {
        entry.used = clock_;
        return entry.image;
      }
    // A bound on producers that never free (each keeps a few in flight).
    constexpr std::size_t cached = 8;
    if (entries_.size() >= cached) {
      auto oldest = std::ranges::min_element(entries_, {}, &Entry::used);
      destroy(*oldest);
      entries_.erase(oldest);
    }
    Entry entry{shared.id, clock_, shared.allocation};
    VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, nullptr,
                                             shared.handle_type};
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &external};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VkFormat(shared.format);
    info.extent = {shared.width, shared.height, 1};
    info.mipLevels = info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = shared.usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    auto result = create_image_(device_, &info, nullptr, &entry.image);
    if (result != VK_SUCCESS) return fail(entry, result, failure);
    VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, nullptr, entry.image,
                                            VK_NULL_HANDLE};
#ifdef _WIN32
    // Importing an NT handle does not take ownership; the producer keeps it.
    VkImportMemoryWin32HandleInfoKHR handle{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR, nullptr,
        VkExternalMemoryHandleTypeFlagBits(shared.handle_type), reinterpret_cast<HANDLE>(shared.handle), nullptr};
#else
    // Importing an FD takes ownership of it; import a duplicate.
    VkImportMemoryFdInfoKHR handle{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, nullptr,
        VkExternalMemoryHandleTypeFlagBits(shared.handle_type), ::dup(int(shared.handle))};
    if (handle.fd < 0) return fail(entry, VK_ERROR_INVALID_EXTERNAL_HANDLE, failure);
#endif
    dedicated.pNext = &handle;
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &dedicated, shared.allocation_size,
                                  shared.memory_type};
    result = allocate_(device_, &allocate, nullptr, &entry.memory);
    if (result != VK_SUCCESS) {
#ifndef _WIN32
      ::close(handle.fd);
#endif
      return fail(entry, result, failure);
    }
    result = bind_(device_, entry.image, entry.memory, 0);
    if (result != VK_SUCCESS) return fail(entry, result, failure);
    entries_.push_back(entry);
    return entry.image;
  }
  void clear() {
    for (auto& entry : entries_) destroy(entry);
    entries_.clear();
  }

 private:
  struct Entry {
    std::uint64_t id = 0, used = 0;
    std::weak_ptr<const void> allocation;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
  };
  VkImage fail(Entry& entry, VkResult result, VkResult* failure) {
    destroy(entry);
    if (failure) *failure = result;
    return VK_NULL_HANDLE;
  }
  void destroy(Entry& entry) {
    if (entry.image) destroy_image_(device_, entry.image, nullptr);
    if (entry.memory) free_(device_, entry.memory, nullptr);
    entry.image = VK_NULL_HANDLE;
    entry.memory = VK_NULL_HANDLE;
  }
  VkDevice device_ = VK_NULL_HANDLE;
  PFN_vkCreateImage create_image_ = nullptr;
  PFN_vkDestroyImage destroy_image_ = nullptr;
  PFN_vkAllocateMemory allocate_ = nullptr;
  PFN_vkFreeMemory free_ = nullptr;
  PFN_vkBindImageMemory bind_ = nullptr;
  std::vector<Entry> entries_;
  std::uint64_t clock_ = 0;
};

}  // namespace venture::gfx

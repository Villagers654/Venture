#pragma once
#include <vulkan/vulkan_core.h>

#include <cstring>
#include <memory>
#include <vector>
#include <optional>
#include <type_traits>

#include "callbacks.h"

namespace venture::vulkan {

// Owns a translated copy; never mutates guest memory. Unknown sTypes are skipped
// without touching their payload. Known unsupported external-memory structures
// fail closed. VkExternalFormatANDROID{externalFormat=0} is dropped as the
// no-op the extension defines; nonzero external formats reach the overrides.
// 24-bit depth formats some hosts cannot attach (RADV offers no D24 depth).
inline bool depth_alias_candidate(VkFormat format) {
  return format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_X8_D24_UNORM_PACK32;
}
// The format the host creates for `format`: D24 depth becomes the 32-bit float
// depth format with the same aspects when the host attaches only the latter.
VkFormat host_depth_format(VkPhysicalDevice physical, VkFormat format);
// The guest D24 format of the VkImageCreateInfo this thread's current guest
// call translated to `host`, once; VK_FORMAT_UNDEFINED when none was.
VkFormat take_guest_depth_format(VkFormat host);
// Tests: substitute even where the host attaches D24 (when it attaches D32).
void force_depth_alias(bool forced);

class Chain {
 public:
  Chain();
  // Deferred queue calls outlive Invocation: also own ABI-identical leaf
  // arrays that ordinary synchronous translation is allowed to borrow.
  void retain_input_arrays() { retain_arrays_ = true; }
  void set_physical(VkPhysicalDevice physical) { physical_ = physical; device_ = VK_NULL_HANDLE; command_ = VK_NULL_HANDLE; density_.reset(); }
  void set_device(VkDevice device) { device_ = device; command_ = VK_NULL_HANDLE; physical_ = VK_NULL_HANDLE; density_.reset(); }
  void set_command(VkCommandBuffer command) { command_ = command; device_ = VK_NULL_HANDLE; physical_ = VK_NULL_HANDLE; density_.reset(); }
  void translate_format(VkFormat& format) { depth_format(format); }
  void translate_stages(VkPipelineStageFlags& flags);
  void translate_stages(VkPipelineStageFlags2& flags);
  void translate_layout(VkImageLayout& layout) { density_layout(layout); }
  const void* translate(const void* next);
  VkResult result() const { return result_; }

 private:
  struct Delete {
    void operator()(void* p) const { ::operator delete(p); }
  };
  std::vector<std::unique_ptr<void, Delete>> copies_;
  std::vector<std::shared_ptr<DebugCallback>> callbacks_;
  VkResult result_ = VK_SUCCESS;
  bool retain_arrays_ = false;
  unsigned depth_ = 0;
  VkDevice device_{};
  VkCommandBuffer command_{};
  VkPhysicalDevice physical_{};
  std::optional<bool> density_;
  void depth_format(VkFormat& value);
  bool density_enabled();
  void density_layout(VkImageLayout& value);
  void density_usage(VkImageUsageFlags& value);
  void density_usage(VkImageUsageFlags2KHR& value);
  void density_flags(VkImageCreateFlags& value);
  void density_view_flags(VkImageViewCreateFlags& value);
  void density_sampler_flags(VkSamplerCreateFlags& value);
  void density_pipeline_flags(VkPipelineCreateFlags& value);
  void density_pipeline_flags(VkPipelineCreateFlags2& value);
  void density_stages(VkPipelineStageFlags& value);
  void density_stages(VkPipelineStageFlags2& value);
  void density_access(VkAccessFlags& value);
  void density_access(VkAccessFlags2& value);
  void translate_members(VkBaseOutStructure* base);
  template<class T> void translate_plain(T& v) {
#include "vk_plain_members.inc"
  }
  template <class T>
  void copy_value(T& value) {
    if constexpr (requires { value.sType; value.pNext; }) {
      auto* copy = static_cast<const T*>(translate(&value));
      if (copy) value = *copy;
    } else {
      translate_plain(value);
    }
  }
  template <class T>
  const T* copy_array(const T* input, std::size_t count) {
    if (!input || !count) return input;
    if (count > 1048576) {
      result_ = VK_ERROR_OUT_OF_HOST_MEMORY;
      return nullptr;
    }
    auto* data = static_cast<T*>(::operator new(sizeof(T) * count));
    copies_.emplace_back(data);
    std::memcpy(data, input, sizeof(T) * count);
    for (std::size_t i = 0; i < count; ++i) copy_value(data[i]);
    return data;
  }
};

std::size_t structure_size(VkStructureType type);
bool structure_translatable(VkStructureType type);
// No allocation or mutation. Every known node and nested struct array must be
// ABI-identical and free of callback/platform translation. Unknown nodes retain
// the original copying/skipping path; bounded walks reject cycles and huge graphs.
bool structure_borrowable(VkStructureType type);
bool borrowable_chain(const void* next);
template <class T>
bool borrowable_array(const T* input, std::size_t count) {
  if (count > 1048576) return false;
  for (std::size_t i = 0; input && i < count; ++i)
    if (!borrowable_chain(input + i)) return false;
  return true;
}

}  // namespace venture::vulkan

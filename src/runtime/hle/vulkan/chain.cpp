#include "chain.h"
#include "external.h"
#include "internal.h"
#include "full_rate_image.h"

#include <vulkan/vulkan_android.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "common/log.h"

namespace venture::vulkan {
// Vulkan-Headers vk_platform.h/vulkan_core.h: AArch64 and host x64 use the
// 64-bit handle ABI, 32-bit enums/flags, and an 8-byte-aligned pNext pointer.
static_assert(sizeof(void*) == 8 && sizeof(VkDeviceSize) == 8 && sizeof(VkResult) == 4);
static_assert(offsetof(VkBaseInStructure, pNext) == 8);
std::size_t structure_size(VkStructureType type) {
#include "vk_structs.inc"
}
bool structure_translatable(VkStructureType type) {
  switch (type) {
    // Android hardware buffer structures are consumed by hardware_buffer.cpp.
    case VK_STRUCTURE_TYPE_DEVICE_DEVICE_MEMORY_REPORT_CREATE_INFO_EXT:
    case VK_STRUCTURE_TYPE_DIRECT_DRIVER_LOADING_LIST_LUNARG:
    case VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT:
      return false;
    default:
      return structure_size(type) != 0;
  }
}
bool structure_borrowable(VkStructureType type) {
#include "vk_borrow_traits.inc"
}
namespace {
std::string_view structure_extensions(VkStructureType type) {
#include "vk_structure_extensions.inc"
}
bool density_structure(VkStructureType type) {
  const auto extensions = structure_extensions(type);
  return extensions.find("VK_EXT_fragment_density_map|") != std::string_view::npos ||
         extensions.find("VK_EXT_fragment_density_map2|") != std::string_view::npos;
}
}
bool Chain::density_enabled() {
  if (!device_ && !command_) return true;
  if (!density_) density_ = device_density_maps_enabled(device_ ? device_ : command_device(command_));
  return *density_;
}
void Chain::density_layout(VkImageLayout& value) {
  if (value == VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT && !density_enabled()) value = VK_IMAGE_LAYOUT_GENERAL;
}
void Chain::density_usage(VkImageUsageFlags& value) {
  if ((value & VK_IMAGE_USAGE_FRAGMENT_DENSITY_MAP_BIT_EXT) && !density_enabled()) {
    value &= ~VK_IMAGE_USAGE_FRAGMENT_DENSITY_MAP_BIT_EXT;
    if (!value) value = VK_IMAGE_USAGE_SAMPLED_BIT;
  }
}
void Chain::density_usage(VkImageUsageFlags2KHR& value) {
  if ((value & VK_IMAGE_USAGE_2_FRAGMENT_DENSITY_MAP_BIT_EXT) && !density_enabled()) {
    value &= ~VK_IMAGE_USAGE_2_FRAGMENT_DENSITY_MAP_BIT_EXT;
    if (!value) value = VK_IMAGE_USAGE_2_SAMPLED_BIT_KHR;
  }
}
void Chain::density_flags(VkImageCreateFlags& value) {
  if ((value & VK_IMAGE_CREATE_SUBSAMPLED_BIT_EXT) && !density_enabled()) value &= ~VK_IMAGE_CREATE_SUBSAMPLED_BIT_EXT;
}
void Chain::density_view_flags(VkImageViewCreateFlags& value) {
  constexpr VkImageViewCreateFlags mask = VK_IMAGE_VIEW_CREATE_FRAGMENT_DENSITY_MAP_DYNAMIC_BIT_EXT |
                                         VK_IMAGE_VIEW_CREATE_FRAGMENT_DENSITY_MAP_DEFERRED_BIT_EXT;
  if ((value & mask) && !density_enabled()) value &= ~mask;
}
void Chain::density_sampler_flags(VkSamplerCreateFlags& value) {
  constexpr VkSamplerCreateFlags mask = VK_SAMPLER_CREATE_SUBSAMPLED_BIT_EXT |
                                       VK_SAMPLER_CREATE_SUBSAMPLED_COARSE_RECONSTRUCTION_BIT_EXT;
  if ((value & mask) && !density_enabled()) value &= ~mask;
}
void Chain::density_pipeline_flags(VkPipelineCreateFlags& value) {
  if ((value & VK_PIPELINE_CREATE_RENDERING_FRAGMENT_DENSITY_MAP_ATTACHMENT_BIT_EXT) && !density_enabled())
    value &= ~VK_PIPELINE_CREATE_RENDERING_FRAGMENT_DENSITY_MAP_ATTACHMENT_BIT_EXT;
}
void Chain::density_pipeline_flags(VkPipelineCreateFlags2& value) {
  if ((value & VK_PIPELINE_CREATE_2_RENDERING_FRAGMENT_DENSITY_MAP_ATTACHMENT_BIT_EXT) && !density_enabled())
    value &= ~VK_PIPELINE_CREATE_2_RENDERING_FRAGMENT_DENSITY_MAP_ATTACHMENT_BIT_EXT;
}
// Without the host feature the density map is never read: the translated
// render pass has no density role. Its stage and access therefore name no host
// work. Drop them instead of widening to ALL_COMMANDS/MEMORY_READ, which made
// every barrier batch touching the guest's density image a full GPU drain. A
// mask that named only the density stage keeps the earliest graphics stage,
// so barrier chains on the density image (layout transitions) still connect
// without waiting for any rasterization work.
void Chain::density_stages(VkPipelineStageFlags& value) {
  if ((value & VK_PIPELINE_STAGE_FRAGMENT_DENSITY_PROCESS_BIT_EXT) && !density_enabled()) {
    value &= ~VK_PIPELINE_STAGE_FRAGMENT_DENSITY_PROCESS_BIT_EXT;
    if (!value) value = VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT;
  }
}
void Chain::density_stages(VkPipelineStageFlags2& value) {
  if ((value & VK_PIPELINE_STAGE_2_FRAGMENT_DENSITY_PROCESS_BIT_EXT) && !density_enabled()) {
    value &= ~VK_PIPELINE_STAGE_2_FRAGMENT_DENSITY_PROCESS_BIT_EXT;
    if (!value) value = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
  }
}
namespace {
std::atomic<bool> depth_alias_forced{false};
}
void force_depth_alias(bool forced) { depth_alias_forced.store(forced); }
VkFormat host_depth_format(VkPhysicalDevice physical, VkFormat format) {
  if (!physical || !depth_alias_candidate(format)) return format;
  struct Aliases { VkFormat d24s8, x8d24; bool d24s8_native, x8d24_native; };
  static std::mutex lock;
  static std::unordered_map<VkPhysicalDevice, Aliases> cache;
  std::lock_guard guard(lock);
  auto it = cache.find(physical);
  if (it == cache.end()) {
    auto query = reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(
        raw_physical_proc(physical, "vkGetPhysicalDeviceFormatProperties"));
    auto attaches = [&](VkFormat f) {
      VkFormatProperties properties{};
      query(physical, f, &properties);
      return (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
    };
    auto alias = [&](VkFormat guest, VkFormat wide) {
      if (!attaches(wide)) return guest;
      if (!attaches(guest)) log::info("vulkan", "host attaches no format {}; guest images use format {}", int(guest), int(wide));
      return wide;
    };
    it = cache.emplace(physical, Aliases{alias(VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT),
                                         alias(VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_D32_SFLOAT),
                                         attaches(VK_FORMAT_D24_UNORM_S8_UINT),
                                         attaches(VK_FORMAT_X8_D24_UNORM_PACK32)}).first;
  }
  const auto& a = it->second;
  const bool forced = depth_alias_forced.load(std::memory_order_relaxed);
  if (format == VK_FORMAT_D24_UNORM_S8_UINT) return a.d24s8_native && !forced ? format : a.d24s8;
  return a.x8d24_native && !forced ? format : a.x8d24;
}
namespace {
// Set by the image create info a guest call translated; vkCreateImage takes it
// on the same thread, which records the image for depth copy conversion.
thread_local struct { VkFormat guest, host; } pending_depth{};
}
Chain::Chain() { pending_depth = {}; }
VkFormat take_guest_depth_format(VkFormat host) {
  const auto pending = std::exchange(pending_depth, {});
  return pending.host == host ? pending.guest : VK_FORMAT_UNDEFINED;
}
void Chain::depth_format(VkFormat& value) {
  if (!depth_alias_candidate(value)) return;
  auto physical = physical_;
  if (!physical && (device_ || command_)) physical = device_physical(device_ ? device_ : command_device(command_));
  value = host_depth_format(physical, value);
}
void Chain::translate_stages(VkPipelineStageFlags& value) { density_stages(value); }
void Chain::translate_stages(VkPipelineStageFlags2& value) { density_stages(value); }
void Chain::density_access(VkAccessFlags& value) {
  if ((value & VK_ACCESS_FRAGMENT_DENSITY_MAP_READ_BIT_EXT) && !density_enabled())
    value &= ~VK_ACCESS_FRAGMENT_DENSITY_MAP_READ_BIT_EXT;
}
void Chain::density_access(VkAccessFlags2& value) {
  if ((value & VK_ACCESS_2_FRAGMENT_DENSITY_MAP_READ_BIT_EXT) && !density_enabled())
    value &= ~VK_ACCESS_2_FRAGMENT_DENSITY_MAP_READ_BIT_EXT;
}
namespace {
bool borrow_chain(const void* next, unsigned depth, std::size_t& budget);
template<class T> bool borrow_value(const T& v, unsigned depth, std::size_t& budget);
template <class T>
bool borrow_array(const T* input, std::size_t count, unsigned depth, std::size_t& budget) {
  if (count > budget) return false;
  for (std::size_t i = 0; input && i < count; ++i)
    if (!borrow_value(input[i], depth, budget)) return false;
  return true;
}
template<class T> bool borrow_value(const T& v, unsigned depth, std::size_t& budget) {
  if constexpr (requires { v.sType; v.pNext; }) {
    return borrow_chain(&v, depth, budget);
  } else {
    if (depth >= 64 || !budget) return false;
    --budget;
#include "vk_plain_borrow.inc"
  }
}
bool borrow_members(const VkBaseInStructure* base, unsigned depth, std::size_t& budget) {
  // KHR shading-rate nodes can refer to an XR two-channel density map rather
  // than a native R8_UINT shading-rate image. Inspect the copied render pass.
  if (base->sType == VK_STRUCTURE_TYPE_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR) return false;
  if (base->sType == VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER) {
    const auto& v = *reinterpret_cast<const VkImageMemoryBarrier*>(base);
    if (v.oldLayout == VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR ||
        v.newLayout == VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR) return false;
  }
  if (base->sType == VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2) {
    const auto& v = *reinterpret_cast<const VkImageMemoryBarrier2*>(base);
    if (v.oldLayout == VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR ||
        v.newLayout == VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR) return false;
  }
  if (base->sType == VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT ||
      base->sType == VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_TAG_INFO_EXT ||
      base->sType == VK_STRUCTURE_TYPE_DEBUG_MARKER_OBJECT_NAME_INFO_EXT ||
      base->sType == VK_STRUCTURE_TYPE_DEBUG_MARKER_OBJECT_TAG_INFO_EXT) return false;
  if (density_structure(base->sType)) return false;
  if (!structure_translatable(base->sType)) return false;
#include "vk_borrow_members.inc"
}
bool borrow_chain(const void* next, unsigned depth, std::size_t& budget) {
  if (depth >= 64) return false;
  unsigned length = 0;
  for (auto* base = static_cast<const VkBaseInStructure*>(next); base; base = base->pNext) {
    if (++length > 256 || !budget) return false;
    --budget;
    if (!borrow_members(base, depth + 1, budget)) return false;
  }
  return true;
}
}
bool borrowable_chain(const void* next) {
  std::size_t budget = 1048576;
  return borrow_chain(next, 0, budget);
}
const void* Chain::translate(const void* next) {
  if (++depth_ > 64) {
    --depth_;
    result_ = VK_ERROR_UNKNOWN;
    log::error("vulkan", "recursive Vulkan structure graph");
    return nullptr;
  }
  struct Depth {
    unsigned& value;
    ~Depth() { --value; }
  } depth{depth_};
  VkBaseOutStructure* first = nullptr;
  VkBaseOutStructure* tail = nullptr;
  std::unordered_set<const void*> visited;
  for (auto* src = static_cast<const VkBaseInStructure*>(next); src; src = src->pNext) {
    if (!visited.insert(src).second || visited.size() > 256) {
      result_ = VK_ERROR_UNKNOWN;
      log::error("vulkan", "cyclic or oversized pNext chain");
      return nullptr;
    }
    if (src->sType == VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID &&
        reinterpret_cast<const VkExternalFormatANDROID*>(src)->externalFormat == 0)
      continue;
    // Registry ownership, not title or vendor identity. The guest-facing XR
    // density interface is an identity map; hosts without the feature must see
    // an ordinary full-rate render pass, never an unsupported attachment role.
    if (density_structure(src->sType) && !density_enabled()) continue;
    // Vulkan fundamentals: unknown extending structures are skipped using only
    // their sType/pNext header. Never guess their size or copy their payload.
    const auto size = structure_size(src->sType);
    if (!size) continue;
    if (!structure_translatable(src->sType)) {
      result_ = VK_ERROR_EXTENSION_NOT_PRESENT;
      log::error("vulkan", "unsupported pNext structure {}", static_cast<int>(src->sType));
      return nullptr;
    }
    auto* copy = static_cast<VkBaseOutStructure*>(::operator new(size));
    copies_.emplace_back(copy);
    std::memcpy(copy, src, size);
    copy->pNext = nullptr;
    translate_members(copy);
    if (copy->sType == VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT) {
      auto& info = *reinterpret_cast<VkDebugUtilsObjectNameInfoEXT*>(copy);
      if (info.objectType == VK_OBJECT_TYPE_QUEUE)
        info.objectHandle = h2g(native_queue(g2h<VkQueue_T>(info.objectHandle)));
    } else if (copy->sType == VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_TAG_INFO_EXT) {
      auto& info = *reinterpret_cast<VkDebugUtilsObjectTagInfoEXT*>(copy);
      if (info.objectType == VK_OBJECT_TYPE_QUEUE)
        info.objectHandle = h2g(native_queue(g2h<VkQueue_T>(info.objectHandle)));
    } else if (copy->sType == VK_STRUCTURE_TYPE_DEBUG_MARKER_OBJECT_NAME_INFO_EXT) {
      auto& info = *reinterpret_cast<VkDebugMarkerObjectNameInfoEXT*>(copy);
      if (info.objectType == VK_DEBUG_REPORT_OBJECT_TYPE_QUEUE_EXT)
        info.object = h2g(native_queue(g2h<VkQueue_T>(info.object)));
    } else if (copy->sType == VK_STRUCTURE_TYPE_DEBUG_MARKER_OBJECT_TAG_INFO_EXT) {
      auto& info = *reinterpret_cast<VkDebugMarkerObjectTagInfoEXT*>(copy);
      if (info.objectType == VK_DEBUG_REPORT_OBJECT_TYPE_QUEUE_EXT)
        info.object = h2g(native_queue(g2h<VkQueue_T>(info.object)));
    }
    if (result_ != VK_SUCCESS) return nullptr;
    hardware_buffer_chain(copy);
    result_ = sync_fd_chain(copy);
    if (result_ == VK_SUCCESS) result_ = external_chain(copy);
    if (result_ != VK_SUCCESS) return nullptr;
    if (src->sType == VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT) {
      auto& info = *reinterpret_cast<VkDebugUtilsMessengerCreateInfoEXT*>(copy);
      if (info.pfnUserCallback != debug_utils_callback) {
        auto cb = debug_callback(reinterpret_cast<GuestAddr>(info.pfnUserCallback), h2g(info.pUserData));
        info.pfnUserCallback = debug_utils_callback;
        info.pUserData = cb.get();
        callbacks_.push_back(std::move(cb));
      }
    } else if (src->sType == VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT) {
      auto& info = *reinterpret_cast<VkDebugReportCallbackCreateInfoEXT*>(copy);
      if (info.pfnCallback != debug_report_callback) {
        auto cb = debug_callback(reinterpret_cast<GuestAddr>(info.pfnCallback), h2g(info.pUserData));
        info.pfnCallback = debug_report_callback;
        info.pUserData = cb.get();
        callbacks_.push_back(std::move(cb));
      }
    }
    if (tail)
      tail->pNext = copy;
    else
      first = copy;
    tail = copy;
  }
  return first;
}
void Chain::translate_members(VkBaseOutStructure* base) {
  auto* image = base->sType == VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO ? reinterpret_cast<VkImageCreateInfo*>(base) : nullptr;
  const auto guest_format = image ? image->format : VK_FORMAT_UNDEFINED;
#include "vk_members.inc"
  if (image && image->format != guest_format) pending_depth = {guest_format, image->format};
  if (base->sType == VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2) {
    auto& pass = *reinterpret_cast<VkRenderPassCreateInfo2*>(base);
    for (std::uint32_t i = 0; i < pass.subpassCount; ++i) {
      auto* link = reinterpret_cast<VkBaseOutStructure*>(const_cast<VkSubpassDescription2*>(pass.pSubpasses + i));
      while (auto* node = link->pNext) {
        if (node->sType == VK_STRUCTURE_TYPE_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR) {
          const auto* ref = reinterpret_cast<const VkFragmentShadingRateAttachmentInfoKHR*>(node)->pFragmentShadingRateAttachment;
          if (ref && ref->attachment < pass.attachmentCount &&
              pass.pAttachments[ref->attachment].format == VK_FORMAT_R8G8_UNORM) {
            // XR_FB_foveation_vulkan supplies R8G8 density, including our
            // immutable (1,1) fallback. It is not KHR's R8_UINT rate encoding.
            // Consuming it as a native shading-rate attachment can fault the
            // GPU. An identity density map means full-rate rendering; omit
            // this role while preserving attachment indices and real KHR maps.
            auto& attachment = const_cast<VkAttachmentDescription2&>(pass.pAttachments[ref->attachment]);
            if (attachment.initialLayout == VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR)
              attachment.initialLayout = VK_IMAGE_LAYOUT_GENERAL;
            if (attachment.finalLayout == VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR)
              attachment.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
            link->pNext = node->pNext;
            continue;
          }
        }
        link = node;
      }
    }
  }
  auto identity_layout = [&](auto& barrier) {
    if (!device_ && !command_) return;
    if (barrier.oldLayout != VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR &&
        barrier.newLayout != VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR) return;
    if (auto layout = full_rate_image_layout(device_ ? device_ : command_device(command_), barrier.image)) {
      if (barrier.oldLayout == VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR) barrier.oldLayout = *layout;
      if (barrier.newLayout == VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR) barrier.newLayout = *layout;
    }
  };
  if (base->sType == VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER)
    identity_layout(*reinterpret_cast<VkImageMemoryBarrier*>(base));
  if (base->sType == VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2)
    identity_layout(*reinterpret_cast<VkImageMemoryBarrier2*>(base));
}
}  // namespace venture::vulkan

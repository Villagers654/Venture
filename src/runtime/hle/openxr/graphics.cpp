#include "xr/dev_dump.h"
#include "gfx/gpu_device.h"
#include "common/log.h"
#include "hle/vulkan/vulkan.h"
#include "hle/vulkan/internal.h"
#include "hle/vulkan/chain.h"
#include "hle/openxr/state.h"
#include "xr/queue.h"
#include "xr/vulkan_extensions.h"
#include "hle/vulkan/timing.h"
#include "core/wait.h"
#include "platform/profile.h"
#include <algorithm>
#include <atomic>
#include <bit>
#include <cstring>
#include <sstream>

namespace venture::hle::openxr {
namespace {
// A host operation pins this map entry and its parent session while the API
// lock is dropped. Destruction and another wait/release reject unsynchronized
// access; other swapchains and frame APIs can continue. Unwind restores state.
struct ImageOperation {
  Swapchain& image;
  std::unique_lock<std::recursive_mutex>& api;
  ImageOperation(Swapchain& image, std::unique_lock<std::recursive_mutex>& api)
      : image(image), api(api) { image.operation_in_progress = true; }
  ~ImageOperation() {
    if (!api.owns_lock()) api.lock();
    image.operation_in_progress = false;
  }
};
XrResult validate_image_next(const void* next, XrStructureType parent) {
  std::set<const void*> seen;
  // OpenXR "Valid Usage for Structure Pointer Chains" applies to output
  // chains too. Preserve unknown and disabled payloads and their headers.
  for (auto p = static_cast<const XrBaseInStructure*>(next); p; p = p->next) {
    if (!seen.insert(p).second || seen.size() > 64) return XR_ERROR_VALIDATION_FAILURE;
    const StructureInfo* metadata = nullptr;
    for (const auto& m : structure_metadata())
      if (m.type == p->type) { metadata = &m; break; }
    if (!metadata || (*metadata->extension && !enabled(metadata->extension))) continue;
    if (p->type == XR_TYPE_SWAPCHAIN_IMAGE_FOVEATION_VULKAN_FB &&
        parent == XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR) continue;
    return extends_structure(*metadata, parent) ? XR_ERROR_FEATURE_UNSUPPORTED
                                               : XR_ERROR_VALIDATION_FAILURE;
  }
  return XR_SUCCESS;
}
bool srgb_format(std::int64_t format) {
  switch (static_cast<VkFormat>(format)) {
  case VK_FORMAT_R8_SRGB: case VK_FORMAT_R8G8_SRGB: case VK_FORMAT_R8G8B8_SRGB: case VK_FORMAT_B8G8R8_SRGB:
  case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B8G8R8A8_SRGB: case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
    return true;
  default:
    return false;
  }
}
}  // namespace
// XR_META_vulkan_swapchain_create_info: Vulkan image flags beyond the
// swapchain usage flags, expressed through the core usage bits every runtime
// implements (a mutable format is how applications view SRGB images as UNORM).
// Bits without a core equivalent are refused, as the extension requires.
XrResult guest::vulkan_swapchain_flags(const XrVulkanSwapchainCreateInfoMETA& info, XrSwapchainUsageFlags& usage) {
  constexpr std::pair<VkImageUsageFlags, XrSwapchainUsageFlags> usages[] = {
      {VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT},
      {VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT},
      {VK_IMAGE_USAGE_STORAGE_BIT, XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT},
      {VK_IMAGE_USAGE_TRANSFER_SRC_BIT, XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT},
      {VK_IMAGE_USAGE_TRANSFER_DST_BIT, XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT},
      {VK_IMAGE_USAGE_SAMPLED_BIT, XR_SWAPCHAIN_USAGE_SAMPLED_BIT},
      {VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT, XR_SWAPCHAIN_USAGE_INPUT_ATTACHMENT_BIT_KHR},
  };
  VkImageUsageFlags unmapped_usage = info.additionalUsageFlags;
  for (const auto& [vk, xr] : usages)
    if (unmapped_usage & vk) {
      usage |= xr;
      unmapped_usage &= ~vk;
    }
  VkImageCreateFlags unmapped_create = info.additionalCreateFlags;
  if (unmapped_create & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) {
    usage |= XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;
    unmapped_create &= ~VkImageCreateFlags(VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT);
  }
  // This bridge keeps color images full-rate, including the private immutable
  // resource proof. Subsampled allocation is unnecessary at unit density.
  if (unmapped_create & VK_IMAGE_CREATE_SUBSAMPLED_BIT_EXT) {
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) log::info("xr", "Subsampled swapchain request uses full-resolution color images");
    unmapped_create &= ~VkImageCreateFlags(VK_IMAGE_CREATE_SUBSAMPLED_BIT_EXT);
  }
  if (!unmapped_usage && !unmapped_create) return XR_SUCCESS;
  log::warn("xr", "Unsupported Vulkan swapchain flags: create={:#x} usage={:#x}", unmapped_create, unmapped_usage);
  return XR_ERROR_FEATURE_UNSUPPORTED;
}
namespace {
// The guest renders and resolves into the runtime's Vulkan eye images without
// creating them; the Vulkan layer keeps framebuffers inside their extents.
void note_images(const Swapchain& r) {
  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ci.extent = {r.info.width, r.info.height, 1};
  if (r.info.usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) ci.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  if (r.info.usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
    ci.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
  if (r.info.usageFlags & XR_SWAPCHAIN_USAGE_INPUT_ATTACHMENT_BIT_KHR) ci.usage |= VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
  for (const auto& image : r.images) vulkan::note_image(image.image, ci);
}
void forget_images(const Swapchain& r) {
  for (const auto& image : r.images) vulkan::forget_image(image.image);
}
}
VkFormat gl_to_vk(std::int64_t format) {
  switch (format) {
  case 0x8058:
    return VK_FORMAT_R8G8B8A8_UNORM;
  case 0x8c43:
    return VK_FORMAT_R8G8B8A8_SRGB;
  case 0x881a:
    return VK_FORMAT_R16G16B16A16_SFLOAT;
  case 0x8059:
    return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
  case 0x8cac:
    return VK_FORMAT_D32_SFLOAT;
  case 0x88f0:
    return VK_FORMAT_D24_UNORM_S8_UINT;
  case 0x81a5:
    return VK_FORMAT_D16_UNORM;
  default:
    return VK_FORMAT_UNDEFINED;
  }
}
std::int64_t vk_to_gl(std::int64_t format) {
  for (auto gl : {0x8058, 0x8c43, 0x881a, 0x8059, 0x8cac, 0x88f0, 0x81a5})
    if (gl_to_vk(gl) == format)
      return gl;
  return 0;
}
XrResult convert_color_image(Swapchain& r, std::uint32_t index, XrColorSpaceFB space,
                             bool external_queue_held) {
  if (!enabled("XR_FB_color_space") ||
      host_xr().enabled_extensions.contains("XR_FB_color_space") ||
      space == XR_COLOR_SPACE_REC709_FB || space == XR_COLOR_SPACE_UNMANAGED_FB ||
      !(r.info.usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT)) return XR_SUCCESS;
  if (!r.color_images)
    r.color_images = std::make_unique<ColorImages>(state().graphics,
        gfx::gpu_device().get_instance_proc_addr, r.host_info, r.host_images);
  const auto matrix = color_matrix(space, XR_COLOR_SPACE_REC709_FB);
  // Snapshot to a Venture-owned VkImage, then render back into the acquired
  // host image. No extra XR swapchain: every submitted layer keeps its own
  // released image, including static images and simultaneously visible quads.
  return r.color_images->convert(index, matrix, external_queue_held);
}
XrResult destroy_host_swapchain(Swapchain& r) {
  if (!r.host_handle) return XR_SUCCESS;
  const auto drained = r.backing->drain();
  if (drained != XR_SUCCESS) return drained;
  r.color_images.reset();
  HostQueueLock queue(session().uses_shared_gpu);
  if (!queue) return XR_ERROR_RUNTIME_FAILURE;
  const auto result = venture::xr::dev_destroy_swapchain(r.host_handle);
  if (XR_SUCCEEDED(result)) {
    r.host_handle = XR_NULL_HANDLE;
    r.host_images.clear();
    r.host_serial = 0;
    r.host_color = XR_COLOR_SPACE_UNMANAGED_FB;
    r.host_flip = false;
  }
  return result;
}
XrResult prepare_swapchains(const std::set<XrSwapchain>& submitted) {
  auto& s = state();
  XrResult qualified = XR_SUCCESS;
  const auto live_hosts = [&] {
    return std::count_if(s.swapchains.begin(), s.swapchains.end(),
        [](const auto& entry) { return bool(entry.second.host_handle); });
  };
  auto evict = [&]() -> XrResult {
    Swapchain* oldest = nullptr;
    for (auto& [handle, chain] : s.swapchains) {
      // Pin the entire frame before attempting any allocation. Two eyes,
      // depth, cube and quad references may all alias the same guest chain.
      if (!chain.host_handle || chain.operation_in_progress || submitted.contains(handle)) continue;
      if (!oldest || chain.last_submitted < oldest->last_submitted) oldest = &chain;
    }
    if (!oldest) return XR_ERROR_LIMIT_REACHED;
    const auto result = destroy_host_swapchain(*oldest);
    if (XR_SUCCEEDED(result)) {
      ++s.host_evictions;
      log::info("xr-budget", "recycled inactive host swapchain; live={} learned_limit={}", live_hosts(), s.host_swapchain_limit);
    }
    return result;
  };
  // First allocate all host handles; never evict a member of this frame while
  // preparing another member. The limit is learned solely from host results.
  for (auto handle : submitted) {
    auto& r = s.swapchains.at(handle);
    if (r.operation_in_progress) return XR_ERROR_CALL_ORDER_INVALID;
    if (r.released_index < 0) return XR_ERROR_LAYER_INVALID;
    if (r.host_handle) continue;
    r.host_info = r.info;
    r.host_info.next = nullptr;
    r.host_info.createFlags &= ~XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT;
    // Guest images keep every mip; the host compositor owns filtering. Some hosts
    // accept a mipmapped request but allocate one level (SteamVR), as in VrApi.
    r.host_info.mipCount = 1;
    // Fit the host's maximum with the guest aspect; the copy scales and layer
    // rects follow (map_subimage). Sizes within the maximum stay 1:1. A zero
    // maximum is no limit: the Oculus PC runtime (Link 2.18) reports 0x0.
    XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES};
    if (XR_SUCCEEDED(::xrGetSystemProperties(host_xr().instance, host_xr().system, &properties)) &&
        properties.graphicsProperties.maxSwapchainImageWidth && properties.graphicsProperties.maxSwapchainImageHeight) {
      const auto& limits = properties.graphicsProperties;
      const double scale = std::min({1.0, double(limits.maxSwapchainImageWidth) / r.info.width,
                                     double(limits.maxSwapchainImageHeight) / r.info.height});
      if (scale < 1.0) {
        r.host_info.width = std::max(1u, std::uint32_t(r.info.width * scale));
        r.host_info.height = std::max(1u, std::uint32_t(r.info.height * scale));
        log::info("xr", "guest swapchain {}x{} exceeds the host maximum {}x{}; presenting through {}x{}",
                  r.info.width, r.info.height, limits.maxSwapchainImageWidth,
                  limits.maxSwapchainImageHeight, r.host_info.width, r.host_info.height);
      }
    }
    r.host_info.usageFlags |= XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT;
    // Match the Vulkan attachment-layout contract even for transfer-only chains.
    switch (VkFormat(r.info.format)) {
    case VK_FORMAT_D16_UNORM: case VK_FORMAT_X8_D24_UNORM_PACK32: case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_D16_UNORM_S8_UINT: case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_D32_SFLOAT_S8_UINT:
    case VK_FORMAT_S8_UINT: r.host_info.usageFlags |= XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT; break;
    default: r.host_info.usageFlags |= XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT; break;
    }
    for (;;) {
      if (s.host_swapchain_limit && std::size_t(live_hosts()) >= s.host_swapchain_limit) {
        const auto result = evict();
        if (XR_FAILED(result)) return result;
      }
      XrResult result;
      {
        HostQueueLock queue(session().uses_shared_gpu);
        if (!queue) return XR_ERROR_RUNTIME_FAILURE;
        result = s.gles ? venture::xr::create_host_swapchain(session().handle, r.host_info, &r.host_handle)
                       : venture::xr::dev_create_swapchain(session().handle, &r.host_info, &r.host_handle);
      }
      if (result != XR_ERROR_LIMIT_REACHED) {
        if (XR_FAILED(result)) return result;
        if (result == XR_SESSION_LOSS_PENDING) qualified = result;
        break;
      }
      s.host_swapchain_limit = std::size_t(live_hosts());
      const auto recycled = evict();
      if (XR_FAILED(recycled)) return recycled;
    }
    std::uint32_t count{};
    auto result = ::xrEnumerateSwapchainImages(r.host_handle, 0, &count, nullptr);
    if (XR_SUCCEEDED(result) && !count) result = XR_ERROR_RUNTIME_FAILURE;
    if (XR_SUCCEEDED(result)) {
      r.host_images.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
      result = ::xrEnumerateSwapchainImages(r.host_handle, count, &count,
          reinterpret_cast<XrSwapchainImageBaseHeader*>(r.host_images.data()));
    }
    if (XR_SUCCEEDED(result) && r.sampler_updated) result = update_host_sampler(r.host_handle, r.sampler_host);
    if (XR_FAILED(result)) { destroy_host_swapchain(r); return result; }
  }
  ++s.presentation_epoch;
  for (auto handle : submitted) {
    auto& r = s.swapchains.at(handle);
    r.last_submitted = s.presentation_epoch;
    if (r.host_serial == r.release_serial && r.host_color == s.color_space && r.host_flip == r.flip_rows) continue;
    std::uint32_t target{};
    {
      HostQueueLock queue(session().uses_shared_gpu);
      if (!queue) return XR_ERROR_RUNTIME_FAILURE;
      const auto result = ::xrAcquireSwapchainImage(r.host_handle, nullptr, &target);
      if (XR_FAILED(result)) return result;
      if (result == XR_SESSION_LOSS_PENDING) qualified = result;
    }
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, XR_INFINITE_DURATION};
    auto result = ::xrWaitSwapchainImage(r.host_handle, &wait);
    // LOSS_PENDING simulates success: the image is available and retains its
    // layout guarantee. A timeout, unlike qualified success, is not ready.
    if (result != XR_SUCCESS && result != XR_SESSION_LOSS_PENDING) {
      destroy_host_swapchain(r); return result;
    }
    if (result == XR_SESSION_LOSS_PENDING) qualified = result;
    result = r.backing->copy(std::uint32_t(r.released_index), r.host_images.at(target).image, VkFormat(r.host_info.format),
                             {r.host_info.width, r.host_info.height}, r.flip_rows);
    if (XR_SUCCEEDED(result)) result = convert_color_image(r, target, s.color_space);
    // Always balance an acquisition after a copy/conversion failure. The
    // serial is advanced only after a successful release; retries upload again.
    {
      HostQueueLock queue(session().uses_shared_gpu);
      if (!queue) return XR_ERROR_RUNTIME_FAILURE;
      const auto released = venture::xr::dev_release_swapchain(r.host_handle, nullptr, r.host_images.at(target).image);
      if (XR_SUCCEEDED(result)) result = released;
    }
    if (XR_FAILED(result)) return result;
    if (result == XR_SESSION_LOSS_PENDING) qualified = result;
    r.host_index = target;
    r.host_serial = r.release_serial;
    r.host_color = s.color_space;
    r.host_flip = r.flip_rows;
  }
  return qualified;
}
namespace guest {
XrResult xrGetOpenGLESGraphicsRequirementsKHR(XrInstance instance, XrSystemId system,
                                              GlesRequirements *out) {
  std::lock_guard lock(state().mutex);
  if (!valid_handle(instance, "XrInstance"))
    return XR_ERROR_HANDLE_INVALID;
  if (!enabled("XR_KHR_opengl_es_enable") || !xr_images())
    return XR_ERROR_FUNCTION_UNSUPPORTED;
  if (!out || out->type != XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR || out->next)
    return XR_ERROR_VALIDATION_FAILURE;
  XrGraphicsRequirementsVulkanKHR vk{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
  auto result = host_call<PFN_xrGetVulkanGraphicsRequirements2KHR, "xrGetVulkanGraphicsRequirements2KHR">(instance, system, &vk);
  if (XR_SUCCEEDED(result)) {
    out->minimum = XR_MAKE_VERSION(3, 0, 0);
    out->maximum = XR_MAKE_VERSION(3, 2, 0);
  }
  return result;
}
// A Vulkan guest's D24 depth chain on a host whose GPU attaches no D24: its
// private images use the substituted 32-bit format (vulkan::host_depth_format),
// which the host chain carries 1:1. UNDEFINED when no substitution applies.
static bool depth_stencil_format(VkFormat format) {
  switch (format) {
  case VK_FORMAT_D16_UNORM: case VK_FORMAT_X8_D24_UNORM_PACK32: case VK_FORMAT_D32_SFLOAT:
  case VK_FORMAT_D16_UNORM_S8_UINT: case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_D32_SFLOAT_S8_UINT:
  case VK_FORMAT_S8_UINT: return true;
  default: return false;
  }
}
static VkFormat substituted_depth(std::int64_t format, const std::vector<std::int64_t>& host) {
  if (state().gles || !vulkan::depth_alias_candidate(VkFormat(format))) return VK_FORMAT_UNDEFINED;
  const auto wide = vulkan::host_depth_format(state().graphics.physicalDevice, VkFormat(format));
  const auto offered = [&](std::int64_t f) { return std::find(host.begin(), host.end(), f) != host.end(); };
  return wide != format && !offered(format) && offered(wide) ? wide : VK_FORMAT_UNDEFINED;
}
XrResult xrEnumerateSwapchainFormats(XrSession handle, std::uint32_t capacity, std::uint32_t *count,
                                     std::int64_t *out) {
  std::lock_guard lock(state().mutex);
  if (!valid_handle(handle, "XrSession"))
    return XR_ERROR_HANDLE_INVALID;
  if (!count || (capacity && !out))
    return XR_ERROR_VALIDATION_FAILURE;
  std::uint32_t n = 0;
  auto result = ::xrEnumerateSwapchainFormats(handle, 0, &n, nullptr);
  if (XR_FAILED(result))
    return result;
  std::vector<std::int64_t> formats(n);
  result = ::xrEnumerateSwapchainFormats(handle, n, &n, formats.data());
  if (XR_FAILED(result))
    return result;
  formats.resize(n);
  std::vector<std::int64_t> gl;
  if (state().gles) {
    for (auto f : formats)
      if (auto translated = vk_to_gl(f))
        gl.push_back(translated);
  } else {
    // Unity's space warp ends the session unless its D24S8 motion depth is offered.
    gl = formats;
    for (auto depth : {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_X8_D24_UNORM_PACK32})
      if (substituted_depth(depth, formats) != VK_FORMAT_UNDEFINED) gl.push_back(depth);
  }
  *count = static_cast<std::uint32_t>(gl.size());
  if (!capacity)
    return XR_SUCCESS;
  if (capacity < gl.size())
    return XR_ERROR_SIZE_INSUFFICIENT;
  std::copy(gl.begin(), gl.end(), out);
  return XR_SUCCESS;
}
XrResult create_swapchain(XrSession handle, const XrSwapchainCreateInfo *info, XrSwapchain *out,
                         bool surface) {
  std::lock_guard lock(state().mutex);
  if (!valid_handle(handle, "XrSession"))
    return XR_ERROR_HANDLE_INVALID;
  if (!out || !info || info->type != XR_TYPE_SWAPCHAIN_CREATE_INFO)
    return XR_ERROR_VALIDATION_FAILURE;
  *out = XR_NULL_HANDLE;
  // An empty image cannot exist. OVRPlugin creates its space-warp motion-vector and depth swapchains at 0x0
  // when the extension is not enabled; a host that accepts them (SteamVR) hands back images the driver
  // later faults on in vkCmdPipelineBarrier. The game handles the rejection.
  if (!info->width || !info->height || !info->arraySize || !info->mipCount || !info->faceCount || !info->sampleCount)
    return XR_ERROR_VALIDATION_FAILURE;
  auto copy = *info;
  copy.next = nullptr;
  bool full_rate_resource_requested = false;
  std::set<const void*> seen;
  std::set<XrStructureType> types;
  for (auto p = static_cast<const XrBaseInStructure*>(info->next); p; p = p->next) {
    if (!seen.insert(p).second || seen.size() > 64) return XR_ERROR_VALIDATION_FAILURE;
    const StructureInfo* metadata = nullptr;
    for (const auto& m : structure_metadata()) if (m.type == p->type) { metadata = &m; break; }
    // OpenXR "Valid Usage for Structure Pointer Chains": ignore unknown and
    // disabled extension nodes without reading their payload. OVRPlugin still
    // attaches foveation nodes when the extension is unavailable.
    // https://registry.khronos.org/OpenXR/specs/1.1/html/xrspec.html#fundamentals-validusage-next
    if (!metadata || !types.insert(p->type).second) continue;
    if (*metadata->extension && !enabled(metadata->extension)) continue;
    if (!extends_structure(*metadata, XR_TYPE_SWAPCHAIN_CREATE_INFO))
      return XR_ERROR_VALIDATION_FAILURE;
    if (p->type == XR_TYPE_SWAPCHAIN_CREATE_INFO_FOVEATION_FB) {
      const auto flags = reinterpret_cast<const XrSwapchainCreateInfoFoveationFB*>(p)->flags;
      constexpr auto supported = XR_SWAPCHAIN_CREATE_FOVEATION_SCALED_BIN_BIT_FB |
                                 XR_SWAPCHAIN_CREATE_FOVEATION_FRAGMENT_DENSITY_MAP_BIT_FB;
      if (flags & ~supported) return XR_ERROR_VALIDATION_FAILURE;
      if (flags & XR_SWAPCHAIN_CREATE_FOVEATION_FRAGMENT_DENSITY_MAP_BIT_FB) {
        if (enabled("XR_FB_foveation_vulkan")) {
          full_rate_resource_requested = true;
        } else {
          // OVRPlugin attaches this flag even without negotiating the Vulkan
          // extension that supplies density-map images. Keep full-rate color
          // images; no density-map resource was promised to the application.
          log::warn("xr", "Unnegotiated fragment-density-map flag uses full-rate color images");
        }
      }
      // A profile is a quality/performance hint. Full-rate color rendering
      // preserves fidelity at every level, including scaled-bin requests.
      continue;
    }
    if (p->type == XR_TYPE_VULKAN_SWAPCHAIN_CREATE_INFO_META) {
      const auto result = vulkan_swapchain_flags(*reinterpret_cast<const XrVulkanSwapchainCreateInfoMETA*>(p),
                                                 copy.usageFlags);
      if (XR_FAILED(result)) return result;
      continue;
    }
    log::warn("xr", "Unsupported enabled swapchain structure {}", metadata->name);
    return XR_ERROR_FEATURE_UNSUPPORTED;
  }
  // The Vulkan image resource contract cannot be fulfilled by a GLES session.
  if (state().gles && full_rate_resource_requested) return XR_ERROR_FEATURE_UNSUPPORTED;
  // Quest's runtime hands Vulkan applications SRGB eye images they may view
  // as UNORM (Unity does so without asking for a mutable format). Request the
  // core mutable-format usage so the host images allow those views.
  if (!state().gles && !surface && srgb_format(copy.format))
    copy.usageFlags |= XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;
  if (state().gles && !surface) {
    copy.format = gl_to_vk(info->format);
    if (copy.format == VK_FORMAT_UNDEFINED)
      return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
    copy.usageFlags |= XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
  }
  // Lazy host allocation must not defer advertised incompatibilities until
  // EndFrame. Render-only guest chains obey the same creation contract.
  std::uint32_t format_count{};
  auto compatibility = ::xrEnumerateSwapchainFormats(handle, 0, &format_count, nullptr);
  if (XR_FAILED(compatibility)) return compatibility;
  std::vector<std::int64_t> formats(format_count);
  compatibility = ::xrEnumerateSwapchainFormats(handle, format_count, &format_count, formats.data());
  if (XR_FAILED(compatibility)) return compatibility;
  const auto offered = [&](std::int64_t format) {
    return std::find(formats.begin(), formats.end(), format) != formats.end();
  };
  if (const auto wide = substituted_depth(copy.format, formats); wide != VK_FORMAT_UNDEFINED) copy.format = wide;
  bool format_supported = offered(copy.format);
  // A Quest takes any depth/stencil chain; OVRPlugin asks for D24S8 without
  // checking the list and its frames fail when refused (SteamVR on AMD offers
  // neither D24S8 nor D32S8). Such a chain stays the guest's own: its images
  // use the device's depth format, and its depth is left out of host frames.
  bool guest_only = false;
  if (!format_supported && !state().gles && !surface && depth_stencil_format(VkFormat(copy.format))) {
    copy.format = vulkan::host_depth_format(state().graphics.physicalDevice, VkFormat(copy.format));
    format_supported = guest_only = true;
    static std::atomic_flag reported = ATOMIC_FLAG_INIT;
    if (!reported.test_and_set(std::memory_order_relaxed))
      log::warn("xr", "The host offers no swapchain format {}; the game's depth stays its own and is not "
                      "given to the host compositor", info->format);
  }
  // A host may list a depth format the GPU cannot attach (Virtual Desktop
  // lists D24S8 on AMD): the images take the device's depth format, as the
  // game's own images do, and the host gets that depth only if it lists it.
  if (format_supported && !state().gles && !surface && depth_stencil_format(VkFormat(copy.format))) {
    const auto device = vulkan::host_depth_format(state().graphics.physicalDevice, VkFormat(copy.format));
    if (device != VkFormat(copy.format)) {
      copy.format = device;
      guest_only = !offered(device);
    }
  }
  // A Quest offers every 8-bit RGBA order and encoding (SteamVR on AMD omits
  // R8G8B8A8_UNORM); the host chain takes another with a converting copy.
  if (!format_supported && (state().gles || !surface)) {
    const auto rgba8 = [](std::int64_t f) {
      return f == VK_FORMAT_R8G8B8A8_UNORM || f == VK_FORMAT_B8G8R8A8_UNORM ||
             f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_SRGB;
    };
    if (rgba8(copy.format)) format_supported = std::any_of(formats.begin(), formats.end(), rgba8);
  }
  if (!format_supported) return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
  // Guest images are private, so the guest device's image limits apply to them
  // (GuestImages). A size over the host's swapchain maximum is presented
  // through a smaller host chain (prepare_swapchains), as a Quest accepts it.
  std::uint32_t view_count{};
  compatibility = ::xrEnumerateViewConfigurationViews(host_xr().instance, host_xr().system,
      XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &view_count, nullptr);
  if (XR_FAILED(compatibility)) return compatibility;
  std::vector<XrViewConfigurationView> views(view_count, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
  compatibility = ::xrEnumerateViewConfigurationViews(host_xr().instance, host_xr().system,
      XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, view_count, &view_count, views.data());
  if (XR_FAILED(compatibility)) return compatibility;
  if (!std::has_single_bit(copy.sampleCount) || std::none_of(views.begin(), views.end(),
      [&](const auto& view) { return copy.sampleCount <= view.maxSwapchainSampleCount; }))
    return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
  // The fallback converts only when necessary. Transfer usage allows a GPU
  // snapshot and restore the original pixels without changing guest handles.
  if (enabled("XR_FB_color_space") &&
      !host_xr().enabled_extensions.contains("XR_FB_color_space") &&
      (copy.usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT))
    copy.usageFlags |= XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
  Swapchain record;
  record.full_rate_resource_requested = full_rate_resource_requested;
  record.guest_only = guest_only;
  record.info = copy;
  record.desc = {info->format,    static_cast<VkFormat>(copy.format),
                 info->width,     info->height,
                 info->arraySize, info->sampleCount,
                 info->mipCount,  info->faceCount};
  // Guest image ownership is independent of host compositor resources. This
  // also preserves render-only, static, sampled and partially redrawn images
  // when an inactive host presentation chain is recycled.
  if (copy.createFlags & XR_SWAPCHAIN_CREATE_PROTECTED_CONTENT_BIT)
    return XR_ERROR_FEATURE_UNSUPPORTED;
  if (copy.createFlags & ~XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT)
    return XR_ERROR_VALIDATION_FAILURE;
  try {
    record.backing = std::make_unique<GuestImages>(state().graphics,
        gfx::gpu_device().get_instance_proc_addr, copy);
    const auto images = record.backing->images();
    record.images.assign(images.begin(), images.end());
    record.identity = std::make_unique<std::uint64_t>(0);
    record.handle = reinterpret_cast<XrSwapchain>(record.identity.get());
  } catch (XrResult result) {
    return result;
  } catch (const std::exception& error) {
    log::error("xr", "Guest swapchain allocation failed: {}", error.what());
    return XR_ERROR_OUT_OF_MEMORY;
  }
  XrResult result = XR_SUCCESS;
  if (state().gles && !surface) {
    record.textures.resize(record.images.size());
    result = xr_images()->create_ring(record.desc, record.textures);
    if (XR_SUCCEEDED(result) && enabled("XR_FB_swapchain_update_state"))
      result = xr_images()->update_sampler(record.textures, record.desc, record.sampler_gles);
  } else if (!surface) {
    note_images(record);
  }
  if (XR_FAILED(result)) {
    forget_images(record);
    if (!record.textures.empty()) xr_images()->destroy_ring(record.textures);
    return result;
  }
  *out = record.handle;
  if (venture::environment::getenv("VENTURE_TRACE_XR") ||
       venture::environment::getenv("VENTURE_TRACE_XR_FRAMES"))
    log::info("xr-object", "create_swapchain host={:#x} images={} format={} size={}x{} arrays={}",
        reinterpret_cast<std::uintptr_t>(record.handle), record.images.size(), record.info.format,
        record.info.width, record.info.height, record.info.arraySize);
  state().swapchains.emplace(*out, std::move(record));
  if (venture::environment::getenv("VENTURE_XR_SWAPCHAIN_STATS"))
    log::info("xr-budget", "created guest_live={}", state().swapchains.size());
  track_handle(*out, "XrSwapchain", handle);
  return XR_SUCCESS;
}
XrResult xrCreateSwapchain(XrSession handle, const XrSwapchainCreateInfo* info, XrSwapchain* out) {
  const auto result = create_swapchain(handle, info, out, false);
  if (XR_FAILED(result)) {
    if (info && info->type == XR_TYPE_SWAPCHAIN_CREATE_INFO)
      log::warn("xr", "xrCreateSwapchain rejected: result={} flags={} usage={} format={} size={}x{} samples={} faces={} arrays={} mips={} next={}",
                int(result), info->createFlags, info->usageFlags, info->format,
                info->width, info->height, info->sampleCount, info->faceCount,
                info->arraySize, info->mipCount, bool(info->next));
    else
      log::warn("xr", "xrCreateSwapchain rejected: result={} invalid create-info header", int(result));
  }
  return result;
}
XrResult xrDestroySwapchain(XrSwapchain handle) {
  std::lock_guard lock(state().mutex);
  auto it = state().swapchains.find(handle);
  if (it == state().swapchains.end())
    return XR_ERROR_HANDLE_INVALID;
  if (it->second.operation_in_progress) return XR_ERROR_CALL_ORDER_INVALID;
  if (venture::environment::getenv("VENTURE_TRACE_XR") ||
       venture::environment::getenv("VENTURE_TRACE_XR_FRAMES"))
    log::info("xr-object", "destroy_swapchain host={:#x}", reinterpret_cast<std::uintptr_t>(it->second.handle));
  // Before the queue lock, which an asynchronous host End in flight takes.
  session().finish_host_end();
  std::unique_lock queue(gfx::gpu_device().queue_lock, std::defer_lock);
  if (session().uses_shared_gpu) queue.lock();
  if (!it->second.textures.empty()) {
    std::vector<VkImage> images;
    for (auto image : it->second.images)
      images.push_back(image.image);
    auto released = xr_images()->release_images(images);
    if (XR_FAILED(released))
      return released;
  }
  auto result = destroy_host_swapchain(it->second);
  if (XR_SUCCEEDED(result)) {
    forget_images(it->second);
    if (!it->second.textures.empty())
      xr_images()->destroy_ring(it->second.textures);
    state().swapchains.erase(it);
    forget_handle(handle);
  }
  return result;
}
XrResult xrEnumerateSwapchainImages(XrSwapchain handle, std::uint32_t capacity,
                                    std::uint32_t *count, XrSwapchainImageBaseHeader *out) {
  std::lock_guard lock(state().mutex);
  auto it = state().swapchains.find(handle);
  if (it == state().swapchains.end())
    return XR_ERROR_HANDLE_INVALID;
  if (it->second.android_surface) return XR_ERROR_VALIDATION_FAILURE;
  if (!count || (capacity && !out))
    return XR_ERROR_VALIDATION_FAILURE;
  auto &r = it->second;
  *count = static_cast<std::uint32_t>(r.images.size());
  if (!capacity)
    return XR_SUCCESS;
  if (capacity < *count)
    return XR_ERROR_SIZE_INSUFFICIENT;
  for (std::uint32_t i = 0; i < *count; ++i) {
    if (state().gles) {
      auto &image = reinterpret_cast<GlesImage *>(out)[i];
      if (image.type != XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR)
        return XR_ERROR_VALIDATION_FAILURE;
      auto result = validate_image_next(image.next, image.type);
      if (XR_FAILED(result)) return result;
      image.image = r.textures[i];
    } else {
      auto &image = reinterpret_cast<XrSwapchainImageVulkanKHR *>(out)[i];
      if (image.type != XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR) {
        log::warn("xr", "Vulkan swapchain image {} has invalid type {}", i, int(image.type));
        return XR_ERROR_VALIDATION_FAILURE;
      }
      auto result = validate_image_next(image.next, image.type);
      if (XR_FAILED(result)) {
        const auto* next = static_cast<const XrBaseInStructure*>(image.next);
        log::warn("xr", "Vulkan swapchain image {} has invalid output chain: result={} next={}",
                  i, int(result), next ? int(next->type) : 0);
        return result;
      }
      image.image = r.images[i].image;
      if (venture::environment::getenv("VENTURE_TRACE_XR") ||
          venture::environment::getenv("VENTURE_TRACE_XR_FRAMES"))
        log::info("xr-object", "enumerate_swapchain host={:#x} index={} count={} image={:#x}",
            reinterpret_cast<std::uintptr_t>(r.handle), i, *count,
            reinterpret_cast<std::uintptr_t>(image.image));
      if (enabled("XR_FB_foveation_vulkan")) {
        for (auto* p = static_cast<XrBaseOutStructure*>(image.next); p; p = p->next) {
          if (p->type != XR_TYPE_SWAPCHAIN_IMAGE_FOVEATION_VULKAN_FB) continue;
          if (!r.full_rate_resource_requested) {
            // The extension defines these output fields only for a swapchain
            // created with FRAGMENT_DENSITY_MAP_BIT. Some clients reuse their
            // output chain for ordinary full-rate swapchains too. Leave that
            // inapplicable payload alone, just like an unnegotiated node, and
            // still enumerate every real color image. Failing partway through
            // the array would leave the caller with incomplete image handles.
            continue;
          }
          if (!r.full_rate_resource) {
            const auto& binding = state().graphics;
            try {
              r.full_rate_resource = std::make_unique<vulkan::FullRateImage>(
                  binding.physicalDevice, binding.device, binding.queueFamilyIndex,
                  binding.queueIndex, r.info.arraySize * r.info.faceCount, r.info.width, r.info.height);
            } catch (const std::exception& e) {
              log::error("xr", "Full-resolution startup resource failed: {}", e.what());
              return XR_ERROR_RUNTIME_FAILURE;
            }
          }
          auto& resource = *reinterpret_cast<XrSwapchainImageFoveationVulkanFB*>(p);
          resource.image = r.full_rate_resource->image();
          resource.width = r.full_rate_resource->width();
          resource.height = r.full_rate_resource->height();
        }
      }
    }
  }
  return XR_SUCCESS;
}
XrResult xrAcquireSwapchainImage(XrSwapchain handle, const XrSwapchainImageAcquireInfo *info,
                                 std::uint32_t *index) {
  std::lock_guard lock(state().mutex);
  auto it = state().swapchains.find(handle);
  if (it == state().swapchains.end())
    return XR_ERROR_HANDLE_INVALID;
  if (it->second.android_surface) return XR_ERROR_VALIDATION_FAILURE;
  if (!index || (info && XR_FAILED(validate_struct(info, XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO))))
    return XR_ERROR_VALIDATION_FAILURE;
  auto &r = it->second;
  if (r.acquired.size() == r.images.size())
    return XR_ERROR_CALL_ORDER_INVALID;
  if ((r.info.createFlags & XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) && r.static_acquired)
    return XR_ERROR_CALL_ORDER_INVALID;
  for (std::uint32_t n = 0; n < r.images.size(); ++n) {
    const auto candidate = (r.next_index + n) % r.images.size();
    if (std::find(r.acquired.begin(), r.acquired.end(), candidate) != r.acquired.end()) continue;
    *index = candidate;
    r.next_index = (candidate + 1) % r.images.size();
    r.acquired.push_back(candidate);
    r.static_acquired = true;
    return XR_SUCCESS;
  }
  return XR_ERROR_CALL_ORDER_INVALID;
}
XrResult xrWaitSwapchainImage(XrSwapchain handle, const XrSwapchainImageWaitInfo *info) {
  std::unique_lock lock(state().mutex);
  auto it = state().swapchains.find(handle);
  if (it == state().swapchains.end())
    return XR_ERROR_HANDLE_INVALID;
  if (it->second.android_surface) return XR_ERROR_VALIDATION_FAILURE;
  if (XR_FAILED(validate_struct(info, XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO)))
    return XR_ERROR_VALIDATION_FAILURE;
  if (it->second.operation_in_progress || it->second.acquired.empty() || it->second.waited)
    return XR_ERROR_CALL_ORDER_INVALID;
  ImageOperation operation(it->second, lock);
  lock.unlock();
  const auto wait_start = vulkan::timing_enabled() ? monotonic_ns() : 0;
  auto result = it->second.backing->wait(it->second.acquired.front(), info->timeout);
  if (wait_start) vulkan::timing_host_wait("xr_image", monotonic_ns() - wait_start);
  lock.lock();
  if (result == XR_SUCCESS) {
    auto& r = it->second;
    r.waited = true;
  }
  return result;
}
XrResult xrReleaseSwapchainImage(XrSwapchain handle, const XrSwapchainImageReleaseInfo *info) {
  std::unique_lock lock(state().mutex);
  auto it = state().swapchains.find(handle);
  if (it == state().swapchains.end())
    return XR_ERROR_HANDLE_INVALID;
  if (it->second.android_surface) return XR_ERROR_VALIDATION_FAILURE;
  if (info && XR_FAILED(validate_struct(info, XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO)))
    return XR_ERROR_VALIDATION_FAILURE;
  auto &r = it->second;
  if (r.operation_in_progress || r.acquired.empty() || !r.waited)
    return XR_ERROR_CALL_ORDER_INVALID;
  std::unique_lock queue(gfx::gpu_device().queue_lock, std::defer_lock);
  ImageOperation operation(r, lock);
  const bool timed_native = r.textures.empty() && vulkan::timing_enabled();
  const auto start = timed_native ? monotonic_ns() : 0;
  const auto* native = timed_native ? [] {
    thread_local const auto value = platform::profile_current_thread();
    return &value;
  }() : nullptr;
  std::uint64_t cpu_begin = 0;
  const bool cpu_timed = native && platform::profile_cpu_time(*native, cpu_begin);
  if (!r.textures.empty()) {
    auto index = r.acquired.front();
    if (index >= r.images.size())
      return XR_ERROR_RUNTIME_FAILURE;
    // Guest OpenXR GLES swapchains are framebuffers: always bottom-up rows.
    lock.unlock();
    auto result = xr_images()->copy_to_image(r.textures[index], r.images[index].image, r.desc, 0,
                                             true);
    lock.lock();
    if (XR_FAILED(result))
      return result;
  }
  // Retain the most recently released guest image. Host acquisition, upload
  // and gamut conversion happen only if a composition layer actually uses it.
  r.released_index = r.acquired.front();
  ++r.release_serial;
  auto result = XR_SUCCESS;
  if (timed_native) {
    vulkan::XrCopySample sample{"openxr_native_release", vulkan::timing_frame(),
        monotonic_ns() - start, 0, 0, 0, 0, 0, 0};
    sample.gpu_transfer = false;
    std::uint64_t cpu_end = 0;
    if (cpu_timed && platform::profile_cpu_time(*native, cpu_end) && cpu_end >= cpu_begin)
      sample.thread_cpu_ns = std::int64_t(cpu_end - cpu_begin);
    vulkan::timing_xr_copy(sample);
  }
  if (XR_SUCCEEDED(result)) {
    r.acquired.pop_front();
    r.waited = false;
  }
  return result;
}
XrResult xrGetVulkanGraphicsRequirementsKHR(XrInstance i, XrSystemId s,
                                            XrGraphicsRequirementsVulkanKHR *out) {
  std::lock_guard lock(state().mutex);
  if (!valid_handle(i, "XrInstance"))
    return XR_ERROR_HANDLE_INVALID;
  if (XR_FAILED(validate_struct(out, XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR)))
    return XR_ERROR_VALIDATION_FAILURE;
  return host_call<PFN_xrGetVulkanGraphicsRequirements2KHR, "xrGetVulkanGraphicsRequirements2KHR">(i, s, out);
}
XrResult xrGetVulkanGraphicsRequirements2KHR(XrInstance i, XrSystemId s,
                                             XrGraphicsRequirementsVulkanKHR *out) {
  return guest::xrGetVulkanGraphicsRequirementsKHR(i, s, out);
}
XrResult xrGetVulkanInstanceExtensionsKHR(XrInstance i, XrSystemId s, std::uint32_t cap,
                                          std::uint32_t *count, char *out) {
  std::lock_guard lock(state().mutex);
  if (!valid_handle(i, "XrInstance"))
    return XR_ERROR_HANDLE_INVALID;
  if (!count || (cap && !out))
    return XR_ERROR_VALIDATION_FAILURE;
  return xr::guest_vulkan_extensions(i, s, false, cap, count, out, vulkan::extension_translatable);
}
XrResult xrGetVulkanDeviceExtensionsKHR(XrInstance i, XrSystemId s, std::uint32_t cap,
                                        std::uint32_t *count, char *out) {
  std::lock_guard lock(state().mutex);
  if (!valid_handle(i, "XrInstance"))
    return XR_ERROR_HANDLE_INVALID;
  if (!count || (cap && !out))
    return XR_ERROR_VALIDATION_FAILURE;
  return xr::guest_vulkan_extensions(i, s, true, cap, count, out, vulkan::extension_translatable);
}
XrResult xrGetVulkanGraphicsDevice2KHR(XrInstance i, const XrVulkanGraphicsDeviceGetInfoKHR *info,
                                       VkPhysicalDevice *out) {
  std::lock_guard lock(state().mutex);
  if (!valid_handle(i, "XrInstance"))
    return XR_ERROR_HANDLE_INVALID;
  if (!out || XR_FAILED(validate_struct(info, XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR)))
    return XR_ERROR_VALIDATION_FAILURE;
  // Guest instances are created by the game or by Venture, not by the host's
  // xrCreateVulkanInstanceKHR, so even enable2 games query through enable.
  auto result = venture::xr::host_graphics_device(info->systemId, info->vulkanInstance, out);
  if (XR_FAILED(result)) {
    // The host runtime already chose the GPU for Venture's shared device. Some
    // runtimes refuse the query for an instance the game made itself (Oculus
    // with an AMD GPU: RUNTIME_FAILURE, Epic Roller Coasters); that GPU in the
    // game's instance is the answer the query stands for.
    const auto physical = vulkan::shared_physical_device_in(info->vulkanInstance);
    log::warn("xr", "Host Vulkan graphics device query failed ({}); {}", static_cast<int>(result),
              physical ? "using the shared device's GPU in the game's instance" : "the shared device's GPU is not in it");
    if (!physical) return result;
    *out = physical;
    result = XR_SUCCESS;
  }
  vulkan::adopt_physical_device(info->vulkanInstance, *out);
  return result;
}
XrResult xrGetVulkanGraphicsDeviceKHR(XrInstance i, XrSystemId s, VkInstance vk,
                                      VkPhysicalDevice *out) {
  XrVulkanGraphicsDeviceGetInfoKHR info{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
  info.systemId = s;
  info.vulkanInstance = vk;
  return guest::xrGetVulkanGraphicsDevice2KHR(i, &info, out);
}
namespace {
std::vector<std::string> merge_extensions(XrInstance i, XrSystemId s, bool device,
                                          std::uint32_t count, const char *const *names) {
  std::set<std::string> merged;
  for (std::uint32_t n = 0; n < count; ++n)
    if (names[n])
      merged.insert(names[n]);
  auto fn = device ? host_proc<PFN_xrGetVulkanDeviceExtensionsKHR, "xrGetVulkanDeviceExtensionsKHR">()
                   : host_proc<PFN_xrGetVulkanInstanceExtensionsKHR, "xrGetVulkanInstanceExtensionsKHR">();
  std::uint32_t length = 0;
  if (fn && XR_SUCCEEDED(fn(i, s, 0, &length, nullptr)) && length) {
    std::string required(length, '\0');
    if (XR_SUCCEEDED(fn(i, s, length, &length, required.data()))) {
      required.resize(std::strlen(required.c_str()));
      std::istringstream stream(required);
      for (std::string item; stream >> item;)
        merged.insert(item);
    }
  }
  return {merged.begin(), merged.end()};
}
} // namespace
XrResult xrCreateVulkanInstanceKHR(XrInstance i, const XrVulkanInstanceCreateInfoKHR *info,
                                   VkInstance *out, VkResult *vk_result) {
  std::lock_guard lock(state().mutex);
  if (!valid_handle(i, "XrInstance"))
    return XR_ERROR_HANDLE_INVALID;
  if (!out || !vk_result ||
      XR_FAILED(validate_struct(info, XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR)) ||
      !info->vulkanCreateInfo || !info->pfnGetInstanceProcAddr)
    return XR_ERROR_VALIDATION_FAILURE;
  auto vk = *info->vulkanCreateInfo;
  if ((vk.pApplicationInfo && vk.pApplicationInfo->pNext) ||
      (vk.enabledExtensionCount && !vk.ppEnabledExtensionNames))
    return XR_ERROR_VALIDATION_FAILURE;
  auto names = merge_extensions(i, info->systemId, false, vk.enabledExtensionCount,
                                vk.ppEnabledExtensionNames);
  std::vector<const char *> ptrs;
  for (auto &n : names)
    ptrs.push_back(n.c_str());
  vk.enabledExtensionCount = static_cast<std::uint32_t>(ptrs.size());
  vk.ppEnabledExtensionNames = ptrs.data();
  auto copy = *info;
  copy.vulkanCreateInfo = &vk;
  vulkan::XrResolverScope resolver(reinterpret_cast<GuestAddr>(info->pfnGetInstanceProcAddr));
  copy.pfnGetInstanceProcAddr = vulkan::XrResolverScope::proc;
  return host_call<PFN_xrCreateVulkanInstanceKHR, "xrCreateVulkanInstanceKHR">(i, &copy, out,
                                                  vk_result);
}
XrResult xrCreateVulkanDeviceKHR(XrInstance i, const XrVulkanDeviceCreateInfoKHR *info,
                                 VkDevice *out, VkResult *vk_result) {
  std::lock_guard lock(state().mutex);
  if (!valid_handle(i, "XrInstance"))
    return XR_ERROR_HANDLE_INVALID;
  if (!out || !vk_result ||
      XR_FAILED(validate_struct(info, XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR)) ||
      !info->vulkanCreateInfo || !info->pfnGetInstanceProcAddr)
    return XR_ERROR_VALIDATION_FAILURE;
  auto vk = *info->vulkanCreateInfo;
  if ((vk.enabledExtensionCount && !vk.ppEnabledExtensionNames) ||
      (vk.queueCreateInfoCount && !vk.pQueueCreateInfos))
    return XR_ERROR_VALIDATION_FAILURE;
  for (std::uint32_t n = 0; n < vk.queueCreateInfoCount; ++n)
    if (vk.pQueueCreateInfos[n].pNext)
      return XR_ERROR_FEATURE_UNSUPPORTED;
  auto names = merge_extensions(i, info->systemId, true, vk.enabledExtensionCount,
                                vk.ppEnabledExtensionNames);
  std::vector<const char *> ptrs;
  for (auto &n : names)
    ptrs.push_back(n.c_str());
  vk.enabledExtensionCount = static_cast<std::uint32_t>(ptrs.size());
  vk.ppEnabledExtensionNames = ptrs.data();
  auto copy = *info;
  copy.vulkanCreateInfo = &vk;
  vulkan::XrResolverScope resolver(reinterpret_cast<GuestAddr>(info->pfnGetInstanceProcAddr));
  copy.pfnGetInstanceProcAddr = vulkan::XrResolverScope::proc;
  *vk_result = VK_ERROR_INITIALIZATION_FAILED;
  auto result = host_call<PFN_xrCreateVulkanDeviceKHR, "xrCreateVulkanDeviceKHR">(i, &copy, out,
                                                       vk_result);
  if (XR_FAILED(result) || *vk_result != VK_SUCCESS)
    log::error("xr", "Vulkan device creation failed: XrResult={} VkResult={}", int(result),
               int(*vk_result));
  return result;
}
} // namespace guest
} // namespace venture::hle::openxr

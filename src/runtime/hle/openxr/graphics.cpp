#include "xr/dev_dump.h"
#include "gfx/gpu_device.h"
#include "gfx/foveation_vulkan.h"
#include "xr/eye_gaze.h"
#include "xr/math.h"
#include "common/environment.h"
#include "common/log.h"
#include "hle/vulkan/vulkan.h"
#include "hle/vulkan/internal.h"
#include "hle/vulkan/chain.h"
#include "hle/openxr/state.h"
#include "hle/openxr/sphere_layers.h"
#include "xr/presentation.h"
#include "xr/desktop_mirror.h"
#include "xr/display_override.h"
#include "xr/eye_scale.h"
#include "xr/queue.h"
#include "xr/vulkan_extensions.h"
#include "hle/vulkan/timing.h"
#include "core/wait.h"
#include "platform/profile.h"
#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <array>
#include <optional>
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
  for (const auto& image : r.images)
    if (image.image) vulkan::forget_image(image.image);
}
bool staged(const Swapchain& r) { return r.backing && !r.backing->staged().empty(); }
// The slot storage of a GLES chain: the provider's staging, or private images
// for providers without it (and the opt-in bridge experiments, which need them).
XrResult create_gles_storage(Swapchain& record) {
  const auto plain = [] {
    for (const char* name : {"VENTURE_XR_DIRECT_TEXTURES", "VENTURE_XR_GPU_OWNERSHIP", "VENTURE_XR_BATCH_HANDOFF"})
      if (const char* value = venture::environment::getenv(name); value && std::string_view(value) == "1")
        return true;
    return false;
  }();
  record.textures.resize(record.images.size());
  // On failure the caller destroys the ring; a failed staging allocation
  // releases its own partial storage.
  auto result = xr_images()->create_ring(record.desc, record.textures);
  if (XR_FAILED(result)) return result;
  if (!plain) {
    result = xr_images()->create_staging(record.desc, record.textures, record.backing->staged());
    if (XR_SUCCEEDED(result)) return result;
    if (result != XR_ERROR_FEATURE_UNSUPPORTED) return result;
  }
  // Private images replace the unused staged slots.
  try {
    record.backing = std::make_unique<GuestImages>(state().graphics,
        gfx::gpu_device().get_instance_proc_addr, record.info);
  } catch (XrResult failure) {
    return failure;
  } catch (const std::exception& error) {
    log::error("xr", "Guest swapchain allocation failed: {}", error.what());
    return XR_ERROR_OUT_OF_MEMORY;
  }
  const auto images = record.backing->images();
  record.images.assign(images.begin(), images.end());
  return XR_SUCCESS;
}
bool depth_format(std::int64_t format) {
  switch (VkFormat(format)) {
  case VK_FORMAT_D16_UNORM: case VK_FORMAT_X8_D24_UNORM_PACK32: case VK_FORMAT_D32_SFLOAT:
  case VK_FORMAT_D16_UNORM_S8_UINT: case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_D32_SFLOAT_S8_UINT:
  case VK_FORMAT_S8_UINT: return true;
  default: return false;
  }
}
// Zero-copy chains (docs/design/zero-copy-eyes.md). The game's queue work
// must reach the host queue before the host may read an image.
XrResult flush_guest_queue() {
  return vulkan::flush_queue_submissions(state().graphics.device, state().graphics.queueFamilyIndex,
                                         state().guest_graphics_queue_index) == VK_SUCCESS
             ? XR_SUCCESS : XR_ERROR_RUNTIME_FAILURE;
}
// The game left the held image in another layout than OpenXR's attachment
// layout (tracked from its submissions): move it back before the host takes it.
XrResult restore_attachment_layout(Swapchain& r, std::uint32_t index) {
  const auto image = r.images.at(index).image;
  const auto layouts = vulkan::tracked_layouts(image);
  const bool depth = depth_format(r.info.format);
  const auto attachment = depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  if (std::all_of(layouts.begin(), layouts.end(), [&](VkImageLayout l) { return l == attachment; })) return XR_SUCCESS;
  static std::atomic_flag reported = ATOMIC_FLAG_INIT;
  if (!reported.test_and_set(std::memory_order_relaxed))
    log::info("xr", "The game released a zero-copy swapchain image in layout {}; it goes to the host in the "
              "attachment layout", int(layouts.empty() ? VK_IMAGE_LAYOUT_UNDEFINED : layouts.front()));
  const auto layers = r.info.arraySize * r.info.faceCount;
  auto result = r.backing->submit(index, [&](VkCommandBuffer command, VkImage target, VkImageLayout layout) {
    std::vector<VkImageMemoryBarrier> barriers;
    for (std::uint32_t layer = 0; layer < layers && layer < layouts.size(); ++layer) {
      if (layouts[layer] == layout) continue;
      VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      b.image = target;
      b.oldLayout = layouts[layer];
      b.newLayout = layout;
      b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
      b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
      b.subresourceRange = {VkImageAspectFlags(depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, 1,
                            layer, 1};
      if (depth && (r.info.format == VK_FORMAT_D24_UNORM_S8_UINT || r.info.format == VK_FORMAT_D32_SFLOAT_S8_UINT ||
                    r.info.format == VK_FORMAT_D16_UNORM_S8_UINT))
        b.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
      barriers.push_back(b);
    }
    reinterpret_cast<PFN_vkCmdPipelineBarrier>(gfx::gpu_device().get_instance_proc_addr(
        state().graphics.instance, "vkCmdPipelineBarrier"))(
        command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr,
        std::uint32_t(barriers.size()), barriers.data());
    return XR_SUCCESS;
  });
  if (XR_SUCCEEDED(result)) vulkan::track_layouts(image, 1, layers, attachment);
  return result;
}
// Gives the held image (the front of the host's acquisitions) back to the host.
XrResult release_held(Swapchain& r) {
  if (!r.held) return XR_SUCCESS;
  const auto index = *r.held;
  if (r.host_acquired.empty() || r.host_acquired.front() != index) return XR_ERROR_RUNTIME_FAILURE;
  auto result = flush_guest_queue();
  if (XR_SUCCEEDED(result)) result = restore_attachment_layout(r, index);
  if (XR_FAILED(result)) return result;
  {
    HostQueueLock queue(session().uses_shared_gpu);
    if (!queue) return XR_ERROR_RUNTIME_FAILURE;
    result = venture::xr::dev_release_swapchain(r.direct_handle, nullptr, r.images.at(index).image);
  }
  if (XR_FAILED(result)) return result;
  r.backing->set_owned(index, false);
  r.ownership.at(index)->guest.store(false, std::memory_order_release);
  r.host_acquired.pop_front();
  r.held.reset();
  return result;
}
// A chain whose images the game used while they were the host's, or one a
// consumer needed after its image went back, keeps its images from now on.
void check_ownership(Swapchain& r) {
  if (!r.direct_handle || r.copy_always) return;
  const auto owner = std::find_if(r.ownership.begin(), r.ownership.end(),
      [](const auto& o) { return o && o->violated.load(std::memory_order_acquire); });
  const bool violated = owner != r.ownership.end();
  if (!violated && !r.backing->refused_unowned()) return;
  r.copy_always = true;
  if (violated)
    log::warn("xr", "Swapchain {}x{}: the game moved zero-copy image {} to layout {} while the host runtime had it; "
              "it presents through a copy from now on", r.info.width, r.info.height, owner - r.ownership.begin(),
              (*owner)->violation_layout.load(std::memory_order_relaxed));
  else
    log::warn("xr", "Swapchain {}x{}: an image was needed after it went back to the host runtime; it presents "
              "through a copy from now on", r.info.width, r.info.height);
}
// Releases every host acquisition (waiting those the game never waited) and
// destroys the zero-copy host chain.
XrResult destroy_direct_swapchain(Swapchain& r) {
  if (!r.direct_handle) return XR_SUCCESS;
  const auto drained = r.backing->drain();
  if (drained != XR_SUCCESS) return drained;
  for (const auto& image : r.images) vulkan::untrack_layouts(image.image);
  HostQueueLock queue(session().uses_shared_gpu);
  if (!queue) return XR_ERROR_RUNTIME_FAILURE;
  while (!r.host_acquired.empty()) {
    const auto index = r.host_acquired.front();
    if (r.host_unwaited.at(index)) {
      XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, 1'000'000'000};
      if (::xrWaitSwapchainImage(r.direct_handle, &wait) != XR_SUCCESS) break;
      r.host_unwaited[index] = 0;
    }
    if (XR_FAILED(::xrReleaseSwapchainImage(r.direct_handle, nullptr))) break;
    r.host_acquired.pop_front();
  }
  // A chain with acquisitions left is destroyed, never parked for reuse.
  const auto result = r.host_acquired.empty() ? venture::xr::dev_destroy_swapchain(r.direct_handle)
                                              : ::xrDestroySwapchain(r.direct_handle);
  if (XR_SUCCEEDED(result)) {
    r.direct_handle = XR_NULL_HANDLE;
    r.host_acquired.clear();
    r.held.reset();
  }
  return result;
}
// Why the chain keeps private images and a copy; nullptr when its images can
// be the host's. `offered`: the host lists `info.format`.
const char* zero_copy_refusal(const XrSwapchainCreateInfo& info, bool guest_only, bool offered) {
  if (!zero_copy_enabled()) return "zero-copy is off";
  if (info.createFlags & XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) return "a static image";
  if (info.mipCount != 1) return "mipmapped";
  if (info.faceCount != 1) return "a cube";
  if (guest_only) return "depth the host does not take";
  if (!offered) return "its format is converted for the host";
  if ((info.usageFlags & XR_SWAPCHAIN_USAGE_INPUT_ATTACHMENT_BIT_KHR) &&
      !host_xr().enabled_extensions.contains("XR_KHR_swapchain_usage_input_attachment_bit"))
    return "input attachment usage the host lacks";
  XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES};
  if (XR_SUCCEEDED(::xrGetSystemProperties(host_xr().instance, host_xr().system, &properties))) {
    const auto& limits = properties.graphicsProperties;
    if ((limits.maxSwapchainImageWidth && info.width > limits.maxSwapchainImageWidth) ||
        (limits.maxSwapchainImageHeight && info.height > limits.maxSwapchainImageHeight))
      return "larger than the host's maximum";
  }
  return nullptr;
}
// Makes `record` a zero-copy chain: a host chain like the game's, whose images
// the game gets. On any refusal the record is unchanged and `why` says why.
void create_direct_swapchain(Swapchain& record, bool guest_only, bool offered, std::string& why) {
  if (const auto* refusal = zero_copy_refusal(record.info, guest_only, offered)) {
    why = refusal;
    return;
  }
  const bool depth = depth_format(record.info.format);
  auto host = record.info;
  host.next = nullptr;
  // Venture's own reads of a held image (mirror, eye check, a copy on frames
  // that need one) and the attachment layout OpenXR promises.
  host.usageFlags |= XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT |
      (depth ? XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
             : XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT);
  XrSwapchain handle{};
  XrResult result;
  {
    HostQueueLock queue(session().uses_shared_gpu);
    if (!queue) { why = "the host queue is unavailable"; return; }
    result = venture::xr::dev_create_swapchain(session().handle, &host, &handle);
  }
  std::vector<XrSwapchainImageVulkanKHR> images;
  std::uint32_t count{};
  if (XR_SUCCEEDED(result)) result = ::xrEnumerateSwapchainImages(handle, 0, &count, nullptr);
  if (XR_SUCCEEDED(result) && !count) result = XR_ERROR_RUNTIME_FAILURE;
  if (XR_SUCCEEDED(result)) {
    images.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    result = ::xrEnumerateSwapchainImages(handle, count, &count,
        reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
  }
  std::unique_ptr<GuestImages> backing;
  if (XR_SUCCEEDED(result)) {
    std::vector<VkImage> handles;
    for (const auto& image : images) handles.push_back(image.image);
    try {
      backing = std::make_unique<GuestImages>(state().graphics, gfx::gpu_device().get_instance_proc_addr,
                                              record.info, handles, !depth);
    } catch (...) {
      result = XR_ERROR_RUNTIME_FAILURE;
    }
  }
  if (XR_FAILED(result)) {
    if (handle) {
      HostQueueLock queue(session().uses_shared_gpu);
      if (queue) ::xrDestroySwapchain(handle);
    }
    why = std::format("the host refused its chain ({})", int(result));
    return;
  }
  record.direct_handle = handle;
  record.backing = std::move(backing);
  record.images = std::move(images);
  record.host_unwaited.assign(count, 0);
  record.initialized.assign(count, 0);
  for (const auto& image : record.images) {
    vulkan::track_layouts(image.image, 1, record.info.arraySize * record.info.faceCount, VK_IMAGE_LAYOUT_UNDEFINED);
    auto& owner = record.ownership.emplace_back(std::make_shared<vulkan::ImageOwnership>());
    vulkan::watch_ownership(image.image, owner);
  }
}
}
bool zero_copy_enabled() {
  const auto* value = venture::environment::getenv("VENTURE_XR_ZERO_COPY");
  return !value || std::string_view(value) != "0";
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
std::vector<std::int64_t> gles_swapchain_formats(const std::vector<std::int64_t>& host) {
  std::vector<std::int64_t> gl;
  for (auto f : host)
    if (auto translated = vk_to_gl(f)) gl.push_back(translated);
  // A Quest lists GL_RGBA8 and GL_SRGB8_ALPHA8 for GLES sessions; SteamVR lists
  // no R8G8B8A8_UNORM, AMD hosts other orders. create_swapchain takes either
  // through any host 8-bit RGBA chain with a converting copy, so both are listed
  // (after the host's own order). Unity's OpenXR plugin, which picks a color
  // format from this list, ended the session without them.
  const auto rgba8 = [](std::int64_t f) {
    return f == VK_FORMAT_R8G8B8A8_UNORM || f == VK_FORMAT_B8G8R8A8_UNORM ||
           f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_SRGB;
  };
  if (std::any_of(host.begin(), host.end(), rgba8))
    for (std::int64_t f : {0x8c43, 0x8058})
      if (std::find(gl.begin(), gl.end(), f) == gl.end()) gl.push_back(f);
  return gl;
}
namespace {
// The color space / alpha pass a host image of this chain needs, if any.
struct ColorPass {
  ColorImages* images;
  ColorMatrix matrix;
  ColorImages::Alpha alpha;
  float opacity;
  const std::array<float, 3>* key;
};
std::optional<ColorPass> color_pass(Swapchain& r, XrColorSpaceFB space, bool force_opaque, float underlay_opacity,
                                    bool premultiply, const std::array<float, 3>* key) {
  // A game's XR_FB_color_space on a host without it is not converted: the
  // host has no colour management to say what its panel shows, and a pass over
  // every eye texel each frame costs more than the guess is worth (FramePort
  // passes colours through too). Only alpha work takes a pass.
  (void)space;
  const bool alpha = force_opaque || underlay_opacity != 1 || key;
  if (!alpha || !(r.host_info.usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT)) return std::nullopt;
  auto& images = alpha ? r.alpha_images : r.color_images;
  if (!images) images = std::make_unique<ColorImages>(state().graphics,
      gfx::gpu_device().get_instance_proc_addr, r.host_info, r.host_images, false, alpha);
  return ColorPass{images.get(),
                   color_matrix(XR_COLOR_SPACE_REC709_FB, XR_COLOR_SPACE_REC709_FB),
                   force_opaque ? ColorImages::Alpha::opaque
                   : premultiply ? ColorImages::Alpha::premultiply : ColorImages::Alpha::preserve,
                   force_opaque ? 1 : underlay_opacity, force_opaque ? nullptr : key};
}
// Whether the pass can draw from the game's image straight into the host
// image (one pass that only writes the host image) rather than copy into it
// and convert it in place, which reads the host image back.
bool direct_color_pass(const Swapchain& r) {
  return r.backing && r.backing->sampled() && r.info.format == r.host_info.format &&
         r.info.width == r.host_info.width && r.info.height == r.host_info.height && !r.flip_rows &&
         r.info.sampleCount == r.host_info.sampleCount && r.host_info.mipCount == 1 &&
         r.info.arraySize * r.info.faceCount == r.host_info.arraySize * r.host_info.faceCount;
}
}  // namespace
XrResult convert_color_image(Swapchain& r, std::uint32_t index, XrColorSpaceFB space,
                             bool external_queue_held, bool force_opaque, float underlay_opacity, bool premultiply,
                             const std::array<float, 3>* key) {
  const auto pass = color_pass(r, space, force_opaque, underlay_opacity, premultiply, key);
  if (!pass) return XR_SUCCESS;
  return pass->images->convert(index, pass->matrix, external_queue_held, pass->alpha, pass->opacity, pass->key);
}

XrResult destroy_host_swapchain(Swapchain& r) {
  if (!r.host_handle) return XR_SUCCESS;
  const auto drained = r.backing->drain();
  if (drained != XR_SUCCESS) return drained;
  r.color_images.reset();
  r.alpha_images.reset();
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
namespace {
// A guest subimage rect in the host image, as PresentationLayers maps it for
// the host: clipped to the guest chain, rows flipped like the copy, scaled to
// a smaller host chain. Row 0 is the top the host displays.
XrRect2Di host_image_rect(const Swapchain& r, XrRect2Di rect) {
  const auto gw = std::int64_t(r.info.width), gh = std::int64_t(r.info.height);
  const auto x0 = std::clamp<std::int64_t>(rect.offset.x, 0, gw - 1), y0 = std::clamp<std::int64_t>(rect.offset.y, 0, gh - 1);
  const auto x1 = std::clamp<std::int64_t>(std::int64_t(rect.offset.x) + rect.extent.width, x0 + 1, gw);
  const auto y1 = std::clamp<std::int64_t>(std::int64_t(rect.offset.y) + rect.extent.height, y0 + 1, gh);
  rect = {{std::int32_t(x0), std::int32_t(y0)}, {std::int32_t(x1 - x0), std::int32_t(y1 - y0)}};
  // A zero-copy image is the game's own: no flip, no scaling.
  if (r.present_direct) return rect;
  if (r.textures.empty() == r.flip_rows) rect.offset.y = std::int32_t(gh - y1);
  return map_presentation_rect(rect, {r.info.width, r.info.height}, {r.host_info.width, r.host_info.height});
}
}  // namespace
std::optional<MirrorEye> mirror_eye(std::span<const XrCompositionLayerBaseHeader* const> layers, std::uint32_t eye) {
  for (auto* layer : layers)
    if (layer && layer->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
      const auto& projection = *reinterpret_cast<const XrCompositionLayerProjection*>(layer);
      if (!projection.viewCount || !projection.views) return std::nullopt;
      const auto& view = projection.views[venture::xr::mirror_view(projection.viewCount, eye)];
      return MirrorEye{view.subImage, view.fov};
    }
  return std::nullopt;
}
void register_foveation(const Swapchain& r) {
  // Eye chains are colour images the game renders into; small chains (UI
  // quads, icons) are left out. A chain that never shows in a projection
  // layer keeps a full-rate map.
  if (!gfx::foveation::wanted() || r.images.empty() || depth_format(r.info.format) ||
      !(r.info.usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) || r.info.width < 512 || r.info.height < 512)
    return;
  std::vector<gfx::foveation::EyeImage> images;
  for (const auto& image : r.images) images.push_back({image.image, 0});
  gfx::foveation::register_eye_swapchain(state().graphics.device, r.handle, images, r.info.width, r.info.height,
                                         r.info.arraySize * r.info.faceCount, false);
}
void forget_foveation(const Swapchain& r) {
  if (gfx::foveation::wanted()) gfx::foveation::forget_eye_swapchain(state().graphics.device, r.handle);
}
namespace {
std::set<XrSwapchain>& fixed_chains() {
  thread_local std::set<XrSwapchain> chains;
  return chains;
}
// Where each eye of a projection layer looks, as a direction in that eye's
// view space: straight ahead for fixed foveation; for eye-tracked, the gaze
// (from the head, in its frame) meets a point 2 m away, and each eye looks at
// that point from its own position, so both eyes keep the same part of the
// scene sharp (and canted displays get their own centres). The last gaze is
// held through a blink; after that, nullopt (fixed regions).
std::optional<std::array<gfx::foveation::Centre, 2>> gaze_centres(const XrCompositionLayerProjection& projection,
                                                                 XrTime time) {
  if (!gfx::foveation::eye_tracked() || projection.viewCount < 2 || !projection.views) return std::nullopt;
  static std::optional<XrVector3f> held;
  static XrTime held_time = 0;
  constexpr XrTime kBlink = 300'000'000;  // ns
  if (auto gaze = venture::xr::eye_gaze::direction(time)) {
    held = gaze;
    held_time = time;
  } else if (!held || time - held_time > kBlink) {
    held.reset();
    return std::nullopt;
  }
  using namespace venture::xr;
  const auto& a = projection.views[0].pose;
  const auto& b = projection.views[1].pose;
  // The head: between the eyes, turned halfway between them.
  auto qb = b.orientation;
  if (a.orientation.x * qb.x + a.orientation.y * qb.y + a.orientation.z * qb.z + a.orientation.w * qb.w < 0)
    qb = {-qb.x, -qb.y, -qb.z, -qb.w};
  XrQuaternionf head{a.orientation.x + qb.x, a.orientation.y + qb.y, a.orientation.z + qb.z, a.orientation.w + qb.w};
  const float n = std::sqrt(head.x * head.x + head.y * head.y + head.z * head.z + head.w * head.w);
  if (!(n > 0)) return std::nullopt;
  head = {head.x / n, head.y / n, head.z / n, head.w / n};
  const XrVector3f middle{(a.position.x + b.position.x) / 2, (a.position.y + b.position.y) / 2,
                          (a.position.z + b.position.z) / 2};
  const float gaze_length = length(*held);
  if (!(gaze_length > 0)) return std::nullopt;
  constexpr float kDistance = 2.0f;  // m
  const auto offset = rotate(head, {held->x / gaze_length * kDistance, held->y / gaze_length * kDistance,
                                    held->z / gaze_length * kDistance});
  const XrVector3f point{middle.x + offset.x, middle.y + offset.y, middle.z + offset.z};
  std::array<gfx::foveation::Centre, 2> out;
  for (int eye = 0; eye < 2; ++eye) {
    const auto& pose = projection.views[eye].pose;
    const auto d = rotate(inverse(pose.orientation), {point.x - pose.position.x, point.y - pose.position.y,
                                                      point.z - pose.position.z});
    out[eye] = {d.x, d.y, d.z};
  }
  return out;
}
}  // namespace

void foveate_eye_views(std::span<const XrCompositionLayerBaseHeader* const> layers, XrTime time) {
  if (!gfx::foveation::wanted()) return;
  std::map<XrSwapchain, std::vector<gfx::foveation::EyeView>> chains;
  bool scene_done = false;
  for (auto* layer : layers) {
    if (!layer || layer->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) continue;
    const auto& projection = *reinterpret_cast<const XrCompositionLayerProjection*>(layer);
    const auto centres = gaze_centres(projection, time);
    std::vector<gfx::foveation::EyeView> layer_views;
    for (std::uint32_t i = 0; projection.views && i < projection.viewCount; ++i) {
      const auto& view = projection.views[i];
      const auto& rect = view.subImage.imageRect;
      gfx::foveation::EyeView v{view.subImage.imageArrayIndex, rect.offset.x, rect.offset.y,
                                std::uint32_t(std::max(0, rect.extent.width)),
                                std::uint32_t(std::max(0, rect.extent.height)),
                                {view.fov.angleLeft, view.fov.angleRight, view.fov.angleUp, view.fov.angleDown},
                                centres && i < 2 ? (*centres)[i] : gfx::foveation::Centre{}};
      chains[view.subImage.swapchain].push_back(v);
      layer_views.push_back(v);
    }
    // The first projection layer's eyes also shape the game's own stereo targets.
    if (!scene_done && layer_views.size() >= 2) {
      scene_done = true;
      const auto it = state().swapchains.find(projection.views[0].subImage.swapchain);
      const auto requested = it == state().swapchains.end() ? Foveation{} : it->second.foveation_requested;
      // The game's profile (level, vertical offset) is its own fixed foveation;
      // Venture's setting decides the pattern (gfx/foveation.h effective_level).
      gfx::foveation::update_scene_views(layer_views[0], layer_views[1],
                                         gfx::foveation::Level(std::clamp<int>(requested.level, 0, 3)), 0,
                                         centres.has_value());
    }
    // A chain shows the gaze only when all its views come from layers that follow it.
    if (!centres)
      for (std::uint32_t i = 0; projection.views && i < projection.viewCount; ++i)
        fixed_chains().insert(projection.views[i].subImage.swapchain);
  }
  for (auto& [chain, views] : chains) {
    const auto it = state().swapchains.find(chain);
    if (it == state().swapchains.end()) continue;
    const auto& requested = it->second.foveation_requested;
    const bool gaze = !fixed_chains().contains(chain);
    gfx::foveation::update_eye_views(chain, views, gfx::foveation::Level(std::clamp<int>(requested.level, 0, 3)), 0,
                                     gaze);
  }
  fixed_chains().clear();
}
namespace {
// A color format the guest copy can scale with a filtered blit (guest_images.cpp).
bool scalable_color_format(std::int64_t format) {
  switch (VkFormat(format)) {
  case VK_FORMAT_D16_UNORM: case VK_FORMAT_X8_D24_UNORM_PACK32: case VK_FORMAT_D32_SFLOAT:
  case VK_FORMAT_D16_UNORM_S8_UINT: case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_D32_SFLOAT_S8_UINT:
  case VK_FORMAT_S8_UINT: return false;
  default: return gfx::supports_filtered_blit(format);
  }
}
// Whether this copy into a multi-slice color host chain is sampled for the eye
// slice check (GuestImages::sample_layers): a one-time GPU readback of an 8x8
// texel grid per slice of the game's image and of the host image, which tells a
// slice the game never drew from one the copy or the host lost (a single-eye
// report: H3VR2 on an Intel Arc with Virtual Desktop). By default the 600th and
// 5400th copies (about 7 s and 1 min at 90 Hz) of each such chain are checked,
// at most four checks a session; VENTURE_XR_LAYER_CHECK=0 turns it off and
// VENTURE_XR_LAYER_CHECK=<n> checks every n-th copy instead (diagnostics only).
bool eye_slice_check_due(Swapchain& r) {
  ++r.host_copies;
  const bool color = r.present_direct ? !depth_format(r.info.format)
                                      : bool(r.host_info.usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT);
  if (r.info.arraySize * r.info.faceCount < 2 || r.info.sampleCount != 1 || !color)
    return false;
  static const long every = [] {
    const auto* value = venture::environment::getenv("VENTURE_XR_LAYER_CHECK");
    return value && *value ? std::strtol(value, nullptr, 10) : -1L;
  }();
  if (every == 0) return false;
  if (every > 0) return r.host_copies % std::uint64_t(every) == 0;
  static std::atomic_uint checks{};
  if (r.host_copies != 600 && r.host_copies != 5400) return false;
  return checks.fetch_add(1, std::memory_order_relaxed) < 4;
}
}  // namespace
XrResult prepare_swapchains(const std::set<XrSwapchain>& submitted, bool force_opaque, const UnderlayAlpha* underlay,
                            const MirrorEye* mirror, const std::map<XrSwapchain, VkExtent2D>* eye_rects) {
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
  // This frame's alpha pass for a chain (none: opaque false, opacity 1, no key).
  struct FrameAlpha {
    bool opaque = false;
    float opacity = 1;
    bool premultiply = false;
    std::optional<std::array<float, 3>> key;
  };
  const auto frame_alpha = [&](XrSwapchain handle, bool color) {
    FrameAlpha a;
    a.opaque = force_opaque && color;
    const bool lowest = !a.opaque && color && underlay && underlay->swapchains.contains(handle);
    a.opacity = lowest ? underlay->opacity : 1;
    a.premultiply = lowest && underlay->premultiply;
    a.key = lowest ? underlay->key : std::nullopt;
    return a;
  };
  for (auto handle : submitted) {
    auto& r = s.swapchains.at(handle);
    if (r.operation_in_progress) return XR_ERROR_CALL_ORDER_INVALID;
    if (r.released_index < 0) return XR_ERROR_LAYER_INVALID;
    r.present_direct = r.release_direct = false;
  }
  // First allocate all host handles; never evict a member of this frame while
  // preparing another member. The limit is learned solely from host results.
  for (auto handle : submitted) {
    auto& r = s.swapchains.at(handle);
    // A zero-copy chain presents its held image as it is unless this frame
    // needs a copy: a flip or an alpha pass. An eye rendered past the size the
    // game was told goes to the host as it is: the compositor samples it once
    // either way, and a downscale would read and write every texel first.
    if (r.direct_handle) {
      check_ownership(r);
      {
        const VkExtent2D* eye_rect = nullptr;
        if (eye_rects)
          if (const auto found = eye_rects->find(handle); found != eye_rects->end()) eye_rect = &found->second;
        const auto reported = venture::xr::reported_eye();
        if (eye_rect && !std::exchange(r.eye_observed, true))
          venture::xr::eye_scale::observe(reported.told, reported.target, *eye_rect);
        const bool color = !depth_format(r.info.format);
        const auto alpha = frame_alpha(handle, color);
        const bool copy = r.copy_always || r.flip_rows || alpha.opaque || alpha.opacity != 1 || alpha.key;
        if (!copy) {
          r.present_direct = true;
          r.release_direct = r.held.has_value();
          continue;
        }
        if (!r.held) {
          // The image is already the host's (presented, or given back before
          // the game's next wait): the host chain keeps its last copy (for a
          // copy_always chain even an older one), or without one the image is
          // shown as it is.
          if (!r.host_handle || (!r.copy_always && r.host_serial != r.release_serial)) {
            r.present_direct = true;
            static std::atomic_flag reported_late = ATOMIC_FLAG_INIT;
            if (!reported_late.test_and_set(std::memory_order_relaxed))
              log::warn("xr", "A zero-copy swapchain needed a copy after its image went back to the host; "
                        "that image is shown as it is");
          }
          continue;
        }
        r.release_direct = true;  // copied into the host chain below, then given back
      }
    }
    if (r.host_handle) continue;
    r.host_info = r.info;
    r.host_info.next = nullptr;
    r.host_info.createFlags &= ~XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT;
    // Guest images keep every mip; the host compositor owns filtering. Some hosts
    // accept a mipmapped request but allocate one level (SteamVR), as in VrApi.
    r.host_info.mipCount = 1;
    // An eye image rendered past the eye size the game was told presents
    // through a host chain of that size (eye_presentation_extent). Scaled
    // copies blit, so only single-sample color chains in a format that blits
    // with filtering qualify; depth stays.
    const VkExtent2D* eye_rect = nullptr;
    if (eye_rects)
      if (const auto found = eye_rects->find(handle); found != eye_rects->end()) eye_rect = &found->second;
    if (eye_rect) {
      // The game's eye against the size it was told teaches its multiplier.
      const auto reported = venture::xr::reported_eye();
      venture::xr::eye_scale::observe(reported.told, reported.target, *eye_rect);
      const auto size = venture::xr::eye_presentation_extent({r.info.width, r.info.height}, *eye_rect, reported.target);
      if ((size.width != r.info.width || size.height != r.info.height) && r.info.sampleCount == 1 &&
          scalable_color_format(r.info.format)) {
        r.host_info.width = size.width;
        r.host_info.height = size.height;
        log::info("xr", "eye swapchain {}x{} (eye rect {}x{}) exceeds the eye size {}x{}; presenting "
                  "through {}x{}", r.info.width, r.info.height, eye_rect->width, eye_rect->height,
                  reported.target.width, reported.target.height, size.width, size.height);
      }
    }
    // Fit the host's maximum with the guest aspect; the copy scales and layer
    // rects follow (map_subimage). Sizes within the maximum stay 1:1. A zero
    // maximum is no limit: the Oculus PC runtime (Link 2.18) reports 0x0.
    XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES};
    if (XR_SUCCEEDED(::xrGetSystemProperties(host_xr().instance, host_xr().system, &properties)) &&
        properties.graphicsProperties.maxSwapchainImageWidth && properties.graphicsProperties.maxSwapchainImageHeight) {
      const auto& limits = properties.graphicsProperties;
      const VkExtent2D wanted{r.host_info.width, r.host_info.height};
      const auto size = fit_presentation_extent(wanted, limits.maxSwapchainImageWidth, limits.maxSwapchainImageHeight);
      if (size.width != wanted.width || size.height != wanted.height) {
        r.host_info.width = size.width;
        r.host_info.height = size.height;
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
    // What the game asked for, next to the host chain's own line: a report then
    // shows a layer's format, static flag, mips and usage as the game made it.
    log::info("xr", "Host swapchain is for the game's {}x{} format={} usage={:#x} flags={:#x} mips={} samples={} "
              "arrays={} faces={}", r.info.width, r.info.height, r.desc.gl_format, r.info.usageFlags,
              r.info.createFlags, r.info.mipCount, r.info.sampleCount, r.info.arraySize, r.info.faceCount);
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
    if (r.present_direct) {
      if (!r.release_direct) continue;  // the host has had this release since an earlier frame
      // The held image is the game's eye itself: the eye slice check and the
      // desktop mirror read it before it goes back to the host.
      const auto held = *r.held;
      const auto image = r.images.at(held).image;
      const auto layers = r.info.arraySize * r.info.faceCount;
      if (eye_slice_check_due(r))
        log::info("xr", "Eye slice check after {} presentations ({}x{} arrays={}, zero-copy): {}", r.host_copies,
                  r.info.width, r.info.height, r.info.arraySize,
                  r.backing->sample_layers(held, image, VkFormat(r.info.format), {r.info.width, r.info.height},
                                           layers));
      if (mirror && handle == mirror->sub.swapchain && r.info.sampleCount == 1 && !depth_format(r.info.format) &&
          mirror->sub.imageArrayIndex < r.info.arraySize) {
        venture::xr::MirrorSource source{image, VkFormat(r.info.format), mirror->sub.imageArrayIndex,
                                         host_image_rect(r, mirror->sub.imageRect)};
        venture::xr::mirror_axis(mirror->fov, source.axis_x, source.axis_y);
        venture::xr::mirror_capture(session().binding, session().uses_shared_gpu, source);
      }
      ++r.direct_frames;
      continue;
    }
    // A zero-copy image the host already has keeps the host chain's last copy.
    if (r.direct_handle && !r.held) continue;
    const bool color = r.host_info.usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    const auto alpha = frame_alpha(handle, color);
    const bool opaque = alpha.opaque;
    const float opacity = alpha.opacity;
    const bool premultiply = alpha.premultiply;
    const auto key = alpha.key;
    if (r.host_serial == r.release_serial && r.host_color == s.color_space && r.host_flip == r.flip_rows &&
        r.host_opaque == opaque && r.host_underlay == opacity && r.host_premultiplied == premultiply &&
        r.host_key == key) continue;
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
    const auto released = std::uint32_t(r.released_index);
    const auto host_image = r.host_images.at(target).image;
    const auto sample = [&] {
      return r.backing->sample_layers(released, host_image, VkFormat(r.host_info.format),
                                      {r.host_info.width, r.host_info.height},
                                      r.host_info.arraySize * r.host_info.faceCount);
    };
    const bool check = eye_slice_check_due(r);
    const auto pass = color_pass(r, s.color_space, opaque, opacity, premultiply, key ? &*key : nullptr);
    // A color pass draws from the game's image into the host image, which it
    // only writes; reading a runtime's externally shared image back gave a
    // solid fill on one driver (AMD with Virtual Desktop).
    const char* path = pass ? "copy, then conversion in place" : "copy";
    result = XR_ERROR_LIMIT_REACHED;
    if (pass && direct_color_pass(r)) {
      result = r.backing->submit(released, [&](VkCommandBuffer command, VkImage image, VkImageLayout layout) {
        return pass->images->record_from(command, image, layout, target, pass->matrix, pass->alpha, pass->opacity,
                                         pass->key);
      });
      path = "conversion pass from the game's image";
    }
    std::string after_copy;
    if (result == XR_ERROR_LIMIT_REACHED) {
      result = r.backing->copy(released, host_image, VkFormat(r.host_info.format),
                               {r.host_info.width, r.host_info.height}, r.flip_rows);
      // Sampled between the stages too, so a report tells a lost copy from a
      // broken conversion.
      if (XR_SUCCEEDED(result) && check && pass) after_copy = sample();
      if (XR_SUCCEEDED(result) && pass)
        result = pass->images->convert(target, pass->matrix, false, pass->alpha, pass->opacity, pass->key);
    }
    if (XR_SUCCEEDED(result) && check)
      log::info("xr", "Eye slice check after {} copies ({}x{} arrays={} -> host {}x{} arrays={}, {}): {}{}{}",
                r.host_copies, r.info.width, r.info.height, r.info.arraySize, r.host_info.width,
                r.host_info.height, r.host_info.arraySize, path,
                after_copy.empty() ? "" : "after the copy: ", after_copy.empty() ? "" : after_copy + "; at the end: ",
                sample());
    // The desktop mirror blits the eye exactly as the headset gets it, before
    // the image goes back to the runtime.
    if (XR_SUCCEEDED(result) && mirror && handle == mirror->sub.swapchain && r.info.sampleCount == 1 &&
        (r.host_info.usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) &&
        mirror->sub.imageArrayIndex < r.host_info.arraySize) {
      venture::xr::MirrorSource source{r.host_images.at(target).image, VkFormat(r.host_info.format),
                                       mirror->sub.imageArrayIndex, host_image_rect(r, mirror->sub.imageRect)};
      venture::xr::mirror_axis(mirror->fov, source.axis_x, source.axis_y);
      venture::xr::mirror_capture(session().binding, session().uses_shared_gpu, source);
    }
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
    r.host_opaque = opaque;
    r.host_underlay = opacity;
    r.host_premultiplied = premultiply;
    r.host_key = key;
  }
  return qualified;
}
XrResult release_direct_images(const std::set<XrSwapchain>& submitted) {
  auto& s = state();
  XrResult result = XR_SUCCESS;
  for (auto handle : submitted) {
    const auto found = s.swapchains.find(handle);
    if (found == s.swapchains.end()) continue;
    auto& r = found->second;
    if (!std::exchange(r.release_direct, false) || !r.held) continue;
    const auto released = release_held(r);
    if (XR_FAILED(released)) result = released;
  }
  return result;
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
    gl = gles_swapchain_formats(formats);
  } else {
    // Unity's space warp ends the session unless its D24S8 motion depth is offered.
    gl = formats;
    for (auto depth : {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_X8_D24_UNORM_PACK32})
      if (substituted_depth(depth, formats) != VK_FORMAT_UNDEFINED) gl.push_back(depth);
    // A Quest lists depth chains whatever the host offers, and create_swapchain
    // keeps an unoffered depth chain the guest's own. Unity checks this list and
    // ends the session ("Failed to create swapchain") when its depth format is
    // missing: Epic Roller Coasters on a host listing no depth (SteamVR on AMD).
    for (auto depth : {VK_FORMAT_D16_UNORM, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT})
      if (std::find(gl.begin(), gl.end(), depth) == gl.end()) gl.push_back(depth);
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
  const bool gles_ring = state().gles && !surface;
  // A Vulkan chain renders straight into a host chain's images when nothing
  // needs a private copy (docs/design/zero-copy-eyes.md).
  std::string copy_reason = gles_ring ? "OpenGL ES" : surface ? "an Android surface" : "";
  if (!gles_ring && !surface) create_direct_swapchain(record, guest_only, offered(copy.format), copy_reason);
  {
    static std::atomic_uint reports{};
    if (reports.fetch_add(1, std::memory_order_relaxed) < 48)
      log::info("xr", "Swapchain {}x{} format={} usage={:#x} arrays={} samples={} mips={}: {}", copy.width,
                copy.height, copy.format, copy.usageFlags, copy.arraySize, copy.sampleCount, copy.mipCount,
                record.direct_handle ? std::format("zero-copy, the host's {} images", record.images.size())
                                     : "private images and a copy (" + copy_reason + ")");
  }
  try {
    if (!record.direct_handle) {
      // A GLES chain's slots start without private images (create_gles_storage).
      record.backing = std::make_unique<GuestImages>(state().graphics,
          gfx::gpu_device().get_instance_proc_addr, copy, gles_ring);
      const auto images = record.backing->images();
      record.images.assign(images.begin(), images.end());
    }
    record.identity = std::make_unique<std::uint64_t>(0);
    record.handle = reinterpret_cast<XrSwapchain>(record.identity.get());
  } catch (XrResult result) {
    return result;
  } catch (const std::exception& error) {
    log::error("xr", "Guest swapchain allocation failed: {}", error.what());
    return XR_ERROR_OUT_OF_MEMORY;
  }
  XrResult result = XR_SUCCESS;
  if (gles_ring) {
    // Everything a GLES chain needs is allocated here, so a device out of
    // memory fails creation (OUT_OF_MEMORY), never a later release.
    result = create_gles_storage(record);
    if (XR_SUCCEEDED(result) && enabled("XR_FB_swapchain_update_state"))
      result = xr_images()->update_sampler(record.textures, record.desc, record.sampler_gles);
  } else if (!surface) {
    note_images(record);
    register_foveation(record);
  }
  if (XR_FAILED(result)) {
    forget_images(record);
    if (staged(record)) xr_images()->destroy_staging(record.backing->staged());
    if (!record.textures.empty()) xr_images()->destroy_ring(record.textures);
    log::warn("xr", "Guest swapchain {}x{} arrays={} format={} not created: result={}", record.info.width,
              record.info.height, record.info.arraySize, info->format, int(result));
    return result;
  }
  if (gles_ring && venture::environment::getenv("VENTURE_XR_SWAPCHAIN_STATS"))
    log::info("xr-budget", "gles chain {}x{} arrays={} slots={} staged={} private_bytes={}", record.info.width,
              record.info.height, record.info.arraySize, record.images.size(), staged(record),
              record.backing->private_bytes());
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
  if (!it->second.textures.empty() && !staged(it->second)) {
    std::vector<VkImage> images;
    for (auto image : it->second.images)
      images.push_back(image.image);
    auto released = xr_images()->release_images(images);
    if (XR_FAILED(released))
      return released;
  }
  auto result = destroy_host_swapchain(it->second);
  if (XR_SUCCEEDED(result)) result = forget_sphere_source(handle);
  if (XR_SUCCEEDED(result)) result = destroy_direct_swapchain(it->second);
  // Staging is freed only after every Vulkan copy that reads it completed.
  if (XR_SUCCEEDED(result) && staged(it->second)) result = it->second.backing->drain();
  if (XR_SUCCEEDED(result)) {
    forget_foveation(it->second);
    forget_images(it->second);
    if (staged(it->second)) xr_images()->destroy_staging(it->second.backing->staged());
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
            // On a GPU with density maps the game's eye passes read this map:
            // Venture's foveation fills it with its pattern.
            if (gfx::foveation::wanted())
              gfx::foveation::attach_density_map(binding.device, r.handle, r.full_rate_resource->image(),
                                                 r.full_rate_resource->width(), r.full_rate_resource->height(),
                                                 r.info.arraySize * r.info.faceCount);
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
  if (r.direct_handle) {
    if (r.operation_in_progress) return XR_ERROR_CALL_ORDER_INVALID;
    check_ownership(r);
    const auto count = std::uint32_t(r.images.size());
    if (r.host_acquired.size() == count) {
      // The game holds every other image: its last released one goes back first.
      const auto released = release_held(r);
      if (XR_FAILED(released)) return released;
    }
    std::uint32_t acquired{};
    XrResult result;
    {
      HostQueueLock queue(session().uses_shared_gpu);
      if (!queue) return XR_ERROR_RUNTIME_FAILURE;
      result = ::xrAcquireSwapchainImage(r.direct_handle, nullptr, &acquired);
    }
    if (XR_FAILED(result)) return result;
    if (acquired >= count || std::find(r.host_acquired.begin(), r.host_acquired.end(), acquired) != r.host_acquired.end()) {
      log::error("xr", "The host returned swapchain image {} it had already given out", acquired);
      return XR_ERROR_RUNTIME_FAILURE;
    }
    r.host_acquired.push_back(acquired);
    r.host_unwaited[acquired] = 1;
    r.backing->set_owned(acquired, true);
    r.ownership[acquired]->guest.store(true, std::memory_order_release);
    r.acquired.push_back(acquired);
    *index = acquired;
    return result;
  }
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
  // The host waits one image at a time: the held one, which it waited for the
  // game, goes back before the next. A frame ended after this presents it as
  // the host's last released image.
  if (it->second.direct_handle && it->second.host_unwaited.at(it->second.acquired.front()))
    if (const auto released = release_held(it->second); XR_FAILED(released)) return released;
  ImageOperation operation(it->second, lock);
  lock.unlock();
  const auto wait_start = vulkan::timing_enabled() ? monotonic_ns() : 0;
  auto& chain = it->second;
  const auto front = chain.acquired.front();
  XrResult result = XR_SUCCESS;
  if (chain.direct_handle && chain.host_unwaited.at(front)) {
    // The pin keeps every other call on this chain out while the host waits.
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, info->timeout};
    result = ::xrWaitSwapchainImage(chain.direct_handle, &wait);
    if (result == XR_SUCCESS || result == XR_SESSION_LOSS_PENDING) chain.host_unwaited[front] = 0;
  }
  if (chain.direct_handle && !chain.initialized.at(front) && (result == XR_SUCCESS || result == XR_SESSION_LOSS_PENDING)) {
    const auto initialized = chain.backing->initialize(front);
    if (XR_FAILED(initialized)) result = initialized;
    else {
      chain.initialized[front] = 1;
      vulkan::track_layouts(chain.images[front].image, 1, chain.info.arraySize * chain.info.faceCount,
                            depth_format(chain.info.format) ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                                                            : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    }
  }
  if (result == XR_SUCCESS) result = chain.backing->wait(front, info->timeout);
  else if (result == XR_SESSION_LOSS_PENDING && XR_FAILED(chain.backing->wait(front, info->timeout)))
    result = XR_ERROR_RUNTIME_FAILURE;
  if (wait_start) vulkan::timing_host_wait("xr_image", monotonic_ns() - wait_start);
  lock.lock();
  if (result == XR_SUCCESS || (result == XR_SESSION_LOSS_PENDING && chain.direct_handle)) {
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
    // ANGLE may take staging back only once no Vulkan copy still reads it.
    // xrWaitSwapchainImage normally waited that copy already; the operation
    // pin keeps uploads of this chain out until the release returns.
    auto result = staged(r) ? r.backing->wait(index) : XR_SUCCESS;
    if (XR_SUCCEEDED(result))
      result = staged(r)
          ? xr_images()->stage(r.textures[index], r.backing->staged()[index], r.desc)
          : xr_images()->copy_to_image(r.textures[index], r.images[index].image, r.desc, 0, true);
    lock.lock();
    if (XR_FAILED(result))
      return result;
  }
  if (r.direct_handle) {
    const auto index = r.acquired.front();
    {
      // The previous release, never presented (or presented through a copy),
      // goes back to the host; this one stays Venture's until its frame.
      const auto superseded = release_held(r);
      if (XR_FAILED(superseded)) return superseded;
      r.held = index;
    }
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
  // A game's own instance is asked through enable; one the host made through
  // xrCreateVulkanInstanceKHR through enable2 first (host_graphics_device).
  auto result = venture::xr::host_graphics_device(info->systemId, info->vulkanInstance, out);
  if (XR_FAILED(result)) {
    // The host runtime already chose the GPU for Venture's shared device. Some
    // runtimes refuse the query for an instance the game made itself (Oculus
    // with an AMD GPU: RUNTIME_FAILURE, Epic Roller Coasters); that GPU in the
    // game's instance is the answer the query stands for.
    const auto physical = vulkan::shared_physical_device_in(info->vulkanInstance);
    log::warn("xr", "Host Vulkan graphics device query failed ({}); {}", static_cast<int>(result),
              physical ? std::format("using the shared device's GPU ({}) in the game's instance",
                                     vulkan::physical_device_name(physical))
                       : std::string("the shared device's GPU is not in it"));
    if (!physical) {
      // Engines stop here (Unity's OpenXR plugin aborts); the launcher names
      // this line instead of that abort (library::runtime_failure).
      log::error("xr", "The VR session could not be created: {} did not name the game's Vulkan GPU (XrResult {})",
                 venture::xr::host_xr().runtime_name, static_cast<int>(result));
      return result;
    }
    *out = physical;
    state().vouched_vulkan_gpus[physical] = info->vulkanInstance;
    result = XR_SUCCESS;
  } else {
    state().vouched_vulkan_gpus.erase(*out);
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
  // The game's own Vulkan layers are Android ones the host does not have; the
  // instance starts without them, so any layer left in the list Venture's
  // vkCreateInstance receives is one the host XR stack added (xr/vulkan_layers.h).
  if (vk.enabledLayerCount) {
    std::string layers;
    for (std::uint32_t n = 0; n < vk.enabledLayerCount && vk.ppEnabledLayerNames; ++n)
      if (vk.ppEnabledLayerNames[n]) layers += (layers.empty() ? "" : ", ") + std::string(vk.ppEnabledLayerNames[n]);
    log::warn("xr", "The game's Vulkan instance asked for layers {}; it starts without them", layers);
    vk.enabledLayerCount = 0;
    vk.ppEnabledLayerNames = nullptr;
  }
  auto copy = *info;
  copy.vulkanCreateInfo = &vk;
  vulkan::XrResolverScope resolver(reinterpret_cast<GuestAddr>(info->pfnGetInstanceProcAddr));
  copy.pfnGetInstanceProcAddr = vulkan::XrResolverScope::proc;
  const auto result = host_call<PFN_xrCreateVulkanInstanceKHR, "xrCreateVulkanInstanceKHR">(i, &copy, out,
                                                                                         vk_result);
  if (XR_SUCCEEDED(result) && *vk_result == VK_SUCCESS)
    venture::xr::note_runtime_created_instance(*out);
  return result;
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
  const auto vouched = state().vouched_vulkan_gpus.find(info->vulkanPhysicalDevice);
  if (XR_FAILED(result) && !resolver.device_created() && vouched != state().vouched_vulkan_gpus.end()) {
    // The host refused the GPU query for this game's instance, so Venture
    // named the GPU (xrGetVulkanGraphicsDevice2KHR), and the host then refuses
    // to create a device on it without having called vkCreateDevice (Oculus
    // on an AMD + NVIDIA laptop: XR_ERROR_HANDLE_INVALID, Kaiju Battle
    // Simulator). Create it as the runtime would: through the game's resolver,
    // with the runtime's device extensions added. XR_KHR_vulkan_enable games
    // make that same device themselves, and sessions accept it on that GPU.
    log::warn("xr", "Host xrCreateVulkanDeviceKHR refused the game's GPU ({}); creating the device through the "
              "game's resolver with the runtime's extensions", int(result));
    const auto create = reinterpret_cast<PFN_vkCreateDevice>(
        vulkan::XrResolverScope::proc(vouched->second, "vkCreateDevice"));
    *vk_result = create ? create(info->vulkanPhysicalDevice, &vk, info->vulkanAllocator, out)
                        : VK_ERROR_INITIALIZATION_FAILED;
    result = *vk_result == VK_SUCCESS ? XR_SUCCESS : XR_ERROR_RUNTIME_FAILURE;
  }
  if (XR_FAILED(result) || *vk_result != VK_SUCCESS)
    log::error("xr", "Vulkan device creation failed: XrResult={} VkResult={}", int(result),
               int(*vk_result));
  return result;
}
} // namespace guest
} // namespace venture::hle::openxr

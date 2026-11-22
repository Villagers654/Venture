#include "gfx/foveation_vulkan.h"

#include "common/environment.h"
#include "common/log.h"
#include "xr/eye_gaze.h"

#include <algorithm>
#include <array>
#include <bit>
#include <optional>
#include <string>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace venture::gfx::foveation {
namespace {
// Every foveation image has this usage and format, so a full-rate image and an
// eye map can stand in for each other in an imageless framebuffer.
constexpr VkImageUsageFlags kMapUsage =
    VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
constexpr VkFormat kMapFormat = VK_FORMAT_R8_UINT;
// Foveation images stay in GENERAL: render passes read them there and the
// uploads write them there, so no pass changes their layout.
constexpr VkImageLayout kMapLayout = VK_IMAGE_LAYOUT_GENERAL;

std::atomic<int> host_eye_tracking{-1};  // -1: ask xr/eye_gaze.h

Mode configured_mode() {
  static const Mode mode = [] {
    const char* value = environment::getenv("VENTURE_FOVEATION");
    return parse_mode(value ? value : "");
  }();
  return mode;
}

struct Image {
  VkImage image{};
  VkDeviceMemory memory{};
  std::uint32_t width = 0, height = 0, layers = 1;
  bool cleared = false;  // initialized to full rate on the GPU
  std::map<std::pair<std::uint32_t, std::uint32_t>, VkImageView> views;  // (first layer, count)
};

struct Swapchain;
struct EyeEntry {
  Swapchain* swapchain = nullptr;
  std::uint32_t first_layer = 0;
};
struct ViewEntry {
  VkImage image{};
  std::uint32_t base_layer = 0, layer_count = 1;
};

// A foveation map and its contents: an eye swapchain's (one layer per eye
// layer plus their union) or a scene map's (two eyes plus their union).
struct MapState {
  std::unique_ptr<Image> map;
  std::vector<std::uint8_t> bytes, uploaded;
  std::string signature;
  std::uint32_t frames = 0;
};
// The eye views of the latest frame, for scene passes that render into the
// game's own stereo targets rather than its eye images.
struct ScenePattern {
  bool valid = false;
  Pattern pattern;
  Fov fov[2];
  Centre centre[2];
  std::string signature;
};
std::mutex scene_mutex;  // a leaf: nothing is locked while it is held
ScenePattern scene_pattern;
ScenePattern current_scene() {
  std::lock_guard lock(scene_mutex);
  return scene_pattern;
}

struct PassInfo {
  std::uint32_t index = 0;  // the attachment Venture added
  bool scene = false;       // stereo (multiview over two views) with depth: the game's 3D scene
};

struct Staging {
  VkBuffer buffer{};
  VkDeviceMemory memory{};
  void* mapped = nullptr;
  VkDeviceSize size = 0;
};

struct Device {
  VkDevice device{};
  VkPhysicalDevice physical{};
  VkPhysicalDeviceMemoryProperties memory{};
  VkExtent2D texel{16, 16};
  bool layered = false;  // layeredShadingRateAttachments
  bool active = false;   // shading-rate attachments enabled: passes get them
#define VFN(name) PFN_##name name = nullptr;
  VFN(vkCreateRenderPass) VFN(vkCreateRenderPass2) VFN(vkDestroyRenderPass) VFN(vkCreateFramebuffer)
  VFN(vkCreateImageView) VFN(vkDestroyImageView) VFN(vkCmdBeginRenderPass) VFN(vkCmdBeginRenderPass2)
  VFN(vkCmdBeginRendering) VFN(vkCreateImage) VFN(vkDestroyImage) VFN(vkAllocateMemory) VFN(vkFreeMemory)
  VFN(vkBindImageMemory) VFN(vkGetImageMemoryRequirements) VFN(vkCreateBuffer) VFN(vkDestroyBuffer)
  VFN(vkBindBufferMemory) VFN(vkGetBufferMemoryRequirements) VFN(vkMapMemory) VFN(vkCmdPipelineBarrier)
  VFN(vkCmdCopyBufferToImage) VFN(vkCmdClearColorImage)
#undef VFN
  std::mutex mutex;
  std::unordered_map<VkRenderPass, PassInfo> passes;
  std::unordered_map<VkImageView, ViewEntry> views;        // views of eye images
  std::unordered_map<VkImage, EyeEntry> eyes;
  std::unordered_map<VkFramebuffer, VkExtent2D> imageless;  // the rate image size each declares
  std::map<std::pair<std::uint32_t, std::uint32_t>, std::unique_ptr<Image>> full_rate;  // by extent in texels
  std::map<std::pair<std::uint32_t, std::uint32_t>, std::unique_ptr<MapState>> scene_maps;  // by extent in texels
  std::vector<std::unique_ptr<Image>> retired;  // eye maps of destroyed swapchains, freed with the device
  // Work recorded into the next command buffer that begins a pass: images to
  // clear to full rate, and swapchains whose map has new contents. Staging
  // buffers rotate; a slot is reused only after kStagingSlots more uploads,
  // far more frames than any game keeps in flight.
  std::vector<Image*> clears;
  std::vector<MapState*> uploads;
  std::atomic<bool> pending{false};
  static constexpr std::size_t kStagingSlots = 8;
  std::array<Staging, kStagingSlots> staging{};
  std::size_t next_staging = 0;
  std::vector<Staging> retired_staging;
};

struct Swapchain : MapState {  // map contents: the eye layers, then the combined layer
  Device* device = nullptr;
  std::uint32_t width = 0, height = 0, layers = 1;  // the eye images', in pixels
  bool bottom_up = false;
  std::vector<EyeImage> images;
};

std::mutex registry_mutex;  // devices and swapchains; each device's own state under its mutex
// By the loader's dispatch key, the first pointer of every dispatchable
// handle: a device and its command buffers share it.
std::unordered_map<void*, std::shared_ptr<Device>> devices;
std::unordered_map<const void*, std::unique_ptr<Swapchain>> swapchains;

void* dispatch_key(const void* handle) { return handle ? *static_cast<void* const*>(handle) : nullptr; }
std::shared_ptr<Device> find(const void* dispatchable) {
  std::lock_guard lock(registry_mutex);
  const auto it = devices.find(dispatch_key(dispatchable));
  return it == devices.end() ? nullptr : it->second;
}
// The driver's function for a device Venture does not foveate on (its
// devices are registered right after creation; this covers any other).
PFN_vkGetInstanceProcAddr fallback_gipa = nullptr;
VkInstance fallback_instance = VK_NULL_HANDLE;
template <class F>
F fallback(const char* name) {
  return fallback_gipa ? reinterpret_cast<F>(fallback_gipa(fallback_instance, name)) : nullptr;
}
std::uint32_t memory_type(const Device& d, std::uint32_t bits, VkMemoryPropertyFlags wanted) {
  for (std::uint32_t i = 0; i < d.memory.memoryTypeCount; ++i)
    if ((bits & (1u << i)) && (d.memory.memoryTypes[i].propertyFlags & wanted) == wanted) return i;
  return UINT32_MAX;
}

std::unique_ptr<Image> create_image(Device& d, std::uint32_t width, std::uint32_t height, std::uint32_t layers) {
  auto image = std::make_unique<Image>();
  image->width = std::max(1u, width);
  image->height = std::max(1u, height);
  image->layers = std::max(1u, layers);
  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ci.imageType = VK_IMAGE_TYPE_2D;
  ci.format = kMapFormat;
  ci.extent = {image->width, image->height, 1};
  ci.mipLevels = 1;
  ci.arrayLayers = image->layers;
  ci.samples = VK_SAMPLE_COUNT_1_BIT;
  ci.tiling = VK_IMAGE_TILING_OPTIMAL;
  ci.usage = kMapUsage;
  if (d.vkCreateImage(d.device, &ci, nullptr, &image->image) != VK_SUCCESS) return nullptr;
  VkMemoryRequirements requirements{};
  d.vkGetImageMemoryRequirements(d.device, image->image, &requirements);
  auto type = memory_type(d, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (type == UINT32_MAX) type = memory_type(d, requirements.memoryTypeBits, 0);
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, requirements.size, type};
  if (type == UINT32_MAX || d.vkAllocateMemory(d.device, &ai, nullptr, &image->memory) != VK_SUCCESS ||
      d.vkBindImageMemory(d.device, image->image, image->memory, 0) != VK_SUCCESS) {
    if (image->memory) d.vkFreeMemory(d.device, image->memory, nullptr);
    d.vkDestroyImage(d.device, image->image, nullptr);
    return nullptr;
  }
  d.clears.push_back(image.get());  // full rate until its first use clears it
  d.pending = true;
  return image;
}
void destroy_image(Device& d, Image& image) {
  for (auto& [key, view] : image.views) d.vkDestroyImageView(d.device, view, nullptr);
  if (image.image) d.vkDestroyImage(d.device, image.image, nullptr);
  if (image.memory) d.vkFreeMemory(d.device, image.memory, nullptr);
  image = {};
}
VkImageView image_view(Device& d, Image& image, std::uint32_t first, std::uint32_t count) {
  first = std::min(first, image.layers - 1);
  count = std::clamp(count, 1u, image.layers - first);
  auto& view = image.views[{first, count}];
  if (!view) {
    VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    ci.image = image.image;
    ci.viewType = count > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    ci.format = kMapFormat;
    ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, first, count};
    if (d.vkCreateImageView(d.device, &ci, nullptr, &view) != VK_SUCCESS) view = VK_NULL_HANDLE;
  }
  return view;
}
// A full-rate map of exactly this extent in texels (imageless framebuffers declare it).
VkImageView full_rate_view(Device& d, std::uint32_t width, std::uint32_t height) {
  auto& image = d.full_rate[{width, height}];
  if (!image) image = create_image(d, width, height, 1);
  return image ? image_view(d, *image, 0, 1) : VK_NULL_HANDLE;
}
std::uint32_t texels(std::uint32_t pixels, std::uint32_t texel) { return (pixels + texel - 1) / texel; }
std::uint32_t area(std::uint8_t value) { return (1u << (value >> 2)) * (1u << (value & 3)); }

// Fills a scene map (two eye layers and their union) from the latest eye
// views: the target covers each eye's whole view, as the game's stereo
// targets do. With the device's mutex held. True when its contents changed.
bool fill_scene_map(MapState& m, const ScenePattern& p) {
  if (!m.map || p.signature == m.signature) return false;
  m.signature = p.signature;
  const auto w = m.map->width, h = m.map->height;
  const auto layer = std::size_t(w) * h;
  std::fill(m.bytes.begin(), m.bytes.end(), std::uint8_t(0));
  if (p.valid) {
    for (std::uint32_t eye = 0; eye < 2; ++eye) {
      const RateField field(p.pattern, p.fov[eye], p.centre[eye]);
      auto* out = m.bytes.data() + eye * layer;
      for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x)
          out[std::size_t(y) * w + x] =
              shading_rate_value(field.at((float(x) + 0.5f) / float(w), (float(y) + 0.5f) / float(h)));
    }
    auto* combined = m.bytes.data() + 2 * layer;
    for (std::size_t i = 0; i < layer; ++i)
      combined[i] = area(m.bytes[i]) <= area(m.bytes[layer + i]) ? m.bytes[i] : m.bytes[layer + i];
  }
  return true;
}
void queue_upload(Device& d, MapState& m) {
  if (std::find(d.uploads.begin(), d.uploads.end(), &m) == d.uploads.end()) d.uploads.push_back(&m);
  d.pending = true;
}
// The scene map view for a stereo pass whose rate image is `width` x `height`
// texels: both eye layers where the GPU takes layered rate images and the
// view may have two layers, else their union.
VkImageView scene_map_view(Device& d, std::uint32_t width, std::uint32_t height, bool single_layer) {
  auto& m = d.scene_maps[{width, height}];
  if (!m) {
    m = std::make_unique<MapState>();
    m->map = create_image(d, width, height, 3);
    if (!m->map) {
      m.reset();
      return VK_NULL_HANDLE;
    }
    m->bytes.assign(std::size_t(width) * height * 3, 0);
    if (fill_scene_map(*m, current_scene())) queue_upload(d, *m);
  }
  if (single_layer || !d.layered) return image_view(d, *m->map, 2, 1);
  return image_view(d, *m->map, 0, 2);
}

// The eye map view for a pass drawing into `entry`'s image layers
// [base, base + count) with `views` multiview views (0: none), or null when
// the map does not cover the framebuffer.
VkImageView eye_map_view(Device& d, const EyeEntry& entry, std::uint32_t base, std::uint32_t count,
                         std::uint32_t multiview_views, std::uint32_t width, std::uint32_t height,
                         bool single_layer) {
  auto& s = *entry.swapchain;
  if (!s.map || texels(width, d.texel.width) > s.map->width || texels(height, d.texel.height) > s.map->height)
    return VK_NULL_HANDLE;
  const auto first = entry.first_layer + base;
  const auto combined = s.layers;  // the map's last layer: the union of the eyes' full-rate regions
  if (multiview_views > 1 || count > 1) {
    if (single_layer || !d.layered) return image_view(d, *s.map, combined, 1);
    return image_view(d, *s.map, first, std::max(count, multiview_views));
  }
  return image_view(d, *s.map, first, 1);
}

// Converts a vkCreateRenderPass description to its vkCreateRenderPass2 form,
// so the shading-rate attachment (a VkSubpassDescription2 extension) can be
// added. False for a description it cannot convert exactly.
struct Pass2 {
  VkRenderPassCreateInfo2 info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2};
  std::vector<VkAttachmentDescription2> attachments;
  std::vector<VkSubpassDescription2> subpasses;
  std::vector<std::vector<VkAttachmentReference2>> references;
  std::vector<VkSubpassDependency2> dependencies;
  std::vector<std::uint32_t> correlation;
  std::vector<VkFragmentShadingRateAttachmentInfoKHR> rates;
  VkAttachmentReference2 rate_reference{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2};
};
VkImageAspectFlags format_aspects(VkFormat format) {
  switch (format) {
    case VK_FORMAT_D16_UNORM: case VK_FORMAT_X8_D24_UNORM_PACK32: case VK_FORMAT_D32_SFLOAT:
      return VK_IMAGE_ASPECT_DEPTH_BIT;
    case VK_FORMAT_S8_UINT: return VK_IMAGE_ASPECT_STENCIL_BIT;
    case VK_FORMAT_D16_UNORM_S8_UINT: case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_D32_SFLOAT_S8_UINT:
      return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    default: return VK_IMAGE_ASPECT_COLOR_BIT;
  }
}
bool to_pass2(const VkRenderPassCreateInfo& in, Pass2& out) {
  const VkRenderPassMultiviewCreateInfo* multiview = nullptr;
  const VkRenderPassInputAttachmentAspectCreateInfo* aspects = nullptr;
  const void* density = nullptr;
  for (auto* p = static_cast<const VkBaseInStructure*>(in.pNext); p; p = p->pNext) {
    if (p->sType == VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO)
      multiview = reinterpret_cast<const VkRenderPassMultiviewCreateInfo*>(p);
    else if (p->sType == VK_STRUCTURE_TYPE_RENDER_PASS_INPUT_ATTACHMENT_ASPECT_CREATE_INFO)
      aspects = reinterpret_cast<const VkRenderPassInputAttachmentAspectCreateInfo*>(p);
    else if (p->sType == VK_STRUCTURE_TYPE_RENDER_PASS_FRAGMENT_DENSITY_MAP_CREATE_INFO_EXT)
      density = p;
    else
      return false;
  }
  if (density) return false;  // the game foveates this pass itself
  out.info.flags = in.flags;
  for (std::uint32_t i = 0; i < in.attachmentCount; ++i) {
    const auto& a = in.pAttachments[i];
    out.attachments.push_back({VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2, nullptr, a.flags, a.format, a.samples,
                               a.loadOp, a.storeOp, a.stencilLoadOp, a.stencilStoreOp, a.initialLayout,
                               a.finalLayout});
  }
  out.references.resize(in.subpassCount);
  for (std::uint32_t s = 0; s < in.subpassCount; ++s) {
    const auto& sub = in.pSubpasses[s];
    auto& refs = out.references[s];
    const auto total = sub.inputAttachmentCount + sub.colorAttachmentCount * 2 + 1;
    refs.reserve(total);
    auto add = [&](const VkAttachmentReference& r, VkImageAspectFlags aspect) {
      refs.push_back({VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, r.attachment, r.layout, aspect});
      return &refs.back();
    };
    VkSubpassDescription2 d{VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2};
    d.flags = sub.flags;
    d.pipelineBindPoint = sub.pipelineBindPoint;
    d.viewMask = multiview && s < multiview->subpassCount ? multiview->pViewMasks[s] : 0;
    d.inputAttachmentCount = sub.inputAttachmentCount;
    if (sub.inputAttachmentCount) d.pInputAttachments = refs.data() + refs.size();
    for (std::uint32_t i = 0; i < sub.inputAttachmentCount; ++i) {
      const auto& r = sub.pInputAttachments[i];
      VkImageAspectFlags aspect = r.attachment < in.attachmentCount
                                      ? format_aspects(in.pAttachments[r.attachment].format)
                                      : VK_IMAGE_ASPECT_COLOR_BIT;
      if (aspects)
        for (std::uint32_t a = 0; a < aspects->aspectReferenceCount; ++a)
          if (aspects->pAspectReferences[a].subpass == s && aspects->pAspectReferences[a].inputAttachmentIndex == i)
            aspect = aspects->pAspectReferences[a].aspectMask;
      add(r, aspect);
    }
    d.colorAttachmentCount = sub.colorAttachmentCount;
    if (sub.colorAttachmentCount) d.pColorAttachments = refs.data() + refs.size();
    for (std::uint32_t i = 0; i < sub.colorAttachmentCount; ++i) add(sub.pColorAttachments[i], 0);
    if (sub.pResolveAttachments) {
      d.pResolveAttachments = refs.data() + refs.size();
      for (std::uint32_t i = 0; i < sub.colorAttachmentCount; ++i) add(sub.pResolveAttachments[i], 0);
    }
    if (sub.pDepthStencilAttachment) d.pDepthStencilAttachment = add(*sub.pDepthStencilAttachment, 0);
    d.preserveAttachmentCount = sub.preserveAttachmentCount;
    d.pPreserveAttachments = sub.pPreserveAttachments;
    out.subpasses.push_back(d);
  }
  for (std::uint32_t i = 0; i < in.dependencyCount; ++i) {
    const auto& dep = in.pDependencies[i];
    out.dependencies.push_back({VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2, nullptr, dep.srcSubpass, dep.dstSubpass,
                                dep.srcStageMask, dep.dstStageMask, dep.srcAccessMask, dep.dstAccessMask,
                                dep.dependencyFlags,
                                multiview && i < multiview->dependencyCount ? multiview->pViewOffsets[i] : 0});
  }
  if (multiview)
    out.correlation.assign(multiview->pCorrelationMasks, multiview->pCorrelationMasks + multiview->correlationMaskCount);
  return true;
}
// Adds the shading-rate attachment to every subpass with colour output; the
// attachment's index, or nullopt when the pass is left as it is (no colour
// output, or the game gives it a density map or shading-rate image itself).
std::optional<std::uint32_t> add_rate_attachment(const Device& d, Pass2& p) {
  if (p.info.pNext) return std::nullopt;
  bool colour = false;
  for (auto& sub : p.subpasses) {
    colour |= sub.colorAttachmentCount > 0;
    for (auto* n = static_cast<const VkBaseInStructure*>(sub.pNext); n; n = n->pNext)
      if (n->sType == VK_STRUCTURE_TYPE_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR) return std::nullopt;
  }
  if (!colour) return std::nullopt;
  const auto index = std::uint32_t(p.attachments.size());
  p.attachments.push_back({VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2, nullptr, 0, kMapFormat,
                           VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
                           VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, kMapLayout,
                           kMapLayout});
  p.rate_reference = {VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, index, kMapLayout,
                      VK_IMAGE_ASPECT_COLOR_BIT};
  p.rates.reserve(p.subpasses.size());
  for (auto& sub : p.subpasses) {
    if (!sub.colorAttachmentCount) continue;
    p.rates.push_back({VK_STRUCTURE_TYPE_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR, sub.pNext, &p.rate_reference,
                       d.texel});
    sub.pNext = &p.rates.back();
  }
  return index;
}
// The game's 3D scene: a subpass with colour over two views (multiview) and a
// depth attachment. Its target may be the game's own (an HDR image it later
// copies into its eye images), so it is foveated even when it is not an eye image.
bool scene_pass(const Pass2& p) {
  for (const auto& sub : p.subpasses)
    if (sub.colorAttachmentCount && sub.viewMask == 3 && sub.pDepthStencilAttachment &&
        sub.pDepthStencilAttachment->attachment != VK_ATTACHMENT_UNUSED)
      return true;
  return false;
}
void finish_pass2(Pass2& p) {
  p.info.attachmentCount = std::uint32_t(p.attachments.size());
  p.info.pAttachments = p.attachments.data();
  p.info.subpassCount = std::uint32_t(p.subpasses.size());
  p.info.pSubpasses = p.subpasses.data();
  p.info.dependencyCount = std::uint32_t(p.dependencies.size());
  p.info.pDependencies = p.dependencies.data();
  p.info.correlatedViewMaskCount = std::uint32_t(p.correlation.size());
  p.info.pCorrelatedViewMasks = p.correlation.data();
}

// Destroyed passes and views stay in the maps (their destruction may be
// deferred by other layers); a new one that reuses a handle replaces or
// removes the stale entry.
VkResult forget_pass(Device& d, VkResult result, const VkRenderPass* out) {
  if (result == VK_SUCCESS) {
    std::lock_guard lock(d.mutex);
    d.passes.erase(*out);
  }
  return result;
}
VkResult VKAPI_CALL create_render_pass(VkDevice device, const VkRenderPassCreateInfo* info,
                                       const VkAllocationCallbacks* alloc, VkRenderPass* out) {
  const auto d = find(device);
  if (!d) return fallback<PFN_vkCreateRenderPass>("vkCreateRenderPass")(device, info, alloc, out);
  Pass2 p;
  std::optional<std::uint32_t> index;
  if (d->active && d->vkCreateRenderPass2 && to_pass2(*info, p)) index = add_rate_attachment(*d, p);
  if (!index) return forget_pass(*d, d->vkCreateRenderPass(device, info, alloc, out), out);
  const bool scene = scene_pass(p);
  finish_pass2(p);
  const auto result = d->vkCreateRenderPass2(device, &p.info, alloc, out);
  if (result == VK_SUCCESS) {
    std::lock_guard lock(d->mutex);
    d->passes[*out] = {*index, scene};
  }
  return result;
}
VkResult VKAPI_CALL create_render_pass2(VkDevice device, const VkRenderPassCreateInfo2* info,
                                        const VkAllocationCallbacks* alloc, VkRenderPass* out) {
  const auto d = find(device);
  if (!d) return fallback<PFN_vkCreateRenderPass2>("vkCreateRenderPass2")(device, info, alloc, out);
  if (!d->active) return d->vkCreateRenderPass2(device, info, alloc, out);
  Pass2 p;
  p.info = *info;
  p.attachments.assign(info->pAttachments, info->pAttachments + info->attachmentCount);
  p.subpasses.assign(info->pSubpasses, info->pSubpasses + info->subpassCount);
  p.dependencies.assign(info->pDependencies, info->pDependencies + info->dependencyCount);
  p.correlation.assign(info->pCorrelatedViewMasks, info->pCorrelatedViewMasks + info->correlatedViewMaskCount);
  const auto index = add_rate_attachment(*d, p);
  if (!index) return forget_pass(*d, d->vkCreateRenderPass2(device, info, alloc, out), out);
  const bool scene = scene_pass(p);
  finish_pass2(p);
  const auto result = d->vkCreateRenderPass2(device, &p.info, alloc, out);
  if (result == VK_SUCCESS) {
    std::lock_guard lock(d->mutex);
    d->passes[*out] = {*index, scene};
  }
  return result;
}
VkResult VKAPI_CALL create_image_view(VkDevice device, const VkImageViewCreateInfo* info,
                                      const VkAllocationCallbacks* alloc, VkImageView* out) {
  const auto d = find(device);
  if (!d) return fallback<PFN_vkCreateImageView>("vkCreateImageView")(device, info, alloc, out);
  const auto result = d->vkCreateImageView(device, info, alloc, out);
  if (result == VK_SUCCESS) {
    std::lock_guard lock(d->mutex);
    if (!d->eyes.contains(info->image))
      d->views.erase(*out);
    else
      d->views[*out] = {info->image, info->subresourceRange.baseArrayLayer,
                        info->subresourceRange.layerCount == VK_REMAINING_ARRAY_LAYERS
                            ? 0
                            : info->subresourceRange.layerCount};
  }
  return result;
}
// The eye image among `views`, if any: the entry and the view's layers.
const EyeEntry* eye_among(Device& d, std::span<const VkImageView> views, std::uint32_t& base,
                          std::uint32_t& count) {
  for (const auto view : views) {
    const auto v = d.views.find(view);
    if (v == d.views.end()) continue;
    const auto e = d.eyes.find(v->second.image);
    if (e == d.eyes.end()) continue;
    base = v->second.base_layer;
    const auto layers = e->second.swapchain->layers;
    count = v->second.layer_count ? v->second.layer_count : (layers > base ? layers - base : 1);
    return &e->second;
  }
  return nullptr;
}
std::uint32_t view_count(std::uint32_t mask) { return std::uint32_t(std::popcount(mask)); }


VkResult VKAPI_CALL create_framebuffer(VkDevice device, const VkFramebufferCreateInfo* info,
                                       const VkAllocationCallbacks* alloc, VkFramebuffer* out) {
  const auto d = find(device);
  if (!d) return fallback<PFN_vkCreateFramebuffer>("vkCreateFramebuffer")(device, info, alloc, out);
  std::unique_lock lock(d->mutex);
  const auto pass = d->passes.find(info->renderPass);
  if (pass == d->passes.end() || info->attachmentCount != pass->second.index) {
    lock.unlock();
    return d->vkCreateFramebuffer(device, info, alloc, out);
  }
  const auto w = texels(info->width, d->texel.width), h = texels(info->height, d->texel.height);
  auto copy = *info;
  std::vector<VkImageView> views;
  std::vector<VkFramebufferAttachmentImageInfo> images;
  VkFramebufferAttachmentsCreateInfo attachments{};
  static constexpr VkFormat format = kMapFormat;
  if (info->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT) {
    // The rate image is chosen when the pass begins (add_begin_view); declare
    // one of exactly the size the framebuffer needs, as every foveation image
    // given to it is.
    const VkFramebufferAttachmentsCreateInfo* declared = nullptr;
    bool other = false;
    for (auto* p = static_cast<const VkBaseInStructure*>(info->pNext); p; p = p->pNext) {
      if (p->sType == VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENTS_CREATE_INFO)
        declared = reinterpret_cast<const VkFramebufferAttachmentsCreateInfo*>(p);
      else
        other = true;
    }
    if (!declared || other || declared->attachmentImageInfoCount != info->attachmentCount) {
      // Another extension in the chain, not seen in games: this framebuffer
      // has no rate image, so passes with it would not begin correctly.
      lock.unlock();
      log::warn("gfx", "Foveation: an imageless framebuffer with other extensions has no shading-rate image");
      return d->vkCreateFramebuffer(device, info, alloc, out);
    }
    images.assign(declared->pAttachmentImageInfos, declared->pAttachmentImageInfos + info->attachmentCount);
    images.push_back({VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENT_IMAGE_INFO, nullptr, 0, kMapUsage, w, h, 1, 1,
                      &format});
    attachments = *declared;
    attachments.pNext = nullptr;
    attachments.attachmentImageInfoCount = std::uint32_t(images.size());
    attachments.pAttachmentImageInfos = images.data();
    copy.pNext = &attachments;
    copy.attachmentCount = info->attachmentCount + 1;
    lock.unlock();
    const auto result = d->vkCreateFramebuffer(device, &copy, alloc, out);
    if (result == VK_SUCCESS) {
      lock.lock();
      d->imageless[*out] = {w, h};
    }
    return result;
  } else {
    views.assign(info->pAttachments, info->pAttachments + info->attachmentCount);
    std::uint32_t base = 0, count = 1;
    VkImageView rate{};
    if (const auto* eye = eye_among(*d, views, base, count))
      rate = eye_map_view(*d, *eye, base, count, 0, info->width, info->height, false);
    else if (pass->second.scene)
      rate = scene_map_view(*d, w, h, false);
    if (!rate) rate = full_rate_view(*d, w, h);
    if (!rate) {
      lock.unlock();
      log::warn("gfx", "Foveation: no shading-rate image for a {}x{} framebuffer", info->width, info->height);
      return d->vkCreateFramebuffer(device, info, alloc, out);
    }
    views.push_back(rate);
    copy.attachmentCount = std::uint32_t(views.size());
    copy.pAttachments = views.data();
  }
  lock.unlock();
  const auto result = d->vkCreateFramebuffer(device, &copy, alloc, out);
  if (result == VK_SUCCESS) {
    lock.lock();
    d->imageless.erase(*out);
  }
  return result;
}

// A staging buffer of at least `size` bytes. Slots rotate and one is reused
// only after kStagingSlots more uploads, far more frames than any game keeps
// in flight (an upload happens at most once a frame).
Staging* staging_slot(Device& d, VkDeviceSize size) {
  auto& slot = d.staging[d.next_staging++ % Device::kStagingSlots];
  if (slot.size >= size) return &slot;
  if (slot.buffer) d.retired_staging.push_back(slot);  // may still be read; freed with the device
  slot = {};
  VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  ci.size = std::max<VkDeviceSize>(size, 64 * 1024);
  ci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  if (d.vkCreateBuffer(d.device, &ci, nullptr, &slot.buffer) != VK_SUCCESS) {
    slot = {};
    return nullptr;
  }
  VkMemoryRequirements requirements{};
  d.vkGetBufferMemoryRequirements(d.device, slot.buffer, &requirements);
  const auto type = memory_type(d, requirements.memoryTypeBits,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, requirements.size, type};
  if (type == UINT32_MAX || d.vkAllocateMemory(d.device, &ai, nullptr, &slot.memory) != VK_SUCCESS ||
      d.vkBindBufferMemory(d.device, slot.buffer, slot.memory, 0) != VK_SUCCESS ||
      d.vkMapMemory(d.device, slot.memory, 0, VK_WHOLE_SIZE, 0, &slot.mapped) != VK_SUCCESS) {
    if (slot.memory) d.vkFreeMemory(d.device, slot.memory, nullptr);
    d.vkDestroyBuffer(d.device, slot.buffer, nullptr);
    slot = {};
    return nullptr;
  }
  slot.size = ci.size;
  return &slot;
}
// Records the pending clears and map uploads into `command` before the pass
// it begins: queue order puts them after every earlier frame's passes and
// before this one's.
void record_pending(Device& d, VkCommandBuffer command) {
  if (!d.pending.load(std::memory_order_acquire)) return;
  std::lock_guard lock(d.mutex);
  if (!d.pending) return;
  d.pending = false;
  struct Copy {
    Image* image;
    Staging* staging;
  };
  std::vector<Copy> copies;
  for (auto* s : d.uploads) {
    if (!s->map) continue;
    auto* slot = staging_slot(d, s->bytes.size());
    if (!slot) continue;
    std::memcpy(slot->mapped, s->bytes.data(), s->bytes.size());
    s->uploaded = s->bytes;
    copies.push_back({s->map.get(), slot});
  }
  d.uploads.clear();
  std::vector<Image*> clears;
  for (auto* image : d.clears)
    if (std::none_of(copies.begin(), copies.end(), [&](const Copy& c) { return c.image == image; }))
      clears.push_back(image);
  d.clears.clear();
  auto barrier = [](const Image& image, VkImageLayout from, VkAccessFlags src, VkAccessFlags dst) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.oldLayout = from;
    b.newLayout = kMapLayout;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, image.layers};
    return b;
  };
  std::vector<VkImageMemoryBarrier> before, after;
  for (auto* image : clears)
    before.push_back(barrier(*image, image->cleared ? kMapLayout : VK_IMAGE_LAYOUT_UNDEFINED,
                             VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT));
  for (auto& c : copies)
    before.push_back(barrier(*c.image, c.image->cleared ? kMapLayout : VK_IMAGE_LAYOUT_UNDEFINED,
                             VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT));
  if (before.empty()) return;
  d.vkCmdPipelineBarrier(command,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, std::uint32_t(before.size()),
                         before.data());
  const VkClearColorValue zero{};
  for (auto* image : clears) {
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, image->layers};
    d.vkCmdClearColorImage(command, image->image, kMapLayout, &zero, 1, &range);
    image->cleared = true;
    after.push_back(barrier(*image, kMapLayout, VK_ACCESS_TRANSFER_WRITE_BIT,
                            VK_ACCESS_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT_KHR));
  }
  for (auto& c : copies) {
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, c.image->layers};
    region.imageExtent = {c.image->width, c.image->height, 1};
    d.vkCmdCopyBufferToImage(command, c.staging->buffer, c.image->image, kMapLayout, 1, &region);
    c.image->cleared = true;
    after.push_back(barrier(*c.image, kMapLayout, VK_ACCESS_TRANSFER_WRITE_BIT,
                            VK_ACCESS_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT_KHR));
  }
  d.vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR, 0, 0, nullptr, 0, nullptr,
                         std::uint32_t(after.size()), after.data());
}

// Imageless framebuffers: the rate view joins the views the pass begins with.
bool add_begin_view(Device& d, const VkRenderPassBeginInfo* info, VkRenderPassBeginInfo& copy,
                    VkRenderPassAttachmentBeginInfo& attachments, std::vector<VkImageView>& views) {
  std::lock_guard lock(d.mutex);
  const auto pass = d.passes.find(info->renderPass);
  if (pass == d.passes.end()) return false;
  const VkRenderPassAttachmentBeginInfo* begin = nullptr;
  for (auto* p = static_cast<const VkBaseInStructure*>(info->pNext); p; p = p->pNext) {
    if (p->sType != VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO) return false;
    begin = reinterpret_cast<const VkRenderPassAttachmentBeginInfo*>(p);
  }
  if (!begin || begin->attachmentCount != pass->second.index) return false;
  const auto declared = d.imageless.find(info->framebuffer);
  if (declared == d.imageless.end()) return false;
  views.assign(begin->pAttachments, begin->pAttachments + begin->attachmentCount);
  // The framebuffer's size is not known here: the render area from the
  // origin bounds it. An eye map is used only when that is the eye image's
  // full size, which the framebuffer then has too (its declared rate image is
  // ceil(framebuffer / texel), the map's size).
  std::uint32_t base = 0, count = 1;
  VkImageView rate{};
  const auto* eye = eye_among(d, views, base, count);
  const auto width = std::uint32_t(info->renderArea.offset.x) + info->renderArea.extent.width;
  const auto height = std::uint32_t(info->renderArea.offset.y) + info->renderArea.extent.height;
  if (eye && width == eye->swapchain->width && height == eye->swapchain->height &&
      texels(width, d.texel.width) == declared->second.width &&
      texels(height, d.texel.height) == declared->second.height)
    rate = eye_map_view(d, *eye, base, count, count, width, height, true);
  else if (!eye && pass->second.scene)
    rate = scene_map_view(d, declared->second.width, declared->second.height, true);
  // Imageless views must match the declared size exactly.
  if (!rate) rate = full_rate_view(d, declared->second.width, declared->second.height);
  if (!rate) return false;
  views.push_back(rate);
  attachments = *begin;
  attachments.pNext = nullptr;
  attachments.attachmentCount = std::uint32_t(views.size());
  attachments.pAttachments = views.data();
  copy = *info;
  copy.pNext = &attachments;
  return true;
}
void VKAPI_CALL begin_render_pass(VkCommandBuffer command, const VkRenderPassBeginInfo* info,
                                  VkSubpassContents contents) {
  const auto d = find(command);
  if (!d) return fallback<PFN_vkCmdBeginRenderPass>("vkCmdBeginRenderPass")(command, info, contents);
  VkRenderPassBeginInfo copy;
  VkRenderPassAttachmentBeginInfo attachments;
  std::vector<VkImageView> views;
  // The view first: a new full-rate image is cleared by record_pending.
  const bool added = add_begin_view(*d, info, copy, attachments, views);
  record_pending(*d, command);
  d->vkCmdBeginRenderPass(command, added ? &copy : info, contents);
}
void VKAPI_CALL begin_render_pass2(VkCommandBuffer command, const VkRenderPassBeginInfo* info,
                                   const VkSubpassBeginInfo* sub) {
  const auto d = find(command);
  if (!d) return fallback<PFN_vkCmdBeginRenderPass2>("vkCmdBeginRenderPass2")(command, info, sub);
  VkRenderPassBeginInfo copy;
  VkRenderPassAttachmentBeginInfo attachments;
  std::vector<VkImageView> views;
  // The view first: a new full-rate image is cleared by record_pending.
  const bool added = add_begin_view(*d, info, copy, attachments, views);
  record_pending(*d, command);
  d->vkCmdBeginRenderPass2(command, added ? &copy : info, sub);
}
// Dynamic rendering into an eye image gets its map; other rendering none.
void VKAPI_CALL begin_rendering(VkCommandBuffer command, const VkRenderingInfo* info) {
  const auto d = find(command);
  if (!d) return fallback<PFN_vkCmdBeginRendering>("vkCmdBeginRendering")(command, info);
  VkRenderingFragmentShadingRateAttachmentInfoKHR rate{
      VK_STRUCTURE_TYPE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR};
  VkRenderingInfo copy;
  {
    std::lock_guard lock(d->mutex);
    std::vector<VkImageView> views;
    for (std::uint32_t i = 0; i < info->colorAttachmentCount; ++i) {
      views.push_back(info->pColorAttachments[i].imageView);
      views.push_back(info->pColorAttachments[i].resolveImageView);
    }
    std::uint32_t base = 0, count = 1;
    bool own = false;  // the game foveates this pass itself
    for (auto* p = static_cast<const VkBaseInStructure*>(info->pNext); p; p = p->pNext)
      own |= p->sType == VK_STRUCTURE_TYPE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR ||
             p->sType == VK_STRUCTURE_TYPE_RENDERING_FRAGMENT_DENSITY_MAP_ATTACHMENT_INFO_EXT;
    const auto* eye = d->active && !own ? eye_among(*d, views, base, count) : nullptr;
    const auto width = std::uint32_t(info->renderArea.offset.x) + info->renderArea.extent.width;
    const auto height = std::uint32_t(info->renderArea.offset.y) + info->renderArea.extent.height;
    if (eye)
      rate.imageView = eye_map_view(*d, *eye, base, count, view_count(info->viewMask), width, height, false);
    else if (d->active && !own && info->colorAttachmentCount && info->viewMask == 3 && info->pDepthAttachment &&
             info->pDepthAttachment->imageView)
      rate.imageView = scene_map_view(*d, texels(width, d->texel.width), texels(height, d->texel.height), false);
    if (rate.imageView) {
      rate.imageLayout = kMapLayout;
      rate.shadingRateAttachmentTexelSize = d->texel;
      rate.pNext = info->pNext;
      copy = *info;
      copy.pNext = &rate;
    }
  }
  // Pending uploads (a map created just now among them) go first. Nothing may
  // come between a suspended render pass instance and its resumption.
  if (!(info->flags & VK_RENDERING_RESUMING_BIT)) record_pending(*d, command);
  d->vkCmdBeginRendering(command, rate.imageView ? &copy : info);
}

template <class F>
PFN_vkVoidFunction fn(F function) {
  return reinterpret_cast<PFN_vkVoidFunction>(function);
}
}  // namespace

void set_host_eye_tracking(bool available) { host_eye_tracking = available ? 1 : 0; }
bool eye_tracked() {
  if (configured_mode() != Mode::dynamic) return false;
  const int known = host_eye_tracking.load();
  return known >= 0 ? known != 0 : xr::eye_gaze::available();
}
bool wanted() { return configured_mode() == Mode::fixed || eye_tracked(); }

void add_to_device(PFN_vkGetInstanceProcAddr gipa, VkInstance instance, VkPhysicalDevice physical,
                   const VkDeviceCreateInfo& in, DeviceAdditions& out) {
  out.info = in;
  out.added = false;
  if (!wanted() || !gipa) return;
  const auto enumerate = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
      gipa(instance, "vkEnumerateDeviceExtensionProperties"));
  const auto features2 =
      reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(gipa(instance, "vkGetPhysicalDeviceFeatures2"));
  const auto properties =
      reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(instance, "vkGetPhysicalDeviceProperties"));
  if (!enumerate || !features2 || !properties) return;
  std::uint32_t count = 0;
  enumerate(physical, nullptr, &count, nullptr);
  std::vector<VkExtensionProperties> extensions(count);
  enumerate(physical, nullptr, &count, extensions.data());
  extensions.resize(count);
  const auto has = [&](std::string_view name) {
    return std::any_of(extensions.begin(), extensions.end(),
                       [&](const VkExtensionProperties& e) { return name == e.extensionName; });
  };
  VkPhysicalDeviceProperties device{};
  properties(physical, &device);
  VkPhysicalDeviceFragmentShadingRateFeaturesKHR supported{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR};
  if (has(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME) && device.apiVersion >= VK_API_VERSION_1_2) {
    VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &supported};
    features2(physical, &query);
  }
  if (!supported.attachmentFragmentShadingRate) {
    log::info("gfx", "Foveation: the GPU has no shading-rate attachments; rendering stays at full resolution");
    return;
  }
  for (auto* p = static_cast<const VkBaseInStructure*>(in.pNext); p; p = p->pNext)
    if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR) {
      // The device asks for shading rates itself; foveate only if it enables attachments.
      out.added = reinterpret_cast<const VkPhysicalDeviceFragmentShadingRateFeaturesKHR*>(p)
                      ->attachmentFragmentShadingRate;
      return;
    }
  out.extensions.assign(in.ppEnabledExtensionNames, in.ppEnabledExtensionNames + in.enabledExtensionCount);
  if (std::none_of(out.extensions.begin(), out.extensions.end(), [](const char* e) {
        return std::string_view(e) == VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME;
      }))
    out.extensions.push_back(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME);
  out.features = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR, const_cast<void*>(in.pNext)};
  out.features.attachmentFragmentShadingRate = VK_TRUE;
  out.info.pNext = &out.features;
  out.info.enabledExtensionCount = std::uint32_t(out.extensions.size());
  out.info.ppEnabledExtensionNames = out.extensions.data();
  out.added = true;
}

void register_device(VkDevice device, VkPhysicalDevice physical, PFN_vkGetInstanceProcAddr gipa,
                     VkInstance instance, PFN_vkGetDeviceProcAddr raw, bool active) {
  if (!wanted() || !device || !raw || !gipa) return;
  auto d = std::make_shared<Device>();
  d->device = device;
  d->physical = physical;
  d->active = active;
#define VFN(name) d->name = reinterpret_cast<PFN_##name>(raw(device, #name));
  VFN(vkCreateRenderPass) VFN(vkCreateRenderPass2) VFN(vkDestroyRenderPass) VFN(vkCreateFramebuffer)
  VFN(vkCreateImageView) VFN(vkDestroyImageView) VFN(vkCmdBeginRenderPass) VFN(vkCmdBeginRenderPass2)
  VFN(vkCmdBeginRendering) VFN(vkCreateImage) VFN(vkDestroyImage) VFN(vkAllocateMemory) VFN(vkFreeMemory)
  VFN(vkBindImageMemory) VFN(vkGetImageMemoryRequirements) VFN(vkCreateBuffer) VFN(vkDestroyBuffer)
  VFN(vkBindBufferMemory) VFN(vkGetBufferMemoryRequirements) VFN(vkMapMemory) VFN(vkCmdPipelineBarrier)
  VFN(vkCmdCopyBufferToImage) VFN(vkCmdClearColorImage)
#undef VFN
  if (!d->vkCmdBeginRendering)
    d->vkCmdBeginRendering = reinterpret_cast<PFN_vkCmdBeginRendering>(raw(device, "vkCmdBeginRenderingKHR"));
  if (!d->vkCreateRenderPass2)
    d->vkCreateRenderPass2 = reinterpret_cast<PFN_vkCreateRenderPass2>(raw(device, "vkCreateRenderPass2KHR"));
  if (!d->vkCmdBeginRenderPass2)
    d->vkCmdBeginRenderPass2 = reinterpret_cast<PFN_vkCmdBeginRenderPass2>(raw(device, "vkCmdBeginRenderPass2KHR"));
  reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(gipa(instance, "vkGetPhysicalDeviceMemoryProperties"))(
      physical, &d->memory);
  if (active) {
    VkPhysicalDeviceFragmentShadingRatePropertiesKHR rates{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &rates};
    reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(gipa(instance, "vkGetPhysicalDeviceProperties2"))(
        physical, &properties);
    // The largest square texel the GPU takes, at most 16: the smallest map.
    auto size = std::min({rates.maxFragmentShadingRateAttachmentTexelSize.width,
                          rates.maxFragmentShadingRateAttachmentTexelSize.height, 16u});
    size = std::max({size, rates.minFragmentShadingRateAttachmentTexelSize.width,
                     rates.minFragmentShadingRateAttachmentTexelSize.height, 1u});
    d->texel = {size, size};
    d->layered = rates.layeredShadingRateAttachments;
    log::info("gfx", "Foveation: {} with shading-rate attachments ({}x{} pixels per texel{})",
              eye_tracked() ? "eye-tracked" : "fixed", size, size, d->layered ? ", per-eye layers" : "");
  }
  std::lock_guard lock(registry_mutex);
  fallback_gipa = gipa;
  fallback_instance = instance;
  devices[dispatch_key(device)] = std::move(d);
}

void release_device(VkDevice device) {
  std::shared_ptr<Device> d;
  std::vector<std::unique_ptr<Swapchain>> owned;
  {
    std::lock_guard lock(registry_mutex);
    const auto it = devices.find(dispatch_key(device));
    if (it == devices.end() || it->second->device != device) return;
    d = std::move(it->second);
    devices.erase(it);
    for (auto s = swapchains.begin(); s != swapchains.end();)
      if (s->second->device == d.get()) {
        owned.push_back(std::move(s->second));
        s = swapchains.erase(s);
      } else {
        ++s;
      }
  }
  std::lock_guard lock(d->mutex);
  for (auto& s : owned)
    if (s->map) destroy_image(*d, *s->map);
  for (auto& [extent, image] : d->full_rate) destroy_image(*d, *image);
  for (auto& [extent, m] : d->scene_maps)
    if (m->map) destroy_image(*d, *m->map);
  for (auto& image : d->retired) destroy_image(*d, *image);
  auto free_staging = [&](Staging& s) {
    if (s.buffer) d->vkDestroyBuffer(device, s.buffer, nullptr);
    if (s.memory) d->vkFreeMemory(device, s.memory, nullptr);
  };
  for (auto& s : d->staging) free_staging(s);
  for (auto& s : d->retired_staging) free_staging(s);
}

bool device_active(VkDevice device) {
  const auto d = find(device);
  return d && d->active;
}

PFN_vkVoidFunction proc(std::string_view name) {
  if (!wanted()) return nullptr;
  if (name == "vkCreateRenderPass") return fn(create_render_pass);
  if (name == "vkCreateRenderPass2" || name == "vkCreateRenderPass2KHR") return fn(create_render_pass2);
  if (name == "vkCreateFramebuffer") return fn(create_framebuffer);
  if (name == "vkCreateImageView") return fn(create_image_view);
  if (name == "vkCmdBeginRenderPass") return fn(begin_render_pass);
  if (name == "vkCmdBeginRenderPass2" || name == "vkCmdBeginRenderPass2KHR") return fn(begin_render_pass2);
  if (name == "vkCmdBeginRendering" || name == "vkCmdBeginRenderingKHR") return fn(begin_rendering);
  return nullptr;
}

void patch_pipelines(VkDevice device, std::span<VkGraphicsPipelineCreateInfo> infos, PipelineStorage& storage) {
  if (!device_active(device)) return;
  storage.states.clear();
  storage.states.reserve(infos.size());
  for (auto& info : infos) {
    bool skip = false;
    for (auto* p = static_cast<const VkBaseInStructure*>(info.pNext); p && !skip; p = p->pNext)
      skip = p->sType == VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR ||
             p->sType == VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR;
    if (info.pDynamicState)
      for (std::uint32_t i = 0; i < info.pDynamicState->dynamicStateCount && !skip; ++i)
        skip = info.pDynamicState->pDynamicStates[i] == VK_DYNAMIC_STATE_FRAGMENT_SHADING_RATE_KHR;
    if (skip) continue;
    // Full rate from the pipeline; the attachment's rate replaces it. Where
    // the GPU cannot shade a pipeline coarsely (shader depth or sample-mask
    // writes it reports it cannot combine, sample shading) it keeps 1x1.
    storage.states.push_back({VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR,
                              info.pNext,
                              {1, 1},
                              {VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR,
                               VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR}});
    info.pNext = &storage.states.back();
    if (info.renderPass == VK_NULL_HANDLE)
      info.flags |= VK_PIPELINE_CREATE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR;
  }
}

void register_eye_swapchain(VkDevice device, const void* swapchain, std::span<const EyeImage> images,
                            std::uint32_t width, std::uint32_t height, std::uint32_t layers, bool bottom_up) {
  const auto d = find(device);
  if (!d || !d->active || images.empty() || !width || !height) return;
  auto s = std::make_unique<Swapchain>();
  s->device = d.get();
  s->width = width;
  s->height = height;
  s->layers = std::max(1u, layers);
  s->bottom_up = bottom_up;
  s->images.assign(images.begin(), images.end());
  {
    std::lock_guard lock(d->mutex);
    // One layer per eye layer plus the union of them (multiview passes on a
    // GPU without per-layer rate images).
    s->map = create_image(*d, texels(width, d->texel.width), texels(height, d->texel.height), s->layers + 1);
    if (!s->map) {
      log::warn("gfx", "Foveation: no map for a {}x{} eye swapchain", width, height);
      return;
    }
    s->bytes.assign(std::size_t(s->map->width) * s->map->height * s->map->layers, 0);
    for (const auto& image : images) d->eyes[image.image] = {s.get(), image.first_layer};
  }
  std::lock_guard lock(registry_mutex);
  swapchains[swapchain] = std::move(s);
}

void forget_eye_swapchain(VkDevice device, const void* swapchain) {
  std::unique_ptr<Swapchain> s;
  {
    std::lock_guard lock(registry_mutex);
    const auto it = swapchains.find(swapchain);
    if (it == swapchains.end()) return;
    s = std::move(it->second);
    swapchains.erase(it);
  }
  const auto d = find(device);
  if (!d || d.get() != s->device) return;
  std::lock_guard lock(d->mutex);
  for (const auto& image : s->images) d->eyes.erase(image.image);
  for (auto v = d->views.begin(); v != d->views.end();)
    v = d->eyes.contains(v->second.image) ? std::next(v) : d->views.erase(v);
  std::erase(d->uploads, s.get());
  if (s->map) {
    std::erase(d->clears, s->map.get());
    d->retired.push_back(std::move(s->map));  // command buffers in flight may still read it
  }
}

namespace {
// Eye-tracked regions only while the gaze is known; without it (tracking
// lost, a blink held too long) the wider fixed regions, as Quest does.
Pattern frame_pattern(Level requested, float vertical_offset_degrees, bool gaze) {
  return {effective_level(requested), eye_tracked() && gaze, vertical_offset_degrees};
}
// What a map is filled from. Centres go in quarter-degree steps (gaze
// trackers are accurate to about a degree): a map refills when the gaze moves
// that far, not on every jitter.
std::string signature_of(const Pattern& pattern, std::span<const EyeView> views) {
  const auto q = [](float value, float scale) { return std::to_string(std::lround(value * scale)); };
  std::string signature = std::to_string(int(pattern.level)) + (pattern.eye_tracked ? "e" : "f") +
                          q(pattern.vertical_offset_degrees, 10);
  for (const auto& v : views)
    signature += "|" + std::to_string(v.layer) + "," + std::to_string(v.x) + "," + std::to_string(v.y) + "," +
                 std::to_string(v.width) + "," + std::to_string(v.height) + "," + q(v.fov.left, 1000) + "," +
                 q(v.fov.right, 1000) + "," + q(v.fov.up, 1000) + "," + q(v.fov.down, 1000) + "," +
                 q(v.centre.x / std::max(1e-3f, -v.centre.z), 229) + "," +
                 q(v.centre.y / std::max(1e-3f, -v.centre.z), 229);
  return signature;
}
}  // namespace

void update_scene_views(const EyeView& left, const EyeView& right, Level requested, float vertical_offset_degrees,
                        bool gaze) {
  const auto pattern = frame_pattern(requested, vertical_offset_degrees, gaze);
  const EyeView pair[2] = {left, right};
  auto signature = signature_of(pattern, pair);
  // Only the angles matter to a scene map, not where the views sit in the eye images.
  signature += "s";
  {
    std::lock_guard lock(scene_mutex);
    if (scene_pattern.signature == signature) return;
    scene_pattern.valid = true;
    scene_pattern.pattern = pattern;
    for (int eye = 0; eye < 2; ++eye) {
      scene_pattern.fov[eye] = pair[eye].fov;
      scene_pattern.centre[eye] = pair[eye].centre;
    }
    scene_pattern.signature = signature;
  }
  const auto scene = current_scene();
  std::lock_guard registry(registry_mutex);
  for (auto& [key, device] : devices) {
    if (!device->active) continue;
    std::lock_guard lock(device->mutex);
    for (auto& [extent, m] : device->scene_maps)
      if (fill_scene_map(*m, scene)) queue_upload(*device, *m);
  }
}

void update_eye_views(const void* swapchain, std::span<const EyeView> views, Level requested,
                      float vertical_offset_degrees, bool gaze) {
  if (views.empty()) return;
  const auto pattern = frame_pattern(requested, vertical_offset_degrees, gaze);
  auto signature = signature_of(pattern, views);
  std::lock_guard registry(registry_mutex);
  const auto it = swapchains.find(swapchain);
  if (it == swapchains.end()) return;
  auto& s = *it->second;
  auto& d = *s.device;
  std::lock_guard lock(d.mutex);
  if (!s.map) return;
  // Uploaded again now and then in case a command buffer carrying an upload
  // was never submitted.
  const bool refresh = ++s.frames % 600 == 0;
  if (signature != s.signature) {
    s.signature = std::move(signature);
    const auto w = s.map->width, h = s.map->height;
    const auto layer_size = std::size_t(w) * h;
    std::fill(s.bytes.begin(), s.bytes.end(), std::uint8_t(0));
    std::vector<bool> drawn(layer_size * s.layers, false);
    for (const auto& v : views) {
      if (v.layer >= s.layers || !v.width || !v.height) continue;
      const RateField field(pattern, v.fov, v.centre);
      const auto left = std::uint32_t(std::max(0, v.x)), top = std::uint32_t(std::max(0, v.y));
      const auto x0 = left / d.texel.width, y0 = top / d.texel.height;
      const auto x1 = std::min(w, texels(left + v.width, d.texel.width));
      const auto y1 = std::min(h, texels(top + v.height, d.texel.height));
      for (auto ty = y0; ty < y1; ++ty)
        for (auto tx = x0; tx < x1; ++tx) {
          const float px = (float(tx) + 0.5f) * float(d.texel.width);
          const float py = (float(ty) + 0.5f) * float(d.texel.height);
          const float u = std::clamp((px - float(v.x)) / float(v.width), 0.0f, 1.0f);
          float t = std::clamp((py - float(v.y)) / float(v.height), 0.0f, 1.0f);
          if (s.bottom_up) t = 1 - t;  // GLES rows run up the view
          const auto value = shading_rate_value(field.at(u, t));
          const auto i = v.layer * layer_size + std::size_t(ty) * w + tx;
          // Overlapping views keep the finer rate.
          if (!drawn[i] || area(value) < area(s.bytes[i])) s.bytes[i] = value;
          drawn[i] = true;
        }
    }
    // The combined layer: per texel the finest rate of any eye layer.
    auto* combined = s.bytes.data() + s.layers * layer_size;
    for (std::size_t i = 0; i < layer_size; ++i) {
      auto finest = s.bytes[i];
      for (std::uint32_t l = 1; l < s.layers; ++l)
        if (area(s.bytes[l * layer_size + i]) < area(finest)) finest = s.bytes[l * layer_size + i];
      combined[i] = finest;
    }
    static std::once_flag reported;
    std::call_once(reported, [&] {
      log::info("gfx", "Foveation: {} level {}, {:.0f}% of an eye's fragments shaded",
                pattern.eye_tracked ? "eye-tracked" : "fixed", int(pattern.level),
                100 * shaded_fraction({s.bytes.data(), layer_size}));
    });
  } else if (!refresh && s.bytes == s.uploaded) {
    return;
  }
  queue_upload(d, s);
}
}  // namespace venture::gfx::foveation

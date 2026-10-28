// Render-target bounds. Quest drivers render tile by tile and tolerate
// rendering that strays past a render pass instance's bounds; desktop GPUs
// write such fragments and fault past an attachment's edge (Moss: NVIDIA Xid 13
// "CT Violation", then VK_ERROR_DEVICE_LOST at a later vkQueueSubmit).
//
// - "The application must ensure (using scissor if necessary) that all
//   rendering is contained within the render area" (Vulkan spec, Render Pass
//   Commands). While a render pass instance is open, dynamic scissors reach the
//   host clamped to its render area, which discards the same fragments a tiler
//   would.
// - A framebuffer must not be larger than any of its attachments
//   (VUID-VkFramebufferCreateInfo-flags-04533/04534), and a render area must lie
//   inside the framebuffer and its attachments. UE4's grow-only scene targets
//   break this: after a 1536x1536 scene capture, Moss renders its eyes through
//   a 1536x1536 multisampled scene color whose resolve attachment is the
//   768x768 eye image. Host framebuffers and render areas are clamped to the
//   smallest attachment, which keeps exactly the pixels every attachment has.
// - SwiftShader workaround: attachments may be larger than the framebuffer and
//   a subpass resolve covers only the render area, but SwiftShader resolves the
//   source's whole extent and writes past a smaller resolve attachment (the
//   clamped Moss eye pass faulted the host). On SwiftShader only (driverID), a
//   multisampled resolve source larger than its resolve attachment that no
//   subpass reads as an input attachment is replaced in the host framebuffer by
//   a transient image of the framebuffer's size. The begun render pass decides
//   per instance: if it loads or stores the source, a plain host framebuffer
//   with the guest's views is used instead. Otherwise the stand-in is moved to
//   the pass's initial layout before the instance, and the guest's image to its
//   final layout after it. Separate depth/stencil layouts and attachment
//   feedback loops are not handled on this path.
// - Render passes reach the host with the subpass dependencies their
//   attachment accesses need (subpass_sync.cpp).
#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <optional>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "chain.h"
#include "common/log.h"
#include "internal.h"

namespace venture::vulkan {
namespace {
struct Scissors {
  std::optional<VkRect2D> area;  // the open render pass instance's render area
  std::vector<VkRect2D> rects;   // the guest's dynamic scissors, unclamped
  bool with_count{};             // last set by vkCmdSetScissorWithCount
  std::vector<VkRect2D> host;    // the scissors last sent to the host
  // Final-layout transitions of guest images replaced in the open instance.
  std::vector<VkImageMemoryBarrier> after;
};
std::mutex mutex;
std::unordered_map<VkCommandBuffer, Scissors> states;
// Graphics pipelines whose scissors are static: binding one invalidates the
// dynamic scissors ("made invalid by another pipeline bind with that state
// specified as static", Pipeline Dynamic State).
std::unordered_set<VkPipeline> static_scissor;
// Images that can be attachments (extent of mip 0), their views (extent of the
// base mip), and host framebuffers created smaller than the guest asked.
struct Image {
  VkExtent2D extent;
  std::uint32_t layers;
  VkSampleCountFlagBits samples;
  VkImageUsageFlags usage;
};
struct View {
  VkExtent2D extent;
  VkImage image;
  VkFormat format;
  VkImageViewType type;
  VkImageSubresourceRange range;
};
std::unordered_map<VkImage, Image> images;
std::unordered_map<VkImageView, View> views;
std::unordered_map<VkFramebuffer, VkExtent2D> clamped;
struct Pass {
  // The attachments its subpasses use as input, color, resolve or
  // depth/stencil attachments. Only those must cover the framebuffer
  // (VUID-VkFramebufferCreateInfo-flags-04533/04534); unused, preserve-only,
  // shading-rate and density-map attachments may be smaller.
  std::vector<bool> used;
  // Attachments whose contents are neither loaded nor stored, and attachments
  // some subpass reads as input attachments.
  std::vector<bool> transient, input;
  std::vector<VkImageLayout> initial_layouts, final_layouts;
  // Attachment formats and sample counts, and the layers a multiview subpass renders (1 without multiview).
  std::vector<VkFormat> formats;
  std::vector<VkSampleCountFlagBits> samples;
  std::uint32_t view_layers = 1;
  // (source, resolve attachment) pairs of color and depth/stencil resolves.
  std::vector<std::pair<std::uint32_t, std::uint32_t>> resolves;
};
std::unordered_map<VkRenderPass, Pass> passes;
// SwiftShader: host images standing in for guest resolve sources.
struct StandIn {
  std::uint32_t index;  // the attachment it replaces
  VkImage image;
  VkImageView view;
  VkDeviceMemory memory;
  VkImageSubresourceRange range;
  VkImage guest;
  VkImageSubresourceRange guest_range;
};
struct Substitution {
  VkDevice device;
  VkFramebufferCreateInfo info;  // the host framebuffer's, without pNext
  std::vector<VkImageView> views;  // the guest's attachments
  std::vector<StandIn> stand_ins;
  VkFramebuffer plain{};  // with the guest's views, for passes that keep a source's contents
};
std::unordered_map<VkFramebuffer, Substitution> substitutions;
// Host placeholders standing in for a degenerate guest framebuffer's null attachments.
std::unordered_map<VkFramebuffer, std::pair<VkDevice, std::vector<StandIn>>> placeholders;
std::unordered_map<VkPhysicalDevice, bool> whole_extent_resolves;
std::optional<bool> forced_stand_ins;

template <class F>
F host(VkCommandBuffer cmd, const char* name) {
  return reinterpret_cast<F>(raw_device_proc(command_device(cmd), name));
}
template <class F, ProcedureName Name>
F next(VkDevice device) {
  if (auto f = cached_layer_proc<Layer::render_area, Name>()) return reinterpret_cast<F>(f);
  return reinterpret_cast<F>(raw_device_proc(device, Name.value));
}
template <class T>
const T* find_chain(const void* next, VkStructureType type) {
  for (auto* p = static_cast<const VkBaseInStructure*>(next); p; p = p->pNext)
    if (p->sType == type) return reinterpret_cast<const T*>(p);
  return nullptr;
}
VkRect2D clamp(const VkRect2D& r, const VkRect2D& area) {
  const auto x0 = std::max<std::int64_t>(r.offset.x, area.offset.x);
  const auto y0 = std::max<std::int64_t>(r.offset.y, area.offset.y);
  const auto x1 = std::max(x0, std::min<std::int64_t>(std::int64_t(r.offset.x) + r.extent.width,
                                                      std::int64_t(area.offset.x) + area.extent.width));
  const auto y1 = std::max(y0, std::min<std::int64_t>(std::int64_t(r.offset.y) + r.extent.height,
                                                      std::int64_t(area.offset.y) + area.extent.height));
  return {{static_cast<std::int32_t>(x0), static_cast<std::int32_t>(y0)},
          {static_cast<std::uint32_t>(x1 - x0), static_cast<std::uint32_t>(y1 - y0)}};
}
bool same(const VkRect2D& a, const VkRect2D& b) {
  return a.offset.x == b.offset.x && a.offset.y == b.offset.y && a.extent.width == b.extent.width &&
         a.extent.height == b.extent.height;
}
// Caller holds mutex. Passes this layer did not see narrow over every attachment.
bool used(VkRenderPass pass, std::uint32_t index) {
  auto it = passes.find(pass);
  return it == passes.end() || (index < it->second.used.size() && it->second.used[index]);
}
// Narrows `bounds` to a view's extent when the view is known. Caller holds mutex.
void narrow(VkExtent2D& bounds, VkImageView view) {
  if (auto it = views.find(view); it != views.end()) {
    bounds.width = std::min(bounds.width, it->second.extent.width);
    bounds.height = std::min(bounds.height, it->second.extent.height);
  }
}
void report_clamp(const char* what, std::uint32_t width, std::uint32_t height, VkExtent2D to) {
  static std::atomic<int> reports{0};
  if (reports.fetch_add(1, std::memory_order_relaxed) < 8)
    log::warn("vulkan", "{} {}x{} exceeds its smallest attachment; the host uses {}x{}", what, width, height,
              to.width, to.height);
}
// Scissors to forward from `first` on: clamped inside a render pass.
struct Forward {
  bool with_count{};
  std::uint32_t first{};
  std::vector<VkRect2D> rects;
};
// Also records them as the host's scissors.
Forward forwarded(Scissors& s, std::uint32_t first, std::uint32_t count) {
  Forward out{s.with_count, first, {s.rects.begin() + first, s.rects.begin() + first + count}};
  if (s.area)
    for (auto& r : out.rects) r = clamp(r, *s.area);
  if (s.with_count) s.host.clear();
  if (s.host.size() < first + count) s.host.resize(first + count);
  std::copy(out.rects.begin(), out.rects.end(), s.host.begin() + first);
  return out;
}
void forward(VkCommandBuffer cmd, const Forward& f) {
  const auto count = static_cast<std::uint32_t>(f.rects.size());
  if (f.with_count)
    host<PFN_vkCmdSetScissorWithCount>(cmd, "vkCmdSetScissorWithCount")(cmd, count, f.rects.data());
  else
    host<PFN_vkCmdSetScissor>(cmd, "vkCmdSetScissor")(cmd, f.first, count, f.rects.data());
}
// Dynamic state persists into a render pass instance: scissors the guest set
// before it began are clamped to the new render area, unless the host already
// has them. A subpass recorded in secondary command buffers takes no other
// command (VkSubpassContents), and its secondaries inherit no dynamic state.
void begin_area(VkCommandBuffer cmd, const VkRect2D& area, bool inline_contents,
                std::vector<VkImageMemoryBarrier> after = {}) {
  Forward f;
  {
    std::lock_guard lock(mutex);
    auto& s = states[cmd];
    s.area = area;
    s.after = std::move(after);
    if (!inline_contents || s.rects.empty()) return;
    const auto host = s.host;
    f = forwarded(s, 0, static_cast<std::uint32_t>(s.rects.size()));
    if (host.size() == f.rects.size() && std::equal(host.begin(), host.end(), f.rects.begin(), same)) return;
  }
  forward(cmd, f);
}
bool inline_subpass(VkSubpassContents contents) {
  return contents == VK_SUBPASS_CONTENTS_INLINE || contents == VK_SUBPASS_CONTENTS_INLINE_AND_SECONDARY_COMMAND_BUFFERS_KHR;
}
// Returns the final-layout transitions of guest images replaced in the instance.
std::vector<VkImageMemoryBarrier> end_area(VkCommandBuffer cmd) {
  std::lock_guard lock(mutex);
  auto it = states.find(cmd);
  if (it == states.end()) return {};
  it->second.area.reset();
  return std::exchange(it->second.after, {});
}
// Layout transitions of stand-ins and replaced guest images, outside instances.
void transition(VkCommandBuffer cmd, const std::vector<VkImageMemoryBarrier>& barriers) {
  if (barriers.empty()) return;
  host<PFN_vkCmdPipelineBarrier>(cmd, "vkCmdPipelineBarrier")(
      cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr,
      static_cast<std::uint32_t>(barriers.size()), barriers.data());
}

constexpr VkImageUsageFlags attachment_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                               VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                               VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
VkResult VKAPI_CALL create_image(VkDevice d, const VkImageCreateInfo* info, const VkAllocationCallbacks* alloc,
                                 VkImage* out) {
  auto r = next<PFN_vkCreateImage, "vkCreateImage">(d)(d, info, alloc, out);
  if (r == VK_SUCCESS) note_image(*out, *info);
  return r;
}
void VKAPI_CALL destroy_image(VkDevice d, VkImage image, const VkAllocationCallbacks* alloc) {
  forget_image(image);
  next<PFN_vkDestroyImage, "vkDestroyImage">(d)(d, image, alloc);
}
VkResult VKAPI_CALL create_view(VkDevice d, const VkImageViewCreateInfo* info, const VkAllocationCallbacks* alloc,
                                VkImageView* out) {
  auto r = next<PFN_vkCreateImageView, "vkCreateImageView">(d)(d, info, alloc, out);
  if (r != VK_SUCCESS) return r;
  std::lock_guard lock(mutex);
  if (auto it = images.find(info->image); it != images.end()) {
    const auto mip = info->subresourceRange.baseMipLevel;
    views[*out] = {{std::max(1u, it->second.extent.width >> mip), std::max(1u, it->second.extent.height >> mip)},
                   info->image,
                   info->format,
                   info->viewType,
                   info->subresourceRange};
  }
  return r;
}
void VKAPI_CALL destroy_view(VkDevice d, VkImageView view, const VkAllocationCallbacks* alloc) {
  {
    std::lock_guard lock(mutex);
    views.erase(view);
  }
  next<PFN_vkDestroyImageView, "vkDestroyImageView">(d)(d, view, alloc);
}
template <class Subpass>
void use_subpass(Pass& out, const Subpass& sp) {
  auto use = [&](std::uint32_t index) {
    if (index != VK_ATTACHMENT_UNUSED && index < out.used.size()) out.used[index] = true;
  };
  auto resolve = [&](std::uint32_t source, std::uint32_t target) {
    if (source != VK_ATTACHMENT_UNUSED && target != VK_ATTACHMENT_UNUSED && source < out.used.size() &&
        target < out.used.size())
      out.resolves.emplace_back(source, target);
  };
  for (std::uint32_t i = 0; i < sp.inputAttachmentCount; ++i) {
    const auto index = sp.pInputAttachments[i].attachment;
    use(index);
    if (index != VK_ATTACHMENT_UNUSED && index < out.input.size()) out.input[index] = true;
  }
  for (std::uint32_t i = 0; i < sp.colorAttachmentCount; ++i) {
    use(sp.pColorAttachments[i].attachment);
    if (sp.pResolveAttachments) {
      use(sp.pResolveAttachments[i].attachment);
      resolve(sp.pColorAttachments[i].attachment, sp.pResolveAttachments[i].attachment);
    }
  }
  if (sp.pDepthStencilAttachment) use(sp.pDepthStencilAttachment->attachment);
  if constexpr (std::is_same_v<Subpass, VkSubpassDescription2>)
    if (auto* ds = find_chain<VkSubpassDescriptionDepthStencilResolve>(
            sp.pNext, VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE);
        ds && ds->pDepthStencilResolveAttachment) {
      use(ds->pDepthStencilResolveAttachment->attachment);
      if (sp.pDepthStencilAttachment)
        resolve(sp.pDepthStencilAttachment->attachment, ds->pDepthStencilResolveAttachment->attachment);
    }
}
bool has_stencil(VkFormat format) {
  return format == VK_FORMAT_S8_UINT || format == VK_FORMAT_D16_UNORM_S8_UINT ||
         format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT;
}
template <class Info>
void record_pass(VkRenderPass pass, const Info& info) {
  Pass out;
  out.used.resize(info.attachmentCount);
  out.input.resize(info.attachmentCount);
  for (std::uint32_t i = 0; i < info.attachmentCount; ++i) {
    // STORE_OP_NONE keeps contents the instance did not write.
    const auto& a = info.pAttachments[i];
    const bool stencil = has_stencil(a.format);
    out.transient.push_back(a.loadOp != VK_ATTACHMENT_LOAD_OP_LOAD && a.storeOp == VK_ATTACHMENT_STORE_OP_DONT_CARE &&
                            (!stencil || (a.stencilLoadOp != VK_ATTACHMENT_LOAD_OP_LOAD &&
                                          a.stencilStoreOp == VK_ATTACHMENT_STORE_OP_DONT_CARE)));
    out.initial_layouts.push_back(a.initialLayout);
    out.final_layouts.push_back(a.finalLayout);
    out.formats.push_back(a.format);
    out.samples.push_back(a.samples);
  }
  for (std::uint32_t i = 0; i < info.subpassCount; ++i) use_subpass(out, info.pSubpasses[i]);
  auto views = [&](std::uint32_t mask) { out.view_layers = std::max(out.view_layers, std::uint32_t(std::bit_width(mask))); };
  if constexpr (std::is_same_v<Info, VkRenderPassCreateInfo2>) {
    for (std::uint32_t i = 0; i < info.subpassCount; ++i) views(info.pSubpasses[i].viewMask);
  } else if (auto* multiview = find_chain<VkRenderPassMultiviewCreateInfo>(
                 info.pNext, VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO)) {
    for (std::uint32_t i = 0; i < multiview->subpassCount; ++i) views(multiview->pViewMasks[i]);
  }
  std::lock_guard lock(mutex);
  passes[pass] = std::move(out);
}
void report_added_dependencies(std::size_t added, std::uint32_t subpasses) {
  static std::atomic<int> reports{0};
  if (reports.fetch_add(1, std::memory_order_relaxed) < 8)
    log::info("vulkan", "Render pass with {} subpasses: added {} subpass dependencies its attachment accesses need",
              subpasses, added);
}
// The guest's dependencies plus the missing ones (subpass_sync.cpp). A
// VkRenderPassMultiviewCreateInfo with per-dependency view offsets gets offset
// 0 for each added dependency, which are not view-local.
struct CompletedPass {
  VkRenderPassCreateInfo info;
  std::vector<VkSubpassDependency> dependencies;
  std::vector<std::int32_t> offsets;
  std::vector<std::vector<std::byte>> chain;
};
bool complete(const VkRenderPassCreateInfo& guest, CompletedPass& out) {
  auto added = missing_subpass_dependencies(guest);
  if (added.empty()) return false;
  out.info = guest;
  out.dependencies.assign(guest.pDependencies, guest.pDependencies + guest.dependencyCount);
  out.dependencies.insert(out.dependencies.end(), added.begin(), added.end());
  out.info.dependencyCount = std::uint32_t(out.dependencies.size());
  out.info.pDependencies = out.dependencies.data();
  auto* mv = find_chain<VkRenderPassMultiviewCreateInfo>(guest.pNext, VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO);
  if (mv && mv->dependencyCount) {
    out.offsets.assign(mv->pViewOffsets, mv->pViewOffsets + mv->dependencyCount);
    out.offsets.resize(out.dependencies.size(), 0);
    // Shallow copies of the chain, relinked, with the multiview structure's
    // offsets replaced.
    const void* head = nullptr;
    VkBaseOutStructure* tail = nullptr;
    for (auto* p = static_cast<const VkBaseInStructure*>(guest.pNext); p; p = p->pNext) {
      const auto size = structure_size(p->sType);
      if (!size) continue;
      auto& copy = out.chain.emplace_back(size);
      std::memcpy(copy.data(), p, size);
      auto* base = reinterpret_cast<VkBaseOutStructure*>(copy.data());
      base->pNext = nullptr;
      if (p->sType == VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO) {
        auto* copied = reinterpret_cast<VkRenderPassMultiviewCreateInfo*>(base);
        copied->dependencyCount = std::uint32_t(out.offsets.size());
        copied->pViewOffsets = out.offsets.data();
      }
      if (tail) tail->pNext = base;
      else head = base;
      tail = base;
    }
    out.info.pNext = head;
  }
  report_added_dependencies(added.size(), guest.subpassCount);
  return true;
}
VkResult VKAPI_CALL create_render_pass(VkDevice d, const VkRenderPassCreateInfo* info,
                                       const VkAllocationCallbacks* alloc, VkRenderPass* out) {
  CompletedPass completed;
  const auto* host = complete(*info, completed) ? &completed.info : info;
  auto r = next<PFN_vkCreateRenderPass, "vkCreateRenderPass">(d)(d, host, alloc, out);
  if (r == VK_SUCCESS) record_pass(*out, *info);
  return r;
}
// Also vkCreateRenderPass2KHR, the same command for devices below 1.2.
template <ProcedureName Name>
VkResult VKAPI_CALL create_render_pass2(VkDevice d, const VkRenderPassCreateInfo2* info,
                                        const VkAllocationCallbacks* alloc, VkRenderPass* out) {
  VkRenderPassCreateInfo2 completed;
  std::vector<VkSubpassDependency2> dependencies;
  const auto* host = info;
  if (auto added = missing_subpass_dependencies(*info); !added.empty()) {
    dependencies.assign(info->pDependencies, info->pDependencies + info->dependencyCount);
    dependencies.insert(dependencies.end(), added.begin(), added.end());
    completed = *info;
    completed.dependencyCount = std::uint32_t(dependencies.size());
    completed.pDependencies = dependencies.data();
    host = &completed;
    report_added_dependencies(added.size(), info->subpassCount);
  }
  auto r = next<PFN_vkCreateRenderPass2, Name>(d)(d, host, alloc, out);
  if (r == VK_SUCCESS) record_pass(*out, *info);
  return r;
}
void VKAPI_CALL destroy_render_pass(VkDevice d, VkRenderPass pass, const VkAllocationCallbacks* alloc) {
  {
    std::lock_guard lock(mutex);
    passes.erase(pass);
  }
  next<PFN_vkDestroyRenderPass, "vkDestroyRenderPass">(d)(d, pass, alloc);
}
template <class F>
F raw(VkDevice d, const char* name) {
  return reinterpret_cast<F>(raw_device_proc(d, name));
}
// Whether the device's driver is SwiftShader, which resolves a source's whole
// extent. Queried once per physical device.
bool resolves_whole_extent(VkDevice d) {
  const auto physical = device_physical(d);
  {
    std::lock_guard lock(mutex);
    if (forced_stand_ins) return *forced_stand_ins;
    if (auto it = whole_extent_resolves.find(physical); it != whole_extent_resolves.end()) return it->second;
  }
  VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
  VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &driver};
  if (auto get = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
          raw_physical_proc(physical, "vkGetPhysicalDeviceProperties2")))
    get(physical, &properties);
  std::lock_guard lock(mutex);
  return whole_extent_resolves[physical] = driver.driverID == VK_DRIVER_ID_GOOGLE_SWIFTSHADER;
}
// Without separateDepthStencilLayouts a layout transition covers both aspects.
VkImageSubresourceRange transition_range(VkImageSubresourceRange range, VkFormat format) {
  if (range.aspectMask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))
    range.aspectMask = format == VK_FORMAT_S8_UINT ? VK_IMAGE_ASPECT_STENCIL_BIT
                       : has_stencil(format)      ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
                                                  : VK_IMAGE_ASPECT_DEPTH_BIT;
  return range;
}
VkImageMemoryBarrier layout_barrier(VkImage image, const VkImageSubresourceRange& range, VkImageLayout from,
                                    VkImageLayout to) {
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
  b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  b.oldLayout = from;
  b.newLayout = to;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = image;
  b.subresourceRange = range;
  return b;
}
void release(VkDevice d, const StandIn& s) {
  raw<PFN_vkDestroyImageView>(d, "vkDestroyImageView")(d, s.view, nullptr);
  raw<PFN_vkDestroyImage>(d, "vkDestroyImage")(d, s.image, nullptr);
  raw<PFN_vkFreeMemory>(d, "vkFreeMemory")(d, s.memory, nullptr);
}
// A host-only transient attachment image of `extent` and its view, or nullopt.
std::optional<StandIn> transient_attachment(VkDevice d, std::uint32_t index, VkFormat format,
                                            VkSampleCountFlagBits samples, VkImageViewType type,
                                            VkImageAspectFlags aspect, VkExtent2D extent, std::uint32_t layers,
                                            VkImageUsageFlags usage, VkResult* failure = nullptr) {
  auto failed = [&](VkResult result) -> std::optional<StandIn> {
    if (failure) *failure = result;
    return std::nullopt;
  };
  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ci.imageType = VK_IMAGE_TYPE_2D;
  ci.format = format;
  ci.extent = {extent.width, extent.height, 1};
  ci.mipLevels = 1;
  ci.arrayLayers = layers;
  ci.samples = samples;
  ci.tiling = VK_IMAGE_TILING_OPTIMAL;
  ci.usage = usage | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
  StandIn s{index};
  auto result = raw<PFN_vkCreateImage>(d, "vkCreateImage")(d, &ci, nullptr, &s.image);
  if (result != VK_SUCCESS) return failed(result);
  VkMemoryRequirements req{};
  raw<PFN_vkGetImageMemoryRequirements>(d, "vkGetImageMemoryRequirements")(d, s.image, &req);
  VkPhysicalDeviceMemoryProperties props{};
  const auto physical = device_physical(d);
  reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
      raw_physical_proc(physical, "vkGetPhysicalDeviceMemoryProperties"))(physical, &props);
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = req.size;
  ai.memoryTypeIndex = props.memoryTypeCount;
  for (VkMemoryPropertyFlags wanted : {VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT),
                                       VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT), VkMemoryPropertyFlags{}})
    for (std::uint32_t i = 0; i < props.memoryTypeCount && ai.memoryTypeIndex == props.memoryTypeCount; ++i)
      if ((req.memoryTypeBits >> i & 1) && (props.memoryTypes[i].propertyFlags & wanted) == wanted)
        ai.memoryTypeIndex = i;
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vi.image = s.image;
  vi.viewType = type;
  vi.format = format;
  vi.subresourceRange = {aspect, 0, 1, 0, layers};
  result = ai.memoryTypeIndex == props.memoryTypeCount ? VK_ERROR_FEATURE_NOT_PRESENT
      : raw<PFN_vkAllocateMemory>(d, "vkAllocateMemory")(d, &ai, nullptr, &s.memory);
  if (result == VK_SUCCESS)
    result = raw<PFN_vkBindImageMemory>(d, "vkBindImageMemory")(d, s.image, s.memory, 0);
  if (result == VK_SUCCESS)
    result = raw<PFN_vkCreateImageView>(d, "vkCreateImageView")(d, &vi, nullptr, &s.view);
  if (result != VK_SUCCESS) {
    release(d, s);
    return failed(result);
  }
  s.range = transition_range(vi.subresourceRange, format);
  return s;
}
// A host-only transient image of `extent` for attachment `index`'s view, or nullopt.
std::optional<StandIn> stand_in(VkDevice d, std::uint32_t index, const View& view, const Image& image,
                                VkExtent2D extent) {
  if (view.type != VK_IMAGE_VIEW_TYPE_2D && view.type != VK_IMAGE_VIEW_TYPE_2D_ARRAY) return std::nullopt;
  const auto layers = view.range.layerCount == VK_REMAINING_ARRAY_LAYERS
                          ? image.layers - view.range.baseArrayLayer
                          : view.range.layerCount;
  auto s = transient_attachment(d, index, view.format, image.samples, view.type, view.range.aspectMask, extent,
                                layers, image.usage & attachment_usage);
  if (!s) return s;
  s->guest = view.image;
  s->guest_range = transition_range(view.range, view.format);
  return s;
}
VkResult create_host_framebuffer(VkDevice d, const VkFramebufferCreateInfo* info,
                                 const VkAllocationCallbacks* alloc, VkFramebuffer* out) {
  VkExtent2D bounds{info->width, info->height};
  // SwiftShader: resolve sources larger than their resolve attachments.
  struct Source {
    std::uint32_t index;
    View view;
    Image image;
  };
  std::vector<Source> sources;
  const bool imageless = info->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT;
  const bool whole_extent = !imageless && resolves_whole_extent(d);
  {
    std::lock_guard lock(mutex);
    if (!imageless)
      for (std::uint32_t i = 0; i < info->attachmentCount; ++i)
        if (used(info->renderPass, i)) narrow(bounds, info->pAttachments[i]);
    if (auto* list = imageless ? find_chain<VkFramebufferAttachmentsCreateInfo>(
                                     info->pNext, VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENTS_CREATE_INFO)
                               : nullptr)
      for (std::uint32_t i = 0; i < list->attachmentImageInfoCount; ++i) {
        const auto& a = list->pAttachmentImageInfos[i];
        if (!used(info->renderPass, i) || !(a.usage & attachment_usage)) continue;
        bounds.width = std::min(bounds.width, a.width);
        bounds.height = std::min(bounds.height, a.height);
      }
    auto pass = passes.find(info->renderPass);
    if (whole_extent && pass != passes.end())
      for (auto [source, target] : pass->second.resolves) {
        // An input attachment's descriptor names the guest's view.
        if (source >= info->attachmentCount || target >= info->attachmentCount || pass->second.input[source] ||
            std::any_of(sources.begin(), sources.end(), [&](const Source& s) { return s.index == source; }))
          continue;
        auto from = views.find(info->pAttachments[source]);
        auto to = views.find(info->pAttachments[target]);
        if (from == views.end() || to == views.end() ||
            (from->second.extent.width <= to->second.extent.width &&
             from->second.extent.height <= to->second.extent.height))
          continue;
        auto image = images.find(from->second.image);
        if (image == images.end() || image->second.samples <= VK_SAMPLE_COUNT_1_BIT) continue;
        if (!pass->second.transient[source]) {
          static std::atomic<int> kept{0};
          if (kept.fetch_add(1, std::memory_order_relaxed) < 8)
            log::warn("vulkan", "Resolve source {}x{} keeps its contents but exceeds its {}x{} resolve attachment",
                      from->second.extent.width, from->second.extent.height, to->second.extent.width,
                      to->second.extent.height);
          continue;
        }
        sources.push_back({source, from->second, image->second});
      }
  }
  Substitution sub{d, *info};
  std::vector<VkImageView> attachments;
  for (const auto& source : sources)
    if (auto s = stand_in(d, source.index, source.view, source.image, bounds)) {
      static std::atomic<int> reports{0};
      if (reports.fetch_add(1, std::memory_order_relaxed) < 8)
        log::warn("vulkan", "Resolve source {}x{} exceeds its resolve attachment; the host renders it in a {}x{} "
                  "transient image", source.view.extent.width, source.view.extent.height, bounds.width,
                  bounds.height);
      if (attachments.empty()) attachments.assign(info->pAttachments, info->pAttachments + info->attachmentCount);
      attachments[source.index] = s->view;
      sub.stand_ins.push_back(*s);
    }
  const bool resized = bounds.width != info->width || bounds.height != info->height;
  if (!resized && sub.stand_ins.empty())
    return next<PFN_vkCreateFramebuffer, "vkCreateFramebuffer">(d)(d, info, alloc, out);
  if (resized) report_clamp("Framebuffer", info->width, info->height, bounds);
  auto copy = *info;
  copy.width = bounds.width;
  copy.height = bounds.height;
  if (!sub.stand_ins.empty()) copy.pAttachments = attachments.data();
  auto r = next<PFN_vkCreateFramebuffer, "vkCreateFramebuffer">(d)(d, &copy, alloc, out);
  if (r != VK_SUCCESS) {
    for (const auto& s : sub.stand_ins) release(d, s);
    return r;
  }
  std::lock_guard lock(mutex);
  if (resized) clamped[*out] = bounds;
  if (!sub.stand_ins.empty()) {
    sub.info.pNext = nullptr;
    sub.info.width = bounds.width;
    sub.info.height = bounds.height;
    sub.views.assign(info->pAttachments, info->pAttachments + info->attachmentCount);
    substitutions[*out] = std::move(sub);
  }
  return r;
}
// A guest framebuffer with no size or a null attachment view (HITMAN on Monado
// computed 0x0 render targets and never created them) is invalid, and RADV
// dereferences the null view when a pass on it begins. A Quest driver does not
// crash there, so each missing attachment gets a 1x1 host placeholder and the
// instance renders into nothing.
VkResult VKAPI_CALL create_framebuffer(VkDevice d, const VkFramebufferCreateInfo* info,
                                       const VkAllocationCallbacks* alloc, VkFramebuffer* out) {
  if (info->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT ||
      (info->width && info->height &&
       std::none_of(info->pAttachments, info->pAttachments + info->attachmentCount,
                    [](VkImageView v) { return v == VK_NULL_HANDLE; })))
    return create_host_framebuffer(d, info, alloc, out);
  std::optional<Pass> pass;
  {
    std::lock_guard lock(mutex);
    if (auto it = passes.find(info->renderPass); it != passes.end()) pass = it->second;
  }
  std::vector<VkImageView> attachments(info->pAttachments, info->pAttachments + info->attachmentCount);
  std::vector<StandIn> blanks;
  auto failed = [&](VkResult result) {
    for (const auto& blank : blanks) release(d, blank);
    // Image creation has errors that vkCreateFramebuffer cannot return.
    if (result != VK_ERROR_OUT_OF_HOST_MEMORY && result != VK_ERROR_OUT_OF_DEVICE_MEMORY &&
        result != VK_ERROR_VALIDATION_FAILED_EXT)
      return VK_ERROR_UNKNOWN;
    return result;
  };
  const auto layers = std::max(info->layers, pass ? pass->view_layers : 1u);
  for (std::uint32_t i = 0; i < attachments.size(); ++i) {
    if (attachments[i]) continue;
    // Never forward an unresolved null view, even when repair is impossible.
    if (!pass || i >= pass->formats.size()) return failed(VK_ERROR_VALIDATION_FAILED_EXT);
    const auto format = pass->formats[i];
    const bool depth = format == VK_FORMAT_D16_UNORM || format == VK_FORMAT_X8_D24_UNORM_PACK32 ||
                       format == VK_FORMAT_D32_SFLOAT || has_stencil(format);
    const VkImageAspectFlags aspect = !depth ? VK_IMAGE_ASPECT_COLOR_BIT
        : format == VK_FORMAT_S8_UINT ? VK_IMAGE_ASPECT_STENCIL_BIT
        : has_stencil(format) ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
    VkImageUsageFlags usage = depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (pass->input[i]) usage |= VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    VkResult failure = VK_ERROR_UNKNOWN;
    if (auto blank = transient_attachment(d, i, format, pass->samples[i], VK_IMAGE_VIEW_TYPE_2D_ARRAY, aspect, {1, 1},
                                          layers, usage, &failure)) {
      attachments[i] = blank->view;
      blanks.push_back(*blank);
    } else return failed(failure);
  }
  static std::atomic<int> reports{0};
  if (reports.fetch_add(1, std::memory_order_relaxed) < 8)
    log::warn("vulkan", "Guest framebuffer {}x{} has {} null attachment view(s); the host gets a 1x1 framebuffer "
              "with placeholders", info->width, info->height, blanks.size());
  auto copy = *info;
  copy.pAttachments = attachments.data();
  copy.width = copy.height = 1;
  copy.layers = std::max(info->layers, 1u);
  const auto r = create_host_framebuffer(d, &copy, alloc, out);
  if (r != VK_SUCCESS) return failed(r);
  std::lock_guard lock(mutex);
  clamped[*out] = {1, 1};
  if (!blanks.empty()) placeholders[*out] = {d, std::move(blanks)};
  return r;
}
void VKAPI_CALL destroy_framebuffer(VkDevice d, VkFramebuffer framebuffer, const VkAllocationCallbacks* alloc) {
  std::optional<Substitution> owned;
  std::vector<StandIn> blanks;
  {
    std::lock_guard lock(mutex);
    clamped.erase(framebuffer);
    if (auto it = placeholders.find(framebuffer); it != placeholders.end()) {
      blanks = std::move(it->second.second);
      placeholders.erase(it);
    }
    if (auto it = substitutions.find(framebuffer); it != substitutions.end()) {
      owned = std::move(it->second);
      substitutions.erase(it);
    }
  }
  next<PFN_vkDestroyFramebuffer, "vkDestroyFramebuffer">(d)(d, framebuffer, alloc);
  for (const auto& b : blanks) release(d, b);
  if (!owned) return;
  if (owned->plain) next<PFN_vkDestroyFramebuffer, "vkDestroyFramebuffer">(d)(d, owned->plain, nullptr);
  for (const auto& s : owned->stand_ins) release(d, s);
}
// SwiftShader: the host framebuffer for a render pass instance on a framebuffer
// with stand-ins, and the layout transitions before and after the instance.
struct HostBegin {
  VkFramebuffer framebuffer;
  std::vector<VkImageMemoryBarrier> before, after;
};
HostBegin host_begin(const VkRenderPassBeginInfo* info) {
  HostBegin out{info->framebuffer};
  VkDevice device{};
  VkFramebufferCreateInfo ci{};
  std::vector<VkImageView> attachments;
  {
    std::lock_guard lock(mutex);
    auto sub = substitutions.find(info->framebuffer);
    if (sub == substitutions.end()) return out;
    // A compatible render pass may load, store or read what the framebuffer's discarded.
    auto pass = passes.find(info->renderPass);
    bool keeps = pass == passes.end();
    for (const auto& s : sub->second.stand_ins)
      keeps = keeps || s.index >= pass->second.transient.size() || !pass->second.transient[s.index] ||
              pass->second.input[s.index];
    if (!keeps) {
      for (const auto& s : sub->second.stand_ins) {
        const auto initial = pass->second.initial_layouts[s.index];
        const auto final = pass->second.final_layouts[s.index];
        if (initial != VK_IMAGE_LAYOUT_UNDEFINED)
          out.before.push_back(layout_barrier(s.image, s.range, VK_IMAGE_LAYOUT_UNDEFINED, initial));
        // The guest's image keeps what lies outside the render area, as the instance would.
        if (initial != final) out.after.push_back(layout_barrier(s.guest, s.guest_range, initial, final));
      }
      return out;
    }
    if (sub->second.plain) {
      out.framebuffer = sub->second.plain;
      return out;
    }
    device = sub->second.device;
    ci = sub->second.info;
    attachments = sub->second.views;
  }
  ci.pAttachments = attachments.data();
  VkFramebuffer plain{};
  if (next<PFN_vkCreateFramebuffer, "vkCreateFramebuffer">(device)(device, &ci, nullptr, &plain) != VK_SUCCESS)
    return out;
  {
    std::lock_guard lock(mutex);
    if (auto sub = substitutions.find(info->framebuffer); sub != substitutions.end()) {
      if (!sub->second.plain) {
        out.framebuffer = sub->second.plain = plain;
        return out;
      }
      out.framebuffer = sub->second.plain;
    }
  }
  next<PFN_vkDestroyFramebuffer, "vkDestroyFramebuffer">(device)(device, plain, nullptr);
  return out;
}
// The render area inside the host framebuffer and, for imageless framebuffers,
// the views supplied at begin.
VkRect2D pass_area(const VkRenderPassBeginInfo* info) {
  VkExtent2D bounds{~0u, ~0u};
  std::lock_guard lock(mutex);
  if (auto it = clamped.find(info->framebuffer); it != clamped.end()) bounds = it->second;
  if (auto* begin = find_chain<VkRenderPassAttachmentBeginInfo>(info->pNext,
                                                                VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO))
    for (std::uint32_t i = 0; i < begin->attachmentCount; ++i)
      if (used(info->renderPass, i)) narrow(bounds, begin->pAttachments[i]);
  return clamp(info->renderArea, {{0, 0}, bounds});
}
VkRect2D rendering_area(const VkRenderingInfo* info) {
  VkExtent2D bounds{~0u, ~0u};
  std::lock_guard lock(mutex);
  for (std::uint32_t i = 0; i < info->colorAttachmentCount; ++i) {
    narrow(bounds, info->pColorAttachments[i].imageView);
    if (info->pColorAttachments[i].resolveMode != VK_RESOLVE_MODE_NONE)
      narrow(bounds, info->pColorAttachments[i].resolveImageView);
  }
  for (const auto* a : {info->pDepthAttachment, info->pStencilAttachment})
    if (a) {
      narrow(bounds, a->imageView);
      if (a->resolveMode != VK_RESOLVE_MODE_NONE) narrow(bounds, a->resolveImageView);
    }
  return clamp(info->renderArea, {{0, 0}, bounds});
}
template <class Info>
Info with_area(const Info& info, const VkRect2D& area) {
  if (!same(area, info.renderArea))
    report_clamp("Render area", info.renderArea.extent.width, info.renderArea.extent.height, area.extent);
  auto copy = info;
  copy.renderArea = area;
  return copy;
}

void VKAPI_CALL set_scissor(VkCommandBuffer cmd, std::uint32_t first, std::uint32_t count, const VkRect2D* rects) {
  Forward f;
  {
    std::lock_guard lock(mutex);
    auto& s = states[cmd];
    if (s.with_count) s.rects.clear();
    s.with_count = false;
    if (s.rects.size() < first + count) s.rects.resize(first + count);
    std::copy_n(rects, count, s.rects.begin() + first);
    f = forwarded(s, first, count);
  }
  forward(cmd, f);
}
void VKAPI_CALL set_scissor_with_count(VkCommandBuffer cmd, std::uint32_t count, const VkRect2D* rects) {
  Forward f;
  {
    std::lock_guard lock(mutex);
    auto& s = states[cmd];
    s.with_count = true;
    s.rects.assign(rects, rects + count);
    f = forwarded(s, 0, count);
  }
  forward(cmd, f);
}
void VKAPI_CALL begin_pass(VkCommandBuffer cmd, const VkRenderPassBeginInfo* info, VkSubpassContents contents) {
  const auto area = pass_area(info);
  auto begin = host_begin(info);
  transition(cmd, begin.before);
  auto copy = with_area(*info, area);
  copy.framebuffer = begin.framebuffer;
  host<PFN_vkCmdBeginRenderPass>(cmd, "vkCmdBeginRenderPass")(cmd, &copy, contents);
  begin_area(cmd, area, inline_subpass(contents), std::move(begin.after));
}
void VKAPI_CALL begin_pass2(VkCommandBuffer cmd, const VkRenderPassBeginInfo* info, const VkSubpassBeginInfo* sub) {
  const auto area = pass_area(info);
  auto begin = host_begin(info);
  transition(cmd, begin.before);
  auto copy = with_area(*info, area);
  copy.framebuffer = begin.framebuffer;
  host<PFN_vkCmdBeginRenderPass2>(cmd, "vkCmdBeginRenderPass2")(cmd, &copy, sub);
  begin_area(cmd, area, inline_subpass(sub->contents), std::move(begin.after));
}
void VKAPI_CALL begin_rendering(VkCommandBuffer cmd, const VkRenderingInfo* info) {
  const auto area = rendering_area(info);
  const auto copy = with_area(*info, area);
  host<PFN_vkCmdBeginRendering>(cmd, "vkCmdBeginRendering")(cmd, &copy);
  // VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT does not restrict commands.
  begin_area(cmd, area, true);
}
bool bakes_scissor(const VkGraphicsPipelineCreateInfo& info) {
  // Libraries and rasterizer-discard pipelines leave the scissor state to others.
  if ((info.flags & VK_PIPELINE_CREATE_LIBRARY_BIT_KHR) || !info.pViewportState ||
      find_chain<VkPipelineLibraryCreateInfoKHR>(info.pNext, VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR))
    return false;
  if (info.pDynamicState)
    for (std::uint32_t i = 0; i < info.pDynamicState->dynamicStateCount; ++i) {
      const auto state = info.pDynamicState->pDynamicStates[i];
      if (state == VK_DYNAMIC_STATE_SCISSOR || state == VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT) return false;
    }
  return true;
}
VkResult VKAPI_CALL create_graphics_pipelines(VkDevice d, VkPipelineCache cache, std::uint32_t count,
                                              const VkGraphicsPipelineCreateInfo* infos,
                                              const VkAllocationCallbacks* alloc, VkPipeline* out) {
  auto r = next<PFN_vkCreateGraphicsPipelines, "vkCreateGraphicsPipelines">(d)(d, cache, count, infos, alloc, out);
  std::lock_guard lock(mutex);
  for (std::uint32_t i = 0; i < count; ++i)
    if (out[i] && bakes_scissor(infos[i])) static_scissor.insert(out[i]);
  return r;
}
void VKAPI_CALL destroy_pipeline(VkDevice d, VkPipeline pipeline, const VkAllocationCallbacks* alloc) {
  {
    std::lock_guard lock(mutex);
    static_scissor.erase(pipeline);
  }
  next<PFN_vkDestroyPipeline, "vkDestroyPipeline">(d)(d, pipeline, alloc);
}
void VKAPI_CALL bind_pipeline(VkCommandBuffer cmd, VkPipelineBindPoint point, VkPipeline pipeline) {
  if (point == VK_PIPELINE_BIND_POINT_GRAPHICS) {
    std::lock_guard lock(mutex);
    if (static_scissor.contains(pipeline)) {
      auto& s = states[cmd];
      s.rects.clear();
      s.host.clear();
    }
  }
  next<PFN_vkCmdBindPipeline, "vkCmdBindPipeline">(command_device(cmd))(cmd, point, pipeline);
}
void VKAPI_CALL end_pass(VkCommandBuffer cmd) {
  const auto after = end_area(cmd);
  host<PFN_vkCmdEndRenderPass>(cmd, "vkCmdEndRenderPass")(cmd);
  transition(cmd, after);
}
void VKAPI_CALL end_pass2(VkCommandBuffer cmd, const VkSubpassEndInfo* info) {
  const auto after = end_area(cmd);
  host<PFN_vkCmdEndRenderPass2>(cmd, "vkCmdEndRenderPass2")(cmd, info);
  transition(cmd, after);
}
void VKAPI_CALL end_rendering(VkCommandBuffer cmd) {
  end_area(cmd);
  host<PFN_vkCmdEndRendering>(cmd, "vkCmdEndRendering")(cmd);
}
}  // namespace

void note_image(VkImage image, const VkImageCreateInfo& info) {
  // Only images that can be attachments ever bound a framebuffer.
  if (!(info.usage & attachment_usage)) return;
  std::lock_guard lock(mutex);
  images[image] = {{info.extent.width, info.extent.height}, info.arrayLayers, info.samples, info.usage};
}
void forget_image(VkImage image) {
  std::lock_guard lock(mutex);
  images.erase(image);
}
VkExtent2D attachment_extent(VkImage image) {
  std::lock_guard lock(mutex);
  auto it = images.find(image);
  return it == images.end() ? VkExtent2D{} : it->second.extent;
}
void release_render_commands(VkCommandBuffer command) {
  std::lock_guard lock(mutex);
  states.erase(command);
}
void force_resolve_stand_ins(std::optional<bool> enabled) {
  std::lock_guard lock(mutex);
  forced_stand_ins = enabled;
}
PFN_vkVoidFunction render_area_proc(std::string_view name) {
#define FN(command, function) \
  if (name == #command) return reinterpret_cast<PFN_vkVoidFunction>(function)
  FN(vkCreateImage, create_image);
  FN(vkDestroyImage, destroy_image);
  FN(vkCreateImageView, create_view);
  FN(vkDestroyImageView, destroy_view);
  FN(vkCreateRenderPass, create_render_pass);
  FN(vkCreateRenderPass2, create_render_pass2<"vkCreateRenderPass2">);
  FN(vkCreateRenderPass2KHR, create_render_pass2<"vkCreateRenderPass2KHR">);
  FN(vkDestroyRenderPass, destroy_render_pass);
  FN(vkCreateFramebuffer, create_framebuffer);
  FN(vkDestroyFramebuffer, destroy_framebuffer);
  FN(vkCreateGraphicsPipelines, create_graphics_pipelines);
  FN(vkDestroyPipeline, destroy_pipeline);
  FN(vkCmdBindPipeline, bind_pipeline);
  FN(vkCmdSetScissor, set_scissor);
  FN(vkCmdSetScissorWithCount, set_scissor_with_count);
  FN(vkCmdBeginRenderPass, begin_pass);
  FN(vkCmdBeginRenderPass2, begin_pass2);
  FN(vkCmdBeginRendering, begin_rendering);
  FN(vkCmdEndRenderPass, end_pass);
  FN(vkCmdEndRenderPass2, end_pass2);
  FN(vkCmdEndRendering, end_rendering);
#undef FN
  return nullptr;
}
}  // namespace venture::vulkan

#include "common/environment.h"
// VENTURE_TRACE_VULKAN diagnostics for render-target bounds. Out-of-bounds
// attachment writes are invalid usage that tilers often survive, while
// desktop GPUs fault the channel and report VK_ERROR_DEVICE_LOST at a later,
// unrelated submit. These checks name the offending framebuffer, render pass
// or blit at record time.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "common/log.h"
#include "internal.h"
#include "texture.h"

namespace venture::vulkan {
namespace {
struct ImageInfo {
  VkExtent3D extent{};
  std::uint32_t mips{}, layers{};
  VkFormat format{};
  VkSampleCountFlagBits samples{};
  VkImageUsageFlags usage{};
  VkImageCreateFlags flags{};
};
struct ViewInfo {
  ImageInfo image;
  std::uint32_t mip{}, layers{};
};
struct FramebufferInfo {
  std::uint32_t width{}, height{}, layers{};
  std::vector<VkImageView> views;
};
std::mutex mutex;
std::unordered_map<VkImage, ImageInfo> images;
std::unordered_map<VkImageView, ViewInfo> views;
std::unordered_map<VkFramebuffer, FramebufferInfo> framebuffers;
// Layers a render pass's multiview subpasses write: highest view index + 1.
std::unordered_map<VkRenderPass, std::uint32_t> pass_views;
std::unordered_map<VkCommandBuffer, VkRect2D> render_areas;
// Dynamic viewport 0 and scissor 0 per command buffer, and whether a render
// pass instance is open. "The application must ensure (using scissor if
// necessary) that all rendering is contained within the render area."
struct DrawState {
  bool in_pass{};
  VkFramebuffer framebuffer{};
  std::optional<VkViewport> viewport;
  std::optional<VkRect2D> scissor;
};
std::unordered_map<VkCommandBuffer, DrawState> draw_states;
std::atomic<int> reports{0};

bool enabled() {
  static const bool on = venture::environment::getenv("VENTURE_TRACE_VULKAN") != nullptr ||
                         venture::environment::getenv("VENTURE_TRACE_VULKAN_OBJECTS") != nullptr;
  return on;
}
// Public API object-lifetime diagnostics only: never read image, buffer or shader contents.
template <class... A>
void object_trace(std::format_string<A...> fmt, A&&... args) {
  if (!enabled()) return;
  static std::atomic<unsigned> records{0};
  if (records.fetch_add(1, std::memory_order_relaxed) < 8192)
    log::info("vulkan-object", fmt, std::forward<A>(args)...);
}

template <class... A>
void report(std::format_string<A...> fmt, A&&... args) {
  if (reports.fetch_add(1, std::memory_order_relaxed) < 64)
    log::warn("vulkan-check", fmt, std::forward<A>(args)...);
}
template <class F>
F host(VkDevice device, const char* name) {
  return reinterpret_cast<F>(raw_device_proc(device, name));
}
template <class F, ProcedureName Name>
F next(VkDevice device) {
  if (auto f = cached_layer_proc<Layer::render_check, Name>()) return reinterpret_cast<F>(f);
  return host<F>(device, Name.value);
}
template <class T>
const T* find_chain(const void* next, VkStructureType type) {
  for (auto* p = static_cast<const VkBaseInStructure*>(next); p; p = p->pNext)
    if (p->sType == type) return reinterpret_cast<const T*>(p);
  return nullptr;
}
std::uint32_t mip_size(std::uint32_t size, std::uint32_t mip) { return std::max(1u, size >> mip); }
// Caller holds mutex.
const ViewInfo* view(VkImageView handle) {
  auto it = views.find(handle);
  return it == views.end() ? nullptr : &it->second;
}
void check_view(const char* where, std::uint32_t index, VkImageView handle, std::uint32_t width,
                std::uint32_t height, std::uint32_t layers) {
  auto* v = view(handle);
  if (!v) {
    static std::atomic<int> unknown{0};
    if (unknown.fetch_add(1) < 8)
      report("{} attachment {} view {:#x} is not a guest image view (rendering covers {}x{}x{})", where, index,
             reinterpret_cast<std::uintptr_t>(handle), width, height, layers);
    return;
  }
  auto w = mip_size(v->image.extent.width, v->mip), h = mip_size(v->image.extent.height, v->mip);
  if (w < width || h < height || v->layers < layers)
    report("{} attachment {} view {:#x} is {}x{}x{} (image {}x{} mip {} format {} samples {}) but rendering covers "
           "{}x{}x{}",
           where, index, reinterpret_cast<std::uintptr_t>(handle), w, h, v->layers, v->image.extent.width,
           v->image.extent.height, v->mip, int(v->image.format), int(v->image.samples), width, height, layers);
}
void check_area(const char* where, VkCommandBuffer cmd, const VkRect2D& area, std::uint32_t width,
                std::uint32_t height) {
  if (area.offset.x < 0 || area.offset.y < 0 || std::uint64_t(area.offset.x) + area.extent.width > width ||
      std::uint64_t(area.offset.y) + area.extent.height > height)
    report("{} render area ({},{}) {}x{} exceeds framebuffer {}x{} on command buffer {:#x}", where, area.offset.x,
           area.offset.y, area.extent.width, area.extent.height, width, height,
           reinterpret_cast<std::uintptr_t>(cmd));
}
void check_pass(const char* where, VkCommandBuffer cmd, const VkRenderPassBeginInfo* info) {
  if (!info) return;
  std::lock_guard lock(mutex);
  render_areas[cmd] = info->renderArea;
  draw_states[cmd].in_pass = true;
  draw_states[cmd].framebuffer = info->framebuffer;
  auto it = framebuffers.find(info->framebuffer);
  if (it == framebuffers.end()) return;
  auto& fb = it->second;
  check_area(where, cmd, info->renderArea, fb.width, fb.height);
  std::uint32_t multiview = 0;
  if (auto views = pass_views.find(info->renderPass); views != pass_views.end()) multiview = views->second;
  // One inventory line per framebuffer: which targets the title renders to.
  static std::set<VkFramebuffer> listed;
  if (listed.size() < 160 && listed.insert(info->framebuffer).second) {
    std::string line = std::format("{} fb {:#x} {}x{}x{} area ({},{}) {}x{} multiview {}", where,
                                   reinterpret_cast<std::uintptr_t>(info->framebuffer), fb.width, fb.height,
                                   fb.layers, info->renderArea.offset.x, info->renderArea.offset.y,
                                   info->renderArea.extent.width, info->renderArea.extent.height, multiview);
    for (std::uint32_t i = 0; i < fb.views.size(); ++i) {
      if (auto* v = view(fb.views[i]))
        line += std::format(" [{}: {}x{} mip {} layers {}/{} fmt {} x{} usage {:#x} flags {:#x}]", i, v->image.extent.width,
                            v->image.extent.height, v->mip, v->layers, v->image.layers, int(v->image.format),
                            int(v->image.samples), v->image.usage, v->image.flags);
      else
        line += std::format(" [{}: untracked view]", i);
    }
    log::info("vulkan-pass", "{}", line);
  }
  if (multiview) {
    static std::set<std::pair<VkImageView, std::uint32_t>> seen;
    for (std::uint32_t i = 0; i < fb.views.size(); ++i)
      if (seen.insert({fb.views[i], multiview}).second) check_view(where, i, fb.views[i], 0, 0, multiview);
  }
  if (auto* begin = find_chain<VkRenderPassAttachmentBeginInfo>(info->pNext,
                                                                VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO))
    for (std::uint32_t i = 0; i < begin->attachmentCount; ++i)
      check_view(where, i, begin->pAttachments[i], fb.width, fb.height, fb.layers);
}
void check_blit(const char* where, VkImage handle, const VkImageSubresourceLayers& sub, const VkOffset3D* offsets) {
  auto it = images.find(handle);
  if (it == images.end()) return;
  auto& img = it->second;
  auto w = mip_size(img.extent.width, sub.mipLevel), h = mip_size(img.extent.height, sub.mipLevel);
  auto layers = sub.layerCount == VK_REMAINING_ARRAY_LAYERS ? img.layers - sub.baseArrayLayer : sub.layerCount;
  for (int i = 0; i < 2; ++i)
    if (offsets[i].x < 0 || offsets[i].y < 0 || std::uint32_t(offsets[i].x) > w || std::uint32_t(offsets[i].y) > h ||
        sub.mipLevel >= img.mips || sub.baseArrayLayer + layers > img.layers) {
      report("{} offset ({},{}) mip {} layers {}+{} is outside image {:#x} ({}x{}, {} mips, {} layers, format {})",
             where, offsets[i].x, offsets[i].y, sub.mipLevel, sub.baseArrayLayer, layers,
             reinterpret_cast<std::uintptr_t>(handle), img.extent.width, img.extent.height, img.mips, img.layers,
             int(img.format));
      return;
    }
}

VkResult VKAPI_CALL create_image(VkDevice d, const VkImageCreateInfo* info, const VkAllocationCallbacks* alloc,
                                 VkImage* out) {
  auto r = next<PFN_vkCreateImage, "vkCreateImage">(d)(d, info, alloc, out);
  if (r == VK_SUCCESS) {
    auto format = texture_format(info->format);
    const auto planes = format ? emulated_texture_planes(*out).size() : 0;
    std::lock_guard lock(mutex);
    if (format) {
      static std::set<std::tuple<VkFormat, VkImageUsageFlags, VkImageCreateFlags>> listed;
      if (listed.size() < 64 && listed.emplace(info->format, info->usage, info->flags).second)
        log::info("vulkan-image", "compressed format {} usage {:#x} flags {:#x} decoded={} backing_planes={} decode_format={}",
                  int(info->format), info->usage, info->flags, planes != 0, planes, int(format.decoded));
    }
    images[*out] = {info->extent, info->mipLevels, info->arrayLayers, info->format, info->samples,
                    info->usage, info->flags};
  }
  return r;
}
void VKAPI_CALL destroy_image(VkDevice d, VkImage handle, const VkAllocationCallbacks* alloc) {
  {
    std::lock_guard lock(mutex);
    images.erase(handle);
  }
  next<PFN_vkDestroyImage, "vkDestroyImage">(d)(d, handle, alloc);
}
VkResult VKAPI_CALL create_view(VkDevice d, const VkImageViewCreateInfo* info, const VkAllocationCallbacks* alloc,
                                VkImageView* out) {
  auto r = next<PFN_vkCreateImageView, "vkCreateImageView">(d)(d, info, alloc, out);
  object_trace("create_view device={:#x} image={:#x} result={} view={:#x} format={} mip={} layers={}+{}",
      reinterpret_cast<std::uintptr_t>(d), reinterpret_cast<std::uintptr_t>(info->image), int(r),
      r == VK_SUCCESS ? reinterpret_cast<std::uintptr_t>(*out) : 0,
      int(info->format), info->subresourceRange.baseMipLevel,
      info->subresourceRange.baseArrayLayer, info->subresourceRange.layerCount);
  if (r == VK_SUCCESS) {
    std::lock_guard lock(mutex);
    if (auto it = images.find(info->image); it != images.end()) {
      auto& range = info->subresourceRange;
      auto layers = range.layerCount == VK_REMAINING_ARRAY_LAYERS ? it->second.layers - range.baseArrayLayer
                                                                  : range.layerCount;
      views[*out] = {it->second, range.baseMipLevel, layers};
    }
  }
  return r;
}
void VKAPI_CALL destroy_view(VkDevice d, VkImageView handle, const VkAllocationCallbacks* alloc) {
  object_trace("destroy_view device={:#x} view={:#x}",
      reinterpret_cast<std::uintptr_t>(d), reinterpret_cast<std::uintptr_t>(handle));
  {
    std::lock_guard lock(mutex);
    views.erase(handle);
  }
  next<PFN_vkDestroyImageView, "vkDestroyImageView">(d)(d, handle, alloc);
}
VkResult VKAPI_CALL create_framebuffer(VkDevice d, const VkFramebufferCreateInfo* info,
                                       const VkAllocationCallbacks* alloc, VkFramebuffer* out) {
  object_trace("create_framebuffer device={:#x} pass={:#x} size={}x{}x{} flags={:#x} attachments={}",
      reinterpret_cast<std::uintptr_t>(d), reinterpret_cast<std::uintptr_t>(info->renderPass),
      info->width, info->height, info->layers, info->flags, info->attachmentCount);
  if (enabled() && !(info->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT))
    for (std::uint32_t i = 0; i < info->attachmentCount; ++i)
      object_trace("framebuffer_attachment device={:#x} index={} view={:#x}",
          reinterpret_cast<std::uintptr_t>(d), i, reinterpret_cast<std::uintptr_t>(info->pAttachments[i]));
  auto r = next<PFN_vkCreateFramebuffer, "vkCreateFramebuffer">(d)(d, info, alloc, out);
  if (r != VK_SUCCESS) return r;
  std::lock_guard lock(mutex);
  FramebufferInfo fb{info->width, info->height, info->layers, {}};
  if (info->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT) {
    if (auto* list = find_chain<VkFramebufferAttachmentsCreateInfo>(
            info->pNext, VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENTS_CREATE_INFO))
      for (std::uint32_t i = 0; i < list->attachmentImageInfoCount; ++i) {
        auto& a = list->pAttachmentImageInfos[i];
        if (a.width < info->width || a.height < info->height || a.layerCount < info->layers)
          report("imageless framebuffer {}x{}x{} attachment {} is {}x{}x{}", info->width, info->height,
                 info->layers, i, a.width, a.height, a.layerCount);
      }
  } else {
    fb.views.assign(info->pAttachments, info->pAttachments + info->attachmentCount);
    for (std::uint32_t i = 0; i < info->attachmentCount; ++i)
      check_view("vkCreateFramebuffer", i, info->pAttachments[i], info->width, info->height, info->layers);
  }
  framebuffers[*out] = std::move(fb);
  return r;
}
// Pipelines without dynamic scissors bypass the render-area clamp.
VkResult VKAPI_CALL create_graphics(VkDevice d, VkPipelineCache cache, std::uint32_t count,
                                    const VkGraphicsPipelineCreateInfo* infos, const VkAllocationCallbacks* alloc,
                                    VkPipeline* out) {
  static std::atomic<int> listed{0};
  for (std::uint32_t i = 0; i < count; ++i) {
    const auto* vp = infos[i].pViewportState;
    if (!vp || (infos[i].pRasterizationState && infos[i].pRasterizationState->rasterizerDiscardEnable)) continue;
    bool dyn_scissor = false, dyn_viewport = false;
    if (const auto* dyn = infos[i].pDynamicState)
      for (std::uint32_t k = 0; k < dyn->dynamicStateCount; ++k) {
        auto st = dyn->pDynamicStates[k];
        dyn_scissor |= st == VK_DYNAMIC_STATE_SCISSOR || st == VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT;
        dyn_viewport |= st == VK_DYNAMIC_STATE_VIEWPORT || st == VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT;
      }
    if ((!dyn_scissor || !dyn_viewport) && listed.fetch_add(1) < 16) {
      std::string rects;
      if (!dyn_scissor && vp->pScissors)
        for (std::uint32_t k = 0; k < vp->scissorCount; ++k)
          rects += std::format(" scissor ({},{}) {}x{}", vp->pScissors[k].offset.x, vp->pScissors[k].offset.y,
                               vp->pScissors[k].extent.width, vp->pScissors[k].extent.height);
      if (!dyn_viewport && vp->pViewports)
        for (std::uint32_t k = 0; k < vp->viewportCount; ++k)
          rects += std::format(" viewport ({},{}) {}x{}", vp->pViewports[k].x, vp->pViewports[k].y,
                               vp->pViewports[k].width, vp->pViewports[k].height);
      report("graphics pipeline with static{}{}:{}", dyn_scissor ? "" : " scissor", dyn_viewport ? "" : " viewport",
             rects);
    }
  }
  return next<PFN_vkCreateGraphicsPipelines, "vkCreateGraphicsPipelines">(d)(d, cache, count, infos, alloc, out);
}
std::uint32_t highest_view(std::uint32_t mask) { return mask ? 32u - std::uint32_t(__builtin_clz(mask)) : 0u; }
VkResult VKAPI_CALL create_pass(VkDevice d, const VkRenderPassCreateInfo* info, const VkAllocationCallbacks* alloc,
                                VkRenderPass* out) {
  auto r = next<PFN_vkCreateRenderPass, "vkCreateRenderPass">(d)(d, info, alloc, out);
  if (r != VK_SUCCESS) return r;
  std::uint32_t views = 0;
  if (auto* mv = find_chain<VkRenderPassMultiviewCreateInfo>(info->pNext,
                                                             VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO))
    for (std::uint32_t i = 0; i < mv->subpassCount; ++i) views = std::max(views, highest_view(mv->pViewMasks[i]));
  std::lock_guard lock(mutex);
  if (views) pass_views[*out] = views;
  return r;
}
VkResult VKAPI_CALL create_pass2(VkDevice d, const VkRenderPassCreateInfo2* info, const VkAllocationCallbacks* alloc,
                                 VkRenderPass* out) {
  auto r = next<PFN_vkCreateRenderPass2, "vkCreateRenderPass2">(d)(d, info, alloc, out);
  if (r != VK_SUCCESS) return r;
  std::uint32_t views = 0;
  for (std::uint32_t i = 0; i < info->subpassCount; ++i)
    views = std::max(views, highest_view(info->pSubpasses[i].viewMask));
  std::lock_guard lock(mutex);
  if (views) pass_views[*out] = views;
  return r;
}
void VKAPI_CALL destroy_framebuffer(VkDevice d, VkFramebuffer handle, const VkAllocationCallbacks* alloc) {
  {
    std::lock_guard lock(mutex);
    framebuffers.erase(handle);
  }
  next<PFN_vkDestroyFramebuffer, "vkDestroyFramebuffer">(d)(d, handle, alloc);
}
void VKAPI_CALL begin_pass(VkCommandBuffer cmd, const VkRenderPassBeginInfo* info, VkSubpassContents contents) {
  check_pass("vkCmdBeginRenderPass", cmd, info);
  next<PFN_vkCmdBeginRenderPass, "vkCmdBeginRenderPass">(command_device(cmd))(cmd, info, contents);
}
void VKAPI_CALL begin_pass2(VkCommandBuffer cmd, const VkRenderPassBeginInfo* info, const VkSubpassBeginInfo* sub) {
  check_pass("vkCmdBeginRenderPass2", cmd, info);
  next<PFN_vkCmdBeginRenderPass2, "vkCmdBeginRenderPass2">(command_device(cmd))(cmd, info, sub);
}
void VKAPI_CALL begin_rendering(VkCommandBuffer cmd, const VkRenderingInfo* info) {
  {
    std::lock_guard lock(mutex);
    render_areas[cmd] = info->renderArea;
    draw_states[cmd].in_pass = true;
    draw_states[cmd].framebuffer = VK_NULL_HANDLE;
    auto w = std::uint32_t(std::max(0, info->renderArea.offset.x)) + info->renderArea.extent.width;
    auto h = std::uint32_t(std::max(0, info->renderArea.offset.y)) + info->renderArea.extent.height;
    auto layers = info->viewMask ? 32u - std::uint32_t(__builtin_clz(info->viewMask)) : info->layerCount;
    for (std::uint32_t i = 0; i < info->colorAttachmentCount; ++i)
      check_view("vkCmdBeginRendering", i, info->pColorAttachments[i].imageView, w, h, layers);
    if (info->pDepthAttachment) check_view("vkCmdBeginRendering depth", 0, info->pDepthAttachment->imageView, w, h, layers);
  }
  next<PFN_vkCmdBeginRendering, "vkCmdBeginRendering">(command_device(cmd))(cmd, info);
}
void VKAPI_CALL clear_attachments(VkCommandBuffer cmd, std::uint32_t count, const VkClearAttachment* attachments,
                                  std::uint32_t rect_count, const VkClearRect* rects) {
  {
    std::lock_guard lock(mutex);
    if (auto it = render_areas.find(cmd); it != render_areas.end())
      for (std::uint32_t i = 0; i < rect_count; ++i) {
        auto& a = it->second;
        auto& r = rects[i].rect;
        if (r.offset.x < a.offset.x || r.offset.y < a.offset.y ||
            std::int64_t(r.offset.x) + r.extent.width > std::int64_t(a.offset.x) + a.extent.width ||
            std::int64_t(r.offset.y) + r.extent.height > std::int64_t(a.offset.y) + a.extent.height)
          report("vkCmdClearAttachments rect ({},{}) {}x{} exceeds render area ({},{}) {}x{}", r.offset.x,
                 r.offset.y, r.extent.width, r.extent.height, a.offset.x, a.offset.y, a.extent.width,
                 a.extent.height);
      }
  }
  next<PFN_vkCmdClearAttachments, "vkCmdClearAttachments">(command_device(cmd))(cmd, count, attachments,
                                                                                 rect_count, rects);
}
void VKAPI_CALL blit(VkCommandBuffer cmd, VkImage source, VkImageLayout source_layout, VkImage dest,
                     VkImageLayout dest_layout, std::uint32_t count, const VkImageBlit* regions, VkFilter filter) {
  {
    std::lock_guard lock(mutex);
    for (std::uint32_t i = 0; i < count; ++i) {
      check_blit("vkCmdBlitImage source", source, regions[i].srcSubresource, regions[i].srcOffsets);
      check_blit("vkCmdBlitImage destination", dest, regions[i].dstSubresource, regions[i].dstOffsets);
    }
  }
  next<PFN_vkCmdBlitImage, "vkCmdBlitImage">(command_device(cmd))(cmd, source, source_layout, dest, dest_layout,
                                                                   count, regions, filter);
}
void VKAPI_CALL resolve(VkCommandBuffer cmd, VkImage source, VkImageLayout source_layout, VkImage dest,
                        VkImageLayout dest_layout, std::uint32_t count, const VkImageResolve* regions) {
  {
    std::lock_guard lock(mutex);
    for (std::uint32_t i = 0; i < count; ++i) {
      auto& r = regions[i];
      VkOffset3D src[2]{r.srcOffset, {r.srcOffset.x + std::int32_t(r.extent.width),
                                      r.srcOffset.y + std::int32_t(r.extent.height), 0}};
      VkOffset3D dst[2]{r.dstOffset, {r.dstOffset.x + std::int32_t(r.extent.width),
                                      r.dstOffset.y + std::int32_t(r.extent.height), 0}};
      check_blit("vkCmdResolveImage source", source, r.srcSubresource, src);
      check_blit("vkCmdResolveImage destination", dest, r.dstSubresource, dst);
    }
  }
  next<PFN_vkCmdResolveImage, "vkCmdResolveImage">(command_device(cmd))(cmd, source, source_layout, dest,
                                                                         dest_layout, count, regions);
}
void end_pass(VkCommandBuffer cmd) {
  std::lock_guard lock(mutex);
  draw_states[cmd].in_pass = false;
}
void VKAPI_CALL end_render_pass(VkCommandBuffer cmd) {
  end_pass(cmd);
  next<PFN_vkCmdEndRenderPass, "vkCmdEndRenderPass">(command_device(cmd))(cmd);
}
void VKAPI_CALL end_render_pass2(VkCommandBuffer cmd, const VkSubpassEndInfo* info) {
  end_pass(cmd);
  next<PFN_vkCmdEndRenderPass2, "vkCmdEndRenderPass2">(command_device(cmd))(cmd, info);
}
void VKAPI_CALL end_rendering(VkCommandBuffer cmd) {
  end_pass(cmd);
  next<PFN_vkCmdEndRendering, "vkCmdEndRendering">(command_device(cmd))(cmd);
}
void VKAPI_CALL set_viewport(VkCommandBuffer cmd, std::uint32_t first, std::uint32_t count,
                             const VkViewport* viewports) {
  if (first == 0 && count) {
    std::lock_guard lock(mutex);
    draw_states[cmd].viewport = viewports[0];
  }
  next<PFN_vkCmdSetViewport, "vkCmdSetViewport">(command_device(cmd))(cmd, first, count, viewports);
}
void VKAPI_CALL set_scissor(VkCommandBuffer cmd, std::uint32_t first, std::uint32_t count, const VkRect2D* scissors) {
  if (first == 0 && count) {
    std::lock_guard lock(mutex);
    draw_states[cmd].scissor = scissors[0];
  }
  next<PFN_vkCmdSetScissor, "vkCmdSetScissor">(command_device(cmd))(cmd, first, count, scissors);
}
// Fragments come from viewport 0 clipped by scissor 0; report draws whose
// rectangle leaves the render area.
void check_draw(const char* where, VkCommandBuffer cmd) {
  static std::set<std::tuple<std::int32_t, std::int32_t, std::uint32_t, std::uint32_t, std::int64_t, std::int64_t,
                             std::int64_t, std::int64_t>>
      seen;
  std::lock_guard lock(mutex);
  auto state = draw_states.find(cmd);
  auto area = render_areas.find(cmd);
  if (state == draw_states.end() || area == render_areas.end() || !state->second.in_pass ||
      !state->second.scissor)
    return;
  const auto& sc = *state->second.scissor;
  std::int64_t x0 = sc.offset.x, y0 = sc.offset.y, x1 = x0 + sc.extent.width, y1 = y0 + sc.extent.height;
  if (auto& v = state->second.viewport) {
    auto vy0 = std::min(v->y, v->y + v->height), vy1 = std::max(v->y, v->y + v->height);
    x0 = std::max<std::int64_t>(x0, std::int64_t(v->x));
    x1 = std::min<std::int64_t>(x1, std::int64_t(std::ceil(v->x + v->width)));
    y0 = std::max<std::int64_t>(y0, std::int64_t(vy0));
    y1 = std::min<std::int64_t>(y1, std::int64_t(std::ceil(vy1)));
  }
  if (x0 >= x1 || y0 >= y1) return;
  const auto& a = area->second;
  if (x0 >= a.offset.x && y0 >= a.offset.y && x1 <= std::int64_t(a.offset.x) + a.extent.width &&
      y1 <= std::int64_t(a.offset.y) + a.extent.height)
    return;
  if (!seen.insert({a.offset.x, a.offset.y, a.extent.width, a.extent.height, x0, y0, x1, y1}).second) return;
  std::uint32_t fw = 0, fh = 0;
  if (auto fb = framebuffers.find(state->second.framebuffer); fb != framebuffers.end()) {
    fw = fb->second.width;
    fh = fb->second.height;
  }
  auto& v = state->second.viewport;
  report("{} rasterizes ({},{})-({},{}) outside render area ({},{}) {}x{} (framebuffer {}x{}; scissor ({},{}) {}x{}; "
         "viewport ({},{}) {}x{})",
         where, x0, y0, x1, y1, a.offset.x, a.offset.y, a.extent.width, a.extent.height, fw, fh, sc.offset.x,
         sc.offset.y, sc.extent.width, sc.extent.height, v ? v->x : 0.f, v ? v->y : 0.f, v ? v->width : 0.f,
         v ? v->height : 0.f);
}
void VKAPI_CALL draw(VkCommandBuffer cmd, std::uint32_t vertices, std::uint32_t instances, std::uint32_t first,
                     std::uint32_t first_instance) {
  check_draw("vkCmdDraw", cmd);
  next<PFN_vkCmdDraw, "vkCmdDraw">(command_device(cmd))(cmd, vertices, instances, first, first_instance);
}
void VKAPI_CALL draw_indexed(VkCommandBuffer cmd, std::uint32_t indices, std::uint32_t instances,
                             std::uint32_t first, std::int32_t offset, std::uint32_t first_instance) {
  check_draw("vkCmdDrawIndexed", cmd);
  next<PFN_vkCmdDrawIndexed, "vkCmdDrawIndexed">(command_device(cmd))(cmd, indices, instances, first, offset,
                                                                       first_instance);
}
void VKAPI_CALL draw_indirect(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset, std::uint32_t count,
                              std::uint32_t stride) {
  check_draw("vkCmdDrawIndirect", cmd);
  next<PFN_vkCmdDrawIndirect, "vkCmdDrawIndirect">(command_device(cmd))(cmd, buffer, offset, count, stride);
}
void VKAPI_CALL draw_indexed_indirect(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset,
                                      std::uint32_t count, std::uint32_t stride) {
  check_draw("vkCmdDrawIndexedIndirect", cmd);
  next<PFN_vkCmdDrawIndexedIndirect, "vkCmdDrawIndexedIndirect">(command_device(cmd))(cmd, buffer, offset, count,
                                                                                       stride);
}
}  // namespace

PFN_vkVoidFunction render_check_proc(std::string_view name) {
  if (!enabled()) return nullptr;
#define FN(command, function) \
  if (name == #command) return reinterpret_cast<PFN_vkVoidFunction>(function)
  FN(vkCreateImage, create_image);
  FN(vkDestroyImage, destroy_image);
  FN(vkCreateImageView, create_view);
  FN(vkDestroyImageView, destroy_view);
  FN(vkCreateFramebuffer, create_framebuffer);
  FN(vkCreateRenderPass, create_pass);
  FN(vkCreateGraphicsPipelines, create_graphics);
  FN(vkCreateRenderPass2, create_pass2);
  FN(vkDestroyFramebuffer, destroy_framebuffer);
  FN(vkCmdBeginRenderPass, begin_pass);
  FN(vkCmdBeginRenderPass2, begin_pass2);
  FN(vkCmdBeginRendering, begin_rendering);
  FN(vkCmdClearAttachments, clear_attachments);
  FN(vkCmdBlitImage, blit);
  FN(vkCmdEndRenderPass, end_render_pass);
  FN(vkCmdEndRenderPass2, end_render_pass2);
  FN(vkCmdEndRendering, end_rendering);
  FN(vkCmdSetViewport, set_viewport);
  FN(vkCmdSetScissor, set_scissor);
  FN(vkCmdDraw, draw);
  FN(vkCmdDrawIndexed, draw_indexed);
  FN(vkCmdDrawIndirect, draw_indirect);
  FN(vkCmdDrawIndexedIndirect, draw_indexed_indirect);
  FN(vkCmdResolveImage, resolve);
#undef FN
  return nullptr;
}
}  // namespace venture::vulkan

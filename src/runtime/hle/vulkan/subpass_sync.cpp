// Attachment hazards between subpasses. Without a subpass dependency there is
// no ordering between the attachment accesses of two subpasses, nor for the
// layout transition between them (Vulkan spec, Render Pass: "Subpass
// Dependencies"); tilers that run subpasses tile by tile in order tolerate the
// omission, desktop GPUs overlap them. Unity's and UE4's mobile render passes
// resolve or read in subpass 1 what subpass 0 wrote, with dependencies that
// cover only the shader read (docs/DEVICE_LOSS.md, RE4 sync validation).
//
// For each attachment, every pair of consecutive subpasses that use it gets a
// dependency covering both uses whenever one of them writes it or its layout
// changes between them, unless the guest's own direct dependency already
// does. Consecutive pairs suffice: each added dependency's source scope holds
// the attachment's stages in the middle subpass, which the previous one's
// destination scope holds too, so they chain. Attachment accesses are
// framebuffer-local, so BY_REGION is exact; dependencies are not view-local,
// so they also hold across views. Depth/stencil resolve attachments also get
// an incoming external dependency (below). A render pass that already covers
// its accesses reaches the host unchanged.
#include <algorithm>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <vector>

#include "internal.h"

namespace venture::vulkan {
namespace {
template <class T>
const T* find_chain(const void* next, VkStructureType type) {
  for (auto* p = static_cast<const VkBaseInStructure*>(next); p; p = p->pNext)
    if (p->sType == type) return reinterpret_cast<const T*>(p);
  return nullptr;
}
constexpr VkPipelineStageFlags color_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
constexpr VkPipelineStageFlags depth_stages =
    VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
constexpr VkAccessFlags write_accesses = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
constexpr VkAccessFlags read_accesses = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
constexpr VkPipelineStageFlags graphics_stages =
    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | color_stage | depth_stages;

struct Use {
  VkPipelineStageFlags stages = 0;
  VkAccessFlags accesses = 0;
  std::optional<VkImageLayout> layout;
  bool used = false;
  void add(VkPipelineStageFlags s, VkAccessFlags a, std::optional<VkImageLayout> l = {}) {
    used = true;
    stages |= s;
    accesses |= a;
    if (l && !layout) layout = l;
  }
  bool writes() const { return accesses & write_accesses; }
};
bool read_only_depth(VkImageLayout layout) {
  return layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL || layout == VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL ||
         layout == VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL || layout == VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL;
}
bool depth_format(VkFormat format) {
  switch (format) {
    case VK_FORMAT_D16_UNORM: case VK_FORMAT_X8_D24_UNORM_PACK32: case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_S8_UINT: case VK_FORMAT_D16_UNORM_S8_UINT: case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
      return true;
    default:
      return false;
  }
}
// The accesses a dependency's masks cover, with the umbrella stage and access
// bits expanded. A VkMemoryBarrier2 in a VkSubpassDependency2 chain replaces
// the masks.
struct Scope {
  VkPipelineStageFlags src_stages, dst_stages;
  VkAccessFlags src_accesses, dst_accesses;
};
VkPipelineStageFlags expand_stages(std::uint64_t stages) {
  if (stages & (VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT)) return graphics_stages;
  return VkPipelineStageFlags(stages & graphics_stages);
}
VkAccessFlags expand_accesses(std::uint64_t accesses) {
  VkAccessFlags out = VkAccessFlags(accesses & (read_accesses | write_accesses));
  if (accesses & VK_ACCESS_MEMORY_WRITE_BIT) out |= write_accesses;
  if (accesses & VK_ACCESS_MEMORY_READ_BIT) out |= read_accesses;
  return out;
}
template <class Dependency>
Scope scope(const Dependency& d) {
  std::uint64_t src = d.srcStageMask, dst = d.dstStageMask, src_access = d.srcAccessMask,
                dst_access = d.dstAccessMask;
  if constexpr (std::is_same_v<Dependency, VkSubpassDependency2>)
    if (auto* b = find_chain<VkMemoryBarrier2>(d.pNext, VK_STRUCTURE_TYPE_MEMORY_BARRIER_2)) {
      src = b->srcStageMask, dst = b->dstStageMask, src_access = b->srcAccessMask, dst_access = b->dstAccessMask;
    }
  return {expand_stages(src), expand_stages(dst), expand_accesses(src_access), expand_accesses(dst_access)};
}

template <class Info, class Dependency>
std::vector<Dependency> missing(const Info& info) {
  const std::uint32_t attachments = info.attachmentCount, subpasses = info.subpassCount;
  if (!attachments || !subpasses || !info.pAttachments || !info.pSubpasses) return {};
  // uses[subpass * attachments + attachment]
  std::vector<Use> uses(std::size_t(subpasses) * attachments);
  auto at = [&](std::uint32_t subpass, std::uint32_t attachment) -> Use* {
    return attachment < attachments ? &uses[std::size_t(subpass) * attachments + attachment] : nullptr;
  };
  for (std::uint32_t s = 0; s < subpasses; ++s) {
    const auto& sp = info.pSubpasses[s];
    for (std::uint32_t i = 0; sp.pInputAttachments && i < sp.inputAttachmentCount; ++i)
      if (auto* u = at(s, sp.pInputAttachments[i].attachment))
        u->add(VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_INPUT_ATTACHMENT_READ_BIT,
               sp.pInputAttachments[i].layout);
    for (std::uint32_t i = 0; sp.pColorAttachments && i < sp.colorAttachmentCount; ++i) {
      if (auto* u = at(s, sp.pColorAttachments[i].attachment))
        u->add(color_stage, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
               sp.pColorAttachments[i].layout);
      if (sp.pResolveAttachments)
        if (auto* u = at(s, sp.pResolveAttachments[i].attachment))
          u->add(color_stage, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, sp.pResolveAttachments[i].layout);
    }
    if (sp.pDepthStencilAttachment)
      if (auto* u = at(s, sp.pDepthStencilAttachment->attachment)) {
        const auto layout = sp.pDepthStencilAttachment->layout;
        u->add(depth_stages,
               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                   (read_only_depth(layout) ? 0 : VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT),
               layout);
      }
    if constexpr (std::is_same_v<Info, VkRenderPassCreateInfo2>)
      if (auto* ds = find_chain<VkSubpassDescriptionDepthStencilResolve>(
              sp.pNext, VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE);
          ds && ds->pDepthStencilResolveAttachment)
        // Depth/stencil resolves run in the color output stage as color writes.
        if (auto* u = at(s, ds->pDepthStencilResolveAttachment->attachment))
          u->add(color_stage | depth_stages,
                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 ds->pDepthStencilResolveAttachment->layout);
  }
  // Load operations run in the first subpass that uses an attachment, store
  // operations in the last one, whatever kind of use that is.
  for (std::uint32_t a = 0; a < attachments; ++a) {
    const auto& desc = info.pAttachments[a];
    const bool depth = depth_format(desc.format);
    const auto stages = depth ? depth_stages : color_stage;
    const auto read = depth ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT : VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    const auto write = depth ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    auto load = [&](VkAttachmentLoadOp op) -> VkAccessFlags {
      return op == VK_ATTACHMENT_LOAD_OP_LOAD ? read : op == VK_ATTACHMENT_LOAD_OP_NONE_KHR ? 0 : write;
    };
    auto store = [&](VkAttachmentStoreOp op) -> VkAccessFlags { return op == VK_ATTACHMENT_STORE_OP_NONE ? 0 : write; };
    VkAccessFlags first = load(desc.loadOp), last = store(desc.storeOp);
    if (depth) first |= load(desc.stencilLoadOp), last |= store(desc.stencilStoreOp);
    std::optional<std::uint32_t> first_use, last_use;
    for (std::uint32_t s = 0; s < subpasses; ++s)
      if (at(s, a)->used) {
        if (!first_use) first_use = s;
        last_use = s;
      }
    if (!first_use) continue;
    if (first) at(*first_use, a)->add(stages, first);
    if (last) at(*last_use, a)->add(stages, last);
  }

  std::vector<Dependency> out;
  auto covered = [&](std::uint32_t s, std::uint32_t t, const Use& from, const Use& to) {
    Scope have{};
    for (std::uint32_t i = 0; info.pDependencies && i < info.dependencyCount; ++i) {
      const auto& d = info.pDependencies[i];
      if (d.srcSubpass != s || d.dstSubpass != t) continue;
      const auto c = scope(d);
      have.src_stages |= c.src_stages, have.dst_stages |= c.dst_stages;
      have.src_accesses |= c.src_accesses, have.dst_accesses |= c.dst_accesses;
    }
    for (const auto& d : out)
      if (d.srcSubpass == s && d.dstSubpass == t) {
        have.src_stages |= d.srcStageMask, have.dst_stages |= d.dstStageMask;
        have.src_accesses |= d.srcAccessMask, have.dst_accesses |= d.dstAccessMask;
      }
    const VkAccessFlags src_writes = from.accesses & write_accesses;
    return (from.stages & ~have.src_stages) == 0 && (to.stages & ~have.dst_stages) == 0 &&
           (src_writes & ~have.src_accesses) == 0 && (to.accesses & ~have.dst_accesses) == 0;
  };
  for (std::uint32_t a = 0; a < attachments; ++a) {
    std::optional<std::uint32_t> previous;
    for (std::uint32_t t = 0; t < subpasses; ++t) {
      const Use& to = *at(t, a);
      if (!to.used) continue;
      if (previous) {
        const std::uint32_t s = *previous;
        const Use& from = *at(s, a);
        const bool transition = from.layout && to.layout && *from.layout != *to.layout;
        if ((from.writes() || to.writes() || transition) && !covered(s, t, from, to)) {
          auto it = std::find_if(out.begin(), out.end(),
                                 [&](const Dependency& d) { return d.srcSubpass == s && d.dstSubpass == t; });
          if (it == out.end()) {
            Dependency d{};
            if constexpr (std::is_same_v<Dependency, VkSubpassDependency2>)
              d.sType = VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2;
            d.srcSubpass = s, d.dstSubpass = t;
            d.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
            it = out.insert(out.end(), d);
          }
          it->srcStageMask |= from.stages;
          it->srcAccessMask |= from.accesses & write_accesses;
          it->dstStageMask |= to.stages;
          it->dstAccessMask |= to.accesses;
        }
      }
      previous = t;
    }
  }
  // Depth/stencil resolves write in COLOR_ATTACHMENT_OUTPUT as color writes
  // (VkSubpassDescriptionDepthStencilResolve), which guests order like depth
  // writes: their barriers before the pass reach only the fragment tests. The
  // resolve attachment's first subpass gets an incoming dependency from those
  // stages to the resolve, which chains with such a barrier.
  if constexpr (std::is_same_v<Info, VkRenderPassCreateInfo2>)
    for (std::uint32_t t = 0; t < subpasses; ++t) {
      auto* ds = find_chain<VkSubpassDescriptionDepthStencilResolve>(
          info.pSubpasses[t].pNext, VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE);
      if (!ds || !ds->pDepthStencilResolveAttachment) continue;
      const auto a = ds->pDepthStencilResolveAttachment->attachment;
      if (a >= attachments) continue;
      bool earlier = false;
      for (std::uint32_t s = 0; s < t; ++s) earlier |= at(s, a)->used;
      if (earlier) continue;
      constexpr VkPipelineStageFlags src_stages = depth_stages | color_stage;
      constexpr VkAccessFlags src_accesses = write_accesses;
      bool have = false;
      for (std::uint32_t i = 0; !have && info.pDependencies && i < info.dependencyCount; ++i) {
        const auto& d = info.pDependencies[i];
        if (d.srcSubpass != VK_SUBPASS_EXTERNAL || d.dstSubpass != t) continue;
        const auto c = scope(d);
        have = (c.src_stages & src_stages) == src_stages && (c.src_accesses & src_accesses) == src_accesses &&
               (c.dst_stages & color_stage) && (c.dst_accesses & VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
      }
      if (have) continue;
      Dependency d{};
      d.sType = VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2;
      d.srcSubpass = VK_SUBPASS_EXTERNAL, d.dstSubpass = t;
      d.srcStageMask = src_stages, d.srcAccessMask = src_accesses;
      d.dstStageMask = color_stage, d.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      out.push_back(d);
    }
  return out;
}
}  // namespace

std::vector<VkSubpassDependency> missing_subpass_dependencies(const VkRenderPassCreateInfo& info) {
  return missing<VkRenderPassCreateInfo, VkSubpassDependency>(info);
}
std::vector<VkSubpassDependency2> missing_subpass_dependencies(const VkRenderPassCreateInfo2& info) {
  return missing<VkRenderPassCreateInfo2, VkSubpassDependency2>(info);
}
}  // namespace venture::vulkan

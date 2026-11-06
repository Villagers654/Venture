#include "xr/sphere_images.h"
#include "common/log.h"
#include "gfx/gpu_device.h"
#include "gfx/queue_sync.h"
#include "hle/vulkan/timing.h"
#include "xr/queue.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <map>
#include <stdexcept>
#include <utility>
#include <vector>

namespace venture::xr {
namespace vertex {
#include "color_vert.inc"
}
namespace fragment {
#include "color_layer_frag.inc"
}
namespace {
void check(VkResult r, const char *operation) {
  if (r != VK_SUCCESS) throw std::runtime_error(operation);
}
constexpr std::uint32_t max_sources = 64;
} // namespace
struct SphereImages::Impl {
  XrGraphicsBindingVulkanKHR binding;
  PFN_vkGetInstanceProcAddr get;
  XrSwapchainCreateInfo info;
  LayerFrame *frame{};
  VkDescriptorSetLayout descriptor_layout{};
  VkPipelineLayout pipeline_layout{};
  VkPipeline pipeline{};
  VkRenderPass render_pass{};
  VkSampler sampler{};
  VkDescriptorPool descriptors{};
  bool failed = false;
  struct Source {
    VkImage image{};
    VkDeviceMemory memory{};
    VkImageView view{};
    VkDescriptorSet descriptor{};
    VkFormat format{};
    VkExtent2D extent{};
    std::uint32_t layers = 1;
    bool external = false;  // image owned elsewhere, in COLOR_ATTACHMENT_OPTIMAL between renders
    bool filled = false;    // copied into at least once
    bool readable = false;  // SHADER_READ_ONLY_OPTIMAL since its last fill
  };
  std::map<std::uintptr_t, Source> sources;
  struct Target {
    VkImage image{};
    std::array<VkImageView, 2> views{};
    std::array<VkFramebuffer, 2> framebuffers{};
  };
  std::vector<Target> targets;
  template <class T> T fn(const char *name) const { return reinterpret_cast<T>(get(binding.instance, name)); }
  // Recorded commands may still use these objects.
  void wait_all() { frame->idle(); }
  void destroy(Source &source) {
    if (source.descriptor)
      fn<PFN_vkFreeDescriptorSets>("vkFreeDescriptorSets")(binding.device, descriptors, 1, &source.descriptor);
    if (source.view) fn<PFN_vkDestroyImageView>("vkDestroyImageView")(binding.device, source.view, nullptr);
    if (source.image && !source.external) fn<PFN_vkDestroyImage>("vkDestroyImage")(binding.device, source.image, nullptr);
    if (source.memory) fn<PFN_vkFreeMemory>("vkFreeMemory")(binding.device, source.memory, nullptr);
    source = {};
  }
  ~Impl() {
    if (descriptor_layout) frame->idle();
    for (auto &[key, source] : sources) destroy(source);
    for (auto &target : targets) {
      for (auto framebuffer : target.framebuffers)
        if (framebuffer) fn<PFN_vkDestroyFramebuffer>("vkDestroyFramebuffer")(binding.device, framebuffer, nullptr);
      for (auto view : target.views)
        if (view) fn<PFN_vkDestroyImageView>("vkDestroyImageView")(binding.device, view, nullptr);
    }
    if (descriptors) fn<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool")(binding.device, descriptors, nullptr);
    if (pipeline) fn<PFN_vkDestroyPipeline>("vkDestroyPipeline")(binding.device, pipeline, nullptr);
    if (pipeline_layout)
      fn<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout")(binding.device, pipeline_layout, nullptr);
    if (descriptor_layout)
      fn<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout")(binding.device, descriptor_layout, nullptr);
    if (render_pass) fn<PFN_vkDestroyRenderPass>("vkDestroyRenderPass")(binding.device, render_pass, nullptr);
    if (sampler) fn<PFN_vkDestroySampler>("vkDestroySampler")(binding.device, sampler, nullptr);
  }
  void initialize() {
    if (descriptor_layout) return;
    VkDescriptorSetLayoutBinding db{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                                    nullptr};
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.bindingCount = 1;
    dl.pBindings = &db;
    check(fn<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(binding.device, &dl, nullptr,
                                                                             &descriptor_layout),
          "Sphere descriptor layout failed");
    VkPushConstantRange pc{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Constants)};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &descriptor_layout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pc;
    check(fn<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(binding.device, &pl, nullptr, &pipeline_layout),
          "Sphere pipeline layout failed");
    VkAttachmentDescription attachment{};
    attachment.format = VkFormat(info.format);
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount = 1;
    rp.pAttachments = &attachment;
    rp.subpassCount = 1;
    rp.pSubpasses = &sub;
    check(fn<PFN_vkCreateRenderPass>("vkCreateRenderPass")(binding.device, &rp, nullptr, &render_pass),
          "Sphere render pass failed");
    VkSamplerCreateInfo sc{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sc.magFilter = sc.minFilter = VK_FILTER_LINEAR;
    sc.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sc.addressModeU = sc.addressModeV = sc.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    check(fn<PFN_vkCreateSampler>("vkCreateSampler")(binding.device, &sc, nullptr, &sampler),
          "Sphere sampler failed");
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, max_sources};
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dp.maxSets = max_sources;
    dp.poolSizeCount = 1;
    dp.pPoolSizes = &size;
    check(fn<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(binding.device, &dp, nullptr, &descriptors),
          "Sphere descriptor pool failed");
    std::array<VkShaderModule, 2> modules{};
    auto shader = [&](std::span<const std::uint32_t> words, VkShaderModule &module) {
      VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
      sm.codeSize = words.size_bytes();
      sm.pCode = words.data();
      check(fn<PFN_vkCreateShaderModule>("vkCreateShaderModule")(binding.device, &sm, nullptr, &module),
            "Sphere shader failed");
    };
    try {
      shader(vertex::decode_shader, modules[0]);
      shader(fragment::decode_shader, modules[1]);
      std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
      for (unsigned i = 0; i < 2; ++i) {
        stages[i] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[i].stage = i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
        stages[i].module = modules[i];
        stages[i].pName = "main";
      }
      VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
      VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
      ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
      vp.viewportCount = vp.scissorCount = 1;
      VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
      rs.polygonMode = VK_POLYGON_MODE_FILL;
      rs.cullMode = VK_CULL_MODE_NONE;
      rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
      rs.lineWidth = 1;
      VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
      ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
      // The shader writes premultiplied color; later layers go over earlier ones.
      VkPipelineColorBlendAttachmentState ba{};
      ba.blendEnable = VK_TRUE;
      ba.srcColorBlendFactor = ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
      ba.dstColorBlendFactor = ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
      ba.colorBlendOp = ba.alphaBlendOp = VK_BLEND_OP_ADD;
      ba.colorWriteMask = 15;
      VkPipelineColorBlendStateCreateInfo bs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
      bs.attachmentCount = 1;
      bs.pAttachments = &ba;
      std::array<VkDynamicState, 2> dynamic{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
      VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
      dy.dynamicStateCount = 2;
      dy.pDynamicStates = dynamic.data();
      VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
      pi.stageCount = 2;
      pi.pStages = stages.data();
      pi.pVertexInputState = &vi;
      pi.pInputAssemblyState = &ia;
      pi.pViewportState = &vp;
      pi.pRasterizationState = &rs;
      pi.pMultisampleState = &ms;
      pi.pColorBlendState = &bs;
      pi.pDynamicState = &dy;
      pi.layout = pipeline_layout;
      pi.renderPass = render_pass;
      check(fn<PFN_vkCreateGraphicsPipelines>("vkCreateGraphicsPipelines")(binding.device, VK_NULL_HANDLE, 1, &pi,
                                                                           nullptr, &pipeline),
            "Sphere pipeline failed");
    } catch (...) {
      for (auto m : modules)
        if (m) fn<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(binding.device, m, nullptr);
      throw;
    }
    for (auto m : modules) fn<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(binding.device, m, nullptr);
    for (auto &target : targets) {
      for (std::uint32_t eye = 0; eye < 2; ++eye) {
        VkImageViewCreateInfo iv{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        iv.viewType = VK_IMAGE_VIEW_TYPE_2D;
        iv.format = VkFormat(info.format);
        iv.image = target.image;
        iv.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, eye, 1};
        check(fn<PFN_vkCreateImageView>("vkCreateImageView")(binding.device, &iv, nullptr, &target.views[eye]),
              "Sphere target view failed");
        VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fb.renderPass = render_pass;
        fb.attachmentCount = 1;
        fb.pAttachments = &target.views[eye];
        fb.width = info.width;
        fb.height = info.height;
        fb.layers = 1;
        check(fn<PFN_vkCreateFramebuffer>("vkCreateFramebuffer")(binding.device, &fb, nullptr,
                                                                 &target.framebuffers[eye]),
              "Sphere framebuffer failed");
      }
    }
  }
  void create(Source &source, VkFormat format, VkExtent2D extent, std::uint32_t layers) {
    source.format = format;
    source.extent = extent;
    source.layers = layers;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {extent.width, extent.height, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = layers;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(fn<PFN_vkCreateImage>("vkCreateImage")(binding.device, &ci, nullptr, &source.image),
          "Sphere source image failed");
    VkMemoryRequirements requirements{};
    fn<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(binding.device, source.image, &requirements);
    VkPhysicalDeviceMemoryProperties memory{};
    fn<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(binding.physicalDevice,
                                                                                        &memory);
    std::uint32_t type = UINT32_MAX;
    for (std::uint32_t i = 0; i < memory.memoryTypeCount && type == UINT32_MAX; ++i)
      if ((requirements.memoryTypeBits & (1u << i)) &&
          (memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
        type = i;
    if (type == UINT32_MAX) throw std::runtime_error("No sphere image memory type");
    VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ma.allocationSize = requirements.size;
    ma.memoryTypeIndex = type;
    check(fn<PFN_vkAllocateMemory>("vkAllocateMemory")(binding.device, &ma, nullptr, &source.memory),
          "Sphere memory failed");
    check(fn<PFN_vkBindImageMemory>("vkBindImageMemory")(binding.device, source.image, source.memory, 0),
          "Sphere memory binding failed");
    describe(source);
  }
  // The array view and descriptor layer.frag samples.
  void describe(Source &source) {
    VkImageViewCreateInfo iv{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    iv.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    iv.format = source.format;
    iv.image = source.image;
    iv.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, source.layers};
    check(fn<PFN_vkCreateImageView>("vkCreateImageView")(binding.device, &iv, nullptr, &source.view),
          "Sphere source view failed");
    VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    da.descriptorPool = descriptors;
    da.descriptorSetCount = 1;
    da.pSetLayouts = &descriptor_layout;
    check(fn<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")(binding.device, &da, &source.descriptor),
          "Sphere descriptor failed");
    VkDescriptorImageInfo di{sampler, source.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = source.descriptor;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &di;
    fn<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets")(binding.device, 1, &write, 0, nullptr);
  }
};
SphereImages::SphereImages(const XrGraphicsBindingVulkanKHR &binding, PFN_vkGetInstanceProcAddr get,
                           const XrSwapchainCreateInfo &target, std::span<const XrSwapchainImageVulkanKHR> images,
                           LayerFrame &frame)
    : impl_(std::make_unique<Impl>()) {
  impl_->binding = binding;
  impl_->get = get;
  impl_->info = target;
  impl_->info.next = nullptr;
  impl_->frame = &frame;
  for (const auto &image : images) impl_->targets.emplace_back().image = image.image;
}
SphereImages::~SphereImages() = default;
const XrSwapchainCreateInfo &SphereImages::target() const { return impl_->info; }
XrResult SphereImages::source(std::uintptr_t key, VkFormat format, VkExtent2D extent, VkImage &image, bool &fresh,
                              std::uint32_t layers) {
  auto &s = *impl_;
  if (s.failed) return XR_ERROR_RUNTIME_FAILURE;
  try {
    s.initialize();
    auto found = s.sources.find(key);
    if (found != s.sources.end() && (found->second.external || found->second.format != format ||
                                     found->second.extent.width != extent.width ||
                                     found->second.extent.height != extent.height || found->second.layers != layers)) {
      s.wait_all();
      s.destroy(found->second);
      s.sources.erase(found);
      found = s.sources.end();
    }
    if (found == s.sources.end()) {
      if (s.sources.size() >= max_sources) return XR_ERROR_LIMIT_REACHED;
      auto &created = s.sources[key];
      try {
        s.create(created, format, extent, layers);
      } catch (...) {
        s.destroy(created);
        s.sources.erase(key);
        throw;
      }
      found = s.sources.find(key);
    }
    fresh = !found->second.filled;
    image = found->second.image;
    return XR_SUCCESS;
  } catch (const std::exception &e) {
    s.failed = true;
    log::error("xr", "Sphere layer source failed: {}", e.what());
    return XR_ERROR_RUNTIME_FAILURE;
  }
}
void SphereImages::filled(std::uintptr_t key) {
  // The copy leaves it in COLOR_ATTACHMENT_OPTIMAL (GuestImages::copy).
  if (auto found = impl_->sources.find(key); found != impl_->sources.end())
    found->second.filled = true, found->second.readable = false;
}
XrResult SphereImages::external(std::uintptr_t key, VkImage image, VkFormat format, std::uint32_t layers) {
  auto &s = *impl_;
  if (s.failed) return XR_ERROR_RUNTIME_FAILURE;
  try {
    s.initialize();
    auto found = s.sources.find(key);
    if (found != s.sources.end() && found->second.external && found->second.image == image &&
        found->second.format == format && found->second.layers == layers) {
      found->second.readable = false;
      return XR_SUCCESS;
    }
    if (found != s.sources.end() || s.sources.size() >= max_sources) {
      // A changed key, or a full pool: views of images seen before go first.
      s.wait_all();
      if (found != s.sources.end()) {
        s.destroy(found->second);
        s.sources.erase(found);
      }
      for (auto it = s.sources.begin(); s.sources.size() >= max_sources && it != s.sources.end();) {
        if (!it->second.external) { ++it; continue; }
        s.destroy(it->second);
        it = s.sources.erase(it);
      }
      if (s.sources.size() >= max_sources) return XR_ERROR_LIMIT_REACHED;
    }
    auto &source = s.sources[key];
    source.image = image;
    source.format = format;
    source.layers = layers;
    source.external = true;
    try {
      s.describe(source);
    } catch (...) {
      s.destroy(source);
      s.sources.erase(key);
      throw;
    }
    source.filled = true;
    source.readable = false;
    return XR_SUCCESS;
  } catch (const std::exception &e) {
    s.failed = true;
    log::error("xr", "Drawn layer source failed: {}", e.what());
    return XR_ERROR_RUNTIME_FAILURE;
  }
}
void SphereImages::forget(std::span<const VkImage> images) {
  auto &s = *impl_;
  bool waited = false;
  for (auto it = s.sources.begin(); it != s.sources.end();) {
    if (!it->second.external || std::ranges::find(images, it->second.image) == images.end()) {
      ++it;
      continue;
    }
    if (!std::exchange(waited, true)) {
      try {
        s.wait_all();
      } catch (const std::exception &) {
        return;
      }
    }
    s.destroy(it->second);
    it = s.sources.erase(it);
  }
}
void SphereImages::retain(std::span<const std::uintptr_t> keys) {
  auto &s = *impl_;
  bool waited = false;
  for (auto it = s.sources.begin(); it != s.sources.end();) {
    if (std::ranges::find(keys, it->first) != keys.end()) {
      ++it;
      continue;
    }
    if (!std::exchange(waited, true)) {
      try {
        s.wait_all();
      } catch (const std::exception &) {
        return;
      }
    }
    s.destroy(it->second);
    it = s.sources.erase(it);
  }
}
XrResult SphereImages::record(VkCommandBuffer command, std::uint32_t index, std::span<const Draw> draws) {
  auto &s = *impl_;
  if (s.failed || !command || index >= s.targets.size()) return XR_ERROR_RUNTIME_FAILURE;
  try {
    s.initialize();
    auto &target = s.targets[index];
    for (const auto &draw : draws) {
      auto found = s.sources.find(draw.source);
      if (found == s.sources.end() || !found->second.filled || found->second.readable) continue;
      VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b.image = found->second.image;
      b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, found->second.layers};
      b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
      b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      s.fn<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                                                             nullptr, 1, &b);
      found->second.readable = true;
    }
    for (std::uint32_t eye = 0; eye < 2; ++eye) {
      VkClearValue clear{};
      VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
      rp.renderPass = s.render_pass;
      rp.framebuffer = target.framebuffers[eye];
      rp.renderArea = {{0, 0}, {s.info.width, s.info.height}};
      rp.clearValueCount = 1;
      rp.pClearValues = &clear;
      s.fn<PFN_vkCmdBeginRenderPass>("vkCmdBeginRenderPass")(command, &rp, VK_SUBPASS_CONTENTS_INLINE);
      s.fn<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(command, VK_PIPELINE_BIND_POINT_GRAPHICS, s.pipeline);
      VkViewport vp{0, 0, float(s.info.width), float(s.info.height), 0, 1};
      s.fn<PFN_vkCmdSetViewport>("vkCmdSetViewport")(command, 0, 1, &vp);
      for (const auto &draw : draws) {
        auto found = s.sources.find(draw.source);
        if (draw.eye != eye || found == s.sources.end() || !found->second.filled) continue;
        if (!draw.area.extent.width || !draw.area.extent.height) continue;
        // Only the pixels the layer can cover are shaded.
        s.fn<PFN_vkCmdSetScissor>("vkCmdSetScissor")(command, 0, 1, &draw.area);
        s.fn<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets")(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                                                     s.pipeline_layout, 0, 1,
                                                                     &found->second.descriptor, 0, nullptr);
        s.fn<PFN_vkCmdPushConstants>("vkCmdPushConstants")(command, s.pipeline_layout,
                                                           VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Constants),
                                                           draw.constants.data());
        s.fn<PFN_vkCmdDraw>("vkCmdDraw")(command, 3, 1, 0, 0);
      }
      s.fn<PFN_vkCmdEndRenderPass>("vkCmdEndRenderPass")(command);
    }
    // External images go back to the layout their owner expects.
    for (const auto &draw : draws) {
      auto found = s.sources.find(draw.source);
      if (found == s.sources.end() || !found->second.external || !found->second.readable) continue;
      VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      b.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b.image = found->second.image;
      b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, found->second.layers};
      b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
      b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
      s.fn<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(command, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                                                             nullptr, 1, &b);
      found->second.readable = false;
    }
    return XR_SUCCESS;
  } catch (const std::exception &e) {
    s.failed = true;
    log::error("xr", "Sphere layer drawing failed: {}", e.what());
    return XR_ERROR_RUNTIME_FAILURE;
  }
}
struct LayerFrame::Impl {
  XrGraphicsBindingVulkanKHR binding;
  PFN_vkGetInstanceProcAddr get;
  VkQueue queue{};
  VkCommandPool pool{};
  VkQueryPool queries{};
  double period = 0;
  std::uint64_t gpu_ns = 0;
  bool failed = false;
  struct Slot {
    VkCommandBuffer command{};
    VkFence fence{};
    bool pending = false, timed = false;
  };
  std::array<Slot, 3> slots{};
  std::uint32_t next = 0;
  bool recording = false;
  template <class T> T fn(const char *name) const { return reinterpret_cast<T>(get(binding.instance, name)); }
  void initialize() {
    if (pool) return;
    fn<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(binding.device, binding.queueFamilyIndex, binding.queueIndex, &queue);
    VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cp.queueFamilyIndex = binding.queueFamilyIndex;
    check(fn<PFN_vkCreateCommandPool>("vkCreateCommandPool")(binding.device, &cp, nullptr, &pool),
          "Layer command pool failed");
    for (auto &slot : slots) {
      VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
      ca.commandPool = pool;
      ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      ca.commandBufferCount = 1;
      check(fn<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(binding.device, &ca, &slot.command),
            "Layer command buffer failed");
      VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
      check(fn<PFN_vkCreateFence>("vkCreateFence")(binding.device, &fc, nullptr, &slot.fence), "Layer fence failed");
    }
    if (vulkan::timing_enabled()) {
      std::uint32_t n{};
      auto families =
          fn<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties");
      families(binding.physicalDevice, &n, nullptr);
      std::vector<VkQueueFamilyProperties> properties(n);
      families(binding.physicalDevice, &n, properties.data());
      if (binding.queueFamilyIndex < n && properties[binding.queueFamilyIndex].timestampValidBits) {
        VkPhysicalDeviceProperties device{};
        fn<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(binding.physicalDevice, &device);
        period = device.limits.timestampPeriod;
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = 2 * std::uint32_t(slots.size());
        if (fn<PFN_vkCreateQueryPool>("vkCreateQueryPool")(binding.device, &qi, nullptr, &queries) != VK_SUCCESS)
          queries = VK_NULL_HANDLE;
      }
    }
  }
  void wait(Slot &slot) {
    if (!slot.pending) return;
    check(fn<PFN_vkWaitForFences>("vkWaitForFences")(binding.device, 1, &slot.fence, VK_TRUE, UINT64_MAX),
          "Layer wait failed");
    slot.pending = false;
    if (slot.timed) {
      slot.timed = false;
      std::uint64_t ticks[2]{};
      const auto index = std::uint32_t(&slot - slots.data());
      if (fn<PFN_vkGetQueryPoolResults>("vkGetQueryPoolResults")(binding.device, queries, 2 * index, 2, sizeof(ticks),
                                                                 ticks, sizeof(ticks[0]),
                                                                 VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
        vulkan::XrCopySample sample{"layer_draw", vulkan::timing_frame(), 0, 0, 0, 0, 0,
                                    std::uint64_t(double(ticks[0]) * period), std::uint64_t(double(ticks[1]) * period)};
        gpu_ns = sample.vk_end_ns - sample.vk_begin_ns;
        vulkan::timing_xr_copy(sample);
      }
    }
  }
  ~Impl() {
    for (auto &slot : slots)
      if (slot.pending) fn<PFN_vkWaitForFences>("vkWaitForFences")(binding.device, 1, &slot.fence, VK_TRUE, UINT64_MAX);
    for (auto &slot : slots)
      if (slot.fence) fn<PFN_vkDestroyFence>("vkDestroyFence")(binding.device, slot.fence, nullptr);
    if (queries) fn<PFN_vkDestroyQueryPool>("vkDestroyQueryPool")(binding.device, queries, nullptr);
    if (pool) fn<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(binding.device, pool, nullptr);
  }
};
LayerFrame::LayerFrame(const XrGraphicsBindingVulkanKHR &binding, PFN_vkGetInstanceProcAddr get)
    : impl_(std::make_unique<Impl>()) {
  impl_->binding = binding;
  impl_->get = get;
}
LayerFrame::~LayerFrame() = default;
std::uint64_t LayerFrame::last_gpu_ns() const { return impl_->gpu_ns; }
VkCommandBuffer LayerFrame::begin() {
  auto &f = *impl_;
  if (f.failed) return VK_NULL_HANDLE;
  try {
    f.initialize();
    auto &slot = f.slots[f.next];
    if (f.recording) return slot.command;
    f.wait(slot);
    check(f.fn<PFN_vkResetFences>("vkResetFences")(f.binding.device, 1, &slot.fence), "Layer fence reset failed");
    check(f.fn<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")(slot.command, 0), "Layer command reset failed");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(f.fn<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(slot.command, &bi), "Layer command begin failed");
    if (f.queries) {
      f.fn<PFN_vkCmdResetQueryPool>("vkCmdResetQueryPool")(slot.command, f.queries, 2 * f.next, 2);
      f.fn<PFN_vkCmdWriteTimestamp>("vkCmdWriteTimestamp")(slot.command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                                           f.queries, 2 * f.next);
    }
    f.recording = true;
    return slot.command;
  } catch (const std::exception &e) {
    f.failed = true;
    log::error("xr", "Drawn layer frame failed: {}", e.what());
    return VK_NULL_HANDLE;
  }
}
XrResult LayerFrame::submit() {
  auto &f = *impl_;
  if (!f.recording) return XR_SUCCESS;
  auto &slot = f.slots[f.next];
  f.recording = false;
  try {
    if (f.queries)
      f.fn<PFN_vkCmdWriteTimestamp>("vkCmdWriteTimestamp")(slot.command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                                           f.queries, 2 * f.next + 1);
    check(f.fn<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(slot.command), "Layer command end failed");
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &slot.command;
    std::lock_guard queue_lock(*gfx::queue_mutex(f.queue));
    HostQueueLock angle_queue(f.queue == gfx::gpu_device().queue);
    if (!angle_queue) throw std::runtime_error("Layer queue ownership failed");
    check(f.fn<PFN_vkQueueSubmit>("vkQueueSubmit")(f.queue, 1, &si, slot.fence), "Layer submit failed");
    slot.pending = true;
    slot.timed = f.queries != VK_NULL_HANDLE;
    f.next = (f.next + 1) % std::uint32_t(f.slots.size());
    return XR_SUCCESS;
  } catch (const std::exception &e) {
    f.failed = true;
    log::error("xr", "Drawn layer submission failed: {}", e.what());
    return XR_ERROR_RUNTIME_FAILURE;
  }
}
void LayerFrame::abandon() {
  auto &f = *impl_;
  if (!std::exchange(f.recording, false)) return;
  if (f.fn<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")(f.slots[f.next].command, 0) != VK_SUCCESS) f.failed = true;
}
void LayerFrame::idle() {
  auto &f = *impl_;
  try {
    for (auto &slot : f.slots) f.wait(slot);
  } catch (const std::exception &e) {
    f.failed = true;
    log::error("xr", "Drawn layer wait failed: {}", e.what());
  }
}
namespace {
using Matrix4 = std::array<std::array<float, 4>, 4>;
Matrix4 matrix(const XrPosef &pose) {
  const auto &q = pose.orientation;
  return {{{1 - 2 * (q.y * q.y + q.z * q.z), 2 * (q.x * q.y - q.z * q.w), 2 * (q.x * q.z + q.y * q.w), pose.position.x},
           {2 * (q.x * q.y + q.z * q.w), 1 - 2 * (q.x * q.x + q.z * q.z), 2 * (q.y * q.z - q.x * q.w), pose.position.y},
           {2 * (q.x * q.z - q.y * q.w), 2 * (q.y * q.z + q.x * q.w), 1 - 2 * (q.x * q.x + q.y * q.y), pose.position.z},
           {0, 0, 0, 1}}};
}
Matrix4 multiply(const Matrix4 &a, const Matrix4 &b) {
  Matrix4 r{};
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j)
      for (int k = 0; k < 4; ++k) r[i][j] += a[i][k] * b[k][j];
  return r;
}
XrQuaternionf normalized(XrQuaternionf q) {
  const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  if (!(n > 0) || !std::isfinite(n)) return {0, 0, 0, 1};
  return {q.x / n, q.y / n, q.z / n, q.w / n};
}
// IEEE half, rounded to nearest (packHalf2x16's layout: `low` in bits 0-15).
std::uint32_t half(float value) {
  const auto bits = std::bit_cast<std::uint32_t>(value);
  const std::uint32_t sign = (bits >> 16) & 0x8000;
  const int exponent = int((bits >> 23) & 0xff) - 127 + 15;
  std::uint32_t mantissa = bits & 0x7fffff;
  if (((bits >> 23) & 0xff) == 0xff) return sign | 0x7c00 | (mantissa ? 0x200 : 0);
  if (exponent >= 31) return sign | 0x7c00;
  if (exponent <= 0) {
    if (exponent < -10) return sign;
    mantissa |= 0x800000;
    const int shift = 14 - exponent;
    std::uint32_t h = mantissa >> shift;
    if ((mantissa >> (shift - 1)) & 1) ++h;
    return sign | h;
  }
  std::uint32_t h = sign | (std::uint32_t(exponent) << 10) | (mantissa >> 13);
  if (mantissa & 0x1000) ++h;  // a carry moves into the exponent, as it should
  return h;
}
std::uint32_t halves(float low, float high) { return half(low) | (half(high) << 16); }
XrPosef inverse(const XrPosef &p) {
  const XrQuaternionf q{-p.orientation.x, -p.orientation.y, -p.orientation.z, p.orientation.w};
  auto m = matrix({q, {0, 0, 0}});
  return {q, {-(m[0][0] * p.position.x + m[0][1] * p.position.y + m[0][2] * p.position.z),
              -(m[1][0] * p.position.x + m[1][1] * p.position.y + m[1][2] * p.position.z),
              -(m[2][0] * p.position.x + m[2][1] * p.position.y + m[2][2] * p.position.z)}};
}
} // namespace
SphereImages::Constants layer_constants(const LayerPlacement &layer, const XrView &view, XrExtent2Di extent) {
  SphereImages::Constants c{};
  XrPosef frame = layer.frame, eye = view.pose;
  frame.orientation = normalized(frame.orientation);
  eye.orientation = normalized(eye.orientation);
  // The eye in the layer frame, then a pixel (x, y, 1) to its ray (x, y, -1) there.
  const auto e = multiply(matrix(inverse(frame)), matrix(eye));
  const float left = std::tan(view.fov.angleLeft), up = std::tan(view.fov.angleUp);
  const float dx = (std::tan(view.fov.angleRight) - left) / float(extent.width);
  const float dy = (std::tan(view.fov.angleDown) - up) / float(extent.height);
  const Matrix4 pixel{{{dx, 0, left, 0}, {0, dy, up, 0}, {0, 0, -1, 0}, {0, 0, 0, 1}}};
  Matrix4 local{};
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) local[i][j] = layer.local[i][j];
  Matrix4 a{};
  float map[2][3];
  std::copy(&layer.map[0][0], &layer.map[0][0] + 6, &map[0][0]);
  if (layer.shape == LayerShape::plane) {
    // VrApi maps the frame's ray (tx, ty, -1, 1) to (S, T, Q). For the eye's
    // ray d = R r that is (M3 d + m3 w) / w with w = -d.z; scaled by w > 0:
    // B r with B = M3 R - m3 R2, and w = -R2 r.
    float b[3][3];
    for (int i = 0; i < 3; ++i)
      for (int j = 0; j < 3; ++j) {
        b[i][j] = -local[i][3] * e[2][j];
        for (int k = 0; k < 3; ++k) b[i][j] += local[i][k] * e[k][j];
      }
    // The texture matrix works on (S, T, Q) rows alike: (u, v) = map (S/Q, T/Q, 1).
    float rows[4][3];
    for (int i = 0; i < 2; ++i)
      for (int j = 0; j < 3; ++j) rows[i][j] = map[i][0] * b[0][j] + map[i][1] * b[1][j] + map[i][2] * b[2][j];
    for (int j = 0; j < 3; ++j) rows[2][j] = b[2][j], rows[3][j] = -e[2][j];
    for (int i = 0; i < 4; ++i)
      for (int j = 0; j < 3; ++j) a[i][j] = rows[i][0] * pixel[0][j] + rows[i][1] * pixel[1][j] + rows[i][2] * pixel[2][j];
    map[0][0] = 1, map[0][1] = 0, map[0][2] = 0;
    map[1][0] = 0, map[1][1] = 1, map[1][2] = 0;
  } else {
    a = multiply(multiply(local, e), pixel);
  }
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) c[i * 4 + j] = a[i][j];
  c[16] = map[0][0], c[17] = map[0][1], c[18] = map[0][2], c[19] = map[1][0];
  c[20] = map[1][1], c[21] = map[1][2], c[22] = layer.clip[0], c[23] = layer.clip[1];
  c[24] = layer.clip[2], c[25] = layer.clip[3];
  c[26] = float(std::uint32_t(layer.shape) + 8 * layer.source_layer);
  c[27] = float(layer.source_factor + 8 * layer.destination_factor);
  c[28] = std::bit_cast<float>(halves(layer.scale.r, layer.scale.g));
  c[29] = std::bit_cast<float>(halves(layer.scale.b, layer.scale.a));
  c[30] = std::bit_cast<float>(halves(layer.bias.r, layer.bias.g));
  c[31] = std::bit_cast<float>(halves(layer.bias.b, layer.bias.a));
  return c;
}
namespace {
// The inverse of a 4x4 matrix (false when singular).
bool invert(const Matrix4 &m, Matrix4 &out) {
  float a[4][8];
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 8; ++j) a[i][j] = j < 4 ? m[i][j] : (j - 4 == i ? 1.f : 0.f);
  for (int c = 0; c < 4; ++c) {
    int pivot = c;
    for (int r = c + 1; r < 4; ++r)
      if (std::abs(a[r][c]) > std::abs(a[pivot][c])) pivot = r;
    if (!(std::abs(a[pivot][c]) > 1e-12f)) return false;
    if (pivot != c)
      for (int j = 0; j < 8; ++j) std::swap(a[c][j], a[pivot][j]);
    const float d = a[c][c];
    for (int j = 0; j < 8; ++j) a[c][j] /= d;
    for (int r = 0; r < 4; ++r) {
      if (r == c) continue;
      const float f = a[r][c];
      for (int j = 0; j < 8; ++j) a[r][j] -= f * a[c][j];
    }
  }
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) out[i][j] = a[i][j + 4];
  return std::isfinite(out[0][0]);
}
} // namespace
VkRect2D layer_bounds(const LayerPlacement &layer, const XrView &view, XrExtent2Di extent) {
  const VkRect2D whole{{0, 0}, {std::uint32_t(extent.width), std::uint32_t(extent.height)}};
  if (layer.shape == LayerShape::cube) return whole;
  // Shape space to pixels: the inverse of layer_constants' matrix, whose
  // input is the pixel (x, y, 1, w).
  const auto c = layer_constants(layer, view, extent);
  Matrix4 a{}, inverse_a{};
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) a[i][j] = c[i * 4 + j];
  float x0 = float(extent.width), y0 = float(extent.height), x1 = 0, y1 = 0;
  // A pixel position from homogeneous (x, y, z): in front only when z > 0.
  auto add = [&](float x, float y, float z) {
    if (!(z > 1e-6f)) return false;
    x /= z, y /= z;
    if (!std::isfinite(x) || !std::isfinite(y)) return false;
    x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y);
    return true;
  };
  constexpr int steps = 24;
  // The sampled rect's outline in source coordinates, edge by edge.
  auto outline = [&](auto &&visit) {
    const float u[4] = {layer.clip[0], layer.clip[2], layer.clip[2], layer.clip[0]};
    const float v[4] = {layer.clip[1], layer.clip[1], layer.clip[3], layer.clip[3]};
    for (int e = 0; e < 4; ++e)
      for (int k = 0; k < steps; ++k) {
        const float t = float(k) / steps;
        if (!visit(u[e] + (u[(e + 1) % 4] - u[e]) * t, v[e] + (v[(e + 1) % 4] - v[e]) * t)) return false;
      }
    return true;
  };
  if (layer.shape == LayerShape::plane) {
    // (S, T, Q) rows of pixel (x, y, 1): (u, v, 1) back through their inverse;
    // a plane's outline is straight, its corners bound it.
    Matrix4 rows{};
    for (int i = 0; i < 3; ++i)
      for (int j = 0; j < 3; ++j) rows[i][j] = a[i][j];
    rows[3][3] = 1;
    if (!invert(rows, inverse_a)) return whole;
    const float u[4] = {layer.clip[0], layer.clip[2], layer.clip[2], layer.clip[0]};
    const float v[4] = {layer.clip[1], layer.clip[1], layer.clip[3], layer.clip[3]};
    for (int k = 0; k < 4; ++k) {
      const float p[3] = {u[k], v[k], 1};
      float q[3]{};
      for (int i = 0; i < 3; ++i) q[i] = inverse_a[i][0] * p[0] + inverse_a[i][1] * p[1] + inverse_a[i][2] * p[2];
      // q is the pixel (x, y, 1) times s; s > 0 and the front row Q' > 0
      // there, or the corner is not in view.
      const float front = a[3][0] * q[0] + a[3][1] * q[1] + a[3][2] * q[2];
      if (!(front > 0) || !add(q[0], q[1], q[2])) return whole;
    }
  } else {
    if (!invert(a, inverse_a)) return whole;
    // (a, b) of a source point: the inverse of the 2x3 map.
    const float det = layer.map[0][0] * layer.map[1][1] - layer.map[0][1] * layer.map[1][0];
    if (!(std::abs(det) > 1e-12f)) return whole;
    const bool sphere = layer.shape != LayerShape::cylinder;
    const bool far = layer.shape == LayerShape::far_sphere;
    if (!far) {
      // From outside its surface the layer has silhouettes: no cheap bound.
      const float o[4] = {a[0][3], a[1][3], a[2][3], a[3][3]};
      if (!(o[3] > 0)) return whole;
      const float ox = o[0] / o[3], oy = o[1] / o[3], oz = o[2] / o[3];
      if (ox * ox + oz * oz + (sphere ? oy * oy : 0) >= 1) return whole;
    }
    // The rect's corners in (a, b): at most one turn, a sphere's latitudes on it.
    float a_min = 1e30f, a_max = -1e30f, b_min = 1e30f, b_max = -1e30f;
    for (float u : {layer.clip[0], layer.clip[2]})
      for (float v : {layer.clip[1], layer.clip[3]}) {
        const float du = u - layer.map[0][2], dv = v - layer.map[1][2];
        const float sa = (layer.map[1][1] * du - layer.map[0][1] * dv) / det;
        const float sb = (layer.map[0][0] * dv - layer.map[1][0] * du) / det;
        a_min = std::min(a_min, sa), a_max = std::max(a_max, sa), b_min = std::min(b_min, sb), b_max = std::max(b_max, sb);
      }
    constexpr float pi = 3.14159265358979f;
    if (a_max - a_min > 2 * pi || (sphere && (b_min < -pi / 2 || b_max > pi / 2))) return whole;
    const bool inside = outline([&](float u, float v) {
      u -= layer.map[0][2], v -= layer.map[1][2];
      const float sa = (layer.map[1][1] * u - layer.map[0][1] * v) / det;
      const float sb = (layer.map[0][0] * v - layer.map[1][0] * u) / det;
      float p[4];
      if (sphere) {
        const float cb = std::cos(sb);
        p[0] = cb * std::sin(sa), p[1] = std::sin(sb), p[2] = -cb * std::cos(sa);
      } else {
        p[0] = std::sin(sa), p[1] = sb, p[2] = -std::cos(sa);
      }
      p[3] = far ? 0.f : 1.f;
      float q[4]{};
      for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) q[i] += inverse_a[i][j] * p[j];
      // The eye-frame point is (dx x + l z, dy y + u z, -z, w): in front when
      // z / w > 0, at pixel (x / z, y / z). A direction has w = 0.
      if (!far) {
        if (q[3] == 0) return false;
        if (q[3] < 0) q[0] = -q[0], q[1] = -q[1], q[2] = -q[2];
      }
      return add(q[0], q[1], q[2]);
    });
    if (!inside) return whole;
    // Curved edges bow out between samples: a margin of 2% of the view.
    const float mx = 0.02f * float(extent.width), my = 0.02f * float(extent.height);
    x0 -= mx, x1 += mx, y0 -= my, y1 += my;
  }
  x0 = std::max(std::floor(x0) - 1, 0.f), y0 = std::max(std::floor(y0) - 1, 0.f);
  x1 = std::min(std::ceil(x1) + 1, float(extent.width)), y1 = std::min(std::ceil(y1) + 1, float(extent.height));
  if (x1 <= x0 || y1 <= y0) return {};
  return {{std::int32_t(x0), std::int32_t(y0)}, {std::uint32_t(x1 - x0), std::uint32_t(y1 - y0)}};
}
} // namespace venture::xr

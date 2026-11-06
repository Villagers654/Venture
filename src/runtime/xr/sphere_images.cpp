#include "xr/sphere_images.h"
#include "common/log.h"
#include "gfx/gpu_device.h"
#include "gfx/queue_sync.h"
#include "xr/queue.h"
#include <algorithm>
#include <map>
#include <stdexcept>
#include <vector>

namespace venture::xr {
namespace vertex {
#include "color_vert.inc"
}
namespace fragment {
#include "color_sphere_frag.inc"
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
  VkQueue queue{};
  VkCommandPool pool{};
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
    bool filled = false;    // copied into at least once
    bool readable = false;  // SHADER_READ_ONLY_OPTIMAL since its last fill
  };
  std::map<std::uintptr_t, Source> sources;
  struct Target {
    VkImage image{};
    std::array<VkImageView, 2> views{};
    std::array<VkFramebuffer, 2> framebuffers{};
    VkCommandBuffer command{};
    VkFence fence{};
    bool pending = false;
  };
  std::vector<Target> targets;
  template <class T> T fn(const char *name) const { return reinterpret_cast<T>(get(binding.instance, name)); }
  void wait(Target &target) {
    if (!target.pending) return;
    check(fn<PFN_vkWaitForFences>("vkWaitForFences")(binding.device, 1, &target.fence, VK_TRUE, UINT64_MAX),
          "Sphere wait failed");
    target.pending = false;
  }
  void wait_all() {
    for (auto &target : targets) wait(target);
  }
  void destroy(Source &source) {
    if (source.descriptor)
      fn<PFN_vkFreeDescriptorSets>("vkFreeDescriptorSets")(binding.device, descriptors, 1, &source.descriptor);
    if (source.view) fn<PFN_vkDestroyImageView>("vkDestroyImageView")(binding.device, source.view, nullptr);
    if (source.image) fn<PFN_vkDestroyImage>("vkDestroyImage")(binding.device, source.image, nullptr);
    if (source.memory) fn<PFN_vkFreeMemory>("vkFreeMemory")(binding.device, source.memory, nullptr);
    source = {};
  }
  ~Impl() {
    for (auto &target : targets)
      if (target.pending)
        fn<PFN_vkWaitForFences>("vkWaitForFences")(binding.device, 1, &target.fence, VK_TRUE, UINT64_MAX);
    for (auto &[key, source] : sources) destroy(source);
    for (auto &target : targets) {
      for (auto framebuffer : target.framebuffers)
        if (framebuffer) fn<PFN_vkDestroyFramebuffer>("vkDestroyFramebuffer")(binding.device, framebuffer, nullptr);
      for (auto view : target.views)
        if (view) fn<PFN_vkDestroyImageView>("vkDestroyImageView")(binding.device, view, nullptr);
      if (target.fence) fn<PFN_vkDestroyFence>("vkDestroyFence")(binding.device, target.fence, nullptr);
    }
    if (descriptors) fn<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool")(binding.device, descriptors, nullptr);
    if (pipeline) fn<PFN_vkDestroyPipeline>("vkDestroyPipeline")(binding.device, pipeline, nullptr);
    if (pipeline_layout)
      fn<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout")(binding.device, pipeline_layout, nullptr);
    if (descriptor_layout)
      fn<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout")(binding.device, descriptor_layout, nullptr);
    if (render_pass) fn<PFN_vkDestroyRenderPass>("vkDestroyRenderPass")(binding.device, render_pass, nullptr);
    if (sampler) fn<PFN_vkDestroySampler>("vkDestroySampler")(binding.device, sampler, nullptr);
    if (pool) fn<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(binding.device, pool, nullptr);
  }
  void initialize() {
    if (pool) return;
    VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cp.queueFamilyIndex = binding.queueFamilyIndex;
    check(fn<PFN_vkCreateCommandPool>("vkCreateCommandPool")(binding.device, &cp, nullptr, &pool),
          "Sphere command pool failed");
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
      VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
      ca.commandPool = pool;
      ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      ca.commandBufferCount = 1;
      check(fn<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(binding.device, &ca, &target.command),
            "Sphere command buffer failed");
      VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
      check(fn<PFN_vkCreateFence>("vkCreateFence")(binding.device, &fc, nullptr, &target.fence), "Sphere fence failed");
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
  void create(Source &source, VkFormat format, VkExtent2D extent) {
    source.format = format;
    source.extent = extent;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {extent.width, extent.height, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
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
    VkImageViewCreateInfo iv{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    iv.viewType = VK_IMAGE_VIEW_TYPE_2D;
    iv.format = format;
    iv.image = source.image;
    iv.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
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
                           const XrSwapchainCreateInfo &target, std::span<const XrSwapchainImageVulkanKHR> images)
    : impl_(std::make_unique<Impl>()) {
  impl_->binding = binding;
  impl_->get = get;
  impl_->info = target;
  impl_->info.next = nullptr;
  impl_->fn<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(binding.device, binding.queueFamilyIndex, binding.queueIndex,
                                                      &impl_->queue);
  for (const auto &image : images) impl_->targets.emplace_back().image = image.image;
}
SphereImages::~SphereImages() = default;
const XrSwapchainCreateInfo &SphereImages::target() const { return impl_->info; }
XrResult SphereImages::source(std::uintptr_t key, VkFormat format, VkExtent2D extent, VkImage &image, bool &fresh) {
  auto &s = *impl_;
  if (s.failed) return XR_ERROR_RUNTIME_FAILURE;
  try {
    s.initialize();
    auto found = s.sources.find(key);
    if (found != s.sources.end() && (found->second.format != format || found->second.extent.width != extent.width ||
                                     found->second.extent.height != extent.height)) {
      s.wait_all();
      s.destroy(found->second);
      s.sources.erase(found);
      found = s.sources.end();
    }
    if (found == s.sources.end()) {
      if (s.sources.size() >= max_sources) return XR_ERROR_LIMIT_REACHED;
      auto &created = s.sources[key];
      try {
        s.create(created, format, extent);
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
XrResult SphereImages::render(std::uint32_t index, std::span<const Draw> draws) {
  auto &s = *impl_;
  if (s.failed || index >= s.targets.size()) return XR_ERROR_RUNTIME_FAILURE;
  try {
    s.initialize();
    auto &target = s.targets[index];
    s.wait(target);
    check(s.fn<PFN_vkResetFences>("vkResetFences")(s.binding.device, 1, &target.fence), "Sphere fence reset failed");
    check(s.fn<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")(target.command, 0), "Sphere command reset failed");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(s.fn<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(target.command, &bi), "Sphere command begin failed");
    for (const auto &draw : draws) {
      auto found = s.sources.find(draw.source);
      if (found == s.sources.end() || !found->second.filled || found->second.readable) continue;
      VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b.image = found->second.image;
      b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
      b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      s.fn<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(target.command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
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
      s.fn<PFN_vkCmdBeginRenderPass>("vkCmdBeginRenderPass")(target.command, &rp, VK_SUBPASS_CONTENTS_INLINE);
      s.fn<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(target.command, VK_PIPELINE_BIND_POINT_GRAPHICS, s.pipeline);
      VkViewport vp{0, 0, float(s.info.width), float(s.info.height), 0, 1};
      VkRect2D sc{{0, 0}, {s.info.width, s.info.height}};
      s.fn<PFN_vkCmdSetViewport>("vkCmdSetViewport")(target.command, 0, 1, &vp);
      s.fn<PFN_vkCmdSetScissor>("vkCmdSetScissor")(target.command, 0, 1, &sc);
      for (const auto &draw : draws) {
        auto found = s.sources.find(draw.source);
        if (draw.eye != eye || found == s.sources.end() || !found->second.filled) continue;
        s.fn<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets")(target.command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                                                     s.pipeline_layout, 0, 1,
                                                                     &found->second.descriptor, 0, nullptr);
        s.fn<PFN_vkCmdPushConstants>("vkCmdPushConstants")(target.command, s.pipeline_layout,
                                                           VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Constants),
                                                           draw.constants.data());
        s.fn<PFN_vkCmdDraw>("vkCmdDraw")(target.command, 3, 1, 0, 0);
      }
      s.fn<PFN_vkCmdEndRenderPass>("vkCmdEndRenderPass")(target.command);
    }
    check(s.fn<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(target.command), "Sphere command end failed");
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &target.command;
    std::lock_guard queue_lock(*gfx::queue_mutex(s.queue));
    HostQueueLock angle_queue(s.queue == gfx::gpu_device().queue);
    if (!angle_queue) throw std::runtime_error("Sphere queue ownership failed");
    check(s.fn<PFN_vkQueueSubmit>("vkQueueSubmit")(s.queue, 1, &si, target.fence), "Sphere submit failed");
    target.pending = true;
    return XR_SUCCESS;
  } catch (const std::exception &e) {
    s.failed = true;
    log::error("xr", "Sphere layer drawing failed: {}", e.what());
    return XR_ERROR_RUNTIME_FAILURE;
  }
}
} // namespace venture::xr

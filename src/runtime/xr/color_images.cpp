#include "xr/color_images.h"
#include "common/log.h"
#include "core/wait.h"
#include "gfx/queue_sync.h"
#include "hle/vulkan/timing.h"
#include "xr/queue.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace venture::xr {
namespace vertex {
#include "color_vert.inc"
}
namespace fragment {
#include "color_frag.inc"
}
namespace multisample {
#include "color_ms_frag.inc"
}
namespace {
void check(VkResult r, const char *operation) {
  if (r != VK_SUCCESS)
    throw std::runtime_error(operation);
}
} // namespace
struct ColorImages::Impl {
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
  VkQueryPool queries{};
  double timestamp_period = 0;
  unsigned timestamp_bits = 0;
  bool failed = false;
  bool binding_queue_ordered = false;
  struct Subresource {
    VkImageView source{}, target{};
    VkFramebuffer framebuffer{};
    VkDescriptorSet descriptor{};
  };
  struct Image {
    VkImage target{}, source{};
    VkDeviceMemory memory{};
    VkCommandBuffer command{};
    VkFence fence{};
    std::vector<Subresource> subresources;
    bool initialized = false, converted = false, pending = false;
    bool owns_source = false, source_initialized = false;
    bool timing_pending = false;
    std::uint64_t frame = 0;
  };
  std::vector<Image> images;
  template <class T> T fn(const char *name) const {
    return reinterpret_cast<T>(get(binding.instance, name));
  }
  std::uint32_t layers() const { return info.arraySize * info.faceCount; }
  void wait(Image &image, const char* reason = "xr_color_reuse") {
    if (!image.pending)
      return;
    const auto start = vulkan::timing_enabled() ? monotonic_ns() : 0;
    auto result = fn<PFN_vkWaitForFences>("vkWaitForFences")(
              binding.device, 1, &image.fence, VK_TRUE, UINT64_MAX);
    if (start) vulkan::timing_host_wait(reason, monotonic_ns() - start);
    check(result, "Color wait failed");
    image.pending = false;
  }
  ~Impl() {
    for (auto &image : images) {
      if (image.pending)
        fn<PFN_vkWaitForFences>("vkWaitForFences")(
            binding.device, 1, &image.fence, VK_TRUE, UINT64_MAX);
      for (auto &sub : image.subresources) {
        if (sub.framebuffer)
          fn<PFN_vkDestroyFramebuffer>("vkDestroyFramebuffer")(
              binding.device, sub.framebuffer, nullptr);
        if (sub.source)
          fn<PFN_vkDestroyImageView>("vkDestroyImageView")(binding.device,
                                                           sub.source, nullptr);
        if (sub.target)
          fn<PFN_vkDestroyImageView>("vkDestroyImageView")(binding.device,
                                                           sub.target, nullptr);
      }
      if (image.fence)
        fn<PFN_vkDestroyFence>("vkDestroyFence")(binding.device, image.fence,
                                                 nullptr);
      if (image.source && image.owns_source)
        fn<PFN_vkDestroyImage>("vkDestroyImage")(binding.device, image.source,
                                                 nullptr);
      if (image.memory)
        fn<PFN_vkFreeMemory>("vkFreeMemory")(binding.device, image.memory,
                                             nullptr);
    }
    if (queries)
      fn<PFN_vkDestroyQueryPool>("vkDestroyQueryPool")(binding.device, queries,
                                                       nullptr);
    if (descriptors)
      fn<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool")(
          binding.device, descriptors, nullptr);
    if (pipeline)
      fn<PFN_vkDestroyPipeline>("vkDestroyPipeline")(binding.device, pipeline,
                                                     nullptr);
    if (pipeline_layout)
      fn<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout")(
          binding.device, pipeline_layout, nullptr);
    if (descriptor_layout)
      fn<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout")(
          binding.device, descriptor_layout, nullptr);
    if (render_pass)
      fn<PFN_vkDestroyRenderPass>("vkDestroyRenderPass")(binding.device,
                                                         render_pass, nullptr);
    if (sampler)
      fn<PFN_vkDestroySampler>("vkDestroySampler")(binding.device, sampler,
                                                   nullptr);
    if (pool)
      fn<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(binding.device, pool,
                                                           nullptr);
  }
  void initialize() {
    if (pool)
      return;
    VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cp.queueFamilyIndex = binding.queueFamilyIndex;
    check(fn<PFN_vkCreateCommandPool>("vkCreateCommandPool")(
              binding.device, &cp, nullptr, &pool),
          "Color command pool failed");
    VkDescriptorSetLayoutBinding db{0,
                                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                    1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dl{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.bindingCount = 1;
    dl.pBindings = &db;
    check(fn<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(
              binding.device, &dl, nullptr, &descriptor_layout),
          "Color descriptor layout failed");
    VkPushConstantRange pc{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 48};
    VkPipelineLayoutCreateInfo pl{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &descriptor_layout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pc;
    check(fn<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(
              binding.device, &pl, nullptr, &pipeline_layout),
          "Color pipeline layout failed");
    VkAttachmentDescription attachment{};
    attachment.format = VkFormat(info.format);
    attachment.samples = VkSampleCountFlagBits(info.sampleCount);
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = attachment.finalLayout =
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
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
    check(fn<PFN_vkCreateRenderPass>("vkCreateRenderPass")(
              binding.device, &rp, nullptr, &render_pass),
          "Color render pass failed");
    VkSamplerCreateInfo sc{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sc.magFilter = sc.minFilter = VK_FILTER_NEAREST;
    sc.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sc.addressModeU = sc.addressModeV = sc.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    check(fn<PFN_vkCreateSampler>("vkCreateSampler")(binding.device, &sc,
                                                     nullptr, &sampler),
          "Color sampler failed");
    const auto count = std::uint32_t(images.size()) * layers() * info.mipCount;
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, count};
    VkDescriptorPoolCreateInfo dp{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = count;
    dp.poolSizeCount = 1;
    dp.pPoolSizes = &size;
    check(fn<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(
              binding.device, &dp, nullptr, &descriptors),
          "Color descriptor pool failed");
    std::array<VkShaderModule, 2> modules{};
    auto shader = [&](std::span<const std::uint32_t> words,
                      VkShaderModule &module) {
      VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
      sm.codeSize = words.size_bytes();
      sm.pCode = words.data();
      check(fn<PFN_vkCreateShaderModule>("vkCreateShaderModule")(
                binding.device, &sm, nullptr, &module),
            "Color shader failed");
    };
    try {
      shader(vertex::decode_shader, modules[0]);
      shader(info.sampleCount > 1
                 ? std::span<const std::uint32_t>(multisample::decode_shader)
                 : std::span<const std::uint32_t>(fragment::decode_shader),
             modules[1]);
      std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
      for (unsigned i = 0; i < 2; ++i) {
        stages[i] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[i].stage =
            i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
        stages[i].module = modules[i];
        stages[i].pName = "main";
      }
      VkPipelineVertexInputStateCreateInfo vi{
          VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
      VkPipelineInputAssemblyStateCreateInfo ia{
          VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
      ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      VkPipelineViewportStateCreateInfo vp{
          VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
      vp.viewportCount = vp.scissorCount = 1;
      VkPipelineRasterizationStateCreateInfo rs{
          VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
      rs.polygonMode = VK_POLYGON_MODE_FILL;
      rs.cullMode = VK_CULL_MODE_NONE;
      rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
      rs.lineWidth = 1;
      VkPipelineMultisampleStateCreateInfo ms{
          VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
      ms.rasterizationSamples = VkSampleCountFlagBits(info.sampleCount);
      VkPipelineColorBlendAttachmentState ba{};
      ba.colorWriteMask = 15;
      VkPipelineColorBlendStateCreateInfo bs{
          VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
      bs.attachmentCount = 1;
      bs.pAttachments = &ba;
      std::array<VkDynamicState, 2> dynamic{VK_DYNAMIC_STATE_VIEWPORT,
                                            VK_DYNAMIC_STATE_SCISSOR};
      VkPipelineDynamicStateCreateInfo dy{
          VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
      dy.dynamicStateCount = 2;
      dy.pDynamicStates = dynamic.data();
      VkGraphicsPipelineCreateInfo pi{
          VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
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
      check(fn<PFN_vkCreateGraphicsPipelines>("vkCreateGraphicsPipelines")(
                binding.device, VK_NULL_HANDLE, 1, &pi, nullptr, &pipeline),
            "Color pipeline failed");
    } catch (...) {
      for (auto m : modules)
        if (m)
          fn<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(binding.device,
                                                                 m, nullptr);
      throw;
    }
    for (auto m : modules)
      fn<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(binding.device, m,
                                                             nullptr);
    if (vulkan::timing_enabled()) {
      std::uint32_t n = 0;
      fn<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
          "vkGetPhysicalDeviceQueueFamilyProperties")(binding.physicalDevice,
                                                      &n, nullptr);
      std::vector<VkQueueFamilyProperties> families(n);
      fn<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
          "vkGetPhysicalDeviceQueueFamilyProperties")(binding.physicalDevice,
                                                      &n, families.data());
      if (binding.queueFamilyIndex < n &&
          families[binding.queueFamilyIndex].timestampValidBits) {
        VkPhysicalDeviceProperties properties{};
        fn<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(
            binding.physicalDevice, &properties);
        timestamp_period = properties.limits.timestampPeriod;
        timestamp_bits = families[binding.queueFamilyIndex].timestampValidBits;
        VkQueryPoolCreateInfo qp{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qp.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qp.queryCount = 4 * std::uint32_t(images.size());
        check(fn<PFN_vkCreateQueryPool>("vkCreateQueryPool")(
                  binding.device, &qp, nullptr, &queries),
              "Color timestamps failed");
      }
    }
  }
  void prepare(Image &image, VkImage input) {
    const bool borrowed = input != image.target &&
                          (info.usageFlags & XR_SWAPCHAIN_USAGE_SAMPLED_BIT);
    if (image.source && (borrowed ? !image.owns_source && image.source == input
                                  : image.owns_source))
      return;
    // The caller waits for this target's previous submission before replacing
    // its views or descriptors. Application images remain application-owned.
    for (auto &sub : image.subresources) {
      if (sub.source)
        fn<PFN_vkDestroyImageView>("vkDestroyImageView")(binding.device,
                                                         sub.source, nullptr);
      sub.source = VK_NULL_HANDLE;
    }
    if (image.source && image.owns_source)
      fn<PFN_vkDestroyImage>("vkDestroyImage")(binding.device, image.source,
                                               nullptr);
    if (image.memory)
      fn<PFN_vkFreeMemory>("vkFreeMemory")(binding.device, image.memory,
                                           nullptr);
    image.source = VK_NULL_HANDLE;
    image.memory = VK_NULL_HANDLE;
    image.source_initialized = false;
    image.owns_source = !borrowed;
    if (borrowed) {
      image.source = input;
    } else {
      VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      ci.imageType = VK_IMAGE_TYPE_2D;
      ci.format = VkFormat(info.format);
      ci.extent = {info.width, info.height, 1};
      ci.mipLevels = info.mipCount;
      ci.arrayLayers = layers();
      ci.samples = VkSampleCountFlagBits(info.sampleCount);
      ci.tiling = VK_IMAGE_TILING_OPTIMAL;
      ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
      ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      check(fn<PFN_vkCreateImage>("vkCreateImage")(binding.device, &ci, nullptr,
                                                   &image.source),
            "Color source image failed");
      VkMemoryRequirements requirements{};
      fn<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(
          binding.device, image.source, &requirements);
      VkPhysicalDeviceMemoryProperties memory{};
      fn<PFN_vkGetPhysicalDeviceMemoryProperties>(
          "vkGetPhysicalDeviceMemoryProperties")(binding.physicalDevice,
                                                 &memory);
      std::uint32_t type = UINT32_MAX;
      for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i)
        if ((requirements.memoryTypeBits & (1u << i)) &&
            (memory.memoryTypes[i].propertyFlags &
             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
          type = i;
          break;
        }
      if (type == UINT32_MAX)
        throw std::runtime_error("No color image memory type");
      VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      ma.allocationSize = requirements.size;
      ma.memoryTypeIndex = type;
      check(fn<PFN_vkAllocateMemory>("vkAllocateMemory")(
                binding.device, &ma, nullptr, &image.memory),
            "Color memory failed");
      check(fn<PFN_vkBindImageMemory>("vkBindImageMemory")(
                binding.device, image.source, image.memory, 0),
            "Color memory binding failed");
    }
    if (!image.command) {
      VkCommandBufferAllocateInfo ca{
          VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
      ca.commandPool = pool;
      ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      ca.commandBufferCount = 1;
      check(fn<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(
                binding.device, &ca, &image.command),
            "Color command buffer failed");
      VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
      check(fn<PFN_vkCreateFence>("vkCreateFence")(binding.device, &fc, nullptr,
                                                   &image.fence),
            "Color fence failed");
      for (std::uint32_t layer = 0; layer < layers(); ++layer)
        for (std::uint32_t mip = 0; mip < info.mipCount; ++mip) {
          auto &sub = image.subresources.emplace_back();
          VkImageViewCreateInfo iv{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
          iv.viewType = VK_IMAGE_VIEW_TYPE_2D;
          iv.format = VkFormat(info.format);
          iv.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, layer, 1};
          iv.image = image.target;
          check(fn<PFN_vkCreateImageView>("vkCreateImageView")(
                    binding.device, &iv, nullptr, &sub.target),
                "Color target view failed");
          VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
          fb.renderPass = render_pass;
          fb.attachmentCount = 1;
          fb.pAttachments = &sub.target;
          fb.width = std::max(1u, info.width >> mip);
          fb.height = std::max(1u, info.height >> mip);
          fb.layers = 1;
          check(fn<PFN_vkCreateFramebuffer>("vkCreateFramebuffer")(
                    binding.device, &fb, nullptr, &sub.framebuffer),
                "Color framebuffer failed");
          VkDescriptorSetAllocateInfo da{
              VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
          da.descriptorPool = descriptors;
          da.descriptorSetCount = 1;
          da.pSetLayouts = &descriptor_layout;
          check(fn<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")(
                    binding.device, &da, &sub.descriptor),
                "Color descriptor failed");
        }
    }
    for (std::size_t i = 0; i < image.subresources.size(); ++i) {
      auto &sub = image.subresources[i];
      VkImageViewCreateInfo iv{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      iv.viewType = VK_IMAGE_VIEW_TYPE_2D;
      iv.format = VkFormat(info.format);
      iv.image = image.source;
      iv.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,
                             std::uint32_t(i) % info.mipCount, 1,
                             std::uint32_t(i) / info.mipCount, 1};
      check(fn<PFN_vkCreateImageView>("vkCreateImageView")(
                binding.device, &iv, nullptr, &sub.source),
            "Color source view failed");
      VkDescriptorImageInfo di{sampler, sub.source,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      write.dstSet = sub.descriptor;
      write.dstBinding = 0;
      write.descriptorCount = 1;
      write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      write.pImageInfo = &di;
      fn<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets")(
          binding.device, 1, &write, 0, nullptr);
    }
  }
  void barrier(Image &image, VkImage target, VkImageLayout before,
               VkImageLayout after) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = before;
    b.newLayout = after;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = target;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, info.mipCount, 0,
                          layers()};
    b.srcAccessMask =
        before == VK_IMAGE_LAYOUT_UNDEFINED
            ? 0
            : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    fn<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(
        image.command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
  }
  void copy(Image &image, bool restore, VkImage input = VK_NULL_HANDLE) {
    std::vector<VkImageCopy> regions;
    for (std::uint32_t mip = 0; mip < info.mipCount; ++mip) {
      VkImageCopy r{};
      r.srcSubresource =
          r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, layers()};
      r.extent = {std::max(1u, info.width >> mip),
                  std::max(1u, info.height >> mip), 1};
      regions.push_back(r);
    }
    fn<PFN_vkCmdCopyImage>("vkCmdCopyImage")(
        image.command, restore ? image.source : (input ? input : image.target),
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        restore ? image.target : image.source,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, std::uint32_t(regions.size()),
        regions.data());
  }
  void begin(Image &image) {
    wait(image);
    check(
        fn<PFN_vkResetFences>("vkResetFences")(binding.device, 1, &image.fence),
        "Color fence reset failed");
    check(
        fn<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")(image.command, 0),
        "Color command reset failed");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(fn<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(image.command,
                                                               &bi),
          "Color command begin failed");
  }
  void submit(Image &image, bool external_queue_held = false) {
    check(fn<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(image.command),
          "Color command end failed");
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &image.command;
    std::lock_guard queue_lock(*gfx::queue_mutex(queue));
    HostQueueLock angle_queue(!external_queue_held &&
                              queue == gfx::gpu_device().queue);
    if (!angle_queue)
      throw std::runtime_error("Color queue ownership failed");
    check(fn<PFN_vkQueueSubmit>("vkQueueSubmit")(queue, 1, &si, image.fence),
          "Color submit failed");
    image.pending = true;
  }
  void report(Image &image, std::uint32_t index, bool restore = false) {
    if ((!restore && !image.timing_pending) || !queries)
      return;
    std::array<std::uint64_t, 2> times{};
    auto result = fn<PFN_vkGetQueryPoolResults>("vkGetQueryPoolResults")(
        binding.device, queries, 4 * index + (restore ? 2 : 0), 2,
        sizeof(times), times.data(), sizeof(std::uint64_t),
        VK_QUERY_RESULT_64_BIT);
    if (result == VK_SUCCESS) {
      const auto mask = timestamp_bits == 64
                            ? UINT64_MAX
                            : (std::uint64_t(1) << timestamp_bits) - 1;
      const auto duration = (times[1] - times[0]) & mask;
      const auto begin = std::uint64_t((times[0] & mask) * timestamp_period);
      vulkan::XrCopySample sample{
          restore ? "xr_color_restore" : "xr_color",
          restore ? vulkan::timing_frame() : image.frame,
          0,
          0,
          0,
          0,
          0,
          begin,
          begin + std::uint64_t(duration * timestamp_period)};
      vulkan::timing_xr_copy(sample);
      if (!restore)
        image.timing_pending = false;
    }
  }
};
ColorImages::ColorImages(const XrGraphicsBindingVulkanKHR &binding,
                         PFN_vkGetInstanceProcAddr get,
                         const XrSwapchainCreateInfo &info,
                         std::span<const XrSwapchainImageVulkanKHR> images,
                         bool binding_queue_ordered)
    : impl_(std::make_unique<Impl>()) {
  impl_->binding = binding;
  impl_->get = get;
  impl_->info = info;
  impl_->binding_queue_ordered = binding_queue_ordered;
  impl_->info.next = nullptr;
  impl_->fn<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(
      binding.device, binding.queueFamilyIndex, binding.queueIndex,
      &impl_->queue);
  for (const auto &image : images)
    impl_->images.emplace_back().target = image.image;
}
ColorImages::~ColorImages() = default;
XrResult ColorImages::convert(std::uint32_t index, const ColorMatrix &matrix,
                              bool external_queue_held, Alpha alpha, float camera_opacity) {
  if (index >= impl_->images.size()) return XR_ERROR_RUNTIME_FAILURE;
  return convert_from(impl_->images[index].target, index, matrix, external_queue_held, alpha, camera_opacity);
}
XrResult ColorImages::convert_from(VkImage input, std::uint32_t index, const ColorMatrix &matrix, bool external_queue_held, Alpha alpha, float camera_opacity) {
  auto &s = *impl_;
  if (s.failed || index >= s.images.size())
    return XR_ERROR_RUNTIME_FAILURE;
  try {
    s.initialize();
    auto &image = s.images[index];
    s.wait(image);
    s.prepare(image, input);
    s.report(image, index);
    s.begin(image);
    if (s.queries) {
      s.fn<PFN_vkCmdResetQueryPool>("vkCmdResetQueryPool")(
          image.command, s.queries, 4 * index, 2);
      s.fn<PFN_vkCmdWriteTimestamp>("vkCmdWriteTimestamp")(
          image.command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, s.queries,
          4 * index);
    }
    if (!image.owns_source) {
      s.barrier(image, input, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    } else {
      s.barrier(image, image.source,
                image.source_initialized
                    ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                    : VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
      s.barrier(image, input, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
      s.copy(image, false, input);
      s.barrier(image, image.source, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
      s.barrier(image, input, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    }
    if (input != image.target)
      s.barrier(image, image.target,
                image.initialized ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                                  : VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    std::array<float, 12> rows{};
    for (unsigned r = 0; r < 3; ++r)
      for (unsigned c = 0; c < 3; ++c)
        rows[r * 4 + c] = float(matrix[r * 3 + c]);
    rows[3] = alpha == Alpha::premultiply ? 1.0f : 0.0f;
    rows[7] = alpha == Alpha::opaque ? 1.0f : 0.0f;
    rows[11] = camera_opacity;
    for (std::size_t i = 0; i < image.subresources.size(); ++i) {
      const auto mip = std::uint32_t(i) % s.info.mipCount;
      const auto width = std::max(1u, s.info.width >> mip),
                 height = std::max(1u, s.info.height >> mip);
      auto &sub = image.subresources[i];
      VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
      rp.renderPass = s.render_pass;
      rp.framebuffer = sub.framebuffer;
      rp.renderArea = {{0, 0}, {width, height}};
      s.fn<PFN_vkCmdBeginRenderPass>("vkCmdBeginRenderPass")(
          image.command, &rp, VK_SUBPASS_CONTENTS_INLINE);
      s.fn<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(
          image.command, VK_PIPELINE_BIND_POINT_GRAPHICS, s.pipeline);
      s.fn<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets")(
          image.command, VK_PIPELINE_BIND_POINT_GRAPHICS, s.pipeline_layout, 0,
          1, &sub.descriptor, 0, nullptr);
      s.fn<PFN_vkCmdPushConstants>("vkCmdPushConstants")(
          image.command, s.pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
          sizeof(rows), rows.data());
      VkViewport vp{0, 0, float(width), float(height), 0, 1};
      VkRect2D sc{{0, 0}, {width, height}};
      s.fn<PFN_vkCmdSetViewport>("vkCmdSetViewport")(image.command, 0, 1, &vp);
      s.fn<PFN_vkCmdSetScissor>("vkCmdSetScissor")(image.command, 0, 1, &sc);
      s.fn<PFN_vkCmdDraw>("vkCmdDraw")(image.command, 3, 1, 0, 0);
      s.fn<PFN_vkCmdEndRenderPass>("vkCmdEndRenderPass")(image.command);
    }
    if (!image.owns_source)
      s.barrier(image, input, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (s.queries)
      s.fn<PFN_vkCmdWriteTimestamp>("vkCmdWriteTimestamp")(
          image.command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s.queries,
          4 * index + 1);
    s.submit(image, external_queue_held);
    image.initialized = true;
    image.source_initialized = true;
    image.converted = input == image.target;
    image.frame = vulkan::timing_frame();
    image.timing_pending = s.queries != VK_NULL_HANDLE;
    if (input != image.target && !s.binding_queue_ordered) {
      // Finish the read before releasing the application's image back to XR.
      // The application may immediately acquire and overwrite it again.
      s.wait(image, "xr_color_release");
      s.report(image, index);
    }
    // Native XR work uses the session's binding VkQueue. The barriers above
    // order source reads before subsequent writes and restore the XR layout;
    // xrReleaseSwapchainImage transfers ownership on that same queue. Keep the
    // fence pending for command/descriptor reuse and destruction, rather than
    // blocking this releasing thread after every submit. Other source paths
    // retain completion waits because their producer queue may differ.
    return XR_SUCCESS;
  } catch (const std::exception &e) {
    s.failed = true;
    log::error("xr", "Color conversion failed: {}", e.what());
    return XR_ERROR_RUNTIME_FAILURE;
  }
}
XrResult ColorImages::restore(std::uint32_t index) {
  auto &s = *impl_;
  if (s.failed || index >= s.images.size())
    return XR_ERROR_RUNTIME_FAILURE;
  auto &image = s.images[index];
  if (!image.converted)
    return XR_SUCCESS;
  try {
    s.wait(image);
    s.report(image, index);
    s.begin(image);
    if (s.queries) {
      s.fn<PFN_vkCmdResetQueryPool>("vkCmdResetQueryPool")(
          image.command, s.queries, 4 * index + 2, 2);
      s.fn<PFN_vkCmdWriteTimestamp>("vkCmdWriteTimestamp")(
          image.command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, s.queries,
          4 * index + 2);
    }
    s.barrier(image, image.source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    s.barrier(image, image.target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    s.copy(image, true);
    s.barrier(image, image.source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    s.barrier(image, image.target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (s.queries)
      s.fn<PFN_vkCmdWriteTimestamp>("vkCmdWriteTimestamp")(
          image.command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s.queries,
          4 * index + 3);
    s.submit(image);
    s.wait(image);
    s.report(image, index, true);
    image.converted = false;
    return XR_SUCCESS;
  } catch (const std::exception &e) {
    s.failed = true;
    log::error("xr", "Color image restoration failed: {}", e.what());
    return XR_ERROR_RUNTIME_FAILURE;
  }
}
} // namespace venture::xr

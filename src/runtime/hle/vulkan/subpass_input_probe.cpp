// Multiview subpass input self-test (input_attachments.cpp uses the result).
//
// One render pass with two views (layers 0 and 1) and two subpasses:
// subpass 0 draws red in view 0 and green in view 1 into a 4x multisampled
// color attachment resolved to a single-sampled one; subpass 1 reads both with
// subpassLoad (subpassInput and subpassInputMS) and writes them out. The
// outputs are copied back: view 0 must hold red (else the probe itself failed),
// and view 1 green when the driver reads each view's own layer. A driver that
// reads view 0's layer in every view shows red there.
//
// All calls go straight to the host driver on the newly created device, before
// the guest has it, so nothing else submits to its queue. Every failure, and a
// wait past one second, makes the result unavailable; device creation goes on.
#include <array>
#include <chrono>
#include <cstring>
#include <format>
#include <string>

#include "common/log.h"
#include "common/paths.h"
#include "gfx/queue_sync.h"
#include "internal.h"

namespace venture::vulkan {
namespace {
namespace vertex_shader {
#include "subpass_probe_vert.inc"
}
namespace write_shader {
#include "subpass_probe_write_frag.inc"
}
namespace read_shader {
#include "subpass_probe_read_frag.inc"
}
constexpr std::uint32_t size = 64, view_count = 2;
constexpr VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
// Version of the probe in the cache file name: a changed probe runs again.
constexpr int probe_version = 1;

struct Probe {
  VkPhysicalDevice physical{};
  VkDevice device{};
  bool idle = true;  // false when submitted work did not finish: nothing is destroyed
  std::array<VkImage, 4> images{};  // multisampled, resolved, two outputs
  std::array<VkImageView, 4> views{};
  std::vector<VkDeviceMemory> memory;
  VkBuffer buffer{};
  VkRenderPass pass{};
  VkFramebuffer framebuffer{};
  VkDescriptorSetLayout set_layout{};
  VkDescriptorPool pool{};
  VkPipelineLayout layout{};
  std::array<VkShaderModule, 3> modules{};
  std::array<VkPipeline, 2> pipelines{};
  VkCommandPool commands{};
  VkFence fence{};

  template <class F> F vk(const char* name) const {
    auto f = reinterpret_cast<F>(raw_device_proc(device, name));
    if (!f) throw std::runtime_error(std::string("no ") + name);
    return f;
  }
  static void check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) throw std::runtime_error(std::format("{} returned {}", what, int(r)));
  }
  ~Probe() {
    if (!idle) return;
    auto destroy = [&](auto handle, const char* name) {
      using Handle = decltype(handle);
      if (handle) reinterpret_cast<void (*)(VkDevice, Handle, const VkAllocationCallbacks*)>(
                      raw_device_proc(device, name))(device, handle, nullptr);
    };
    destroy(fence, "vkDestroyFence");
    destroy(commands, "vkDestroyCommandPool");
    for (auto p : pipelines) destroy(p, "vkDestroyPipeline");
    for (auto m : modules) destroy(m, "vkDestroyShaderModule");
    destroy(layout, "vkDestroyPipelineLayout");
    destroy(pool, "vkDestroyDescriptorPool");
    destroy(set_layout, "vkDestroyDescriptorSetLayout");
    destroy(framebuffer, "vkDestroyFramebuffer");
    destroy(pass, "vkDestroyRenderPass");
    for (auto v : views) destroy(v, "vkDestroyImageView");
    for (auto i : images) destroy(i, "vkDestroyImage");
    destroy(buffer, "vkDestroyBuffer");
    for (auto m : memory) destroy(m, "vkFreeMemory");
  }

  void bind(const VkMemoryRequirements& requirements, VkMemoryPropertyFlags wanted, VkImage image, VkBuffer buf,
            void** mapped = nullptr) {
    VkPhysicalDeviceMemoryProperties properties{};
    reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
        raw_physical_proc(physical, "vkGetPhysicalDeviceMemoryProperties"))(physical, &properties);
    std::uint32_t type = properties.memoryTypeCount;
    for (auto required : {wanted, wanted & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VkMemoryPropertyFlags{}}) {
      for (std::uint32_t i = 0; i < properties.memoryTypeCount && type == properties.memoryTypeCount; ++i) {
        const auto flags = properties.memoryTypes[i].propertyFlags;
        if ((requirements.memoryTypeBits & (1u << i)) && (flags & required) == required &&
            !(flags & (VK_MEMORY_PROPERTY_PROTECTED_BIT | VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT)))
          type = i;
      }
      if (type != properties.memoryTypeCount) break;
    }
    if (type == properties.memoryTypeCount) throw std::runtime_error("no memory type");
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, requirements.size, type};
    VkDeviceMemory m{};
    check(vk<PFN_vkAllocateMemory>("vkAllocateMemory")(device, &allocate, nullptr, &m), "vkAllocateMemory");
    memory.push_back(m);
    if (image) check(vk<PFN_vkBindImageMemory>("vkBindImageMemory")(device, image, m, 0), "vkBindImageMemory");
    if (buf) check(vk<PFN_vkBindBufferMemory>("vkBindBufferMemory")(device, buf, m, 0), "vkBindBufferMemory");
    if (mapped) check(vk<PFN_vkMapMemory>("vkMapMemory")(device, m, 0, VK_WHOLE_SIZE, 0, mapped), "vkMapMemory");
  }
  VkShaderModule module(std::span<const std::uint32_t> code) {
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = code.size_bytes();
    info.pCode = code.data();
    VkShaderModule m{};
    check(vk<PFN_vkCreateShaderModule>("vkCreateShaderModule")(device, &info, nullptr, &m), "vkCreateShaderModule");
    return m;
  }
  VkPipeline pipeline(std::uint32_t subpass, VkShaderModule fragment, VkSampleCountFlagBits samples,
                      std::uint32_t outputs) {
    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT,
                 modules[0], "main"};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT,
                 fragment, "main"};
    VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport viewport{0, 0, float(size), float(size), 0, 1};
    VkRect2D scissor{{0, 0}, {size, size}};
    VkPipelineViewportStateCreateInfo viewports{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewports.viewportCount = viewports.scissorCount = 1;
    viewports.pViewports = &viewport;
    viewports.pScissors = &scissor;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = samples;
    std::array<VkPipelineColorBlendAttachmentState, 2> blend{};
    for (auto& b : blend)
      b.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                         VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blending{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blending.attachmentCount = outputs;
    blending.pAttachments = blend.data();
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.stageCount = 2;
    info.pStages = stages.data();
    info.pVertexInputState = &vertex;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewports;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pColorBlendState = &blending;
    info.layout = layout;
    info.renderPass = pass;
    info.subpass = subpass;
    VkPipeline p{};
    check(vk<PFN_vkCreateGraphicsPipelines>("vkCreateGraphicsPipelines")(device, VK_NULL_HANDLE, 1, &info, nullptr,
                                                                         &p),
          "vkCreateGraphicsPipelines");
    return p;
  }

  // Returns the readback: two outputs x two layers x size^2 RGBA8 pixels.
  std::vector<std::uint8_t> run(std::uint32_t family) {
    // Attachments: 0 multisampled color, 1 its resolve, 2 and 3 the copies.
    for (std::uint32_t i = 0; i < images.size(); ++i) {
      VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      info.imageType = VK_IMAGE_TYPE_2D;
      info.format = format;
      info.extent = {size, size, 1};
      info.mipLevels = 1;
      info.arrayLayers = view_count;
      info.samples = i == 0 ? VK_SAMPLE_COUNT_4_BIT : VK_SAMPLE_COUNT_1_BIT;
      info.tiling = VK_IMAGE_TILING_OPTIMAL;
      info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                   (i < 2 ? VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT : VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
      check(vk<PFN_vkCreateImage>("vkCreateImage")(device, &info, nullptr, &images[i]), "vkCreateImage");
      VkMemoryRequirements requirements{};
      vk<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(device, images[i], &requirements);
      bind(requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, images[i], VK_NULL_HANDLE);
      VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      view.image = images[i];
      view.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
      view.format = format;
      view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, view_count};
      check(vk<PFN_vkCreateImageView>("vkCreateImageView")(device, &view, nullptr, &views[i]), "vkCreateImageView");
    }
    const VkDeviceSize pixels = VkDeviceSize(size) * size * view_count;
    VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer_info.size = pixels * 4 * 2;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    check(vk<PFN_vkCreateBuffer>("vkCreateBuffer")(device, &buffer_info, nullptr, &buffer), "vkCreateBuffer");
    VkMemoryRequirements requirements{};
    vk<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(device, buffer, &requirements);
    void* mapped = nullptr;
    bind(requirements, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, VK_NULL_HANDLE,
         buffer, &mapped);

    std::array<VkAttachmentDescription, 4> attachments{};
    for (std::uint32_t i = 0; i < attachments.size(); ++i) {
      auto& a = attachments[i];
      a.format = format;
      a.samples = i == 0 ? VK_SAMPLE_COUNT_4_BIT : VK_SAMPLE_COUNT_1_BIT;
      a.loadOp = i == 1 ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_CLEAR;
      a.storeOp = i < 2 ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
      a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
      a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      a.finalLayout = i < 2 ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    }
    const VkAttachmentReference drawn{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    const VkAttachmentReference resolved{1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    const std::array<VkAttachmentReference, 2> inputs{{{1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                                       {0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}}};
    const std::array<VkAttachmentReference, 2> copies{{{2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                                                       {3, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}}};
    std::array<VkSubpassDescription, 2> subpasses{};
    subpasses[0].pipelineBindPoint = subpasses[1].pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpasses[0].colorAttachmentCount = 1;
    subpasses[0].pColorAttachments = &drawn;
    subpasses[0].pResolveAttachments = &resolved;
    subpasses[1].inputAttachmentCount = 2;
    subpasses[1].pInputAttachments = inputs.data();
    subpasses[1].colorAttachmentCount = 2;
    subpasses[1].pColorAttachments = copies.data();
    std::array<VkSubpassDependency, 2> dependencies{};
    dependencies[0] = {0, 1, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                       VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_INPUT_ATTACHMENT_READ_BIT,
                       VK_DEPENDENCY_BY_REGION_BIT | VK_DEPENDENCY_VIEW_LOCAL_BIT};
    dependencies[1] = {1, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                       VK_ACCESS_TRANSFER_READ_BIT, 0};
    const std::array<std::uint32_t, 2> masks{0b11, 0b11};
    const std::uint32_t correlation = 0b11;
    VkRenderPassMultiviewCreateInfo multiview{VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO};
    multiview.subpassCount = 2;
    multiview.pViewMasks = masks.data();
    multiview.correlationMaskCount = 1;
    multiview.pCorrelationMasks = &correlation;
    VkRenderPassCreateInfo pass_info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, &multiview};
    pass_info.attachmentCount = attachments.size();
    pass_info.pAttachments = attachments.data();
    pass_info.subpassCount = subpasses.size();
    pass_info.pSubpasses = subpasses.data();
    pass_info.dependencyCount = dependencies.size();
    pass_info.pDependencies = dependencies.data();
    check(vk<PFN_vkCreateRenderPass>("vkCreateRenderPass")(device, &pass_info, nullptr, &pass), "vkCreateRenderPass");
    VkFramebufferCreateInfo framebuffer_info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    framebuffer_info.renderPass = pass;
    framebuffer_info.attachmentCount = views.size();
    framebuffer_info.pAttachments = views.data();
    framebuffer_info.width = framebuffer_info.height = size;
    framebuffer_info.layers = 1;
    check(vk<PFN_vkCreateFramebuffer>("vkCreateFramebuffer")(device, &framebuffer_info, nullptr, &framebuffer),
          "vkCreateFramebuffer");

    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    for (std::uint32_t i = 0; i < 2; ++i)
      bindings[i] = {i, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_info.bindingCount = 2;
    set_info.pBindings = bindings.data();
    check(vk<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(device, &set_info, nullptr, &set_layout),
          "vkCreateDescriptorSetLayout");
    VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 2};
    VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    check(vk<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(device, &pool_info, nullptr, &pool),
          "vkCreateDescriptorPool");
    VkDescriptorSetAllocateInfo set_allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, pool, 1,
                                             &set_layout};
    VkDescriptorSet set{};
    check(vk<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")(device, &set_allocate, &set),
          "vkAllocateDescriptorSets");
    const std::array<VkDescriptorImageInfo, 2> image_infos{
        {{VK_NULL_HANDLE, views[1], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
         {VK_NULL_HANDLE, views[0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}}};
    std::array<VkWriteDescriptorSet, 2> writes{};
    for (std::uint32_t i = 0; i < 2; ++i) {
      writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      writes[i].dstSet = set;
      writes[i].dstBinding = i;
      writes[i].descriptorCount = 1;
      writes[i].descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
      writes[i].pImageInfo = &image_infos[i];
    }
    vk<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets")(device, 2, writes.data(), 0, nullptr);
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &set_layout;
    check(vk<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(device, &layout_info, nullptr, &layout),
          "vkCreatePipelineLayout");
    modules[0] = module(vertex_shader::decode_shader);
    modules[1] = module(write_shader::decode_shader);
    modules[2] = module(read_shader::decode_shader);
    pipelines[0] = pipeline(0, modules[1], VK_SAMPLE_COUNT_4_BIT, 1);
    pipelines[1] = pipeline(1, modules[2], VK_SAMPLE_COUNT_1_BIT, 2);

    VkCommandPoolCreateInfo command_pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    command_pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    command_pool.queueFamilyIndex = family;
    check(vk<PFN_vkCreateCommandPool>("vkCreateCommandPool")(device, &command_pool, nullptr, &commands),
          "vkCreateCommandPool");
    VkCommandBufferAllocateInfo command_allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, commands,
                                                 VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer command{};
    check(vk<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(device, &command_allocate, &command),
          "vkAllocateCommandBuffers");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vk<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(command, &begin), "vkBeginCommandBuffer");
    std::array<VkClearValue, 4> clears{};
    for (auto& c : clears) c.color = {{0.f, 0.f, 1.f, 1.f}};  // blue: never drawn
    VkRenderPassBeginInfo pass_begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    pass_begin.renderPass = pass;
    pass_begin.framebuffer = framebuffer;
    pass_begin.renderArea = {{0, 0}, {size, size}};
    pass_begin.clearValueCount = clears.size();
    pass_begin.pClearValues = clears.data();
    vk<PFN_vkCmdBeginRenderPass>("vkCmdBeginRenderPass")(command, &pass_begin, VK_SUBPASS_CONTENTS_INLINE);
    auto bind_pipeline = vk<PFN_vkCmdBindPipeline>("vkCmdBindPipeline");
    auto draw = vk<PFN_vkCmdDraw>("vkCmdDraw");
    bind_pipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines[0]);
    draw(command, 3, 1, 0, 0);
    vk<PFN_vkCmdNextSubpass>("vkCmdNextSubpass")(command, VK_SUBPASS_CONTENTS_INLINE);
    bind_pipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines[1]);
    vk<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets")(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0,
                                                                 1, &set, 0, nullptr);
    draw(command, 3, 1, 0, 0);
    vk<PFN_vkCmdEndRenderPass>("vkCmdEndRenderPass")(command);
    for (std::uint32_t i = 0; i < 2; ++i) {
      VkBufferImageCopy copy{};
      copy.bufferOffset = i * pixels * 4;
      copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, view_count};
      copy.imageExtent = {size, size, 1};
      vk<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(command, images[2 + i],
                                                                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
    }
    VkMemoryBarrier to_host{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT,
                            VK_ACCESS_HOST_READ_BIT};
    vk<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                                         VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &to_host, 0, nullptr, 0,
                                                         nullptr);
    check(vk<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(command), "vkEndCommandBuffer");

    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    check(vk<PFN_vkCreateFence>("vkCreateFence")(device, &fence_info, nullptr, &fence), "vkCreateFence");
    VkQueue queue{};
    vk<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(device, family, host_queue_index(device, family, 0), &queue);
    if (!queue) throw std::runtime_error("no queue");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    {
      auto mutex = gfx::queue_mutex(queue);
      std::lock_guard lock(*mutex);
      check(vk<PFN_vkQueueSubmit>("vkQueueSubmit")(queue, 1, &submit, fence), "vkQueueSubmit");
    }
    idle = false;
    check(vk<PFN_vkWaitForFences>("vkWaitForFences")(device, 1, &fence, VK_TRUE, 1'000'000'000), "vkWaitForFences");
    idle = true;
    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, memory.back(), 0, VK_WHOLE_SIZE};
    check(vk<PFN_vkInvalidateMappedMemoryRanges>("vkInvalidateMappedMemoryRanges")(device, 1, &range),
          "vkInvalidateMappedMemoryRanges");
    std::vector<std::uint8_t> out(buffer_info.size);
    std::memcpy(out.data(), mapped, out.size());
    return out;
  }
};

// Whether every pixel of `layer` in output `output` is `rgba`.
bool layer_is(const std::vector<std::uint8_t>& pixels, std::uint32_t output, std::uint32_t layer,
              std::array<std::uint8_t, 4> rgba) {
  const std::size_t count = std::size_t(size) * size;
  const auto* p = pixels.data() + (std::size_t(output) * view_count + layer) * count * 4;
  for (std::size_t i = 0; i < count; ++i)
    if (std::memcmp(p + i * 4, rgba.data(), 4) != 0) return false;
  return true;
}
}  // namespace

SubpassInputProbe probe_subpass_inputs(VkPhysicalDevice physical, VkDevice device,
                                       std::span<const VkDeviceQueueCreateInfo> queues, double* milliseconds) {
  const auto start = std::chrono::steady_clock::now();
  auto result = SubpassInputProbe::unavailable;
  try {
    // A graphics family the device created queues in without flags.
    std::uint32_t count = 0;
    auto families = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
        raw_physical_proc(physical, "vkGetPhysicalDeviceQueueFamilyProperties"));
    if (!families) throw std::runtime_error("no queue family query");
    families(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> properties(count);
    families(physical, &count, properties.data());
    std::optional<std::uint32_t> family;
    for (const auto& q : queues)
      if (!family && !q.flags && q.queueCount && q.queueFamilyIndex < count &&
          (properties[q.queueFamilyIndex].queueFlags & VK_QUEUE_GRAPHICS_BIT))
        family = q.queueFamilyIndex;
    if (!family) throw std::runtime_error("no graphics queue");
    Probe probe{physical, device};
    const auto pixels = probe.run(*family);
    constexpr std::array<std::uint8_t, 4> red{255, 0, 0, 255}, green{0, 255, 0, 255};
    bool view0 = true, view1 = true;
    for (std::uint32_t output = 0; output < 2; ++output) {
      view0 &= layer_is(pixels, output, 0, red);
      view1 &= layer_is(pixels, output, 1, green);
    }
    if (!view0) {
      const auto* p = pixels.data();
      log::warn("vulkan", "Multiview subpass input probe: view 0 read {},{},{},{} instead of red", p[0], p[1], p[2],
                p[3]);
    } else {
      result = view1 ? SubpassInputProbe::own_layer : SubpassInputProbe::other_layer;
    }
  } catch (const std::exception& e) {
    log::warn("vulkan", "Multiview subpass input probe failed: {}", e.what());
  } catch (...) {
  }
  if (milliseconds)
    *milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  return result;
}

std::filesystem::path subpass_input_probe_file(const std::filesystem::path& directory, std::uint32_t vendor_id,
                                               std::uint32_t device_id, std::uint32_t driver_version) {
  return directory / std::format("subpass-inputs-v{}-{:04x}-{:04x}-{:08x}.txt", probe_version, vendor_id,
                                 device_id, driver_version);
}
SubpassInputProbe cached_subpass_input_probe(const std::filesystem::path& file) {
  try {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec)) return SubpassInputProbe::unavailable;
    const auto text = paths::read(file, 64);
    if (text.starts_with("own-layer")) return SubpassInputProbe::own_layer;
    if (text.starts_with("other-layer")) return SubpassInputProbe::other_layer;
  } catch (...) {
  }
  return SubpassInputProbe::unavailable;
}
void cache_subpass_input_probe(const std::filesystem::path& file, SubpassInputProbe result) {
  if (result == SubpassInputProbe::unavailable) return;
  try {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    paths::atomic_write(file, result == SubpassInputProbe::own_layer ? "own-layer\n" : "other-layer\n");
  } catch (const std::exception& e) {
    log::warn("vulkan", "Could not keep the multiview subpass input probe result: {}", e.what());
  } catch (...) {
  }
}
}  // namespace venture::vulkan

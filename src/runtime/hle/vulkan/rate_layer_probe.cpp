// Shading-rate layer self-test (gfx/foveation_vulkan.h uses the result).
//
// A GPU without layered shading-rate attachments takes a single-layer rate
// view in a multiview pass, which the specification applies to every view. A
// GPU may still read it layered, view N at the view's layer plus N (Turnip on
// the Steam Frame's Adreno programs its rate image so for every pass). On such
// a GPU an eye map's per-eye layers give each eye its own rates instead of the
// union of both eyes', which shades less.
//
// One two-view pass (layers 0 and 1) draws a triangle over a 64x64 colour
// attachment whose fragments write their position, with a shading-rate
// attachment viewed at layer 0 of a two-layer map: layer 0 full rate, layer 1
// 2x2. View 0 must come out at full rate (neighbouring pixels differ), else the
// probe itself failed; view 1 at 2x2 (pixel pairs equal) shows the GPU reads
// the next layer for view 1.
//
// All calls go straight to the host driver on the newly created device, before
// the guest has it, so nothing else submits to its queue. Every failure, and a
// wait past one second, makes the result unavailable; device creation goes on.
#include <array>
#include <chrono>
#include <cstring>
#include <format>
#include <optional>
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
namespace fragment_shader {
#include "rate_probe_frag.inc"
}
constexpr std::uint32_t size = 64, view_count = 2;
constexpr VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
constexpr std::uint8_t half_rate = 1 << 2 | 1;  // 2x2
// Version of the probe in the cache file name: a changed probe runs again.
constexpr int probe_version = 1;

struct Probe {
  VkPhysicalDevice physical{};
  VkDevice device{};
  std::uint32_t texel = 16;
  bool idle = true;  // false when submitted work did not finish: nothing is destroyed
  std::array<VkImage, 2> images{};  // colour, rate map
  std::array<VkImageView, 2> views{};
  std::vector<VkDeviceMemory> memory;
  std::array<VkBuffer, 2> buffers{};  // readback, rate upload
  VkRenderPass pass{};
  VkFramebuffer framebuffer{};
  VkPipelineLayout layout{};
  std::array<VkShaderModule, 2> modules{};
  VkPipeline pipeline{};
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
    destroy(pipeline, "vkDestroyPipeline");
    for (auto m : modules) destroy(m, "vkDestroyShaderModule");
    destroy(layout, "vkDestroyPipelineLayout");
    destroy(framebuffer, "vkDestroyFramebuffer");
    destroy(pass, "vkDestroyRenderPass");
    for (auto v : views) destroy(v, "vkDestroyImageView");
    for (auto i : images) destroy(i, "vkDestroyImage");
    for (auto b : buffers) destroy(b, "vkDestroyBuffer");
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
  VkImage image(VkFormat image_format, std::uint32_t width, std::uint32_t height, VkImageUsageFlags usage,
                std::uint32_t index) {
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = image_format;
    info.extent = {width, height, 1};
    info.mipLevels = 1;
    info.arrayLayers = view_count;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    check(vk<PFN_vkCreateImage>("vkCreateImage")(device, &info, nullptr, &images[index]), "vkCreateImage");
    VkMemoryRequirements requirements{};
    vk<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(device, images[index], &requirements);
    bind(requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, images[index], VK_NULL_HANDLE);
    return images[index];
  }
  VkBuffer buffer(VkDeviceSize bytes, VkBufferUsageFlags usage, std::uint32_t index, void** mapped) {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = bytes;
    info.usage = usage;
    check(vk<PFN_vkCreateBuffer>("vkCreateBuffer")(device, &info, nullptr, &buffers[index]), "vkCreateBuffer");
    VkMemoryRequirements requirements{};
    vk<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(device, buffers[index], &requirements);
    bind(requirements, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_NULL_HANDLE,
         buffers[index], mapped);
    return buffers[index];
  }

  // Returns the readback: two layers x size^2 RGBA8 pixels.
  std::vector<std::uint8_t> run(std::uint32_t family) {
    const std::uint32_t map = size / texel;
    image(format, size, size, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0);
    image(VK_FORMAT_R8_UINT, map, map,
          VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 1);
    for (std::uint32_t i = 0; i < 2; ++i) {
      VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      view.image = images[i];
      // The colour target has both views' layers; the rate view only layer 0.
      view.viewType = i == 0 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
      view.format = i == 0 ? format : VK_FORMAT_R8_UINT;
      view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, i == 0 ? view_count : 1};
      check(vk<PFN_vkCreateImageView>("vkCreateImageView")(device, &view, nullptr, &views[i]), "vkCreateImageView");
    }
    const VkDeviceSize pixels = VkDeviceSize(size) * size * view_count;
    void* readback = nullptr;
    buffer(pixels * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 0, &readback);
    void* rates = nullptr;
    const VkDeviceSize map_bytes = VkDeviceSize(map) * map;
    buffer(map_bytes * view_count, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 1, &rates);
    std::memset(rates, 0, std::size_t(map_bytes));
    std::memset(static_cast<std::uint8_t*>(rates) + map_bytes, half_rate, std::size_t(map_bytes));
    VkMappedMemoryRange upload_range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, memory.back(), 0, VK_WHOLE_SIZE};
    check(vk<PFN_vkFlushMappedMemoryRanges>("vkFlushMappedMemoryRanges")(device, 1, &upload_range),
          "vkFlushMappedMemoryRanges");
    const auto readback_memory = memory[memory.size() - 2];

    std::array<VkAttachmentDescription2, 2> attachments{};
    attachments[0] = {VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2};
    attachments[0].format = format;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    attachments[1] = {VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2};
    attachments[1].format = VK_FORMAT_R8_UINT;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;
    VkAttachmentReference2 color{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, 0,
                                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT};
    VkAttachmentReference2 rate{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, 1,
                                VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR, 0};
    VkFragmentShadingRateAttachmentInfoKHR rate_info{VK_STRUCTURE_TYPE_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR,
                                                     nullptr, &rate, {texel, texel}};
    VkSubpassDescription2 subpass{VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2, &rate_info};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.viewMask = 0b11;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color;
    VkSubpassDependency2 dependency{VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2};
    dependency.srcSubpass = 0;
    dependency.dstSubpass = VK_SUBPASS_EXTERNAL;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependency.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    const std::uint32_t correlation = 0b11;
    VkRenderPassCreateInfo2 pass_info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2};
    pass_info.attachmentCount = std::uint32_t(attachments.size());
    pass_info.pAttachments = attachments.data();
    pass_info.subpassCount = 1;
    pass_info.pSubpasses = &subpass;
    pass_info.dependencyCount = 1;
    pass_info.pDependencies = &dependency;
    pass_info.correlatedViewMaskCount = 1;
    pass_info.pCorrelatedViewMasks = &correlation;
    auto create_pass = reinterpret_cast<PFN_vkCreateRenderPass2>(raw_device_proc(device, "vkCreateRenderPass2"));
    if (!create_pass) create_pass = vk<PFN_vkCreateRenderPass2>("vkCreateRenderPass2KHR");
    check(create_pass(device, &pass_info, nullptr, &pass), "vkCreateRenderPass2");
    VkFramebufferCreateInfo framebuffer_info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    framebuffer_info.renderPass = pass;
    framebuffer_info.attachmentCount = std::uint32_t(views.size());
    framebuffer_info.pAttachments = views.data();
    framebuffer_info.width = framebuffer_info.height = size;
    framebuffer_info.layers = 1;
    check(vk<PFN_vkCreateFramebuffer>("vkCreateFramebuffer")(device, &framebuffer_info, nullptr, &framebuffer),
          "vkCreateFramebuffer");

    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    check(vk<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(device, &layout_info, nullptr, &layout),
          "vkCreatePipelineLayout");
    modules[0] = module(vertex_shader::decode_shader);
    modules[1] = module(fragment_shader::decode_shader);
    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT,
                 modules[0], "main"};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT,
                 modules[1], "main"};
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
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blending{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blending.attachmentCount = 1;
    blending.pAttachments = &blend;
    // Full rate from the pipeline; the attachment's rate replaces it, as
    // foveation's pipelines do.
    VkPipelineFragmentShadingRateStateCreateInfoKHR rate_state{
        VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR,
        nullptr,
        {1, 1},
        {VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR, VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR}};
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rate_state};
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
    check(vk<PFN_vkCreateGraphicsPipelines>("vkCreateGraphicsPipelines")(device, VK_NULL_HANDLE, 1, &info, nullptr,
                                                                         &pipeline),
          "vkCreateGraphicsPipelines");

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
    auto barrier = vk<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
    VkImageMemoryBarrier to_copy{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_copy.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_copy.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_copy.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_copy.srcQueueFamilyIndex = to_copy.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_copy.image = images[1];
    to_copy.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, view_count};
    barrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
            &to_copy);
    VkBufferImageCopy upload{};
    upload.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, view_count};
    upload.imageExtent = {map, map, 1};
    vk<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage")(command, buffers[1], images[1],
                                                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &upload);
    VkImageMemoryBarrier to_rate = to_copy;
    to_rate.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_rate.dstAccessMask = VK_ACCESS_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT_KHR;
    to_rate.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_rate.newLayout = VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;
    barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR, 0, 0,
            nullptr, 0, nullptr, 1, &to_rate);
    std::array<VkClearValue, 2> clears{};
    clears[0].color = {{0.f, 0.f, 1.f, 1.f}};  // blue: never drawn
    VkRenderPassBeginInfo pass_begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    pass_begin.renderPass = pass;
    pass_begin.framebuffer = framebuffer;
    pass_begin.renderArea = {{0, 0}, {size, size}};
    pass_begin.clearValueCount = std::uint32_t(clears.size());
    pass_begin.pClearValues = clears.data();
    vk<PFN_vkCmdBeginRenderPass>("vkCmdBeginRenderPass")(command, &pass_begin, VK_SUBPASS_CONTENTS_INLINE);
    vk<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vk<PFN_vkCmdDraw>("vkCmdDraw")(command, 3, 1, 0, 0);
    vk<PFN_vkCmdEndRenderPass>("vkCmdEndRenderPass")(command);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, view_count};
    copy.imageExtent = {size, size, 1};
    vk<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(command, images[0], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                             buffers[0], 1, &copy);
    VkMemoryBarrier to_host{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT,
                            VK_ACCESS_HOST_READ_BIT};
    barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &to_host, 0, nullptr, 0,
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
    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, readback_memory, 0, VK_WHOLE_SIZE};
    check(vk<PFN_vkInvalidateMappedMemoryRanges>("vkInvalidateMappedMemoryRanges")(device, 1, &range),
          "vkInvalidateMappedMemoryRanges");
    std::vector<std::uint8_t> out(std::size_t(pixels) * 4);
    std::memcpy(out.data(), readback, out.size());
    return out;
  }
};

// Whether `layer` came out in 2x2 fragments: every pixel pair (x, x + 1) at even
// x, and every row pair, holds one colour. With `full`, whether no pair does.
bool coarse(const std::vector<std::uint8_t>& pixels, std::uint32_t layer, bool full) {
  const auto* p = pixels.data() + std::size_t(layer) * size * size * 4;
  const auto at = [&](std::uint32_t x, std::uint32_t y) { return p + (std::size_t(y) * size + x) * 4; };
  for (std::uint32_t y = 0; y < size; y += 2)
    for (std::uint32_t x = 0; x < size; x += 2) {
      const bool same = std::memcmp(at(x, y), at(x + 1, y), 4) == 0 && std::memcmp(at(x, y), at(x, y + 1), 4) == 0;
      if (same == full) return false;
    }
  return true;
}
}  // namespace

RateLayerProbe probe_rate_layers(VkPhysicalDevice physical, VkDevice device,
                                 std::span<const VkDeviceQueueCreateInfo> queues, std::uint32_t texel,
                                 double* milliseconds) {
  const auto start = std::chrono::steady_clock::now();
  auto result = RateLayerProbe::unavailable;
  try {
    if (!texel || size % texel) throw std::runtime_error("texel size");
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
    Probe probe{physical, device, texel};
    const auto pixels = probe.run(*family);
    if (!coarse(pixels, 0, true))
      log::warn("vulkan", "Shading-rate layer probe: view 0 was not shaded at full rate");
    else if (coarse(pixels, 1, false))
      result = RateLayerProbe::next_layer;
    else if (coarse(pixels, 1, true))
      result = RateLayerProbe::same_layer;
    else
      log::warn("vulkan", "Shading-rate layer probe: view 1 was shaded at mixed rates");
  } catch (const std::exception& e) {
    log::warn("vulkan", "Shading-rate layer probe failed: {}", e.what());
  } catch (...) {
  }
  if (milliseconds)
    *milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  return result;
}

std::filesystem::path rate_layer_probe_file(const std::filesystem::path& directory, std::uint32_t vendor_id,
                                            std::uint32_t device_id, std::uint32_t driver_version) {
  return directory / std::format("rate-layers-v{}-{:04x}-{:04x}-{:08x}.txt", probe_version, vendor_id, device_id,
                                 driver_version);
}
RateLayerProbe cached_rate_layer_probe(const std::filesystem::path& file) {
  try {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec)) return RateLayerProbe::unavailable;
    const auto text = paths::read(file, 64);
    if (text.starts_with("next-layer")) return RateLayerProbe::next_layer;
    if (text.starts_with("same-layer")) return RateLayerProbe::same_layer;
  } catch (...) {
  }
  return RateLayerProbe::unavailable;
}
void cache_rate_layer_probe(const std::filesystem::path& file, RateLayerProbe result) {
  if (result == RateLayerProbe::unavailable) return;
  try {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    paths::atomic_write(file, result == RateLayerProbe::next_layer ? "next-layer\n" : "same-layer\n");
  } catch (const std::exception& e) {
    log::warn("vulkan", "Could not keep the shading-rate layer probe result: {}", e.what());
  } catch (...) {
  }
}
}  // namespace venture::vulkan

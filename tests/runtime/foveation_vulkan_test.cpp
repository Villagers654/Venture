// Foveation on a real GPU: a game's eye pass, and a stereo scene pass into
// the game's own image (multiview with depth), created and begun through the
// functions foveation intercepts, shade their middles at full rate and their
// edges in coarse fragments. Runs where VENTURE_FOVEATION=fixed is set for the
// whole process (ctest's gfx_foveation) and the GPU has shading-rate
// attachments; elsewhere it reports why and passes.
#include <doctest/doctest.h>

#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include "gfx/foveation_vulkan.h"
#include "platform/gfx_library.h"

namespace {
namespace vert {
#include "foveation_vulkan.vert.inc"
}
namespace frag {
#include "foveation_vulkan.frag.inc"
}
namespace fov = venture::gfx::foveation;

constexpr std::uint32_t kSize = 256;

struct Gpu {
  PFN_vkGetInstanceProcAddr gipa = nullptr;
  PFN_vkGetDeviceProcAddr gdpa = nullptr;
  VkInstance instance{};
  VkPhysicalDevice physical{};
  VkDevice device{};
  std::uint32_t family = 0;
  bool multiview = false;
  VkPhysicalDeviceMemoryProperties memory{};
  template <class F>
  F instance_fn(const char* name) const {
    return reinterpret_cast<F>(gipa(instance, name));
  }
  // The intercepted function where foveation has one, else the driver's.
  template <class F>
  F device_fn(const char* name) const {
    if (auto f = fov::proc(name)) return reinterpret_cast<F>(f);
    return reinterpret_cast<F>(gdpa(device, name));
  }
  std::uint32_t memory_type(std::uint32_t bits, VkMemoryPropertyFlags flags) const {
    for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i)
      if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & flags) == flags) return i;
    return UINT32_MAX;
  }
};

// Creates the instance and a device with foveation's additions; false (with a
// message) where the machine cannot run the case.
bool open(Gpu& g) {
  auto* library = venture::platform::open_graphics_library(venture::platform::vulkan_library_name());
  g.gipa = library ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(
                         venture::platform::graphics_symbol(library, "vkGetInstanceProcAddr"))
                   : nullptr;
  if (!g.gipa) {
    MESSAGE("no Vulkan loader");
    return false;
  }
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.apiVersion = VK_API_VERSION_1_2;
  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  if (reinterpret_cast<PFN_vkCreateInstance>(g.gipa(nullptr, "vkCreateInstance"))(&ici, nullptr, &g.instance) !=
      VK_SUCCESS) {
    MESSAGE("no Vulkan instance");
    return false;
  }
  std::uint32_t count = 0;
  g.instance_fn<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices")(g.instance, &count, nullptr);
  std::vector<VkPhysicalDevice> physicals(count);
  g.instance_fn<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices")(g.instance, &count, physicals.data());
  const auto properties = g.instance_fn<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties");
  const auto families =
      g.instance_fn<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties");
  const auto features2 = g.instance_fn<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2");
  for (const auto physical : physicals) {
    VkPhysicalDeviceProperties p{};
    properties(physical, &p);
    if (p.apiVersion < VK_API_VERSION_1_2 || p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) continue;
    VkPhysicalDeviceFragmentShadingRateFeaturesKHR rates{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR};
    VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &rates};
    features2(physical, &query);
    if (!rates.attachmentFragmentShadingRate) continue;
    std::uint32_t n = 0;
    families(physical, &n, nullptr);
    std::vector<VkQueueFamilyProperties> list(n);
    families(physical, &n, list.data());
    for (std::uint32_t i = 0; i < n; ++i)
      if (list[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
        g.physical = physical;
        g.family = i;
        break;
      }
    if (g.physical) break;
  }
  if (!g.physical) {
    MESSAGE("no GPU with shading-rate attachments");
    return false;
  }
  g.instance_fn<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(g.physical,
                                                                                               &g.memory);
  const float priority = 1;
  VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  queue.queueFamilyIndex = g.family;
  queue.queueCount = 1;
  queue.pQueuePriorities = &priority;
  VkPhysicalDeviceVulkan11Features v11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
  {
    VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &v11};
    features2(g.physical, &query);
    g.multiview = v11.multiview;
    v11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    v11.multiview = g.multiview;
  }
  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.pNext = &v11;
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &queue;
  fov::DeviceAdditions added;
  fov::add_to_device(g.gipa, g.instance, g.physical, dci, added);
  REQUIRE(added.added);
  REQUIRE(g.instance_fn<PFN_vkCreateDevice>("vkCreateDevice")(g.physical, &added.info, nullptr, &g.device) ==
          VK_SUCCESS);
  g.gdpa = g.instance_fn<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
  fov::register_device(g.device, g.physical, g.gipa, g.instance, g.gdpa, added.added);
  REQUIRE(fov::device_active(g.device));
  return true;
}

VkDeviceMemory bind(const Gpu& g, VkMemoryRequirements r, VkMemoryPropertyFlags flags) {
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, r.size,
                          g.memory_type(r.memoryTypeBits, flags)};
  REQUIRE(ai.memoryTypeIndex != UINT32_MAX);
  VkDeviceMemory memory{};
  REQUIRE(g.device_fn<PFN_vkAllocateMemory>("vkAllocateMemory")(g.device, &ai, nullptr, &memory) == VK_SUCCESS);
  return memory;
}

// `scene`: a multiview pass over two layers of the game's own image, with
// depth, shaped by the frame's eye views; else a pass into an eye image.
void run(bool scene, bool game_density = false) {
  if (!fov::wanted()) {
    MESSAGE("VENTURE_FOVEATION=fixed is not set for this process (ctest's gfx_foveation runs it)");
    return;
  }
  Gpu g;
  if (!open(g)) return;
  if (scene && !g.multiview) {
    MESSAGE("no multiview");
    fov::release_device(g.device);
    g.device_fn<PFN_vkDestroyDevice>("vkDestroyDevice")(g.device, nullptr);
    g.instance_fn<PFN_vkDestroyInstance>("vkDestroyInstance")(g.instance, nullptr);
    return;
  }
  const auto dev = g.device;
  const std::uint32_t layers = scene ? 2 : 1;

  // The game's eye image (one layer of an XR swapchain), or its own stereo target.
  VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ici.imageType = VK_IMAGE_TYPE_2D;
  ici.format = VK_FORMAT_R32_UINT;
  ici.extent = {kSize, kSize, 1};
  ici.mipLevels = 1;
  ici.arrayLayers = layers;
  ici.samples = VK_SAMPLE_COUNT_1_BIT;
  ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  VkImage image{};
  REQUIRE(g.device_fn<PFN_vkCreateImage>("vkCreateImage")(dev, &ici, nullptr, &image) == VK_SUCCESS);
  VkMemoryRequirements req{};
  g.device_fn<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(dev, image, &req);
  const auto image_memory = bind(g, req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  REQUIRE(g.device_fn<PFN_vkBindImageMemory>("vkBindImageMemory")(dev, image, image_memory, 0) == VK_SUCCESS);
  // The scene pass's depth.
  VkImage depth{};
  VkDeviceMemory depth_memory{};
  VkImageView depth_view{};
  if (scene) {
    auto dci = ici;
    dci.format = VK_FORMAT_D32_SFLOAT;
    dci.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    REQUIRE(g.device_fn<PFN_vkCreateImage>("vkCreateImage")(dev, &dci, nullptr, &depth) == VK_SUCCESS);
    g.device_fn<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(dev, depth, &req);
    depth_memory = bind(g, req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    REQUIRE(g.device_fn<PFN_vkBindImageMemory>("vkBindImageMemory")(dev, depth, depth_memory, 0) == VK_SUCCESS);
    VkImageViewCreateInfo dvci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    dvci.image = depth;
    dvci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    dvci.format = VK_FORMAT_D32_SFLOAT;
    dvci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 2};
    REQUIRE(g.device_fn<PFN_vkCreateImageView>("vkCreateImageView")(dev, &dvci, nullptr, &depth_view) ==
            VK_SUCCESS);
  }

  int swapchain_key = 0;
  const fov::EyeImage eye{image, 0};
  // A symmetric 92-degree view, fixed High: full rate within 32 degrees of the
  // centre, quarter rate (2x2) beyond 44, which the corners are.
  fov::EyeView view;
  view.width = view.height = kSize;
  view.fov = {-0.8f, 0.8f, 0.8f, -0.8f};
  if (scene) {
    // The frame's eyes (in eye images elsewhere) shape the game's own target.
    fov::update_scene_views(view, view, fov::Level::high, 0, false);
  } else {
    fov::register_eye_swapchain(dev, &swapchain_key, {&eye, 1}, kSize, kSize, 1, false);
    fov::update_eye_views(&swapchain_key, {&view, 1}, fov::Level::high, 0, false);
  }

  // The game's pass, view and framebuffer, through foveation's functions.
  VkAttachmentDescription colour{};
  colour.format = VK_FORMAT_R32_UINT;
  colour.samples = VK_SAMPLE_COUNT_1_BIT;
  colour.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  colour.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  colour.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  colour.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  colour.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  colour.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  VkAttachmentDescription depth_attachment = colour;
  depth_attachment.format = VK_FORMAT_D32_SFLOAT;
  depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  depth_attachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  const VkAttachmentDescription attachments[2] = {colour, depth_attachment};
  VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkAttachmentReference depth_reference{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &reference;
  if (scene) subpass.pDepthStencilAttachment = &depth_reference;
  const std::uint32_t view_mask = 3;
  VkRenderPassMultiviewCreateInfo multiview{VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO};
  multiview.subpassCount = 1;
  multiview.pViewMasks = &view_mask;
  multiview.correlationMaskCount = 1;
  multiview.pCorrelationMasks = &view_mask;
  VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  if (scene) rpci.pNext = &multiview;
  // A game's own density map (Unity's Quest eye passes) gives way to Venture's
  // shading-rate attachment; the layer drops it before the driver sees it.
  VkRenderPassFragmentDensityMapCreateInfoEXT density{VK_STRUCTURE_TYPE_RENDER_PASS_FRAGMENT_DENSITY_MAP_CREATE_INFO_EXT};
  density.fragmentDensityMapAttachment = {VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED};
  if (game_density) {
    density.pNext = rpci.pNext;
    rpci.pNext = &density;
  }
  rpci.attachmentCount = scene ? 2 : 1;
  rpci.pAttachments = attachments;
  rpci.subpassCount = 1;
  rpci.pSubpasses = &subpass;
  VkRenderPass pass{};
  REQUIRE(g.device_fn<PFN_vkCreateRenderPass>("vkCreateRenderPass")(dev, &rpci, nullptr, &pass) == VK_SUCCESS);
  VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vci.image = image;
  vci.viewType = scene ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
  vci.format = VK_FORMAT_R32_UINT;
  vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
  VkImageView image_view{};
  REQUIRE(g.device_fn<PFN_vkCreateImageView>("vkCreateImageView")(dev, &vci, nullptr, &image_view) == VK_SUCCESS);
  VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  fci.renderPass = pass;
  const VkImageView framebuffer_views[2] = {image_view, depth_view};
  fci.attachmentCount = scene ? 2 : 1;
  fci.pAttachments = framebuffer_views;
  fci.width = fci.height = kSize;
  fci.layers = 1;
  VkFramebuffer framebuffer{};
  REQUIRE(g.device_fn<PFN_vkCreateFramebuffer>("vkCreateFramebuffer")(dev, &fci, nullptr, &framebuffer) ==
          VK_SUCCESS);

  // The pipeline, with foveation's pipeline state.
  auto module = [&](const std::uint32_t* code, std::size_t bytes) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0, bytes, code};
    VkShaderModule m{};
    REQUIRE(g.device_fn<PFN_vkCreateShaderModule>("vkCreateShaderModule")(dev, &ci, nullptr, &m) == VK_SUCCESS);
    return m;
  };
  const auto vs = module(vert::decode_shader, sizeof(vert::decode_shader));
  const auto fs = module(frag::decode_shader, sizeof(frag::decode_shader));
  VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  VkPipelineLayout layout{};
  REQUIRE(g.device_fn<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(dev, &lci, nullptr, &layout) ==
          VK_SUCCESS);
  VkPipelineShaderStageCreateInfo stages[2]{
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main"},
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main"}};
  VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  const VkViewport viewport{0, 0, float(kSize), float(kSize), 0, 1};
  const VkRect2D scissor{{0, 0}, {kSize, kSize}};
  VkPipelineViewportStateCreateInfo viewports{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewports.viewportCount = viewports.scissorCount = 1;
  viewports.pViewports = &viewport;
  viewports.pScissors = &scissor;
  VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  raster.cullMode = VK_CULL_MODE_NONE;
  raster.lineWidth = 1;
  VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineDepthStencilStateCreateInfo depth_state{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  VkPipelineColorBlendAttachmentState blend{};
  blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT;
  VkPipelineColorBlendStateCreateInfo blending{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  blending.attachmentCount = 1;
  blending.pAttachments = &blend;
  VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  pci.stageCount = 2;
  pci.pStages = stages;
  pci.pVertexInputState = &vertex;
  pci.pInputAssemblyState = &assembly;
  pci.pViewportState = &viewports;
  pci.pRasterizationState = &raster;
  pci.pMultisampleState = &multisample;
  pci.pColorBlendState = &blending;
  if (scene) pci.pDepthStencilState = &depth_state;
  pci.layout = layout;
  pci.renderPass = pass;
  fov::PipelineStorage storage;
  fov::patch_pipelines(dev, {&pci, 1}, storage);
  REQUIRE(storage.states.size() == 1);
  VkPipeline pipeline{};
  REQUIRE(g.device_fn<PFN_vkCreateGraphicsPipelines>("vkCreateGraphicsPipelines")(dev, VK_NULL_HANDLE, 1, &pci,
                                                                                  nullptr, &pipeline) == VK_SUCCESS);

  // A readback buffer.
  VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = VkDeviceSize(kSize) * kSize * 4 * layers;
  bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  VkBuffer buffer{};
  REQUIRE(g.device_fn<PFN_vkCreateBuffer>("vkCreateBuffer")(dev, &bci, nullptr, &buffer) == VK_SUCCESS);
  g.device_fn<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(dev, buffer, &req);
  const auto buffer_memory =
      bind(g, req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  REQUIRE(g.device_fn<PFN_vkBindBufferMemory>("vkBindBufferMemory")(dev, buffer, buffer_memory, 0) == VK_SUCCESS);

  // One frame: the pass (foveation records its map upload before it), then the copy.
  VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, 0, g.family};
  VkCommandPool pool{};
  REQUIRE(g.device_fn<PFN_vkCreateCommandPool>("vkCreateCommandPool")(dev, &cpci, nullptr, &pool) == VK_SUCCESS);
  VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool,
                                   VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
  VkCommandBuffer command{};
  REQUIRE(g.device_fn<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(dev, &cbai, &command) ==
          VK_SUCCESS);
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  REQUIRE(g.device_fn<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(command, &begin) == VK_SUCCESS);
  VkClearValue clears[2]{};
  clears[0].color.uint32[0] = 0xffffffffu;
  clears[1].depthStencil.depth = 1;
  VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  rpbi.renderPass = pass;
  rpbi.framebuffer = framebuffer;
  rpbi.renderArea = scissor;
  rpbi.clearValueCount = scene ? 2 : 1;
  rpbi.pClearValues = clears;
  g.device_fn<PFN_vkCmdBeginRenderPass>("vkCmdBeginRenderPass")(command, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
  g.device_fn<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  g.device_fn<PFN_vkCmdDraw>("vkCmdDraw")(command, 3, 1, 0, 0);
  g.device_fn<PFN_vkCmdEndRenderPass>("vkCmdEndRenderPass")(command);
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers};
  region.imageExtent = {kSize, kSize, 1};
  g.device_fn<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(
      command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
  REQUIRE(g.device_fn<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(command) == VK_SUCCESS);
  VkQueue queue{};
  g.device_fn<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(dev, g.family, 0, &queue);
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &command;
  REQUIRE(g.device_fn<PFN_vkQueueSubmit>("vkQueueSubmit")(queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS);
  REQUIRE(g.device_fn<PFN_vkQueueWaitIdle>("vkQueueWaitIdle")(queue) == VK_SUCCESS);

  void* mapped = nullptr;
  REQUIRE(g.device_fn<PFN_vkMapMemory>("vkMapMemory")(dev, buffer_memory, 0, VK_WHOLE_SIZE, 0, &mapped) ==
          VK_SUCCESS);
  std::vector<std::uint32_t> all(std::size_t(kSize) * kSize * layers);
  std::memcpy(all.data(), mapped, all.size() * 4);
  const auto own = [](std::uint32_t x, std::uint32_t y) { return x | (y << 16); };
  for (std::uint32_t layer = 0; layer < layers; ++layer) {
  CAPTURE(layer);
  const std::span<const std::uint32_t> pixels(all.data() + std::size_t(kSize) * kSize * layer,
                                              std::size_t(kSize) * kSize);
  const auto at = [&](std::uint32_t x, std::uint32_t y) { return pixels[std::size_t(y) * kSize + x]; };
  // Every pixel was drawn.
  std::size_t coarse = 0;
  for (std::uint32_t y = 0; y < kSize; ++y)
    for (std::uint32_t x = 0; x < kSize; ++x) {
      REQUIRE(at(x, y) != 0xffffffffu);
      coarse += at(x, y) != own(x, y);
    }
  // The middle at full rate: each pixel shaded on its own.
  for (std::uint32_t y = kSize / 2 - 8; y < kSize / 2 + 8; ++y)
    for (std::uint32_t x = kSize / 2 - 8; x < kSize / 2 + 8; ++x) CHECK(at(x, y) == own(x, y));
  // A corner in 2x2 fragments: four pixels, one shading.
  CHECK(at(0, 0) == at(1, 0));
  CHECK(at(0, 0) == at(0, 1));
  CHECK(at(0, 0) == at(1, 1));
  CHECK(at(kSize - 1, kSize - 1) == at(kSize - 2, kSize - 2));
  // A good share of the image is coarse (High shades about half the fragments).
  MESSAGE("pixels shaded with a neighbour: " << coarse * 100 / pixels.size() << "%");
  CHECK(coarse * 4 > pixels.size());
  }

  g.device_fn<PFN_vkDeviceWaitIdle>("vkDeviceWaitIdle")(dev);
  if (!scene) fov::forget_eye_swapchain(dev, &swapchain_key);
  if (scene) {
    g.device_fn<PFN_vkDestroyImageView>("vkDestroyImageView")(dev, depth_view, nullptr);
    g.device_fn<PFN_vkDestroyImage>("vkDestroyImage")(dev, depth, nullptr);
    g.device_fn<PFN_vkFreeMemory>("vkFreeMemory")(dev, depth_memory, nullptr);
  }
  g.device_fn<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(dev, pool, nullptr);
  g.device_fn<PFN_vkDestroyPipeline>("vkDestroyPipeline")(dev, pipeline, nullptr);
  g.device_fn<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout")(dev, layout, nullptr);
  g.device_fn<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(dev, vs, nullptr);
  g.device_fn<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(dev, fs, nullptr);
  g.device_fn<PFN_vkDestroyFramebuffer>("vkDestroyFramebuffer")(dev, framebuffer, nullptr);
  g.device_fn<PFN_vkDestroyImageView>("vkDestroyImageView")(dev, image_view, nullptr);
  g.device_fn<PFN_vkDestroyRenderPass>("vkDestroyRenderPass")(dev, pass, nullptr);
  g.device_fn<PFN_vkDestroyBuffer>("vkDestroyBuffer")(dev, buffer, nullptr);
  g.device_fn<PFN_vkFreeMemory>("vkFreeMemory")(dev, buffer_memory, nullptr);
  g.device_fn<PFN_vkDestroyImage>("vkDestroyImage")(dev, image, nullptr);
  g.device_fn<PFN_vkFreeMemory>("vkFreeMemory")(dev, image_memory, nullptr);
  fov::release_device(dev);
  g.device_fn<PFN_vkDestroyDevice>("vkDestroyDevice")(dev, nullptr);
  g.instance_fn<PFN_vkDestroyInstance>("vkDestroyInstance")(g.instance, nullptr);
}
// A GPU with density maps (the Steam Frame's Adreno): the swapchain's
// XR_FB_foveation_vulkan map takes Venture's pattern, uploaded before the next
// pass begins. The map's layout here is TRANSFER_SRC (this GPU need not have
// density maps); the runtime's is FRAGMENT_DENSITY_MAP_OPTIMAL.
void run_density_map() {
  if (!fov::wanted()) {
    MESSAGE("VENTURE_FOVEATION=fixed is not set for this process (ctest's gfx_foveation runs it)");
    return;
  }
  Gpu g;
  if (!open(g)) return;
  const auto dev = g.device;
  fov::register_device(dev, g.physical, g.gipa, g.instance, g.gdpa, true, true);
  auto make_image = [&](VkFormat format, std::uint32_t size, VkImageUsageFlags usage, VkDeviceMemory& memory) {
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = {size, size, 1};
    ici.mipLevels = ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.usage = usage;
    VkImage image{};
    REQUIRE(g.device_fn<PFN_vkCreateImage>("vkCreateImage")(dev, &ici, nullptr, &image) == VK_SUCCESS);
    VkMemoryRequirements req{};
    g.device_fn<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(dev, image, &req);
    memory = bind(g, req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    REQUIRE(g.device_fn<PFN_vkBindImageMemory>("vkBindImageMemory")(dev, image, memory, 0) == VK_SUCCESS);
    return image;
  };
  constexpr std::uint32_t kTexels = 16;
  VkDeviceMemory eye_memory{}, density_memory{};
  const auto eye_image = make_image(VK_FORMAT_R8G8B8A8_UNORM, kSize, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, eye_memory);
  const auto density = make_image(VK_FORMAT_R8G8_UNORM, kTexels,
                                  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, density_memory);
  int swapchain_key = 0;
  const fov::EyeImage eye{eye_image, 0};
  fov::register_eye_swapchain(dev, &swapchain_key, {&eye, 1}, kSize, kSize, 1, false);
  fov::attach_density_map(dev, &swapchain_key, density, kTexels, kTexels, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  fov::EyeView view;
  view.width = view.height = kSize;
  view.fov = {-0.8f, 0.8f, 0.8f, -0.8f};
  fov::update_eye_views(&swapchain_key, {&view, 1}, fov::Level::high, 0, false);

  // Any pass: the upload is recorded before it begins.
  VkAttachmentDescription colour{};
  colour.format = VK_FORMAT_R8G8B8A8_UNORM;
  colour.samples = VK_SAMPLE_COUNT_1_BIT;
  colour.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  colour.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  colour.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  colour.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  colour.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &reference;
  VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  rpci.attachmentCount = 1;
  rpci.pAttachments = &colour;
  rpci.subpassCount = 1;
  rpci.pSubpasses = &subpass;
  VkRenderPass pass{};
  REQUIRE(g.device_fn<PFN_vkCreateRenderPass>("vkCreateRenderPass")(dev, &rpci, nullptr, &pass) == VK_SUCCESS);
  VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vci.image = eye_image;
  vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vci.format = VK_FORMAT_R8G8B8A8_UNORM;
  vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkImageView eye_view{};
  REQUIRE(g.device_fn<PFN_vkCreateImageView>("vkCreateImageView")(dev, &vci, nullptr, &eye_view) == VK_SUCCESS);
  VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  fci.renderPass = pass;
  fci.attachmentCount = 1;
  fci.pAttachments = &eye_view;
  fci.width = fci.height = kSize;
  fci.layers = 1;
  VkFramebuffer framebuffer{};
  REQUIRE(g.device_fn<PFN_vkCreateFramebuffer>("vkCreateFramebuffer")(dev, &fci, nullptr, &framebuffer) ==
          VK_SUCCESS);
  VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = VkDeviceSize(kTexels) * kTexels * 2;
  bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  VkBuffer buffer{};
  REQUIRE(g.device_fn<PFN_vkCreateBuffer>("vkCreateBuffer")(dev, &bci, nullptr, &buffer) == VK_SUCCESS);
  VkMemoryRequirements req{};
  g.device_fn<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(dev, buffer, &req);
  const auto buffer_memory = bind(g, req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  REQUIRE(g.device_fn<PFN_vkBindBufferMemory>("vkBindBufferMemory")(dev, buffer, buffer_memory, 0) == VK_SUCCESS);

  VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, 0, g.family};
  VkCommandPool pool{};
  REQUIRE(g.device_fn<PFN_vkCreateCommandPool>("vkCreateCommandPool")(dev, &cpci, nullptr, &pool) == VK_SUCCESS);
  VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool,
                                   VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
  VkCommandBuffer command{};
  REQUIRE(g.device_fn<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(dev, &cbai, &command) ==
          VK_SUCCESS);
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  REQUIRE(g.device_fn<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(command, &begin) == VK_SUCCESS);
  VkClearValue clear{};
  VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  rpbi.renderPass = pass;
  rpbi.framebuffer = framebuffer;
  rpbi.renderArea = {{0, 0}, {kSize, kSize}};
  rpbi.clearValueCount = 1;
  rpbi.pClearValues = &clear;
  g.device_fn<PFN_vkCmdBeginRenderPass>("vkCmdBeginRenderPass")(command, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
  g.device_fn<PFN_vkCmdEndRenderPass>("vkCmdEndRenderPass")(command);
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {kTexels, kTexels, 1};
  g.device_fn<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(
      command, density, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
  REQUIRE(g.device_fn<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(command) == VK_SUCCESS);
  VkQueue queue{};
  g.device_fn<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(dev, g.family, 0, &queue);
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &command;
  REQUIRE(g.device_fn<PFN_vkQueueSubmit>("vkQueueSubmit")(queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS);
  REQUIRE(g.device_fn<PFN_vkQueueWaitIdle>("vkQueueWaitIdle")(queue) == VK_SUCCESS);

  void* mapped = nullptr;
  REQUIRE(g.device_fn<PFN_vkMapMemory>("vkMapMemory")(dev, buffer_memory, 0, VK_WHOLE_SIZE, 0, &mapped) ==
          VK_SUCCESS);
  std::vector<std::uint8_t> got(std::size_t(kTexels) * kTexels * 2), expected(got.size());
  std::memcpy(got.data(), mapped, got.size());
  // Venture's level (Medium), fixed, whatever the game asked.
  fov::fill_densities(expected, kTexels, kTexels, fov::Pattern{fov::Level::medium, false, 0}, view.fov, {});
  CHECK(got == expected);
  const auto at = [&](std::uint32_t x, std::uint32_t y, int c) { return got[(std::size_t(y) * kTexels + x) * 2 + c]; };
  CHECK(at(kTexels / 2, kTexels / 2, 0) == 255);  // full density in the middle
  CHECK(at(0, 0, 0) < 255);                       // coarser at the corner
  CHECK(at(0, 0, 1) < 255);

  g.device_fn<PFN_vkDeviceWaitIdle>("vkDeviceWaitIdle")(dev);
  fov::forget_eye_swapchain(dev, &swapchain_key);
  g.device_fn<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(dev, pool, nullptr);
  g.device_fn<PFN_vkDestroyFramebuffer>("vkDestroyFramebuffer")(dev, framebuffer, nullptr);
  g.device_fn<PFN_vkDestroyImageView>("vkDestroyImageView")(dev, eye_view, nullptr);
  g.device_fn<PFN_vkDestroyRenderPass>("vkDestroyRenderPass")(dev, pass, nullptr);
  g.device_fn<PFN_vkDestroyBuffer>("vkDestroyBuffer")(dev, buffer, nullptr);
  g.device_fn<PFN_vkFreeMemory>("vkFreeMemory")(dev, buffer_memory, nullptr);
  g.device_fn<PFN_vkDestroyImage>("vkDestroyImage")(dev, density, nullptr);
  g.device_fn<PFN_vkFreeMemory>("vkFreeMemory")(dev, density_memory, nullptr);
  g.device_fn<PFN_vkDestroyImage>("vkDestroyImage")(dev, eye_image, nullptr);
  g.device_fn<PFN_vkFreeMemory>("vkFreeMemory")(dev, eye_memory, nullptr);
  fov::release_device(dev);
  g.device_fn<PFN_vkDestroyDevice>("vkDestroyDevice")(dev, nullptr);
  g.instance_fn<PFN_vkDestroyInstance>("vkDestroyInstance")(g.instance, nullptr);
}
}  // namespace

TEST_CASE("Foveation shades an eye pass's edges in coarse fragments on the GPU") { run(false); }
TEST_CASE("Foveation shades a stereo scene pass into the game's own image on the GPU") { run(true); }
TEST_CASE("Foveation replaces a game's own density map on an eye pass") { run(false, true); }
TEST_CASE("Foveation fills a swapchain's density map with its pattern on a GPU with density maps") {
  run_density_map();
}

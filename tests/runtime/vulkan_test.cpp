#include "common/environment.h"
#include "hle/vulkan/vulkan.h"
#include "unorm_test_support.h"

#include <doctest/doctest.h>
#include <vulkan/vulkan_android.h>

#include <array>
#include <algorithm>
#include <bit>
#include <cstring>
#include <cstdlib>
#include <condition_variable>
#include <chrono>
#include <future>
#include <filesystem>
#include <fstream>
#include "common/log.h"
#include "common/json.h"
#include <string_view>
#include <thread>
#include <vector>

#include "core/linker.h"
#include "android/hardware_buffer.h"
#include "android/native_window.h"
#include "fake_cpu.h"
#include "gfx/gpu_device.h"
#include "gfx/queue_sync.h"
#include "hle/vulkan/callbacks.h"
#include "hle/vulkan/chain.h"
#include "hle/vulkan/external.h"
#include "hle/vulkan/internal.h"
#include "hle/vulkan/timing.h"
#include "hle/vulkan/sync_trace.h"
#include "platform/profile.h"
#include "vulkan_callbacks_test_support.h"
#include "vulkan_test_support.h"
#include "hle/libc/vfs.h"

#include "../async_wait.h"

using namespace venture;
namespace {
template <class T>
std::uint64_t word(T value) {
  if constexpr (std::is_null_pointer_v<T>)
    return 0;
  else if constexpr (std::is_pointer_v<T>)
    return reinterpret_cast<std::uint64_t>(value);
  else
    return static_cast<std::uint64_t>(value);
}
template <class... A>
std::uint64_t call(const char* name, A... args) {
  return test::vk_call(name, args...);
}
}  // namespace

TEST_CASE("Vulkan registry has core 1.4, aliases and filters unsafe native ABIs") {
  CHECK(vulkan::command_count() > 600);
  CHECK(vulkan::command_handler("vkCmdDraw"));
  CHECK(vulkan::command_handler("vkQueueSubmit2"));
  CHECK(vulkan::command_handler("vkQueueSubmit2KHR"));
  CHECK(vulkan::command_handler("vkCmdBindDescriptorSets2"));
  CHECK(bool(vulkan::command_handler("vkGetMemoryFdKHR")) ==
        vulkan::extension_translatable("VK_KHR_external_memory_fd"));
  // Android hardware buffers are translated; other Android paths are not.
  CHECK(vulkan::command_handler("vkGetAndroidHardwareBufferPropertiesANDROID"));
  CHECK(vulkan::extension_translatable("VK_ANDROID_external_memory_android_hardware_buffer"));
  CHECK_FALSE(vulkan::extension_translatable("VK_ANDROID_external_format_resolve"));
  CHECK_FALSE(vulkan::extension_translatable("VK_QCOM_fragment_density_map_offset"));
  CHECK_FALSE(vulkan::extension_translatable("VK_GOOGLE_display_timing"));
  CHECK(vulkan::instance_proc(nullptr, "vkCreateDevice") == 0);
  CHECK(vulkan::instance_proc(nullptr, "vkNotReal") == 0);
  auto library = vulkan::create_library();
  auto thunk = library->find_symbol("vkCreateInstance");
  CHECK(thunk != 0);
  CHECK(test::vulkan_thunk_info(thunk).handler == vulkan::command_handler("vkCreateInstance"));
}

TEST_CASE("Vulkan chain copies preserve links, keep Android buffer nodes and reject host memory and cycles") {
  VkPhysicalDeviceVulkan11Features f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
  VkExternalFormatANDROID ext{VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID, &f, 0};
  VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &ext};
  vulkan::Chain chain;
  auto* copy = static_cast<const VkDeviceCreateInfo*>(chain.translate(&ci));
  REQUIRE(copy);
  CHECK(copy != &ci);
  CHECK(static_cast<const VkBaseInStructure*>(copy->pNext)->sType == f.sType);  // zero external format: no-op
  CHECK(ci.pNext == &ext);
  // Nonzero external formats and AHB imports reach the hardware-buffer overrides.
  ext.externalFormat = 123;
  vulkan::Chain external;
  copy = static_cast<const VkDeviceCreateInfo*>(external.translate(&ci));
  REQUIRE(copy);
  auto* format = static_cast<const VkExternalFormatANDROID*>(copy->pNext);
  CHECK(format->sType == VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID);
  CHECK(format->externalFormat == 123);
  CHECK(format != &ext);
  VkImportAndroidHardwareBufferInfoANDROID ahb{VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID};
  vulkan::Chain import;
  CHECK(import.translate(&ahb) != nullptr);
  VkImportMemoryHostPointerInfoEXT host{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
  vulkan::Chain host_import;
  CHECK(host_import.translate(&host) == nullptr);
  f.pNext = &f;
  vulkan::Chain cycle;
  CHECK(cycle.translate(&f) == nullptr);
  VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, &host};
  ci.pNext = nullptr;
  ci.queueCreateInfoCount = 1;
  ci.pQueueCreateInfos = &queue;
  vulkan::Chain nested;
  CHECK(nested.translate(&ci) == nullptr);
  CHECK(nested.result() == VK_ERROR_EXTENSION_NOT_PRESENT);
  CHECK(ci.pQueueCreateInfos == &queue);
}

TEST_CASE("Vulkan allocation and debug callbacks adopt driver threads and preserve guest arguments") {
  std::array<std::uint64_t, 8> observed{};
  GuestAddr called{};
  test::set_vulkan_callback([&](GuestAddr fn, const GuestCallArgs& args) {
    called = fn;
    std::copy(args.integers.begin(), args.integers.end(), observed.begin());
    return GuestCallResult{0x4000};
  });
  VkAllocationCallbacks guest{reinterpret_cast<void*>(0x123),
                              reinterpret_cast<PFN_vkAllocationFunction>(test::vulkan_callback_thunk(0x100, 4)),
                              reinterpret_cast<PFN_vkReallocationFunction>(test::vulkan_callback_thunk(0x200, 5)),
                              reinterpret_cast<PFN_vkFreeFunction>(test::vulkan_callback_thunk(0x300, 2)),
                              nullptr,
                              nullptr};
  auto* host = vulkan::translate_allocator(&guest);
  std::thread driver([&] {
    CHECK(host->pfnAllocation(host->pUserData, 8192, 64, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE) ==
          reinterpret_cast<void*>(0x4000));
  });
  driver.join();
  CHECK(called == 0x100);
  CHECK(observed[0] == 0x123);
  CHECK(observed[1] == 8192);
  CHECK(observed[2] == 64);
  CHECK(vulkan::translate_allocator(&guest) == host);
  VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
  debug.pfnUserCallback = reinterpret_cast<PFN_vkDebugUtilsMessengerCallbackEXT>(test::vulkan_callback_thunk(0x500, 4));
  debug.pUserData = reinterpret_cast<void*>(0x777);
  vulkan::Chain chain;
  auto* copy = static_cast<const VkDebugUtilsMessengerCreateInfoEXT*>(chain.translate(&debug));
  REQUIRE(copy);
  copy->pfnUserCallback(VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
                        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT, nullptr, copy->pUserData);
  CHECK(called == 0x500);
  CHECK(observed[3] == 0x777);
  test::set_vulkan_callback({});
}

TEST_SUITE_BEGIN("gpu");
TEST_CASE("Vulkan queries preserve unknown nodes and fill known feature and property chains") {
  test::VulkanDevice gpu;
  struct Unknown {
    VkStructureType sType = static_cast<VkStructureType>(0x7ffffffe);
    void* pNext = nullptr;
    std::array<std::uint64_t, 4> payload{0xdeadbeef, 0x12345678, 0xaabbccdd, 0x98765432};
  } unknown;
  VkPhysicalDeviceMultiviewFeatures multiview{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES};
  unknown.pNext = &multiview;
  const auto original = unknown;
  VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &unknown};
  call("vkGetPhysicalDeviceFeatures2", gpu.physical, &features);
  VkPhysicalDeviceMultiviewFeatures native_multiview{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES};
  VkPhysicalDeviceFeatures2 native_features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &native_multiview};
  reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
      vulkan::raw_physical_proc(gpu.physical, "vkGetPhysicalDeviceFeatures2"))(gpu.physical, &native_features);
  CHECK(multiview.multiview == native_multiview.multiview);
  CHECK(multiview.multiviewGeometryShader == native_multiview.multiviewGeometryShader);
  CHECK(multiview.multiviewTessellationShader == native_multiview.multiviewTessellationShader);
  CHECK(unknown.sType == original.sType);
  CHECK(unknown.pNext == original.pNext);
  CHECK(unknown.payload == original.payload);
  CHECK(features.features.textureCompressionASTC_LDR == VK_TRUE);

  VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
  unknown.pNext = &id;
  VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &unknown};
  call("vkGetPhysicalDeviceProperties2", gpu.physical, &properties);
  VkPhysicalDeviceIDProperties native_id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
  VkPhysicalDeviceProperties2 native_properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &native_id};
  reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
      vulkan::raw_physical_proc(gpu.physical, "vkGetPhysicalDeviceProperties2"))(gpu.physical, &native_properties);
  CHECK(properties.properties.vendorID == native_properties.properties.vendorID);
  CHECK(properties.properties.deviceID == native_properties.properties.deviceID);
  CHECK(std::strcmp(properties.properties.deviceName, native_properties.properties.deviceName) == 0);
  CHECK(std::memcmp(id.deviceUUID, native_id.deviceUUID, VK_UUID_SIZE) == 0);
  CHECK(unknown.pNext == &id);
  CHECK(unknown.payload == original.payload);

  // Input chains follow the same skip rule, without changing guest links.
  unknown.pNext = &multiview;
  VkDeviceCreateInfo create{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &unknown};
  vulkan::Chain chain;
  auto* copy = static_cast<const VkDeviceCreateInfo*>(chain.translate(&create));
  REQUIRE(copy);
  REQUIRE(copy->pNext);
  CHECK(static_cast<const VkBaseInStructure*>(copy->pNext)->sType == multiview.sType);
  CHECK(create.pNext == &unknown);
  CHECK(unknown.pNext == &multiview);
  CHECK(unknown.payload == original.payload);
}

TEST_CASE("Vulkan host GPU instance device queue and command submission through Dynarmic") {
  REQUIRE(gfx::gpu_device().get_instance_proc_addr);
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.apiVersion = VK_API_VERSION_1_2;
  VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ci.pApplicationInfo = &app;
  VkInstance instance{};
  REQUIRE(call("vkCreateInstance", &ci, nullptr, &instance) == VK_SUCCESS);
  std::uint32_t count{};
  REQUIRE(call("vkEnumeratePhysicalDevices", instance, &count, nullptr) == VK_SUCCESS);
  REQUIRE(count == 1);
  VkPhysicalDevice physical{};
  REQUIRE(call("vkEnumeratePhysicalDevices", instance, &count, &physical) == VK_SUCCESS);
  VkPhysicalDeviceProperties properties{};
  call("vkGetPhysicalDeviceProperties", physical, &properties);
  MESSAGE("Host Vulkan GPU: ", properties.deviceName);
  call("vkGetPhysicalDeviceQueueFamilyProperties", physical, &count, nullptr);
  std::vector<VkQueueFamilyProperties> families(count);
  call("vkGetPhysicalDeviceQueueFamilyProperties", physical, &count, families.data());
  std::uint32_t family = 0;
  while (family < count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++family;
  REQUIRE(family < count);
  float priority = 1;
  VkDeviceQueueCreateInfo queue_ci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  queue_ci.queueFamilyIndex = family;
  queue_ci.queueCount = 1;
  queue_ci.pQueuePriorities = &priority;
  VkDeviceCreateInfo device_ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  device_ci.queueCreateInfoCount = 1;
  device_ci.pQueueCreateInfos = &queue_ci;
  VkDevice device{};
  REQUIRE(call("vkCreateDevice", physical, &device_ci, nullptr, &device) == VK_SUCCESS);
  VkQueue queue{};
  call("vkGetDeviceQueue", device, family, 0u, &queue);
  REQUIRE(queue);
  auto binding = vulkan::queue_binding(queue);
  REQUIRE(binding);
  CHECK(binding->instance == instance);
  CHECK(binding->physical == physical);
  CHECK(binding->device == device);
  CHECK(binding->family == family);
  CHECK(binding->index == 0);
  CHECK(call("vkQueueSubmit", queue, 0u, nullptr, VK_NULL_HANDLE) == VK_SUCCESS);
  CHECK(call("vkQueueWaitIdle", queue) == VK_SUCCESS);
  auto thunk = call("vkGetDeviceProcAddr", device, "vkQueueSubmit");
  CHECK(test::vulkan_thunk_info(thunk).handler == vulkan::command_handler("vkQueueSubmit"));
  CHECK(call("vkGetDeviceProcAddr", device, "vkCmdBindDescriptorSets2") == 0);   // device is 1.2
  CHECK(call("vkGetDeviceProcAddr", device, "vkCmdPushDescriptorSetKHR") == 0);  // not enabled
  call("vkDestroyDevice", device, nullptr);
  CHECK_FALSE(vulkan::queue_binding(queue));
  call("vkDestroyInstance", instance, nullptr);
}

namespace {
// Records the framebuffer sizes, render areas and scissors the layer forwards
// to the host.
struct ScissorHooks {
  static inline PFN_vkGetInstanceProcAddr gipa{};
  static inline PFN_vkGetDeviceProcAddr gdpa{};
  static inline PFN_vkCmdSetScissor set_scissor{};
  static inline PFN_vkCreateFramebuffer create_framebuffer{};
  static inline PFN_vkCmdBeginRenderPass begin_pass{};
  static inline PFN_vkCmdPipelineBarrier pipeline_barrier{};
  static inline std::vector<VkRect2D> forwarded, areas;
  static inline std::vector<VkExtent2D> framebuffers;
  static inline std::vector<std::vector<VkImageView>> attachments;
  static inline std::vector<VkFramebuffer> handles, begun;  // created; begun on
  static inline std::vector<VkImageMemoryBarrier> barriers;
  static inline std::vector<std::size_t> barriers_at_begin;
  static inline std::string_view fail_proc;
  static inline VkResult image_failure = VK_ERROR_OUT_OF_DEVICE_MEMORY;
  static inline unsigned fail_call{}, calls{}, images_created{}, views_created{}, memory_allocated{};
  static inline unsigned images_destroyed{}, views_destroyed{}, memory_freed{};
  static bool fail(std::string_view proc) { return proc == fail_proc && ++calls == fail_call; }
  static VkResult VKAPI_CALL on_create_image(VkDevice d, const VkImageCreateInfo* info,
                                              const VkAllocationCallbacks* alloc, VkImage* out) {
    if (fail("vkCreateImage")) return image_failure;
    auto result = reinterpret_cast<PFN_vkCreateImage>(gdpa(d, "vkCreateImage"))(d, info, alloc, out);
    if (result == VK_SUCCESS) ++images_created;
    return result;
  }
  static VkResult VKAPI_CALL on_allocate_memory(VkDevice d, const VkMemoryAllocateInfo* info,
                                                const VkAllocationCallbacks* alloc, VkDeviceMemory* out) {
    if (fail("vkAllocateMemory")) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    auto result = reinterpret_cast<PFN_vkAllocateMemory>(gdpa(d, "vkAllocateMemory"))(d, info, alloc, out);
    if (result == VK_SUCCESS) ++memory_allocated;
    return result;
  }
  static VkResult VKAPI_CALL on_bind_image_memory(VkDevice d, VkImage image, VkDeviceMemory memory,
                                                  VkDeviceSize offset) {
    if (fail("vkBindImageMemory")) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    return reinterpret_cast<PFN_vkBindImageMemory>(gdpa(d, "vkBindImageMemory"))(d, image, memory, offset);
  }
  static VkResult VKAPI_CALL on_create_view(VkDevice d, const VkImageViewCreateInfo* info,
                                            const VkAllocationCallbacks* alloc, VkImageView* out) {
    if (fail("vkCreateImageView")) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    auto result = reinterpret_cast<PFN_vkCreateImageView>(gdpa(d, "vkCreateImageView"))(d, info, alloc, out);
    if (result == VK_SUCCESS) ++views_created;
    return result;
  }
  static void VKAPI_CALL on_destroy_image(VkDevice d, VkImage image, const VkAllocationCallbacks* alloc) {
    if (image) ++images_destroyed;
    reinterpret_cast<PFN_vkDestroyImage>(gdpa(d, "vkDestroyImage"))(d, image, alloc);
  }
  static void VKAPI_CALL on_destroy_view(VkDevice d, VkImageView view, const VkAllocationCallbacks* alloc) {
    if (view) ++views_destroyed;
    reinterpret_cast<PFN_vkDestroyImageView>(gdpa(d, "vkDestroyImageView"))(d, view, alloc);
  }
  static void VKAPI_CALL on_free_memory(VkDevice d, VkDeviceMemory memory, const VkAllocationCallbacks* alloc) {
    if (memory) ++memory_freed;
    reinterpret_cast<PFN_vkFreeMemory>(gdpa(d, "vkFreeMemory"))(d, memory, alloc);
  }
  static void VKAPI_CALL on_set_scissor(VkCommandBuffer cmd, std::uint32_t first, std::uint32_t count,
                                        const VkRect2D* rects) {
    forwarded.insert(forwarded.end(), rects, rects + count);
    set_scissor(cmd, first, count, rects);
  }
  static VkResult VKAPI_CALL on_create_framebuffer(VkDevice d, const VkFramebufferCreateInfo* info,
                                                   const VkAllocationCallbacks* alloc, VkFramebuffer* out) {
    framebuffers.push_back({info->width, info->height});
    attachments.emplace_back(info->pAttachments, info->pAttachments + info->attachmentCount);
    if (fail("vkCreateFramebuffer")) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    const auto r = create_framebuffer(d, info, alloc, out);
    handles.push_back(*out);
    return r;
  }
  static void VKAPI_CALL on_begin_pass(VkCommandBuffer cmd, const VkRenderPassBeginInfo* info,
                                       VkSubpassContents contents) {
    areas.push_back(info->renderArea);
    begun.push_back(info->framebuffer);
    barriers_at_begin.push_back(barriers.size());
    begin_pass(cmd, info, contents);
  }
  static void VKAPI_CALL on_pipeline_barrier(VkCommandBuffer cmd, VkPipelineStageFlags source,
                                             VkPipelineStageFlags dest, VkDependencyFlags flags,
                                             std::uint32_t memory_count, const VkMemoryBarrier* memory,
                                             std::uint32_t buffer_count, const VkBufferMemoryBarrier* buffer,
                                             std::uint32_t image_count, const VkImageMemoryBarrier* image) {
    barriers.insert(barriers.end(), image, image + image_count);
    pipeline_barrier(cmd, source, dest, flags, memory_count, memory, buffer_count, buffer, image_count, image);
  }
  static PFN_vkVoidFunction VKAPI_CALL device_proc(VkDevice d, const char* name) {
    auto real = gdpa(d, name);
    const std::string_view n(name);
    if (!fail_proc.empty() && real) {
      if (n == "vkCreateImage") return reinterpret_cast<PFN_vkVoidFunction>(on_create_image);
      if (n == "vkAllocateMemory") return reinterpret_cast<PFN_vkVoidFunction>(on_allocate_memory);
      if (n == "vkBindImageMemory") return reinterpret_cast<PFN_vkVoidFunction>(on_bind_image_memory);
      if (n == "vkCreateImageView") return reinterpret_cast<PFN_vkVoidFunction>(on_create_view);
      if (n == "vkDestroyImage") return reinterpret_cast<PFN_vkVoidFunction>(on_destroy_image);
      if (n == "vkDestroyImageView") return reinterpret_cast<PFN_vkVoidFunction>(on_destroy_view);
      if (n == "vkFreeMemory") return reinterpret_cast<PFN_vkVoidFunction>(on_free_memory);
    }
    if (real && n == "vkCmdSetScissor") {
      set_scissor = reinterpret_cast<PFN_vkCmdSetScissor>(real);
      return reinterpret_cast<PFN_vkVoidFunction>(on_set_scissor);
    }
    if (real && n == "vkCreateFramebuffer") {
      create_framebuffer = reinterpret_cast<PFN_vkCreateFramebuffer>(real);
      return reinterpret_cast<PFN_vkVoidFunction>(on_create_framebuffer);
    }
    if (real && n == "vkCmdBeginRenderPass") {
      begin_pass = reinterpret_cast<PFN_vkCmdBeginRenderPass>(real);
      return reinterpret_cast<PFN_vkVoidFunction>(on_begin_pass);
    }
    if (real && n == "vkCmdPipelineBarrier") {
      pipeline_barrier = reinterpret_cast<PFN_vkCmdPipelineBarrier>(real);
      return reinterpret_cast<PFN_vkVoidFunction>(on_pipeline_barrier);
    }
    return real;
  }
  static PFN_vkVoidFunction VKAPI_CALL instance_proc(VkInstance i, const char* name) {
    auto real = gipa(i, name);
    if (real && std::string_view(name) == "vkGetDeviceProcAddr") {
      gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(real);
      return reinterpret_cast<PFN_vkVoidFunction>(device_proc);
    }
    return real;
  }
  ScissorHooks() {
    fail_proc = {};
    image_failure = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    fail_call = calls = images_created = views_created = memory_allocated = 0;
    images_destroyed = views_destroyed = memory_freed = 0;
    forwarded.clear();
    areas.clear();
    framebuffers.clear();
    attachments.clear();
    handles.clear();
    begun.clear();
    barriers.clear();
    barriers_at_begin.clear();
    gipa = gfx::gpu_device().get_instance_proc_addr;
    gfx::gpu_device().get_instance_proc_addr = instance_proc;
  }
  ~ScissorHooks() { gfx::gpu_device().get_instance_proc_addr = gipa; }
};
// SwiftShader's resolve stand-ins on or off whatever the driver, for a scope.
struct ForcedStandIns {
  explicit ForcedStandIns(bool enabled) { vulkan::force_resolve_stand_ins(enabled); }
  ~ForcedStandIns() { vulkan::force_resolve_stand_ins(std::nullopt); }
};
bool same(const VkRect2D& a, const VkRect2D& b) {
  return a.offset.x == b.offset.x && a.offset.y == b.offset.y && a.extent.width == b.extent.width &&
         a.extent.height == b.extent.height;
}
}  // namespace

TEST_CASE("Vulkan clamps dynamic scissors to the open render pass instance's render area") {
  // Fragments outside the render area are undefined; a tiler drops them, a
  // desktop GPU writes them (Moss faulted NVIDIA past a smaller attachment).
  test::VulkanDevice gpu;
  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ci.imageType = VK_IMAGE_TYPE_2D;
  ci.format = VK_FORMAT_R8G8B8A8_UNORM;
  ci.extent = {16, 16, 1};
  ci.mipLevels = ci.arrayLayers = 1;
  ci.samples = VK_SAMPLE_COUNT_1_BIT;
  ci.tiling = VK_IMAGE_TILING_OPTIMAL;
  ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  VkImage image{};
  REQUIRE(test::vk_call("vkCreateImage", gpu.device, &ci, nullptr, &image) == VK_SUCCESS);
  VkMemoryRequirements req{};
  test::vk_call("vkGetImageMemoryRequirements", gpu.device, image, &req);
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = req.size;
  ai.memoryTypeIndex = gpu.memory_type(req.memoryTypeBits, 0);
  VkDeviceMemory memory{};
  REQUIRE(test::vk_call("vkAllocateMemory", gpu.device, &ai, nullptr, &memory) == VK_SUCCESS);
  REQUIRE(test::vk_call("vkBindImageMemory", gpu.device, image, memory, 0ull) == VK_SUCCESS);
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vi.image = image;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vi.format = ci.format;
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkImageView view{};
  REQUIRE(test::vk_call("vkCreateImageView", gpu.device, &vi, nullptr, &view) == VK_SUCCESS);
  VkAttachmentDescription attachment{};
  attachment.format = ci.format;
  attachment.samples = VK_SAMPLE_COUNT_1_BIT;
  attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  attachment.initialLayout = attachment.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
  VkAttachmentReference color{0, VK_IMAGE_LAYOUT_GENERAL};
  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &color;
  VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  rpi.attachmentCount = 1;
  rpi.pAttachments = &attachment;
  rpi.subpassCount = 1;
  rpi.pSubpasses = &subpass;
  VkRenderPass pass{};
  REQUIRE(test::vk_call("vkCreateRenderPass", gpu.device, &rpi, nullptr, &pass) == VK_SUCCESS);
  VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  fi.renderPass = pass;
  fi.attachmentCount = 1;
  fi.pAttachments = &view;
  fi.width = fi.height = 16;
  fi.layers = 1;
  VkFramebuffer framebuffer{};
  REQUIRE(test::vk_call("vkCreateFramebuffer", gpu.device, &fi, nullptr, &framebuffer) == VK_SUCCESS);

  gpu.begin();
  {
    ScissorHooks hooks;
    const VkRect2D whole{{0, 0}, {16, 16}}, across{{2, 10}, {20, 3}}, outside{{0, 0}, {2, 2}};
    test::vk_call("vkCmdSetScissor", gpu.command, 0u, 1u, &whole);
    VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    bi.renderPass = pass;
    bi.framebuffer = framebuffer;
    bi.renderArea = {{4, 4}, {8, 8}};
    test::vk_call("vkCmdBeginRenderPass", gpu.command, &bi, VK_SUBPASS_CONTENTS_INLINE);
    test::vk_call("vkCmdSetScissor", gpu.command, 0u, 1u, &across);
    test::vk_call("vkCmdSetScissor", gpu.command, 0u, 1u, &outside);
    test::vk_call("vkCmdEndRenderPass", gpu.command);
    test::vk_call("vkCmdSetScissor", gpu.command, 0u, 1u, &whole);
    // Outside a pass: unchanged. At the pass: the earlier scissor clamped to
    // the render area. Inside: clamped, down to an empty rectangle.
    const VkRect2D expected[] = {whole, {{4, 4}, {8, 8}}, {{4, 10}, {8, 2}}, {{4, 4}, {0, 0}}, whole};
    REQUIRE(ScissorHooks::forwarded.size() == std::size(expected));
    for (std::size_t i = 0; i < std::size(expected); ++i) {
      CAPTURE(i);
      CHECK(same(ScissorHooks::forwarded[i], expected[i]));
    }
  }
  REQUIRE(test::vk_call("vkResetCommandBuffer", gpu.command, 0u) == VK_SUCCESS);
  test::vk_call("vkDestroyFramebuffer", gpu.device, framebuffer, nullptr);
  test::vk_call("vkDestroyRenderPass", gpu.device, pass, nullptr);
  test::vk_call("vkDestroyImageView", gpu.device, view, nullptr);
  test::vk_call("vkDestroyImage", gpu.device, image, nullptr);
  test::vk_call("vkFreeMemory", gpu.device, memory, nullptr);
}

TEST_SUITE_END();

TEST_CASE("Vulkan opaque external FDs share real memory and semaphore payloads") {
  test::VulkanDevice gpu;
  std::uint32_t count = 0;
  REQUIRE(call("vkEnumerateDeviceExtensionProperties", gpu.physical, nullptr, &count, nullptr) == VK_SUCCESS);
  std::vector<VkExtensionProperties> extensions(count);
  REQUIRE(call("vkEnumerateDeviceExtensionProperties", gpu.physical, nullptr, &count, extensions.data()) == VK_SUCCESS);
  auto available = [&](const char* name) {
    return std::ranges::any_of(extensions, [&](auto& e) { return std::strcmp(name, e.extensionName) == 0; });
  };
  if (!available("VK_KHR_external_memory_fd") || !available("VK_KHR_external_semaphore_fd")) {
    MESSAGE("Host lacks external handle extensions; hardware validation required");
    return;
  }
  VkPhysicalDeviceExternalBufferInfo query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO};
  query.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  query.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
  VkExternalBufferProperties properties{VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
  call("vkGetPhysicalDeviceExternalBufferProperties", gpu.physical, &query, &properties);
  REQUIRE((properties.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) != 0);
  CHECK(properties.externalMemoryProperties.compatibleHandleTypes == VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT);
  VkPhysicalDeviceExternalSemaphoreInfo sq{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO};
  sq.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
  VkExternalSemaphoreProperties sp{VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
  call("vkGetPhysicalDeviceExternalSemaphoreProperties", gpu.physical, &sq, &sp);
  // Software drivers (Mesa lavapipe) list the extension without exportable opaque semaphores.
  if (!(sp.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT)) {
    MESSAGE("Host cannot export opaque semaphores; hardware validation required");
    return;
  }
  call("vkGetPhysicalDeviceQueueFamilyProperties", gpu.physical, &count, nullptr);
  std::vector<VkQueueFamilyProperties> families(count);
  call("vkGetPhysicalDeviceQueueFamilyProperties", gpu.physical, &count, families.data());
  std::uint32_t family = 0;
  while (!(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++family;
  float priority = 1;
  VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qi.queueFamilyIndex = family; qi.queueCount = 1; qi.pQueuePriorities = &priority;
  const char* names[]{"VK_KHR_external_memory_fd", "VK_KHR_external_semaphore_fd"};
  VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &qi;
  di.enabledExtensionCount = 2; di.ppEnabledExtensionNames = names;
  VkDevice device{};
  REQUIRE(call("vkCreateDevice", gpu.physical, &di, nullptr, &device) == VK_SUCCESS);
  VkQueue queue{}; call("vkGetDeviceQueue", device, family, 0u, &queue);
  CHECK(vulkan::device_proc(device, "vkGetMemoryFdKHR") != 0);
  VkExternalMemoryBufferCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
  external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
  VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &external};
  bi.size = 4096; bi.usage = query.usage;
  VkBuffer buffer{}, importedBuffer{}, readbackBuffer{};
  REQUIRE(call("vkCreateBuffer", device, &bi, nullptr, &buffer) == VK_SUCCESS);
  REQUIRE(call("vkCreateBuffer", device, &bi, nullptr, &importedBuffer) == VK_SUCCESS);
  CHECK(external.handleTypes == VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT);
  VkMemoryRequirements requirements{};
  call("vkGetBufferMemoryRequirements", device, buffer, &requirements);
  VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
  dedicated.buffer = buffer;
  VkExportMemoryAllocateInfo exportInfo{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, &dedicated};
  exportInfo.handleTypes = external.handleTypes;
  VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &exportInfo};
  allocation.allocationSize = requirements.size;
  allocation.memoryTypeIndex = gpu.memory_type(requirements.memoryTypeBits, 0);
  VkDeviceMemory memory{}, imported{};
  REQUIRE(call("vkAllocateMemory", device, &allocation, nullptr, &memory) == VK_SUCCESS);
  REQUIRE(call("vkBindBufferMemory", device, buffer, memory, 0ull) == VK_SUCCESS);
  VkMemoryGetFdInfoKHR get{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, nullptr, memory,
                          VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
  int fd = -1;
  REQUIRE(call("vkGetMemoryFdKHR", device, &get, &fd) == VK_SUCCESS);
  int repeated_fd = -1;
  REQUIRE(call("vkGetMemoryFdKHR", device, &get, &repeated_fd) == VK_SUCCESS);
  CHECK(repeated_fd != fd);
#ifdef _WIN32
  // Repeated Linux exports must duplicate the cached NT export, not invoke
  // vkGetMemoryWin32HandleKHR a second time for the same allocation.
  CHECK(hle::libc::FdTable::get().file(fd) == hle::libc::FdTable::get().file(repeated_fd));
#endif
  CHECK(hle::libc::FdTable::get().close(repeated_fd) == 0);

  auto& table = hle::libc::FdTable::get();
  VkImportSemaphoreFdInfoKHR wrongKind{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
  wrongKind.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
  wrongKind.fd = fd;
  CHECK(static_cast<std::int32_t>(call("vkImportSemaphoreFdKHR", device, &wrongKind)) ==
        VK_ERROR_INVALID_EXTERNAL_HANDLE);
  CHECK(table.file(fd) != nullptr);
  int duplicate = table.duplicate(fd, 0, true);
  REQUIRE(duplicate >= 0);
  REQUIRE(table.close(fd) == 0);
  VkImportMemoryFdInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, &dedicated,
                                VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT, duplicate};
  dedicated.buffer = importedBuffer;
  allocation.pNext = &import;
  REQUIRE(call("vkAllocateMemory", device, &allocation, nullptr, &imported) == VK_SUCCESS);
  REQUIRE(call("vkBindBufferMemory", device, importedBuffer, imported, 0ull) == VK_SUCCESS);
  CHECK_FALSE(table.file(duplicate));
  CHECK(import.fd == duplicate);
  // External payloads need not be CPU-mappable. Read them back through an
  // ordinary coherent staging allocation, as a real consumer would.
  bi.pNext = nullptr;
  REQUIRE(call("vkCreateBuffer", device, &bi, nullptr, &readbackBuffer) == VK_SUCCESS);
  call("vkGetBufferMemoryRequirements", device, readbackBuffer, &requirements);
  allocation.pNext = nullptr;
  allocation.allocationSize = requirements.size;
  allocation.memoryTypeIndex = gpu.memory_type(requirements.memoryTypeBits,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  VkDeviceMemory readbackMemory{};
  REQUIRE(call("vkAllocateMemory", device, &allocation, nullptr, &readbackMemory) == VK_SUCCESS);
  REQUIRE(call("vkBindBufferMemory", device, readbackBuffer, readbackMemory, 0ull) == VK_SUCCESS);
  VkExportSemaphoreCreateInfo exportSemaphore{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
  exportSemaphore.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
  VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &exportSemaphore};
  VkSemaphore source{}, target{};
  REQUIRE(call("vkCreateSemaphore", device, &semaphoreInfo, nullptr, &source) == VK_SUCCESS);
  semaphoreInfo.pNext = nullptr;
  REQUIRE(call("vkCreateSemaphore", device, &semaphoreInfo, nullptr, &target) == VK_SUCCESS);
  VkSemaphoreGetFdInfoKHR getSemaphore{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR, nullptr, source,
                                     VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT};
  REQUIRE(call("vkGetSemaphoreFdKHR", device, &getSemaphore, &fd) == VK_SUCCESS);
  VkImportSemaphoreFdInfoKHR importSemaphore{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
  importSemaphore.semaphore = target; importSemaphore.handleType = getSemaphore.handleType; importSemaphore.fd = fd;
  REQUIRE(call("vkImportSemaphoreFdKHR", device, &importSemaphore) == VK_SUCCESS);
  CHECK_FALSE(table.file(fd));
  VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  poolInfo.queueFamilyIndex = family;
  VkCommandPool pool{};
  REQUIRE(call("vkCreateCommandPool", device, &poolInfo, nullptr, &pool) == VK_SUCCESS);
  VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  commandInfo.commandPool = pool; commandInfo.commandBufferCount = 1;
  VkCommandBuffer command{};
  REQUIRE(call("vkAllocateCommandBuffers", device, &commandInfo, &command) == VK_SUCCESS);
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  REQUIRE(call("vkBeginCommandBuffer", command, &begin) == VK_SUCCESS);
  call("vkCmdFillBuffer", command, buffer, 0ull, 4096ull, 0xdecafbadU);
  VkMemoryBarrier aliasBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
      VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT};
  call("vkCmdPipelineBarrier", command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
       0u, 1u, &aliasBarrier, 0u, nullptr, 0u, nullptr);
  VkBufferCopy copy{0, 0, 4096};
  call("vkCmdCopyBuffer", command, importedBuffer, readbackBuffer, 1u, &copy);
  VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
  call("vkCmdPipelineBarrier", command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
       0u, 1u, &barrier, 0u, nullptr, 0u, nullptr);
  REQUIRE(call("vkEndCommandBuffer", command) == VK_SUCCESS);
  VkSubmitInfo submissions[2]{{VK_STRUCTURE_TYPE_SUBMIT_INFO}, {VK_STRUCTURE_TYPE_SUBMIT_INFO}};
  submissions[0].commandBufferCount = 1; submissions[0].pCommandBuffers = &command;
  submissions[0].signalSemaphoreCount = 1; submissions[0].pSignalSemaphores = &source;
  VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
  submissions[1].waitSemaphoreCount = 1; submissions[1].pWaitSemaphores = &target; submissions[1].pWaitDstStageMask = &stage;
  VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence{};
  REQUIRE(call("vkCreateFence", device, &fenceInfo, nullptr, &fence) == VK_SUCCESS);
  REQUIRE(call("vkQueueSubmit", queue, 2u, submissions, fence) == VK_SUCCESS);
  REQUIRE(call("vkWaitForFences", device, 1u, &fence, VK_TRUE, 5000000000ull) == VK_SUCCESS);
  void* mapped{};
  REQUIRE(call("vkMapMemory", device, readbackMemory, 0ull, 4096ull, 0u, &mapped) == VK_SUCCESS);
  for (unsigned i = 0; i < 1024; ++i) CHECK(static_cast<std::uint32_t*>(mapped)[i] == 0xdecafbadU);
  call("vkUnmapMemory", device, readbackMemory);
  call("vkDestroyFence", device, fence, nullptr);
  call("vkDestroyCommandPool", device, pool, nullptr);
  call("vkDestroySemaphore", device, source, nullptr);
  call("vkDestroySemaphore", device, target, nullptr);
  call("vkDestroyBuffer", device, buffer, nullptr);
  call("vkDestroyBuffer", device, importedBuffer, nullptr);
  call("vkDestroyBuffer", device, readbackBuffer, nullptr);
  call("vkFreeMemory", device, memory, nullptr);
  call("vkFreeMemory", device, imported, nullptr);
  call("vkFreeMemory", device, readbackMemory, nullptr);
  call("vkDestroyDevice", device, nullptr);
}

TEST_CASE("Vulkan SYNC_FD semaphores and fences exchange Android sync files") {
  test::VulkanDevice gpu;
  std::uint32_t count = 0;
  REQUIRE(call("vkEnumerateDeviceExtensionProperties", gpu.physical, nullptr, &count, nullptr) == VK_SUCCESS);
  std::vector<VkExtensionProperties> extensions(count);
  REQUIRE(call("vkEnumerateDeviceExtensionProperties", gpu.physical, nullptr, &count, extensions.data()) == VK_SUCCESS);
  auto available = [&](const char* name) {
    return std::ranges::any_of(extensions, [&](auto& e) { return std::strcmp(name, e.extensionName) == 0; });
  };
  REQUIRE(available("VK_KHR_external_semaphore_fd"));
  REQUIRE(available("VK_KHR_external_fence_fd"));

  VkPhysicalDeviceExternalSemaphoreInfo sq{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO};
  sq.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
  VkExternalSemaphoreProperties sp{VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
  call("vkGetPhysicalDeviceExternalSemaphoreProperties", gpu.physical, &sq, &sp);
  CHECK(sp.externalSemaphoreFeatures ==
        (VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT));
  CHECK(sp.compatibleHandleTypes == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT);
  VkSemaphoreTypeCreateInfo timeline{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, nullptr, VK_SEMAPHORE_TYPE_TIMELINE};
  sq.pNext = &timeline;
  call("vkGetPhysicalDeviceExternalSemaphoreProperties", gpu.physical, &sq, &sp);
  CHECK(sp.externalSemaphoreFeatures == 0);  // SYNC_FD is defined for binary semaphores only
  VkPhysicalDeviceExternalFenceInfo fq{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_FENCE_INFO};
  fq.handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;
  VkExternalFenceProperties fp{VK_STRUCTURE_TYPE_EXTERNAL_FENCE_PROPERTIES};
  call("vkGetPhysicalDeviceExternalFenceProperties", gpu.physical, &fq, &fp);
  CHECK(fp.externalFenceFeatures == (VK_EXTERNAL_FENCE_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_FENCE_FEATURE_IMPORTABLE_BIT));
  fq.handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT;
  call("vkGetPhysicalDeviceExternalFenceProperties", gpu.physical, &fq, &fp);
  CHECK(fp.externalFenceFeatures == 0);

  call("vkGetPhysicalDeviceQueueFamilyProperties", gpu.physical, &count, nullptr);
  std::vector<VkQueueFamilyProperties> families(count);
  call("vkGetPhysicalDeviceQueueFamilyProperties", gpu.physical, &count, families.data());
  std::uint32_t family = 0;
  while (!(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++family;
  float priority = 1;
  VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qi.queueFamilyIndex = family; qi.queueCount = 1; qi.pQueuePriorities = &priority;
  const char* names[]{"VK_KHR_external_semaphore_fd", "VK_KHR_external_fence_fd"};
  VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &qi;
  di.enabledExtensionCount = 2; di.ppEnabledExtensionNames = names;
  VkDevice device{};
  REQUIRE(call("vkCreateDevice", gpu.physical, &di, nullptr, &device) == VK_SUCCESS);
  CHECK(vulkan::device_proc(device, "vkGetFenceFdKHR") != 0);
  CHECK(vulkan::device_proc(device, "vkImportSemaphoreFdKHR") != 0);
  auto& table = hle::libc::FdTable::get();

  // Before any vkGetDeviceQueue, a -1 import still finds the device's queue.
  VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  VkSemaphore source{}, target{}, signaled{};
  REQUIRE(call("vkCreateSemaphore", device, &semaphoreInfo, nullptr, &signaled) == VK_SUCCESS);
  REQUIRE(call("vkCreateSemaphore", device, &semaphoreInfo, nullptr, &target) == VK_SUCCESS);
  VkImportSemaphoreFdInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
  import.semaphore = signaled; import.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
  import.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT; import.fd = -1;  // an already-signaled sync file
  REQUIRE(call("vkImportSemaphoreFdKHR", device, &import) == VK_SUCCESS);

  VkExportSemaphoreCreateInfo exportSemaphore{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
  exportSemaphore.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
  semaphoreInfo.pNext = &exportSemaphore;
  REQUIRE(call("vkCreateSemaphore", device, &semaphoreInfo, nullptr, &source) == VK_SUCCESS);
  VkQueue queue{};
  call("vkGetDeviceQueue", device, family, 0u, &queue);
  VkSubmitInfo signal{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  signal.signalSemaphoreCount = 1; signal.pSignalSemaphores = &source;
  REQUIRE(call("vkQueueSubmit", queue, 1u, &signal, VK_NULL_HANDLE) == VK_SUCCESS);
  VkSemaphoreGetFdInfoKHR get{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR, nullptr, source,
                              VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
  int fd = -1;
  REQUIRE(call("vkGetSemaphoreFdKHR", device, &get, &fd) == VK_SUCCESS);
  REQUIRE(fd >= 0);
  CHECK((table.get_fd_flags(fd) & 1) != 0);  // FD_CLOEXEC
  auto file = table.file(fd);
  REQUIRE(file);
  for (int i = 0; i < 5000 && !file->ready(hle::libc::poll_in); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  CHECK(file->ready(hle::libc::poll_in | hle::libc::poll_out) == hle::libc::poll_in);  // sync_file: POLLIN once signaled
  file.reset();
  int keep = table.duplicate(fd, 0, true);
  REQUIRE(keep >= 0);

  import.semaphore = target; import.fd = fd; import.flags = 0;
  CHECK(static_cast<std::int32_t>(call("vkImportSemaphoreFdKHR", device, &import)) == VK_ERROR_INVALID_EXTERNAL_HANDLE);
  CHECK(table.file(fd) != nullptr);  // failed imports keep descriptor ownership
  import.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
  REQUIRE(call("vkImportSemaphoreFdKHR", device, &import) == VK_SUCCESS);
  CHECK_FALSE(table.file(fd));
  import.fd = table.insert(std::make_shared<hle::libc::File>());
  CHECK(static_cast<std::int32_t>(call("vkImportSemaphoreFdKHR", device, &import)) == VK_ERROR_INVALID_EXTERNAL_HANDLE);
  table.close(import.fd);

  // Both imported payloads satisfy queue waits.
  VkSemaphore waits[]{target, signaled};
  VkPipelineStageFlags stages[]{VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
  VkSubmitInfo wait{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  wait.waitSemaphoreCount = 2; wait.pWaitSemaphores = waits; wait.pWaitDstStageMask = stages;
  VkExportFenceCreateInfo exportFence{VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO};
  exportFence.handleTypes = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;
  VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, &exportFence};
  VkFence fence{}, imported{};
  REQUIRE(call("vkCreateFence", device, &fenceInfo, nullptr, &fence) == VK_SUCCESS);
  fenceInfo.pNext = nullptr;
  REQUIRE(call("vkCreateFence", device, &fenceInfo, nullptr, &imported) == VK_SUCCESS);
  REQUIRE(call("vkQueueSubmit", queue, 1u, &wait, fence) == VK_SUCCESS);
  REQUIRE(call("vkWaitForFences", device, 1u, &fence, VK_TRUE, 5000000000ull) == VK_SUCCESS);

  // A re-import before any wait replaces one signaled payload with another: the
  // host binary semaphore is signaled once, and a wait consumes it.
  auto signals = vulkan::sync_fd_state(device).import_signals;
  import.semaphore = signaled; import.fd = -1; import.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
  REQUIRE(call("vkImportSemaphoreFdKHR", device, &import) == VK_SUCCESS);
  REQUIRE(call("vkImportSemaphoreFdKHR", device, &import) == VK_SUCCESS);
  CHECK(vulkan::sync_fd_state(device).import_signals == signals + 1);
  VkSubmitInfo consume{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  consume.waitSemaphoreCount = 1; consume.pWaitSemaphores = &signaled; consume.pWaitDstStageMask = stages;
  VkFenceCreateInfo plainFence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VkFence consumed{};
  REQUIRE(call("vkCreateFence", device, &plainFence, nullptr, &consumed) == VK_SUCCESS);
  REQUIRE(call("vkQueueSubmit", queue, 1u, &consume, consumed) == VK_SUCCESS);
  REQUIRE(call("vkWaitForFences", device, 1u, &consumed, VK_TRUE, 5000000000ull) == VK_SUCCESS);
  REQUIRE(call("vkImportSemaphoreFdKHR", device, &import) == VK_SUCCESS);  // the wait consumed it
  CHECK(vulkan::sync_fd_state(device).import_signals == signals + 2);
  REQUIRE(call("vkResetFences", device, 1u, &consumed) == VK_SUCCESS);
  REQUIRE(call("vkQueueSubmit", queue, 1u, &consume, consumed) == VK_SUCCESS);
  REQUIRE(call("vkWaitForFences", device, 1u, &consumed, VK_TRUE, 5000000000ull) == VK_SUCCESS);
  call("vkDestroyFence", device, consumed, nullptr);

  // Fence export: a signaled payload may be -1, and export resets the fence.
  VkFenceGetFdInfoKHR getFence{VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR, nullptr, fence,
                               VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT};
  int fenceFd = 0;
  REQUIRE(call("vkGetFenceFdKHR", device, &getFence, &fenceFd) == VK_SUCCESS);
  CHECK(fenceFd == -1);
  CHECK(static_cast<std::int32_t>(call("vkGetFenceStatus", device, fence)) == VK_NOT_READY);
  VkImportFenceFdInfoKHR importFence{VK_STRUCTURE_TYPE_IMPORT_FENCE_FD_INFO_KHR};
  importFence.fence = imported; importFence.flags = VK_FENCE_IMPORT_TEMPORARY_BIT;
  importFence.handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT; importFence.fd = keep;
  REQUIRE(call("vkImportFenceFdKHR", device, &importFence) == VK_SUCCESS);
  CHECK_FALSE(table.file(keep));
  CHECK(call("vkGetFenceStatus", device, imported) == VK_SUCCESS);
  REQUIRE(call("vkResetFences", device, 1u, &imported) == VK_SUCCESS);
  importFence.fd = -1;
  REQUIRE(call("vkImportFenceFdKHR", device, &importFence) == VK_SUCCESS);
  CHECK(call("vkGetFenceStatus", device, imported) == VK_SUCCESS);

  // Exports closed every frame leave no records behind.
  for (int frame = 0; frame < 8; ++frame) {
    REQUIRE(call("vkQueueSubmit", queue, 1u, &signal, VK_NULL_HANDLE) == VK_SUCCESS);
    REQUIRE(call("vkGetSemaphoreFdKHR", device, &get, &fd) == VK_SUCCESS);
    table.close(fd);
  }
  CHECK(vulkan::sync_fd_state(device).export_records <= 1);

  // vkDestroySemaphore first retires the semaphore's live exports, whose hidden
  // waits reference it; their descriptors stay valid, signaled sync files.
  REQUIRE(call("vkQueueSubmit", queue, 1u, &signal, VK_NULL_HANDLE) == VK_SUCCESS);
  REQUIRE(call("vkGetSemaphoreFdKHR", device, &get, &fd) == VK_SUCCESS);
  CHECK(vulkan::sync_fd_state(device).export_records == 1);
  call("vkDestroySemaphore", device, source, nullptr);
  CHECK(vulkan::sync_fd_state(device).export_records == 0);
  file = table.file(fd);
  REQUIRE(file);
  CHECK(file->ready(hle::libc::poll_in) == hle::libc::poll_in);
  file.reset();
  import.semaphore = target; import.fd = fd; import.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
  REQUIRE(call("vkImportSemaphoreFdKHR", device, &import) == VK_SUCCESS);
  CHECK_FALSE(table.file(fd));

  call("vkDestroyFence", device, fence, nullptr);
  call("vkDestroyFence", device, imported, nullptr);
  call("vkDestroySemaphore", device, target, nullptr);
  call("vkDestroySemaphore", device, signaled, nullptr);
  call("vkDestroyDevice", device, nullptr);
}

TEST_CASE("Vulkan Android hardware buffers share GPU payloads through allocation, import and export") {
  test::VulkanDevice gpu;
  vulkan::register_hardware_buffer_allocator();
  std::uint32_t count = 0;
  REQUIRE(call("vkEnumerateDeviceExtensionProperties", gpu.physical, nullptr, &count, nullptr) == VK_SUCCESS);
  std::vector<VkExtensionProperties> extensions(count);
  REQUIRE(call("vkEnumerateDeviceExtensionProperties", gpu.physical, nullptr, &count, extensions.data()) == VK_SUCCESS);
  auto available = [&](const char* name) {
    return std::ranges::any_of(extensions, [&](auto& e) { return std::strcmp(name, e.extensionName) == 0; });
  };
  namespace ahb = android::ahb;
  android::HardwareBufferDesc desc{16, 16, 1, 1, ahb::gpu_sampled_image | ahb::gpu_framebuffer, 0, 0, 0};
  constexpr auto android_1_1 = VK_MAKE_API_VERSION(0, 1, 1, 0);
  if (!vulkan::hardware_buffers_supported()) {
    // Android's Vulkan 1.1 feature flag promises hardware buffers.
    CHECK_FALSE(available("VK_ANDROID_external_memory_android_hardware_buffer"));
    CHECK(vulkan::android_hardware_version() != android_1_1);
    CHECK_FALSE(android::hardware_buffer_supported(desc));
    MESSAGE("Host lacks external memory handles; hardware validation required");
    return;
  }
  REQUIRE(available("VK_ANDROID_external_memory_android_hardware_buffer"));
  REQUIRE(available("VK_EXT_queue_family_foreign"));
  VkPhysicalDeviceProperties gpu_properties{};
  call("vkGetPhysicalDeviceProperties", gpu.physical, &gpu_properties);
  if (gpu_properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU) CHECK(vulkan::android_hardware_version() == android_1_1);

  constexpr auto ahb_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
  constexpr VkImageUsageFlags usage =
      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  VkPhysicalDeviceExternalImageFormatInfo external_query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
                                                         nullptr, ahb_type};
  VkPhysicalDeviceImageFormatInfo2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, &external_query,
                                         VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                                         usage | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 0};
  VkAndroidHardwareBufferUsageANDROID optimal{VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_USAGE_ANDROID};
  VkExternalImageFormatProperties external_properties{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES, &optimal};
  VkImageFormatProperties2 image_properties{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, &external_properties};
  REQUIRE(call("vkGetPhysicalDeviceImageFormatProperties2", gpu.physical, &query, &image_properties) == VK_SUCCESS);
  CHECK(external_properties.externalMemoryProperties.externalMemoryFeatures ==
        (VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT | VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT |
         VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT));
  CHECK(external_properties.externalMemoryProperties.compatibleHandleTypes == ahb_type);
  CHECK(optimal.androidHardwareBufferUsage == (ahb::gpu_sampled_image | ahb::gpu_framebuffer));
  VkPhysicalDeviceExternalBufferInfo buffer_query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO, nullptr, 0,
                                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, ahb_type};
  VkExternalBufferProperties buffer_properties{VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
  call("vkGetPhysicalDeviceExternalBufferProperties", gpu.physical, &buffer_query, &buffer_properties);
  CHECK(buffer_properties.externalMemoryProperties.externalMemoryFeatures == 0);  // BLOB buffers are not offered

  // GPU buffers are device-local; CPU access is not offered with them.
  auto cpu = desc;
  cpu.usage |= 0x3;
  CHECK_FALSE(android::hardware_buffer_supported(cpu));
  CHECK(android::hardware_buffer_supported(desc));
  GuestAddr buffer = 0, other = 0;
  REQUIRE(android::allocate_hardware_buffer(desc, &buffer) == 0);
  REQUIRE(android::allocate_hardware_buffer(desc, &other) == 0);
  CHECK(android::hardware_buffer(buffer)->desc.stride == 16);

  call("vkGetPhysicalDeviceQueueFamilyProperties", gpu.physical, &count, nullptr);
  std::vector<VkQueueFamilyProperties> families(count);
  call("vkGetPhysicalDeviceQueueFamilyProperties", gpu.physical, &count, families.data());
  std::uint32_t family = 0;
  while (!(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++family;
  float priority = 1;
  VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qi.queueFamilyIndex = family;
  qi.queueCount = 1;
  qi.pQueuePriorities = &priority;
  const char* names[]{"VK_ANDROID_external_memory_android_hardware_buffer", "VK_EXT_queue_family_foreign"};
  VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcr{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES,
                                                       nullptr, VK_TRUE};
  VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &ycbcr};
  di.queueCreateInfoCount = 1;
  di.pQueueCreateInfos = &qi;
  di.enabledExtensionCount = 2;
  di.ppEnabledExtensionNames = names;
  VkDevice device{};
  REQUIRE(call("vkCreateDevice", gpu.physical, &di, nullptr, &device) == VK_SUCCESS);
  CHECK(vulkan::device_proc(device, "vkGetAndroidHardwareBufferPropertiesANDROID") != 0);

  VkAndroidHardwareBufferFormatPropertiesANDROID format{VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID};
  VkAndroidHardwareBufferPropertiesANDROID properties{VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID,
                                                      &format};
  REQUIRE(call("vkGetAndroidHardwareBufferPropertiesANDROID", device, g2h<AHardwareBuffer>(buffer), &properties) ==
          VK_SUCCESS);
  CHECK(properties.allocationSize > 0);
  REQUIRE(std::popcount(properties.memoryTypeBits) == 1);
  CHECK(format.format == VK_FORMAT_R8G8B8A8_UNORM);
  CHECK(format.externalFormat != 0);
  CHECK((format.formatFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0);
  CHECK((format.formatFeatures & VK_FORMAT_FEATURE_MIDPOINT_CHROMA_SAMPLES_BIT) != 0);

  auto make_image = [&](VkFormat image_format, VkImageUsageFlags image_usage, const void* next) {
    VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, next, ahb_type};
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &external};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = image_format;
    ci.extent = {16, 16, 1};
    ci.mipLevels = ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = image_usage;
    VkImage image{};
    REQUIRE(call("vkCreateImage", device, &ci, nullptr, &image) == VK_SUCCESS);
    return image;
  };
  auto import = [&](VkImage image, GuestAddr source) {
    VkAndroidHardwareBufferPropertiesANDROID source_properties{VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID};
    REQUIRE(call("vkGetAndroidHardwareBufferPropertiesANDROID", device, g2h<AHardwareBuffer>(source),
                 &source_properties) == VK_SUCCESS);
    VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, nullptr, image};
    VkImportAndroidHardwareBufferInfoANDROID info{VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,
                                                  &dedicated, g2h<AHardwareBuffer>(source)};
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &info, source_properties.allocationSize,
                                    static_cast<std::uint32_t>(std::countr_zero(source_properties.memoryTypeBits))};
    VkDeviceMemory memory{};
    REQUIRE(call("vkAllocateMemory", device, &allocation, nullptr, &memory) == VK_SUCCESS);
    REQUIRE(call("vkBindImageMemory", device, image, memory, VkDeviceSize{0}) == VK_SUCCESS);
    return memory;
  };
  // Two images bound to one buffer: a clear through one is read through the other.
  VkImage writer = make_image(VK_FORMAT_R8G8B8A8_UNORM, usage, nullptr);
  VkImage reader = make_image(VK_FORMAT_R8G8B8A8_UNORM, usage, nullptr);
  VkDeviceMemory writer_memory = import(writer, buffer), reader_memory = import(reader, buffer);
  android::release_hardware_buffer(buffer);  // the imports keep it alive
  REQUIRE(android::hardware_buffer(buffer));

  VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bi.size = 16 * 16 * 4;
  bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  VkBuffer readback{};
  REQUIRE(call("vkCreateBuffer", device, &bi, nullptr, &readback) == VK_SUCCESS);
  VkMemoryRequirements requirements{};
  call("vkGetBufferMemoryRequirements", device, readback, &requirements);
  VkPhysicalDeviceMemoryProperties memory_properties{};
  call("vkGetPhysicalDeviceMemoryProperties", gpu.physical, &memory_properties);
  std::uint32_t host_type = 0;
  while (!((requirements.memoryTypeBits >> host_type) & 1) ||
         !(memory_properties.memoryTypes[host_type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
    ++host_type;
  VkMemoryAllocateInfo host_allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, requirements.size, host_type};
  VkDeviceMemory readback_memory{};
  REQUIRE(call("vkAllocateMemory", device, &host_allocation, nullptr, &readback_memory) == VK_SUCCESS);
  REQUIRE(call("vkBindBufferMemory", device, readback, readback_memory, VkDeviceSize{0}) == VK_SUCCESS);

  VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, 0, family};
  VkCommandPool pool{};
  REQUIRE(call("vkCreateCommandPool", device, &pi, nullptr, &pool) == VK_SUCCESS);
  VkCommandBufferAllocateInfo ci{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool,
                                 VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
  VkCommandBuffer command{};
  REQUIRE(call("vkAllocateCommandBuffers", device, &ci, &command) == VK_SUCCESS);
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  REQUIRE(call("vkBeginCommandBuffer", command, &begin) == VK_SUCCESS);
  const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  auto barrier = [&](VkImage image, VkImageLayout from, VkImageLayout to, std::uint32_t src, std::uint32_t dst,
                     VkAccessFlags src_access, VkAccessFlags dst_access) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, src_access, dst_access, from, to, src,
                           dst, image, range};
    call("vkCmdPipelineBarrier", command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
         0u, 0u, nullptr, 0u, nullptr, 1u, &b);
  };
  // Acquire from and release to the foreign agent that owns AHB contents.
  barrier(writer, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_FOREIGN_EXT, family,
          0, VK_ACCESS_TRANSFER_WRITE_BIT);
  VkClearColorValue color{};
  // Exact UNORM8 values: 0.5 is a rounding tie that drivers may resolve either way.
  color.float32[0] = 64 / 255.0f;
  color.float32[1] = 128 / 255.0f;
  color.float32[2] = 191 / 255.0f;
  color.float32[3] = 1;
  call("vkCmdClearColorImage", command, writer, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1u, &range);
  barrier(writer, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, family, VK_QUEUE_FAMILY_FOREIGN_EXT,
          VK_ACCESS_TRANSFER_WRITE_BIT, 0);
  barrier(reader, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_QUEUE_FAMILY_FOREIGN_EXT, family,
          0, VK_ACCESS_TRANSFER_READ_BIT);
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {16, 16, 1};
  call("vkCmdCopyImageToBuffer", command, reader, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1u, &region);
  REQUIRE(call("vkEndCommandBuffer", command) == VK_SUCCESS);
  VkQueue queue{};
  call("vkGetDeviceQueue", device, family, 0u, &queue);
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &command;
  REQUIRE(call("vkQueueSubmit", queue, 1u, &submit, VK_NULL_HANDLE) == VK_SUCCESS);
  REQUIRE(call("vkQueueWaitIdle", queue) == VK_SUCCESS);
  void* mapped = nullptr;
  REQUIRE(call("vkMapMemory", device, readback_memory, VkDeviceSize{0}, VK_WHOLE_SIZE, 0u, &mapped) == VK_SUCCESS);
  const auto* texel = static_cast<const std::uint8_t*>(mapped) + (5 * 16 + 7) * 4;
  CHECK(texel[0] == 64);
  CHECK(texel[1] == 128);
  CHECK(texel[2] == 191);
  CHECK(texel[3] == 255);
  call("vkUnmapMemory", device, readback_memory);

  // A producer write (AImageReader) is read through a guest import.
  std::vector<std::uint8_t> pixels(16 * 16 * 4);
  for (std::size_t n = 0; n < pixels.size(); ++n) pixels[n] = std::uint8_t(n * 7);
  CHECK_FALSE(android::write_hardware_buffer(other, std::span(pixels).first(16)));
  REQUIRE(android::write_hardware_buffer(other, pixels));
  VkImage produced = make_image(VK_FORMAT_R8G8B8A8_UNORM, usage, nullptr);
  VkDeviceMemory produced_memory = import(produced, other);
  VkCommandBuffer second_command{};
  REQUIRE(call("vkAllocateCommandBuffers", device, &ci, &second_command) == VK_SUCCESS);
  command = second_command;
  REQUIRE(call("vkBeginCommandBuffer", command, &begin) == VK_SUCCESS);
  barrier(produced, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_QUEUE_FAMILY_FOREIGN_EXT, family,
          0, VK_ACCESS_TRANSFER_READ_BIT);
  call("vkCmdCopyImageToBuffer", command, produced, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1u, &region);
  REQUIRE(call("vkEndCommandBuffer", command) == VK_SUCCESS);
  REQUIRE(call("vkQueueSubmit", queue, 1u, &submit, VK_NULL_HANDLE) == VK_SUCCESS);
  REQUIRE(call("vkQueueWaitIdle", queue) == VK_SUCCESS);
  REQUIRE(call("vkMapMemory", device, readback_memory, VkDeviceSize{0}, VK_WHOLE_SIZE, 0u, &mapped) == VK_SUCCESS);
  CHECK(std::memcmp(mapped, pixels.data(), pixels.size()) == 0);
  call("vkUnmapMemory", device, readback_memory);
  call("vkDestroyImage", device, produced, nullptr);
  call("vkFreeMemory", device, produced_memory, nullptr);

  // Export: the memory's AHB is one object, referenced once per query.
  VkImage exported_image = make_image(VK_FORMAT_R8G8B8A8_UNORM, usage, nullptr);
  call("vkGetImageMemoryRequirements", device, exported_image, &requirements);
  VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, nullptr, exported_image};
  VkExportMemoryAllocateInfo export_info{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, &dedicated, ahb_type};
  VkMemoryAllocateInfo export_allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &export_info, 0,
                                         static_cast<std::uint32_t>(std::countr_zero(requirements.memoryTypeBits))};
  VkDeviceMemory exported_memory{};
  REQUIRE(call("vkAllocateMemory", device, &export_allocation, nullptr, &exported_memory) == VK_SUCCESS);
  REQUIRE(call("vkBindImageMemory", device, exported_image, exported_memory, VkDeviceSize{0}) == VK_SUCCESS);
  VkMemoryGetAndroidHardwareBufferInfoANDROID get{VK_STRUCTURE_TYPE_MEMORY_GET_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,
                                                  nullptr, exported_memory};
  AHardwareBuffer *first = nullptr, *second = nullptr;
  REQUIRE(call("vkGetMemoryAndroidHardwareBufferANDROID", device, &get, &first) == VK_SUCCESS);
  REQUIRE(call("vkGetMemoryAndroidHardwareBufferANDROID", device, &get, &second) == VK_SUCCESS);
  CHECK(first == second);
  auto exported = android::hardware_buffer(h2g(first));
  REQUIRE(exported);
  CHECK(exported->desc.width == 16);
  CHECK(exported->desc.format == 1);
  CHECK(exported->desc.usage == ahb::gpu_sampled_image);
  CHECK(exported->refs == 3);
  exported.reset();
  android::release_hardware_buffer(h2g(first));
  android::release_hardware_buffer(h2g(second));
  VkImage imported_export = make_image(VK_FORMAT_R8G8B8A8_UNORM, usage, nullptr);
  VkDeviceMemory imported_export_memory = import(imported_export, h2g(first));

  // External format: the AHB's identifier with a conversion that keeps RGB.
  VkExternalFormatANDROID external_format{VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID, nullptr, format.externalFormat};
  VkImage external_image = make_image(VK_FORMAT_UNDEFINED, VK_IMAGE_USAGE_SAMPLED_BIT, &external_format);
  VkDeviceMemory external_memory = import(external_image, other);
  VkSamplerYcbcrConversionCreateInfo conversion_info{VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO,
                                                     &external_format, VK_FORMAT_UNDEFINED,
                                                     format.suggestedYcbcrModel, format.suggestedYcbcrRange,
                                                     format.samplerYcbcrConversionComponents,
                                                     format.suggestedXChromaOffset, format.suggestedYChromaOffset,
                                                     VK_FILTER_NEAREST, VK_FALSE};
  VkSamplerYcbcrConversion conversion{};
  REQUIRE(call("vkCreateSamplerYcbcrConversion", device, &conversion_info, nullptr, &conversion) == VK_SUCCESS);
  VkSamplerYcbcrConversionInfo use{VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO, nullptr, conversion};
  VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, &use};
  si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  VkSampler sampler{};
  REQUIRE(call("vkCreateSampler", device, &si, nullptr, &sampler) == VK_SUCCESS);
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, &use};
  vi.image = external_image;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vi.subresourceRange = range;
  VkImageView view{};
  REQUIRE(call("vkCreateImageView", device, &vi, nullptr, &view) == VK_SUCCESS);

  call("vkDestroyImageView", device, view, nullptr);
  call("vkDestroySampler", device, sampler, nullptr);
  call("vkDestroySamplerYcbcrConversion", device, conversion, nullptr);
  call("vkDestroyCommandPool", device, pool, nullptr);
  call("vkDestroyBuffer", device, readback, nullptr);
  call("vkFreeMemory", device, readback_memory, nullptr);
  for (auto image : {writer, reader, exported_image, imported_export, external_image})
    call("vkDestroyImage", device, image, nullptr);
  call("vkFreeMemory", device, writer_memory, nullptr);
  CHECK(android::hardware_buffer(buffer));
  call("vkFreeMemory", device, reader_memory, nullptr);
  CHECK_FALSE(android::hardware_buffer(buffer));  // the last reference was the second import
  call("vkFreeMemory", device, exported_memory, nullptr);
  CHECK(android::hardware_buffer(h2g(first)));  // still bound by the import
  call("vkFreeMemory", device, imported_export_memory, nullptr);
  CHECK_FALSE(android::hardware_buffer(h2g(first)));
  call("vkFreeMemory", device, external_memory, nullptr);
  android::release_hardware_buffer(other);
  CHECK_FALSE(android::hardware_buffer(other));
  call("vkDestroyDevice", device, nullptr);
}

TEST_CASE("Vulkan Android surfaces retain windows and follow buffer geometry") {
  std::uint32_t count = 0;
  REQUIRE(call("vkEnumerateInstanceExtensionProperties", nullptr, &count, nullptr) == VK_SUCCESS);
  std::vector<VkExtensionProperties> extensions(count);
  REQUIRE(call("vkEnumerateInstanceExtensionProperties", nullptr, &count, extensions.data()) == VK_SUCCESS);
  auto has = [&](const char* name) {
    return std::ranges::any_of(extensions, [&](auto& p) { return std::strcmp(p.extensionName, name) == 0; });
  };
  // Emulated on every host (wsi.cpp), with or without host WSI extensions.
  REQUIRE(has("VK_KHR_surface"));
  REQUIRE(has("VK_KHR_android_surface"));
  REQUIRE(has("VK_KHR_get_surface_capabilities2"));
  CHECK_FALSE(has("VK_KHR_xcb_surface"));
  CHECK_FALSE(has("VK_KHR_wayland_surface"));
  CHECK_FALSE(has("VK_EXT_headless_surface"));
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.apiVersion = VK_API_VERSION_1_0;
  const char* names[] = {"VK_KHR_surface", "VK_KHR_android_surface", "VK_KHR_get_surface_capabilities2"};
  VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ci.pApplicationInfo = &app;
  ci.enabledExtensionCount = 3;
  ci.ppEnabledExtensionNames = names;
  struct Resources {
    VkInstance instance{};
    VkSurfaceKHR surface{};
    android::NativeWindow* window{};
    ~Resources() {
      if (surface) call("vkDestroySurfaceKHR", instance, surface, nullptr);
      if (window) android::release(window);
      if (instance) call("vkDestroyInstance", instance, nullptr);
    }
  } resources;
  REQUIRE(call("vkCreateInstance", &ci, nullptr, &resources.instance) == VK_SUCCESS);
  CHECK(call("vkGetInstanceProcAddr", resources.instance, "vkCreateAndroidSurfaceKHR") != 0);
  resources.window = android::create_native_window(64, 64);
  REQUIRE(resources.window);
  const auto address = h2g(resources.window);
  VkAndroidSurfaceCreateInfoKHR surface_info{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
  surface_info.window = reinterpret_cast<ANativeWindow*>(resources.window);
  REQUIRE(call("vkCreateAndroidSurfaceKHR", resources.instance, &surface_info, nullptr, &resources.surface) == VK_SUCCESS);
  REQUIRE(resources.surface != VK_NULL_HANDLE);
  REQUIRE(call("vkEnumeratePhysicalDevices", resources.instance, &count, nullptr) == VK_SUCCESS);
  REQUIRE(count == 1);
  VkPhysicalDevice physical{};
  REQUIRE(call("vkEnumeratePhysicalDevices", resources.instance, &count, &physical) == VK_SUCCESS);
  for (auto size : {VkExtent2D{64, 64}, VkExtent2D{96, 48}}) {
    REQUIRE(android::set_window_geometry(resources.window, size.width, size.height, 0) == 0);
    VkSurfaceCapabilitiesKHR caps{};
    REQUIRE(call("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", physical, resources.surface, &caps) == VK_SUCCESS);
    CHECK(caps.minImageCount > 0);
    CHECK(caps.currentExtent.width == size.width);
    CHECK(caps.currentExtent.height == size.height);
    if (ci.enabledExtensionCount == 3) {
      VkPhysicalDeviceSurfaceInfo2KHR info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR};
      info.surface = resources.surface;
      VkSurfaceCapabilities2KHR caps2{VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR};
      REQUIRE(call("vkGetPhysicalDeviceSurfaceCapabilities2KHR", physical, &info, &caps2) == VK_SUCCESS);
      CHECK(caps2.surfaceCapabilities.currentExtent.width == caps.currentExtent.width);
      CHECK(caps2.surfaceCapabilities.currentExtent.height == caps.currentExtent.height);
    }
  }
  android::release(resources.window);
  resources.window = nullptr;
  CHECK(android::pin_native_window(address));
  call("vkDestroySurfaceKHR", resources.instance, resources.surface, nullptr);
  resources.surface = VK_NULL_HANDLE;
  CHECK_FALSE(android::pin_native_window(address));
}

TEST_CASE("Vulkan WSI publishes completed pixels to the window's shared consumer") {
  test::VulkanDevice gpu(false, true);
  auto* window = android::create_native_window(32, 24);
  REQUIRE(window);
  window->image_consumer = true;
  VkAndroidSurfaceCreateInfoKHR surface_info{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
  surface_info.window = reinterpret_cast<ANativeWindow*>(window);
  VkSurfaceKHR surface{};
  REQUIRE(call("vkCreateAndroidSurfaceKHR", gpu.instance, &surface_info, nullptr, &surface) == VK_SUCCESS);
  VkBool32 present_supported{};
  REQUIRE(call("vkGetPhysicalDeviceSurfaceSupportKHR", gpu.physical, gpu.family, surface, &present_supported) == VK_SUCCESS);
  REQUIRE(present_supported);
  VkSurfaceCapabilitiesKHR capabilities{};
  REQUIRE(call("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", gpu.physical, surface, &capabilities) == VK_SUCCESS);
  std::uint32_t count{};
  REQUIRE(call("vkGetPhysicalDeviceSurfaceFormatsKHR", gpu.physical, surface, &count, nullptr) == VK_SUCCESS);
  std::vector<VkSurfaceFormatKHR> formats(count);
  REQUIRE(call("vkGetPhysicalDeviceSurfaceFormatsKHR", gpu.physical, surface, &count, formats.data()) == VK_SUCCESS);
  REQUIRE_FALSE(formats.empty());
  // Prefer UNORM to make clear-color bytes exact; sRGB is also a valid producer
  // format and the consumer retains the encoding in both cases.
  auto format = formats.front();
  for (auto candidate : formats)
    if (candidate.format == VK_FORMAT_B8G8R8A8_UNORM || candidate.format == VK_FORMAT_R8G8B8A8_UNORM) format = candidate;
  VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  ci.surface = surface; ci.minImageCount = capabilities.minImageCount;
  ci.imageFormat = format.format; ci.imageColorSpace = format.colorSpace;
  ci.imageExtent = capabilities.currentExtent.width == UINT32_MAX ? VkExtent2D{32, 24} : capabilities.currentExtent;
  ci.imageArrayLayers = 1;
  ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  ci.preTransform = capabilities.currentTransform;
  ci.compositeAlpha = (capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
      ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
  ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
  VkSwapchainKHR chain{};
  REQUIRE(call("vkCreateSwapchainKHR", gpu.device, &ci, nullptr, &chain) == VK_SUCCESS);
  REQUIRE(call("vkGetSwapchainImagesKHR", gpu.device, chain, &count, nullptr) == VK_SUCCESS);
  std::vector<VkImage> images(count);
  REQUIRE(call("vkGetSwapchainImagesKHR", gpu.device, chain, &count, images.data()) == VK_SUCCESS);
  VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  VkSemaphore acquired{}, ready{}, imported{};
  REQUIRE(call("vkCreateSemaphore", gpu.device, &semaphore_info, nullptr, &acquired) == VK_SUCCESS);
  REQUIRE(call("vkCreateSemaphore", gpu.device, &semaphore_info, nullptr, &ready) == VK_SUCCESS);
  REQUIRE(call("vkCreateSemaphore", gpu.device, &semaphore_info, nullptr, &imported) == VK_SUCCESS);
  // The present also waits on a SYNC_FD import (an already-signaled sync file;
  // the layer is called directly, as the device enables only the swapchain).
  auto import_fd = reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(vulkan::sync_fd_proc("vkImportSemaphoreFdKHR"));
  VkImportSemaphoreFdInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
  import.semaphore = imported; import.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
  import.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT; import.fd = -1;
  REQUIRE(import_fd(gpu.device, &import) == VK_SUCCESS);
  const auto signals = vulkan::sync_fd_state(gpu.device).import_signals;
  std::uint32_t index{};
  REQUIRE(call("vkAcquireNextImageKHR", gpu.device, chain, UINT64_MAX, acquired, VK_NULL_HANDLE, &index) == VK_SUCCESS);
  gpu.begin();
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = images[index]; barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  call("vkCmdPipelineBarrier", gpu.command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
      0u, 0u, nullptr, 0u, nullptr, 1u, &barrier);
  VkClearColorValue color{{.25f, .5f, .75f, 1}};
  call("vkCmdClearColorImage", gpu.command, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1u, &barrier.subresourceRange);
  barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = 0;
  call("vkCmdPipelineBarrier", gpu.command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
      0u, 0u, nullptr, 0u, nullptr, 1u, &barrier);
  REQUIRE(call("vkEndCommandBuffer", gpu.command) == VK_SUCCESS);
  VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.waitSemaphoreCount = 1; submit.pWaitSemaphores = &acquired; submit.pWaitDstStageMask = &stage;
  submit.commandBufferCount = 1; submit.pCommandBuffers = &gpu.command;
  submit.signalSemaphoreCount = 1; submit.pSignalSemaphores = &ready;
  REQUIRE(call("vkQueueSubmit", gpu.queue, 1u, &submit, VK_NULL_HANDLE) == VK_SUCCESS);
  VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
  VkSemaphore present_waits[]{ready, imported};
  present.waitSemaphoreCount = 2; present.pWaitSemaphores = present_waits;
  present.swapchainCount = 1; present.pSwapchains = &chain; present.pImageIndices = &index;
  REQUIRE(call("vkQueuePresentKHR", gpu.queue, &present) == VK_SUCCESS);
  // The readback submit executed the present's waits, consuming the imported
  // payload: the next import signals the semaphore again.
  REQUIRE(import_fd(gpu.device, &import) == VK_SUCCESS);
  CHECK(vulkan::sync_fd_state(gpu.device).import_signals == signals + 1);
  REQUIRE(window->frame);
  auto gpu_frame = window->frame;
  CHECK(gpu_frame->width == ci.imageExtent.width);
  CHECK(gpu_frame->height == ci.imageExtent.height);
  if (format.format == VK_FORMAT_B8G8R8A8_UNORM || format.format == VK_FORMAT_R8G8B8A8_UNORM) {
    CHECK(test::unorm8_matches(gpu_frame->rgba[0], .25));
    CHECK(test::unorm8_matches(gpu_frame->rgba[1], .5));
    CHECK(test::unorm8_matches(gpu_frame->rgba[2], .75));
  }
  CHECK(gpu_frame->rgba[3] == 255);
  // Switch producer: CPU-posted RGB565 replaces the same frame, preserving
  // the previous immutable GPU snapshot and its sRGB-encoded channel values.
  REQUIRE(android::set_window_geometry(window, 2, 2, 4) == 0);
  android::WindowBuffer pixels{};
  REQUIRE(android::lock_window(window, &pixels, nullptr) == 0);
  auto* rgb565 = g2h<std::uint16_t>(pixels.bits);
  for (int i = 0; i < 4; ++i) rgb565[i] = 0xf800;
  REQUIRE(android::unlock_window(window) == 0);
  CHECK(window->frame->serial == gpu_frame->serial + 1);
  CHECK(window->frame->rgba[0] == 255); CHECK(window->frame->rgba[1] == 0); CHECK(window->frame->rgba[2] == 0);
  CHECK(gpu_frame->rgba[3] == 255);
  REQUIRE(call("vkDeviceWaitIdle", gpu.device) == VK_SUCCESS);
  call("vkDestroySemaphore", gpu.device, acquired, nullptr);
  call("vkDestroySemaphore", gpu.device, ready, nullptr);
  call("vkDestroySemaphore", gpu.device, imported, nullptr);
  call("vkDestroySwapchainKHR", gpu.device, chain, nullptr);
  call("vkDestroySurfaceKHR", gpu.instance, surface, nullptr);
  android::release(window);
}

TEST_CASE("Vulkan preserves guest core robustness and null descriptor enablement") {
  test::VulkanDevice resources;
  auto& gpu = gfx::gpu_device();
  static PFN_vkGetInstanceProcAddr original;
  original = gpu.get_instance_proc_addr;
  static bool buffer, null_descriptor, copied;
  buffer = null_descriptor = copied = false;
  static const void* guest_chain;
  struct Restore {
    gfx::GpuDevice& gpu;
    ~Restore() { gpu.get_instance_proc_addr = original; }
  } restore{gpu};
  // Capture the final host create without depending on the software driver's
  // optional robustness2 support. Failure must leave the guest chain intact.
  gpu.get_instance_proc_addr = +[](VkInstance i, const char* name) -> PFN_vkVoidFunction {
    if (std::string_view(name) == "vkCreateDevice")
      return reinterpret_cast<PFN_vkVoidFunction>(+[](VkPhysicalDevice, const VkDeviceCreateInfo* info,
          const VkAllocationCallbacks*, VkDevice*) -> VkResult {
        buffer = info->pEnabledFeatures && info->pEnabledFeatures->robustBufferAccess;
        for (auto* next = static_cast<const VkBaseInStructure*>(info->pNext); next; next = next->pNext) {
          if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2)
            buffer = reinterpret_cast<const VkPhysicalDeviceFeatures2*>(next)->features.robustBufferAccess;
          if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT) {
            null_descriptor = reinterpret_cast<const VkPhysicalDeviceRobustness2FeaturesEXT*>(next)->nullDescriptor;
            copied = next != guest_chain;
          }
        }
        return VK_ERROR_INITIALIZATION_FAILED;
      });
    return original(i, name);
  };
  VkPhysicalDeviceFeatures features{};
  features.robustBufferAccess = VK_TRUE;
  VkPhysicalDeviceRobustness2FeaturesEXT robust{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
  robust.nullDescriptor = VK_TRUE;
  guest_chain = &robust;
  VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &robust, features};
  float priority = 1;
  VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  queue.queueFamilyIndex = resources.family; queue.queueCount = 1; queue.pQueuePriorities = &priority;
  const char* extension = VK_EXT_ROBUSTNESS_2_EXTENSION_NAME;
  VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &robust};
  info.pEnabledFeatures = &features;
  info.queueCreateInfoCount = 1; info.pQueueCreateInfos = &queue;
  info.enabledExtensionCount = 1; info.ppEnabledExtensionNames = &extension;
  SUBCASE("core features pointer") {}
  SUBCASE("features2 chain") { info.pEnabledFeatures = nullptr; info.pNext = &features2; }
  VkDevice device{};
  CHECK(call("vkCreateDevice", resources.physical, &info, nullptr, &device) == VK_ERROR_INITIALIZATION_FAILED);
  CHECK(buffer);
  CHECK(null_descriptor);
  CHECK(copied);
  CHECK(features.robustBufferAccess);
  CHECK(features2.features.robustBufferAccess);
  CHECK(features2.pNext == &robust);
  CHECK(robust.nullDescriptor);
  CHECK(robust.pNext == nullptr);
}

TEST_CASE("Vulkan swapchains are emulated without a host surface") {
  test::VulkanDevice gpu(false, true);
  auto* window = android::create_native_window(40, 30);
  REQUIRE(window);
  struct Window { android::NativeWindow* w; ~Window() { android::release(w); } } owner{window};
  VkAndroidSurfaceCreateInfoKHR surface_info{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
  surface_info.window = reinterpret_cast<ANativeWindow*>(window);
  VkSurfaceKHR surface{};
  REQUIRE(call("vkCreateAndroidSurfaceKHR", gpu.instance, &surface_info, nullptr, &surface) == VK_SUCCESS);
  VkSurfaceCapabilitiesKHR caps{};
  REQUIRE(call("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", gpu.physical, surface, &caps) == VK_SUCCESS);
  CHECK(caps.currentExtent.width == 40);
  CHECK(caps.currentExtent.height == 30);
  CHECK(caps.maxImageArrayLayers == 1);
  std::uint32_t count{};
  REQUIRE(call("vkGetPhysicalDeviceSurfacePresentModesKHR", gpu.physical, surface, &count, nullptr) == VK_SUCCESS);
  std::vector<VkPresentModeKHR> modes(count);
  REQUIRE(call("vkGetPhysicalDeviceSurfacePresentModesKHR", gpu.physical, surface, &count, modes.data()) == VK_SUCCESS);
  CHECK(std::ranges::find(modes, VK_PRESENT_MODE_FIFO_KHR) != modes.end());
  // Render passes ending in PRESENT_SRC_KHR are valid for the guest.
  VkAttachmentDescription attachment{};
  attachment.format = VK_FORMAT_R8G8B8A8_UNORM; attachment.samples = VK_SAMPLE_COUNT_1_BIT;
  attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1; subpass.pColorAttachments = &reference;
  VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  rp.attachmentCount = 1; rp.pAttachments = &attachment; rp.subpassCount = 1; rp.pSubpasses = &subpass;
  VkRenderPass pass{};
  REQUIRE(call("vkCreateRenderPass", gpu.device, &rp, nullptr, &pass) == VK_SUCCESS);
  call("vkDestroyRenderPass", gpu.device, pass, nullptr);

  VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  ci.surface = surface; ci.minImageCount = 3;
  ci.imageFormat = VK_FORMAT_R8G8B8A8_UNORM; ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
  ci.imageExtent = caps.currentExtent; ci.imageArrayLayers = 1;
  ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  ci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR; ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
  VkSwapchainKHR chain{};
  REQUIRE(call("vkCreateSwapchainKHR", gpu.device, &ci, nullptr, &chain) == VK_SUCCESS);
  REQUIRE(call("vkGetSwapchainImagesKHR", gpu.device, chain, &count, nullptr) == VK_SUCCESS);
  REQUIRE(count == 3);
  std::vector<VkImage> images(count);
  REQUIRE(call("vkGetSwapchainImagesKHR", gpu.device, chain, &count, images.data()) == VK_SUCCESS);
  VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  VkSemaphore acquired{}, ready{};
  REQUIRE(call("vkCreateSemaphore", gpu.device, &semaphore_info, nullptr, &acquired) == VK_SUCCESS);
  REQUIRE(call("vkCreateSemaphore", gpu.device, &semaphore_info, nullptr, &ready) == VK_SUCCESS);
  VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VkFence acquired_fence{};
  REQUIRE(call("vkCreateFence", gpu.device, &fence_info, nullptr, &acquired_fence) == VK_SUCCESS);
  std::vector<std::uint32_t> order;
  for (int frame = 0; frame < 7; ++frame) {
    std::uint32_t index{};
    // Alternate semaphore-only and semaphore-plus-fence acquires.
    const auto fence = frame % 2 ? acquired_fence : VK_NULL_HANDLE;
    REQUIRE(call("vkAcquireNextImageKHR", gpu.device, chain, UINT64_MAX, acquired, fence, &index) == VK_SUCCESS);
    if (fence) {
      REQUIRE(call("vkWaitForFences", gpu.device, 1u, &fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS);
      REQUIRE(call("vkResetFences", gpu.device, 1u, &fence) == VK_SUCCESS);
    }
    order.push_back(index);
    gpu.begin();
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = images[index]; barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    call("vkCmdPipelineBarrier", gpu.command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0u, 0u, nullptr, 0u, nullptr, 1u, &barrier);
    VkClearColorValue color{{float(frame) / 7, 0, 0, 1}};
    call("vkCmdClearColorImage", gpu.command, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1u,
        &barrier.subresourceRange);
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = 0;
    call("vkCmdPipelineBarrier", gpu.command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0u, 0u, nullptr, 0u, nullptr, 1u, &barrier);
    REQUIRE(call("vkEndCommandBuffer", gpu.command) == VK_SUCCESS);
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1; submit.pWaitSemaphores = &acquired; submit.pWaitDstStageMask = &stage;
    submit.commandBufferCount = 1; submit.pCommandBuffers = &gpu.command;
    submit.signalSemaphoreCount = 1; submit.pSignalSemaphores = &ready;
    REQUIRE(call("vkQueueSubmit", gpu.queue, 1u, &submit, VK_NULL_HANDLE) == VK_SUCCESS);
    VkResult result = VK_ERROR_UNKNOWN;
    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1; present.pWaitSemaphores = &ready;
    present.swapchainCount = 1; present.pSwapchains = &chain; present.pImageIndices = &index; present.pResults = &result;
    REQUIRE(call("vkQueuePresentKHR", gpu.queue, &present) == VK_SUCCESS);
    CHECK(result == VK_SUCCESS);
    // The command buffer is reused next frame.
    REQUIRE(call("vkQueueWaitIdle", gpu.queue) == VK_SUCCESS);
  }
  // Images return in presentation order.
  CHECK(order == std::vector<std::uint32_t>{0, 1, 2, 0, 1, 2, 0});
  // A resized window makes later presents suboptimal, until the guest recreates.
  REQUIRE(android::set_window_geometry(window, 48, 32, 0) == 0);
  std::uint32_t index{};
  REQUIRE(call("vkAcquireNextImageKHR", gpu.device, chain, UINT64_MAX, VK_NULL_HANDLE, acquired_fence, &index) ==
          VK_SUCCESS);
  REQUIRE(call("vkWaitForFences", gpu.device, 1u, &acquired_fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS);
  VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
  present.swapchainCount = 1; present.pSwapchains = &chain; present.pImageIndices = &index;
  CHECK(call("vkQueuePresentKHR", gpu.queue, &present) == VK_SUBOPTIMAL_KHR);
  // With every image acquired, a zero timeout reports NOT_READY.
  for (std::uint32_t i = 0; i < 3; ++i) {
    REQUIRE(call("vkResetFences", gpu.device, 1u, &acquired_fence) == VK_SUCCESS);
    REQUIRE(call("vkAcquireNextImageKHR", gpu.device, chain, UINT64_MAX, VK_NULL_HANDLE, acquired_fence, &index) ==
            VK_SUCCESS);
    REQUIRE(call("vkWaitForFences", gpu.device, 1u, &acquired_fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS);
  }
  CHECK(call("vkAcquireNextImageKHR", gpu.device, chain, 0ull, acquired, VK_NULL_HANDLE, &index) == VK_NOT_READY);
  // Replacing the swapchain retires the old one.
  ci.imageExtent = {48, 32};
  ci.oldSwapchain = chain;
  VkSwapchainKHR replacement{};
  REQUIRE(call("vkCreateSwapchainKHR", gpu.device, &ci, nullptr, &replacement) == VK_SUCCESS);
  CHECK(call("vkAcquireNextImageKHR", gpu.device, chain, 0ull, acquired, VK_NULL_HANDLE, &index) ==
        VK_ERROR_OUT_OF_DATE_KHR);
  call("vkDestroySwapchainKHR", gpu.device, chain, nullptr);
  call("vkDestroySwapchainKHR", gpu.device, replacement, nullptr);
  call("vkDestroyFence", gpu.device, acquired_fence, nullptr);
  call("vkDestroySemaphore", gpu.device, acquired, nullptr);
  call("vkDestroySemaphore", gpu.device, ready, nullptr);
  call("vkDestroySurfaceKHR", gpu.instance, surface, nullptr);
}

TEST_CASE("Vulkan command buffers stay tracked per pool through free, reset and destroy") {
  test::VulkanDevice gpu;
  VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pi.queueFamilyIndex = gpu.family;
  VkCommandPool first{}, second{};
  REQUIRE(call("vkCreateCommandPool", gpu.device, &pi, nullptr, &first) == VK_SUCCESS);
  REQUIRE(call("vkCreateCommandPool", gpu.device, &pi, nullptr, &second) == VK_SUCCESS);
  VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ai.commandPool = first; ai.commandBufferCount = 2;
  VkCommandBuffer a[2]{}, b{};
  REQUIRE(call("vkAllocateCommandBuffers", gpu.device, &ai, a) == VK_SUCCESS);
  ai.commandPool = second; ai.commandBufferCount = 1;
  REQUIRE(call("vkAllocateCommandBuffers", gpu.device, &ai, &b) == VK_SUCCESS);
  call("vkFreeCommandBuffers", gpu.device, first, 1u, &a[0]);
  CHECK_THROWS(vulkan::command_pool(a[0]));
  REQUIRE(call("vkResetCommandPool", gpu.device, first, 0u) == VK_SUCCESS);
  CHECK(vulkan::command_pool(a[1]) == first);
  CHECK(vulkan::command_pool(b) == second);
  call("vkDestroyCommandPool", gpu.device, first, nullptr);
  CHECK_THROWS(vulkan::command_pool(a[1]));
  CHECK(vulkan::command_pool(b) == second);
  call("vkDestroyCommandPool", gpu.device, second, nullptr);
  CHECK_THROWS(vulkan::command_pool(b));
}

TEST_CASE("Vulkan guest memory frees wait for the work submitted before them") {
  test::VulkanDevice gpu;
  VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bi.size = 4096; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  VkBuffer buffer{};
  REQUIRE(call("vkCreateBuffer", gpu.device, &bi, nullptr, &buffer) == VK_SUCCESS);
  VkMemoryRequirements requirements{};
  call("vkGetBufferMemoryRequirements", gpu.device, buffer, &requirements);
  VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocation.allocationSize = requirements.size;
  allocation.memoryTypeIndex = gpu.memory_type(requirements.memoryTypeBits, 0);
  VkDeviceMemory idle{}, busy{};
  // Nothing submitted yet: freed at once.
  REQUIRE(call("vkAllocateMemory", gpu.device, &allocation, nullptr, &idle) == VK_SUCCESS);
  call("vkFreeMemory", gpu.device, idle, nullptr);
  CHECK(vulkan::deferred_free_pending(gpu.device) == 0);
  REQUIRE(call("vkAllocateMemory", gpu.device, &allocation, nullptr, &busy) == VK_SUCCESS);
  REQUIRE(call("vkBindBufferMemory", gpu.device, buffer, busy, 0ull) == VK_SUCCESS);
  // Work that writes the memory, held on the GPU until the host sets an event.
  VkEventCreateInfo ei{VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
  VkEvent event{};
  REQUIRE(call("vkCreateEvent", gpu.device, &ei, nullptr, &event) == VK_SUCCESS);
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  REQUIRE(call("vkBeginCommandBuffer", gpu.command, &begin) == VK_SUCCESS);
  call("vkCmdWaitEvents", gpu.command, 1u, &event, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
       0u, nullptr, 0u, nullptr, 0u, nullptr);
  call("vkCmdFillBuffer", gpu.command, buffer, 0ull, 4096ull, 0x5eedU);
  REQUIRE(call("vkEndCommandBuffer", gpu.command) == VK_SUCCESS);
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1; submit.pCommandBuffers = &gpu.command;
  REQUIRE(call("vkQueueSubmit", gpu.queue, 1u, &submit, VK_NULL_HANDLE) == VK_SUCCESS);
  call("vkFreeMemory", gpu.device, busy, nullptr);
  CHECK(vulkan::deferred_free_pending(gpu.device) == 1);
  bool range_released = false;
  vulkan::defer_gpu_release(gpu.device, [&] { range_released = true; });
  CHECK_FALSE(range_released);
  CHECK(vulkan::deferred_free_pending(gpu.device) == 2);
  REQUIRE(call("vkSetEvent", gpu.device, event) == VK_SUCCESS);
  // The next submission to the queue fences the earlier work; completion frees.
  REQUIRE(call("vkQueueSubmit", gpu.queue, 0u, nullptr, VK_NULL_HANDLE) == VK_SUCCESS);
  REQUIRE(call("vkQueueWaitIdle", gpu.queue) == VK_SUCCESS);
  CHECK(vulkan::deferred_free_pending(gpu.device) == 0);
  CHECK(range_released);
  call("vkDestroyEvent", gpu.device, event, nullptr);
  call("vkDestroyBuffer", gpu.device, buffer, nullptr);
}

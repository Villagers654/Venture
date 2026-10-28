#include <doctest/doctest.h>
#include <atomic>
#include <thread>
#include <future>
#include "common/environment.h"
#include "android/native_window.h"

#include "gfx/queue_sync.h"
#include "hle/vulkan/serialized_queues.h"
#include "hle/vulkan/deferred_queue_submissions.h"
#include "vulkan_test_support.h"

using namespace venture;
namespace {
template<class F> F native(VkDevice device, const char* name) {
  auto fn = reinterpret_cast<F>(vulkan::raw_device_proc(device, name));
  REQUIRE(fn);
  return fn;
}
vulkan::SerializedQueues::Driver driver(VkDevice device);

TEST_CASE("Vulkan deferred queue bindings own future timeline waits and binary successors") {
  test::VulkanDevice d(false, false, true);
  auto vk = driver(d.device);
  auto counter = native<PFN_vkGetSemaphoreCounterValue>(d.device, "vkGetSemaphoreCounterValue");
  auto create = native<PFN_vkCreateSemaphore>(d.device, "vkCreateSemaphore");
  auto destroy = native<PFN_vkDestroySemaphore>(d.device, "vkDestroySemaphore");
  VkSemaphore timeline{}, binary{};
  VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
  type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
  VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type};
  REQUIRE(create(d.device, &si, nullptr, &timeline) == VK_SUCCESS);
  si.pNext = nullptr;
  REQUIRE(create(d.device, &si, nullptr, &binary) == VK_SUCCESS);
  VkFence fence{};
  VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  REQUIRE(vk.create_fence(d.device, &fi, nullptr, &fence) == VK_SUCCESS);
  {
    auto backend = std::make_shared<vulkan::SerializedQueues>(d.device, d.queue, 3,
        gfx::queue_mutex(d.queue), vk);
    // Exercise independent native backend bindings on the most constrained
    // driver: both backends share its single real FIFO and exclusion mutex.
    // A future-wait consumer must not reach that FIFO ahead of its producer.
    auto producer = std::make_shared<vulkan::SerializedQueues>(d.device, d.queue, 1,
        gfx::queue_mutex(d.queue), vk);
    std::vector<vulkan::DeferredQueueSubmissions::Binding> bindings{
        {backend, 0}, {producer, 0}, {backend, 2}};
    vulkan::DeferredQueueSubmissions queues(d.device, std::move(bindings), vk.submit, counter);
    queues.add_semaphore(timeline, true); queues.add_semaphore(binary, false);
    std::uint64_t target = 7, ignored = 0;
    VkTimelineSemaphoreSubmitInfo values{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    values.waitSemaphoreValueCount = 1; values.pWaitSemaphoreValues = &target;
    values.signalSemaphoreValueCount = 1; values.pSignalSemaphoreValues = &ignored;
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSemaphore original_wait = timeline, original_signal = binary;
    VkSubmitInfo wait{VK_STRUCTURE_TYPE_SUBMIT_INFO, &values};
    wait.waitSemaphoreCount = 1; wait.pWaitSemaphores = &original_wait; wait.pWaitDstStageMask = &stage;
    wait.signalSemaphoreCount = 1; wait.pSignalSemaphores = &original_signal;
    REQUIRE(queues.submit(0, 1, &wait, VK_NULL_HANDLE) == VK_SUCCESS);
    CHECK(queues.pending(0) == 1);
    CHECK(backend->issued(0) == 0);
    // Invocation's translated arrays go away at API return. Mutating every
    // borrowed field must not change the already accepted deferred operation.
    target = 99; original_wait = VK_NULL_HANDLE; original_signal = VK_NULL_HANDLE;
    values.pWaitSemaphoreValues = nullptr; wait.pNext = nullptr;
    VkSubmitInfo successor{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    successor.waitSemaphoreCount = 1; successor.pWaitSemaphores = &binary;
    successor.pWaitDstStageMask = &stage;
    REQUIRE(queues.submit(2, 1, &successor, fence) == VK_SUCCESS);
    CHECK(queues.pending(2) == 1);
    CHECK(backend->issued(2) == 0);
    std::uint64_t seven = 7;
    VkTimelineSemaphoreSubmitInfo signal_values{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    signal_values.signalSemaphoreValueCount = 1; signal_values.pSignalSemaphoreValues = &seven;
    VkSubmitInfo signal{VK_STRUCTURE_TYPE_SUBMIT_INFO, &signal_values};
    signal.signalSemaphoreCount = 1; signal.pSignalSemaphores = &timeline;
    REQUIRE(queues.submit(1, 1, &signal, VK_NULL_HANDLE) == VK_SUCCESS);
    CHECK(producer->issued(0) == 1);
    CHECK(backend->issued(1) == 0);
    CHECK(queues.pending(0) == 0);
    CHECK(queues.pending(1) == 0);
    CHECK(queues.pending(2) == 0);
    REQUIRE(queues.wait_idle(2) == VK_SUCCESS);
    REQUIRE(queues.wait_idle(1) == VK_SUCCESS);
    CHECK(producer->completed(0) == 1);
    CHECK(vk.fence_status(d.device, fence) == VK_SUCCESS);
    std::uint64_t actual{};
    REQUIRE(counter(d.device, timeline, &actual) == VK_SUCCESS);
    CHECK(actual == 7);
    CHECK(backend->completed(2) == 1);
  }
  vk.destroy_fence(d.device, fence, nullptr);
  destroy(d.device, binary, nullptr); destroy(d.device, timeline, nullptr);
}
vulkan::SerializedQueues::Driver driver(VkDevice device) {
  return {native<PFN_vkCreateFence>(device, "vkCreateFence"),
    native<PFN_vkDestroyFence>(device, "vkDestroyFence"),
    native<PFN_vkResetFences>(device, "vkResetFences"),
    native<PFN_vkGetFenceStatus>(device, "vkGetFenceStatus"),
    native<PFN_vkWaitForFences>(device, "vkWaitForFences"),
    native<PFN_vkQueueSubmit>(device, "vkQueueSubmit")};
}
}
TEST_CASE("Vulkan serialized native queues preserve binary waits and guest fences") {
  test::VulkanDevice d;
  auto vk = driver(d.device);
  auto create_semaphore = native<PFN_vkCreateSemaphore>(d.device, "vkCreateSemaphore");
  auto destroy_semaphore = native<PFN_vkDestroySemaphore>(d.device, "vkDestroySemaphore");
  VkSemaphore semaphore{};
  VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  REQUIRE(create_semaphore(d.device, &si, nullptr, &semaphore) == VK_SUCCESS);
  VkFence guest_fence{};
  VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  REQUIRE(vk.create_fence(d.device, &fi, nullptr, &guest_fence) == VK_SUCCESS);
  {
    vulkan::SerializedQueues queues(d.device, d.queue, 3, gfx::queue_mutex(d.queue), vk);
    const auto caller = std::this_thread::get_id();
    VkSubmitInfo signal{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    signal.signalSemaphoreCount = 1;
    signal.pSignalSemaphores = &semaphore;
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo wait{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    wait.waitSemaphoreCount = 1;
    wait.pWaitSemaphores = &semaphore;
    wait.pWaitDstStageMask = &stage;
    REQUIRE(queues.submit(0, [&](VkQueue q) {
      CHECK(std::this_thread::get_id() == caller);
      CHECK(q == d.queue);
      return vk.submit(q, 1, &signal, guest_fence);
    }) == VK_SUCCESS);
    REQUIRE(queues.submit(1, [&](VkQueue q) {
      CHECK(std::this_thread::get_id() == caller);
      return vk.submit(q, 1, &wait, VK_NULL_HANDLE);
    }) == VK_SUCCESS);
    CHECK(queues.issued(0) == 1);
    CHECK(queues.issued(1) == 1);
    CHECK(queues.issued(2) == 0);
    CHECK(queues.wait_idle(2) == VK_SUCCESS);
    REQUIRE(queues.wait_idle(0) == VK_SUCCESS);
    CHECK(vk.fence_status(d.device, guest_fence) == VK_SUCCESS);
    CHECK(queues.completed(0) == 1);
    REQUIRE(queues.wait_idle(1) == VK_SUCCESS);
    CHECK(queues.completed(1) == 1);
    // Repeated binary payload reuse remains native; no semaphore completion
    // is manufactured by the broker and completion slots remain bounded.
    for (unsigned i = 0; i < 160; ++i) {
      REQUIRE(queues.submit(0, [&](VkQueue q) { return vk.submit(q, 1, &signal, VK_NULL_HANDLE); }) == VK_SUCCESS);
      REQUIRE(queues.submit(1, [&](VkQueue q) { return vk.submit(q, 1, &wait, VK_NULL_HANDLE); }) == VK_SUCCESS);
    }
    REQUIRE(queues.wait_idle(0) == VK_SUCCESS);
    REQUIRE(queues.wait_idle(1) == VK_SUCCESS);
    CHECK(queues.completed(0) == 161);
    CHECK(queues.completed(1) == 161);
    CHECK(queues.wait_idle(3) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(queues.submit(3, [](VkQueue) { return VK_SUCCESS; }) == VK_ERROR_INITIALIZATION_FAILED);
  }
  vk.destroy_fence(d.device, guest_fence, nullptr);
  destroy_semaphore(d.device, semaphore, nullptr);
}

TEST_CASE("Vulkan deferred WSI error consumes waits and permits the next frame") {
  test::VulkanDevice d(false, true, true);
  auto get_families = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
      vulkan::raw_physical_proc(d.physical, "vkGetPhysicalDeviceQueueFamilyProperties"));
  std::uint32_t count{}; get_families(d.physical, &count, nullptr);
  std::vector<VkQueueFamilyProperties> families(count);
  get_families(d.physical, &count, families.data());
  if (families.at(d.family).queueCount >= 3) {
    MESSAGE("Native graphics queues need no shared fallback"); return;
  }
  const float priorities[]{1, .5f, .5f};
  VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qi.queueFamilyIndex = d.family; qi.queueCount = 3; qi.pQueuePriorities = priorities;
  VkPhysicalDeviceTimelineSemaphoreFeatures timelines{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
  timelines.timelineSemaphore = VK_TRUE;
  const char* extension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
  VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &timelines};
  ci.queueCreateInfoCount = 1; ci.pQueueCreateInfos = &qi;
  ci.enabledExtensionCount = 1; ci.ppEnabledExtensionNames = &extension;
  VkDevice device{};
  REQUIRE(test::vk_call("vkCreateDevice", d.physical, &ci, nullptr, &device) == VK_SUCCESS);
  VkQueue queue{}; test::vk_call("vkGetDeviceQueue", device, d.family, 0u, &queue);
  auto* window = android::create_native_window(40, 30);
  REQUIRE(window);
  VkAndroidSurfaceCreateInfoKHR surface_info{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
  surface_info.window = reinterpret_cast<ANativeWindow*>(window);
  VkSurfaceKHR surface{};
  REQUIRE(test::vk_call("vkCreateAndroidSurfaceKHR", d.instance, &surface_info, nullptr, &surface) == VK_SUCCESS);
  VkSwapchainCreateInfoKHR swap{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  swap.surface = surface; swap.minImageCount = 3; swap.imageFormat = VK_FORMAT_R8G8B8A8_UNORM;
  swap.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR; swap.imageExtent = {40, 30};
  swap.imageArrayLayers = 1; swap.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  swap.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR; swap.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  swap.presentMode = VK_PRESENT_MODE_FIFO_KHR;
  VkSwapchainKHR chain{};
  REQUIRE(test::vk_call("vkCreateSwapchainKHR", device, &swap, nullptr, &chain) == VK_SUCCESS);
  VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  VkSemaphore acquired{}, ready{}, timeline{};
  REQUIRE(test::vk_call("vkCreateSemaphore", device, &si, nullptr, &acquired) == VK_SUCCESS);
  REQUIRE(test::vk_call("vkCreateSemaphore", device, &si, nullptr, &ready) == VK_SUCCESS);
  VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
  type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE; si.pNext = &type;
  REQUIRE(test::vk_call("vkCreateSemaphore", device, &si, nullptr, &timeline) == VK_SUCCESS);
  for (std::uint64_t frame = 1; frame <= 2; ++frame) {
    std::uint32_t index{};
    REQUIRE(test::vk_call("vkAcquireNextImageKHR", device, chain, UINT64_MAX, acquired, VK_NULL_HANDLE, &index) == VK_SUCCESS);
    const VkSemaphore waits[]{timeline, acquired};
    const std::uint64_t values[]{frame, 0}, zero = 0;
    const VkPipelineStageFlags stages[]{VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
    VkTimelineSemaphoreSubmitInfo times{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    times.waitSemaphoreValueCount = 2; times.pWaitSemaphoreValues = values;
    times.signalSemaphoreValueCount = 1; times.pSignalSemaphoreValues = &zero;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO, &times};
    submit.waitSemaphoreCount = 2; submit.pWaitSemaphores = waits; submit.pWaitDstStageMask = stages;
    submit.signalSemaphoreCount = 1; submit.pSignalSemaphores = &ready;
    REQUIRE(test::vk_call("vkQueueSubmit", queue, 1u, &submit, VK_NULL_HANDLE) == VK_SUCCESS);
    auto presented_index = frame == 1 ? UINT32_MAX : index;
    VkResult result = VK_ERROR_UNKNOWN;
    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1; present.pWaitSemaphores = &ready;
    present.swapchainCount = 1; present.pSwapchains = &chain; present.pImageIndices = &presented_index;
    present.pResults = &result;
    REQUIRE(test::vk_call("vkQueuePresentKHR", queue, &present) == VK_SUCCESS);
    CHECK(result == VK_SUCCESS);
    result = VK_TIMEOUT; // No deferred callback may write this guest output.
    VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
    signal.semaphore = timeline; signal.value = frame;
    REQUIRE(test::vk_call("vkSignalSemaphore", device, &signal) == VK_SUCCESS);
    REQUIRE(test::vk_call("vkQueueWaitIdle", queue) == VK_SUCCESS);
    CHECK(result == VK_TIMEOUT);
    if (frame == 1) {
      std::uint32_t ignored{};
      CHECK(test::vk_call("vkAcquireNextImageKHR", device, chain, 0ull, VK_NULL_HANDLE, VK_NULL_HANDLE, &ignored) == VK_ERROR_SURFACE_LOST_KHR);
      present.waitSemaphoreCount = 0; present.pImageIndices = &index; present.pResults = nullptr;
      REQUIRE(test::vk_call("vkQueuePresentKHR", queue, &present) == VK_SUCCESS);
      REQUIRE(test::vk_call("vkQueueWaitIdle", queue) == VK_SUCCESS);
    }
  }
  REQUIRE(test::vk_call("vkDeviceWaitIdle", device) == VK_SUCCESS);
  test::vk_call("vkDestroySwapchainKHR", device, chain, nullptr);
  for (auto semaphore : {acquired, ready, timeline}) test::vk_call("vkDestroySemaphore", device, semaphore, nullptr);
  test::vk_call("vkDestroySurfaceKHR", d.instance, surface, nullptr);
  android::release(window);
  test::vk_call("vkDestroyDevice", device, nullptr);
}

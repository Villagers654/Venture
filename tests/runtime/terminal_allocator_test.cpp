#include "vulkan_test_support.h"
#include "vulkan_callbacks_test_support.h"
#include "hle/vulkan/callbacks.h"
#include "hle/vulkan/terminal_allocator.h"
#include "core/os_quit.h"
#include "gfx/gpu_device.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <new>

// Irreversible process state: runs alone in its own process (tests.cmake).
TEST_CASE("Terminal allocator bridge drains a real native device without guest reentry" * doctest::skip()) {
  using namespace venture;
  struct Allocation { std::size_t size, alignment; };
  std::mutex mutex;
  std::map<void*, Allocation> allocations;
  std::atomic<unsigned> calls{0};
  test::set_vulkan_callback([&](GuestAddr fn, const GuestCallArgs& args) {
    std::lock_guard lock(mutex);
    ++calls;
    const auto& words = args.integers;
    if (fn == 3) {
      auto* memory = g2h(words[1]);
      if (memory) {
        auto it = allocations.find(memory);
        REQUIRE(it != allocations.end());
        ::operator delete(memory, std::align_val_t(it->second.alignment));
        allocations.erase(it);
      }
      return GuestCallResult{};
    }
    const bool reallocating = fn == 2;
    const auto bytes = static_cast<std::size_t>(words[reallocating ? 2 : 1]);
    const auto alignment = std::max<std::size_t>(alignof(std::max_align_t), words[reallocating ? 3 : 2]);
    auto* memory = ::operator new(std::max<std::size_t>(bytes, 1), std::align_val_t(alignment));
    allocations[memory] = {bytes, alignment};
    if (reallocating && words[1]) {
      auto* old = g2h(words[1]);
      auto it = allocations.find(old);
      REQUIRE(it != allocations.end());
      std::memcpy(memory, old, std::min(bytes, it->second.size));
      ::operator delete(old, std::align_val_t(it->second.alignment));
      allocations.erase(it);
    }
    return GuestCallResult{h2g(memory)};
  });
  struct Restore {
    bool old_stopping = os_quit::stopping.load();
    ~Restore() { os_quit::stopping.store(old_stopping); test::set_vulkan_callback({}); }
  } restore;
  REQUIRE_FALSE(restore.old_stopping);
  auto& base = gfx::gpu_device();
  VkAllocationCallbacks guest{};
  guest.pfnAllocation = reinterpret_cast<PFN_vkAllocationFunction>(test::vulkan_callback_thunk(1, 4));
  guest.pfnReallocation = reinterpret_cast<PFN_vkReallocationFunction>(test::vulkan_callback_thunk(2, 5));
  guest.pfnFree = reinterpret_cast<PFN_vkFreeFunction>(test::vulkan_callback_thunk(3, 2));
  auto* host_allocator = vulkan::translate_allocator(&guest);
  REQUIRE(host_allocator);
  VkAllocationCallbacks transient_copy = guest;
  CHECK(vulkan::translate_allocator(&transient_copy) == host_allocator);
  VkAllocationCallbacks compatible = guest;
  compatible.pUserData = reinterpret_cast<void*>(0x456);
  auto* compatible_host = vulkan::translate_allocator(&compatible);
  REQUIRE(compatible_host != host_allocator);
  float priority = 0.5f;
  VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  queue.queueFamilyIndex = base.queue_family;
  queue.queueCount = 1;
  queue.pQueuePriorities = &priority;
  VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  info.queueCreateInfoCount = 1;
  info.pQueueCreateInfos = &queue;
  VkDevice device{};
  auto create = reinterpret_cast<PFN_vkCreateDevice>(base.get_instance_proc_addr(base.instance, "vkCreateDevice"));
  REQUIRE(create);
  REQUIRE(create(base.physical_device, &info, host_allocator, &device) == VK_SUCCESS);
  auto device_proc = reinterpret_cast<PFN_vkGetDeviceProcAddr>(base.get_instance_proc_addr(base.instance, "vkGetDeviceProcAddr"));
  REQUIRE(device_proc);
  auto idle = reinterpret_cast<PFN_vkDeviceWaitIdle>(device_proc(device, "vkDeviceWaitIdle"));
  REQUIRE(idle);
  const auto before_calls = calls.load();
  const auto before = idle(device);
  const auto running_calls = calls.load();
  os_quit::stopping.store(true);
  const auto after = idle(device);
  const auto stopped_calls = calls.load();
  // Stopped before the terminal bridge: drivers that allocate in vkDeviceWaitIdle
  // (RADV) are served host memory, without reentering the guest.
  CHECK(after == VK_SUCCESS);
  REQUIRE(stopped_calls == running_calls);
  REQUIRE(vulkan::begin_terminal_allocator_bridge());
  const auto terminal = idle(device);
  const auto terminal_calls = calls.load();
  CHECK(terminal == VK_SUCCESS);
  CHECK(terminal_calls == stopped_calls);
  CHECK(vulkan::terminal_allocator_bridge_healthy());
  CHECK(vulkan::wait_devices_for_os_quit(std::chrono::seconds(2)));
  CHECK(calls.load() == stopped_calls);
  CHECK(vulkan::terminal_allocator_bridge_healthy());
  // Use the already translated compatible allocator, exactly as a retained
  // native object does. No new guest allocator or guest callback is admitted.
  auto destroy = reinterpret_cast<PFN_vkDestroyDevice>(device_proc(device, "vkDestroyDevice"));
  REQUIRE(destroy);
  destroy(device, compatible_host);
  CHECK(calls.load() == stopped_calls);
  CHECK(vulkan::terminal_allocator_bridge_healthy());
  std::printf("TERMINAL_ALLOCATOR running=%d stopped=%d terminal=%d guest_callbacks_running=%u guest_callbacks_stopped=%u guest_callbacks_terminal=%u guest_allocations_retained=%zu\n",
      int(before), int(after), int(terminal), running_calls-before_calls,
      stopped_calls-running_calls, terminal_calls-stopped_calls, allocations.size());
  CHECK(before == VK_SUCCESS);
  // Only the TEST OWNER reclaims its own original allocations AFTER native
  // device destruction. The bridge never frees them with a host allocator.
  for (const auto& [memory, allocation] : allocations)
    ::operator delete(memory, std::align_val_t(allocation.alignment));
  allocations.clear();
}

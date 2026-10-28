#include "core/api_result_trace.h"
#include <doctest/doctest.h>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <new>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <xmmintrin.h>
#include "android/native_window.h"
#include "common/json.h"
#include "hle/vulkan/timing.h"

#include "core/hle_runtime.h"
#include "gfx/gpu_device.h"
#include "hle/vulkan/callbacks.h"
#include "hle/vulkan/internal.h"
#include "vulkan_callbacks_test_support.h"
#include "vulkan_test_support.h"

namespace {
struct Resolver {
  PFN_vkGetInstanceProcAddr original;
  PFN_vkGetDeviceProcAddr gdpa{};
  PFN_vkCmdSetDeviceMask mask{};
  PFN_vkCmdPipelineBarrier barrier{};
  PFN_vkCmdSetViewport viewport{};
  PFN_vkGetPipelineCacheData cache_data{};
  const VkMemoryBarrier* expected{};
  const VkAllocationCallbacks* allocator{};
  unsigned calls{}, fence_calls{}, borrowed{}, cache_calls{}, query_calls{}, viewport_mxcsr{};
  bool probe_fp = false, throw_fence = false;
  static void VKAPI_CALL set_viewport(VkCommandBuffer command, std::uint32_t first,
      std::uint32_t count, const VkViewport* views) {
    auto& self = *active;
    if (self.probe_fp) {
      self.viewport_mxcsr = _mm_getcsr();
    }
    self.viewport(command, first, count, views);
    if (self.probe_fp)
      _mm_setcsr(_mm_getcsr() | 0x21); // Driver invalid/inexact must not leak to guest FPSR.
  }
  static VkResult VKAPI_CALL pipeline_cache_data(VkDevice device, VkPipelineCache cache,
      std::size_t* size, void* data) {
    auto& self = *active;
    ++self.cache_calls;
    if (self.allocator) {
      auto* a = self.allocator;
      auto* memory = a->pfnAllocation(a->pUserData, 64, 16, VK_SYSTEM_ALLOCATION_SCOPE_CACHE);
      REQUIRE(memory);
      a->pfnFree(a->pUserData, memory);
    }
    return self.cache_data(device, cache, size, data);
  }
  static inline Resolver* active{};
  static void VKAPI_CALL set_mask(VkCommandBuffer command, std::uint32_t value) {
    auto& self = *active;
    ++self.calls;
    // Force the same nested guest allocation that a recording driver is
    // permitted to make when this pool owns guest allocation callbacks.
    if (self.allocator) {
      auto* a = self.allocator;
      auto* memory = a->pfnAllocation(a->pUserData, 64, 16, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
      REQUIRE(memory);
      a->pfnFree(a->pUserData, memory);
    }
    self.mask(command, value);
  }
  static void VKAPI_CALL pipeline_barrier(VkCommandBuffer command, VkPipelineStageFlags src,
      VkPipelineStageFlags dst, VkDependencyFlags flags, std::uint32_t memories, const VkMemoryBarrier* memory,
      std::uint32_t buffers, const VkBufferMemoryBarrier* buffer,
      std::uint32_t images, const VkImageMemoryBarrier* image) {
    auto& self = *active;
    if (memory == self.expected) ++self.borrowed;
    self.barrier(command, src, dst, flags, memories, memory, buffers, buffer, images, image);
  }
  static VkResult VKAPI_CALL fence_status(VkDevice, VkFence) {
    ++active->fence_calls;
    if (active->throw_fence) throw std::bad_alloc();
    if (active->allocator) {
      auto* a = active->allocator;
      auto* memory = a->pfnAllocation(a->pUserData, 64, 16, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
      REQUIRE(memory);
      a->pfnFree(a->pUserData, memory);
    }
    return VK_NOT_READY;
  }
  static VkResult VKAPI_CALL query_results(VkDevice, VkQueryPool, std::uint32_t,
      std::uint32_t, std::size_t, void*, VkDeviceSize, VkQueryResultFlags) {
    ++active->query_calls;
    return VK_SUCCESS;
  }
  static PFN_vkVoidFunction VKAPI_CALL device_proc(VkDevice device, const char* name) {
    auto& self = *active;
    auto function = self.gdpa(device, name);
    if (std::strcmp(name, "vkCmdSetDeviceMask") == 0) {
      self.mask = reinterpret_cast<PFN_vkCmdSetDeviceMask>(function);
      return reinterpret_cast<PFN_vkVoidFunction>(set_mask);
    }
    if (std::strcmp(name, "vkCmdPipelineBarrier") == 0) {
      self.barrier = reinterpret_cast<PFN_vkCmdPipelineBarrier>(function);
      return reinterpret_cast<PFN_vkVoidFunction>(pipeline_barrier);
    }
    if (std::strcmp(name, "vkGetFenceStatus") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(fence_status);
    if (std::strcmp(name, "vkGetQueryPoolResults") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(query_results);
    if (std::strcmp(name, "vkCmdSetViewport") == 0) {
      self.viewport = reinterpret_cast<PFN_vkCmdSetViewport>(function);
      return reinterpret_cast<PFN_vkVoidFunction>(set_viewport);
    }
    if (std::strcmp(name, "vkGetPipelineCacheData") == 0) {
      self.cache_data = reinterpret_cast<PFN_vkGetPipelineCacheData>(function);
      return reinterpret_cast<PFN_vkVoidFunction>(pipeline_cache_data);
    }
    return function;
  }
  static PFN_vkVoidFunction VKAPI_CALL instance_proc(VkInstance instance, const char* name) {
    auto& self = *active;
    auto function = self.original(instance, name);
    if (std::strcmp(name, "vkGetDeviceProcAddr") == 0) {
      self.gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(function);
      return reinterpret_cast<PFN_vkVoidFunction>(device_proc);
    }
    return function;
  }
  Resolver() : original(venture::gfx::gpu_device().get_instance_proc_addr) {
    active = this;
    venture::gfx::gpu_device().get_instance_proc_addr = instance_proc;
  }
  ~Resolver() {
    venture::gfx::gpu_device().get_instance_proc_addr = original;
    active = nullptr;
  }
};
}

TEST_CASE("Vulkan fast forwarding admits warm scalar commands and declines interrupted or stale calls") {
  using namespace venture;
  Resolver resolver;
  test::VulkanDevice d;
  d.begin();
  const auto thunk = vulkan::device_proc(d.device, "vkCmdSetDeviceMask");
  REQUIRE(thunk);
  const auto& info = test::vulkan_thunk_info(thunk);
  REQUIRE(info.execution == hle::Execution::InJitFp);
  REQUIRE(info.try_handler);
  const auto width = vulkan::device_proc(d.device, "vkCmdSetLineWidth");
  const auto idle = vulkan::device_proc(d.device, "vkQueueWaitIdle");
  REQUIRE(width); REQUIRE(idle);
  CHECK(test::vulkan_thunk_info(width).execution == hle::Execution::InJitFp);
  CHECK(test::vulkan_thunk_info(idle).execution == hle::Execution::RunLoop);
  auto& thread = GuestThread::adopt_current();
  test::FakeCpu cpu;
  cpu.set_x(0, h2g(d.command)); cpu.set_x(1, 1);
  hle::CallContext ctx(thread, cpu, info);
  CHECK_FALSE(info.try_handler(ctx)); // cold snapshot: no locking in the SVC callback
  CHECK(resolver.calls == 0);
  test::vk_call("vkCmdSetDeviceMask", d.command, 1u);
  CHECK(info.try_handler(ctx));
  CHECK(resolver.calls == 2);
  thread.interrupted.store(true);
  const auto declined = info.try_handler(ctx);
  thread.interrupted.store(false);
  CHECK_FALSE(declined);
  CHECK(cpu.x(0) == h2g(d.command));
  CHECK(resolver.calls == 2);
  vulkan::invalidate_layer_procedures();
  CHECK_FALSE(info.try_handler(ctx));
  test::vk_call("vkCmdSetDeviceMask", d.command, 1u);
  CHECK(info.try_handler(ctx));
  CHECK(resolver.calls == 4);
  REQUIRE(test::vk_call("vkEndCommandBuffer", d.command) == VK_SUCCESS);
}

TEST_CASE("Vulkan native pointer-argument forwards isolate host FP state from guest FPCR and FPSR") {
  using namespace venture;
  Resolver resolver;
  test::VulkanDevice d;
  d.begin();
  const auto thunk = vulkan::device_proc(d.device, "vkCmdSetViewport");
  REQUIRE(thunk);
  const auto& info = test::vulkan_thunk_info(thunk);
  REQUIRE(info.execution == hle::Execution::InJitFp);
  VkViewport view{0, 0, 64, 64, 0, 1};
  test::vk_call("vkCmdSetViewport", d.command, 0u, 1u, &view);
  auto& thread = GuestThread::adopt_current();
  test::FakeCpu cpu;
  cpu.set_x(0, h2g(d.command)); cpu.set_x(1, 0); cpu.set_x(2, 1); cpu.set_x(3, h2g(&view));
  hle::CallContext ctx(thread, cpu, info);
  REQUIRE(info.try_handler(ctx)); // Warm admission is available for the real JIT call below.
  const auto saved_fpcr = thread.cpu().fpcr(), saved_fpsr = thread.cpu().fpsr();
  const auto saved_host = _mm_getcsr();
  struct Restore {
    GuestThread& thread;
    std::uint32_t fpcr, fpsr;
    unsigned host;
    ~Restore() { thread.cpu().set_fpcr(fpcr); thread.cpu().set_fpsr(fpsr); _mm_setcsr(host); }
  } restore{thread, saved_fpcr, saved_fpsr, saved_host};
  // GuestThread::call restores its saved CPU state, which would hide leaked
  // guest flags. Run an actual BLR/SVC stub and inspect FPSR before restoration.
  constexpr std::array code{0xd63f0280u, 0xd4000001u | (kSvcReturnToHost << 5)};
  const auto entry = hle::code_stub(code, "Vulkan native FP-state probe");
  const auto host_mode = (saved_host & ~0xe07fu) | 0x2000u; // Host round down, no FTZ/DAZ/flags.
  thread.cpu().set_fpcr(0x01400000); // Guest round up and flush to zero.
  thread.cpu().set_fpsr(2);
  resolver.probe_fp = true;
  _mm_setcsr(host_mode);
  thread.cpu().set_x(0, h2g(d.command)); thread.cpu().set_x(1, 0);
  thread.cpu().set_x(2, 1); thread.cpu().set_x(3, h2g(&view));
  thread.cpu().set_x(20, thunk); thread.cpu().set_pc(entry);
  thread.run_until_return(0);
  const auto result_fpcr = thread.cpu().fpcr(), result_fpsr = thread.cpu().fpsr();
  _mm_setcsr(saved_host);
  thread.cpu().set_fpcr(saved_fpcr); thread.cpu().set_fpsr(saved_fpsr);
  resolver.probe_fp = false;
  CHECK((resolver.viewport_mxcsr & 0xffc0u) == (host_mode & 0xffc0u));
  CHECK(result_fpcr == 0x01400000);
  CHECK(result_fpsr == 2);
  REQUIRE(test::vk_call("vkEndCommandBuffer", d.command) == VK_SUCCESS);
}

TEST_CASE("Vulkan fast forwarding defers driver status without repeating the native call") {
  using namespace venture;
  Resolver resolver;
  test::VulkanDevice d;
  const auto thunk = vulkan::device_proc(d.device, "vkGetFenceStatus");
  REQUIRE(thunk);
  const auto& info = test::vulkan_thunk_info(thunk);
  REQUIRE(info.try_handler);
  // The resolver supplies a deterministic NOT_READY without accessing a fence.
  CHECK(test::vk_call("vkGetFenceStatus", d.device, VK_NULL_HANDLE) == static_cast<std::uint64_t>(VK_NOT_READY));
  const auto before = resolver.fence_calls;
  auto* result_ring = api_result_trace::enabled() ? api_result_trace::thread_ring() : nullptr;
  const auto result_start = result_ring ? result_ring->total() : 0;
  auto& thread = GuestThread::adopt_current();
  test::FakeCpu cpu;
  cpu.set_x(0, h2g(d.device)); cpu.set_x(1, 0);
  hle::CallContext ctx(thread, cpu, info);
  CHECK_FALSE(info.try_handler(ctx));
  CHECK(resolver.fence_calls == before + 1);
  CHECK(cpu.x(0) == h2g(d.device)); // no consumed arguments on decline
  info.handler(ctx);
  CHECK(cpu.x(0) == VK_NOT_READY);
  CHECK(resolver.fence_calls == before + 1);
  CHECK(thread.call(thunk, {h2g(d.device), 0}) == VK_NOT_READY);
  CHECK(resolver.fence_calls == before + 2);
  if (result_ring) {
    std::vector<api_result_trace::Record> records;
    result_ring->visit([&](const auto& r) {
      if (r.sequence > result_start && r.api == api_result_trace::api_id("vkGetFenceStatus"))
        records.push_back(r);
    });
    REQUIRE(records.size() == 4); // Two fast calls, each followed by result delivery; no native replay.
    for (std::size_t i = 0; i < records.size(); ++i) {
      CHECK(records[i].state == api_result_trace::State::returned);
      CHECK(records[i].result == VK_NOT_READY);
      CHECK(records[i].phase == (i % 2 ? api_result_trace::Phase::vulkan_delivery : api_result_trace::Phase::vulkan_fast));
    }
  }
}

TEST_CASE("Vulkan retained guest allocators force recording and child queries through RunLoop") {
  using namespace venture;
  bool child_cache = false;
  SUBCASE("command pool allocator") {}
  SUBCASE("pipeline cache allocator on an otherwise callback-free device") { child_cache = true; }
  struct Allocation { std::size_t size, alignment; };
  std::mutex mutex;
  std::map<void*, Allocation> allocations;
  std::atomic<unsigned> callback_calls{};
  test::set_vulkan_callback([&](GuestAddr fn, const GuestCallArgs& args) {
    std::lock_guard lock(mutex);
    ++callback_calls;
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
    const bool realloc = fn == 2;
    const auto size = static_cast<std::size_t>(words[realloc ? 2 : 1]);
    const auto alignment = std::max<std::size_t>(alignof(std::max_align_t), words[realloc ? 3 : 2]);
    auto* memory = ::operator new(std::max<std::size_t>(size, 1), std::align_val_t(alignment));
    allocations[memory] = {size, alignment};
    if (realloc && words[1]) {
      auto* old = g2h(words[1]);
      auto it = allocations.find(old);
      REQUIRE(it != allocations.end());
      std::memcpy(memory, old, std::min(size, it->second.size));
      ::operator delete(old, std::align_val_t(it->second.alignment));
      allocations.erase(it);
    }
    return GuestCallResult{h2g(memory)};
  });
  struct ResetCallback { ~ResetCallback() { test::set_vulkan_callback({}); } } reset_callback;
  {
    Resolver resolver;
    test::VulkanDevice d;
    VkAllocationCallbacks guest{};
    guest.pfnAllocation = reinterpret_cast<PFN_vkAllocationFunction>(test::vulkan_callback_thunk(1, 4));
    guest.pfnReallocation = reinterpret_cast<PFN_vkReallocationFunction>(test::vulkan_callback_thunk(2, 5));
    guest.pfnFree = reinterpret_cast<PFN_vkFreeFunction>(test::vulkan_callback_thunk(3, 2));
    if (child_cache) {
      VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
      VkPipelineCache normal{}, retained{};
      REQUIRE(test::vk_call("vkCreatePipelineCache", d.device, &ci, nullptr, &normal) == VK_SUCCESS);
      std::size_t size = 0;
      const auto thunk = vulkan::device_proc(d.device, "vkGetPipelineCacheData");
      const auto& info = test::vulkan_thunk_info(thunk);
      REQUIRE(info.try_handler);
      test::vk_call("vkGetPipelineCacheData", d.device, normal, &size, nullptr);
      test::FakeCpu cpu;
      cpu.set_x(0, h2g(d.device)); cpu.set_x(1, test::vk_word(normal));
      cpu.set_x(2, h2g(&size)); cpu.set_x(3, 0);
      hle::CallContext ctx(GuestThread::adopt_current(), cpu, info);
      REQUIRE(info.try_handler(ctx)); // A warm, clean device was eligible before child creation.
      REQUIRE(test::vk_call("vkCreatePipelineCache", d.device, &ci, &guest, &retained) == VK_SUCCESS);
      cpu.set_x(0, h2g(d.device));
      CHECK_FALSE(info.try_handler(ctx)); // Reject the previously warm snapshot.
      test::vk_call("vkGetPipelineCacheData", d.device, retained, &size, nullptr);
      cpu.set_x(1, test::vk_word(retained));
      const auto before = resolver.cache_calls;
      CHECK_FALSE(info.try_handler(ctx)); // Refilling must retain the device quarantine.
      CHECK(resolver.cache_calls == before);
      resolver.allocator = vulkan::translate_allocator(&guest);
      const auto callbacks_before = callback_calls.load();
      REQUIRE(GuestThread::adopt_current().call(thunk,
          {h2g(d.device), test::vk_word(retained), h2g(&size), 0}) == VK_SUCCESS);
      CHECK(resolver.cache_calls == before + 1);
      CHECK(callback_calls > callbacks_before); // Real guest callback through the outer boundary.
      resolver.allocator = nullptr;
      test::vk_call("vkDestroyPipelineCache", d.device, retained, &guest);
      test::vk_call("vkGetPipelineCacheData", d.device, normal, &size, nullptr);
      cpu.set_x(0, h2g(d.device)); cpu.set_x(1, test::vk_word(normal));
      CHECK_FALSE(info.try_handler(ctx)); // Destroying the child does not re-enable the device.
      d.begin();
      test::vk_call("vkCmdSetDeviceMask", d.command, 1u);
      const auto& mask_info = test::vulkan_thunk_info(vulkan::device_proc(d.device, "vkCmdSetDeviceMask"));
      cpu.set_x(0, h2g(d.command)); cpu.set_x(1, 1);
      hle::CallContext mask_ctx(GuestThread::adopt_current(), cpu, mask_info);
      CHECK_FALSE(mask_info.try_handler(mask_ctx)); // Quarantine also covers unrelated recording.
      REQUIRE(test::vk_call("vkEndCommandBuffer", d.command) == VK_SUCCESS);
      test::vk_call("vkDestroyPipelineCache", d.device, normal, nullptr);
    } else {
      VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
      pool_info.queueFamilyIndex = d.family;
      VkCommandPool pool{};
      REQUIRE(test::vk_call("vkCreateCommandPool", d.device, &pool_info, &guest, &pool) == VK_SUCCESS);
      VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
      allocation.commandPool = pool; allocation.commandBufferCount = 1;
      VkCommandBuffer command{};
      REQUIRE(test::vk_call("vkAllocateCommandBuffers", d.device, &allocation, &command) == VK_SUCCESS);
      VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      REQUIRE(test::vk_call("vkBeginCommandBuffer", command, &begin) == VK_SUCCESS);
      resolver.allocator = vulkan::translate_allocator(&guest);
      test::vk_call("vkCmdSetDeviceMask", command, 1u); // warm ownership snapshot
      const auto thunk = vulkan::device_proc(d.device, "vkCmdSetDeviceMask");
      REQUIRE(thunk);
      const auto& info = test::vulkan_thunk_info(thunk);
      test::FakeCpu cpu;
      cpu.set_x(0, h2g(command)); cpu.set_x(1, 1);
      hle::CallContext ctx(GuestThread::adopt_current(), cpu, info);
      const auto before = resolver.calls;
      REQUIRE(info.try_handler);
      CHECK_FALSE(info.try_handler(ctx));
      CHECK(resolver.calls == before);
      GuestThread::adopt_current().call(thunk, {h2g(command), 1});
      CHECK(resolver.calls == before + 1);
      CHECK(callback_calls > 0);
      REQUIRE(test::vk_call("vkEndCommandBuffer", command) == VK_SUCCESS);
      resolver.allocator = nullptr;
      test::vk_call("vkDestroyCommandPool", d.device, pool, &guest);
    }
  }
  test::set_vulkan_callback({});
  CHECK(allocations.empty());
}

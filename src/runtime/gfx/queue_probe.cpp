#include "gfx/queue_probe.h"
#include "common/log.h"
#include "core/shutdown_trace.h"
#include "gfx/angle.h"
#include "gfx/gpu_device.h"
#include "platform/process_info.h"
#include <vulkan/vulkan.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

namespace venture::gfx {
namespace {
// Shutdown diagnostics: does the shared queue still complete work? An empty
// submit's fence signals once everything submitted before it has finished, so
// a fence that never signals means earlier work is stuck (for example waiting
// on a semaphore nobody will signal). The probe runs on its own thread and is
// abandoned, never joined, if a lock or the driver blocks it.
enum Stage : int { started, queue_locked, angle_locked, submitted, signaled, failed };
const char* stage_name(int stage) {
  switch (stage) {
    case started: return "waiting for the queue lock";
    case queue_locked: return "waiting for ANGLE's queue lock";
    case angle_locked: return "submitting";
    case submitted: return "waiting for the fence";
    case signaled: return "fence signaled";
    default: return "failed";
  }
}
void probe(const char* label) {
  auto& gpu = gpu_device();
  if (!gpu.device || !gpu.queue) return;
  auto stage = std::make_shared<std::atomic<int>>(started);
  auto result = std::make_shared<std::atomic<int>>(VK_SUCCESS);
  std::thread([stage, result] {
    auto& gpu = gpu_device();
    auto get = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        gpu.get_instance_proc_addr(gpu.instance, "vkGetDeviceProcAddr"));
    auto create = reinterpret_cast<PFN_vkCreateFence>(get(gpu.device, "vkCreateFence"));
    auto submit = reinterpret_cast<PFN_vkQueueSubmit>(get(gpu.device, "vkQueueSubmit"));
    auto wait = reinterpret_cast<PFN_vkWaitForFences>(get(gpu.device, "vkWaitForFences"));
    auto destroy = reinterpret_cast<PFN_vkDestroyFence>(get(gpu.device, "vkDestroyFence"));
    VkFenceCreateInfo info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence{};
    if (!create || !submit || !wait || !destroy || create(gpu.device, &info, nullptr, &fence) != VK_SUCCESS) {
      stage->store(failed);
      return;
    }
    VkResult r;
    {
      // Never wait on the queue lock: the probe must not delay the quit path.
      std::unique_lock queue(gpu.queue_lock, std::defer_lock);
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
      while (!queue.try_lock()) {
        if (std::chrono::steady_clock::now() > deadline) {
          destroy(gpu.device, fence, nullptr);
          return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      stage->store(queue_locked);
      const bool angle = lock_angle_queue();
      stage->store(angle_locked);
      r = submit(gpu.queue, 0, nullptr, fence);
      if (angle) unlock_angle_queue();
    }
    if (r != VK_SUCCESS) {
      result->store(r);
      stage->store(failed);
      destroy(gpu.device, fence, nullptr);
      return;
    }
    stage->store(submitted);
    r = wait(gpu.device, 1, &fence, VK_TRUE, 5'000'000'000ull);
    result->store(r);
    if (r == VK_SUCCESS) {
      stage->store(signaled);
      destroy(gpu.device, fence, nullptr);
    }
  }).detach();
  const auto start = std::chrono::steady_clock::now();
  while (stage->load() != signaled && stage->load() != failed &&
         std::chrono::steady_clock::now() - start < std::chrono::milliseconds(700))
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
  log::info("gpu-queue", "snapshot={} probe {} after {} ms result={} queue_lock_owner={}", label,
            stage_name(stage->load()), ms, result->load(), platform::mutex_owner_thread(gpu.queue_lock));
}
}  // namespace
void register_queue_probe() { shutdown_trace::add_snapshot_source(probe); }
}  // namespace venture::gfx

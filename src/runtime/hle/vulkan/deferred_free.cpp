// Guest memory frees wait for the GPU work submitted before them.
//
// Drivers that do not track per-submission memory use (RADV on amdgpu among
// them) unmap freed device memory from the GPU at once, so work still reading
// or writing it faults. Quest's kernel driver (KGSL), like WDDM, keeps a freed
// allocation until the GPU retires the work using it, and Android titles free
// memory their in-flight frames still reference on that basis
// (VUID-vkFreeMemory-memory-00677). Here the host free waits until every
// queue of the device has completed the guest work submitted before the free:
// at the free, an empty batch on each such queue signals a fence covering it.
#include "hle/vulkan/internal.h"
#include "hle/vulkan/vulkan.h"

#include <algorithm>
#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <vector>

namespace venture::vulkan {
namespace {
struct QueueState {
  VkDevice device{};
  std::shared_ptr<std::recursive_mutex> submission;  // shared with every native user of the queue
  // Guest submissions, counted before they reach the driver: a free on
  // another thread while one is in progress waits for it, as cover() queues
  // behind it on the submission mutex the submitting thread holds.
  std::uint64_t submitted = 0;
  std::uint64_t completed = 0;  // known complete through this serial
  std::deque<std::pair<std::uint64_t, VkFence>> fences;  // in flight, serial order
  std::vector<VkFence> spare;
};
struct Pending {
  VkDevice device{};
  std::function<void()> release;
  std::vector<std::pair<VkQueue, std::uint64_t>> waits;
};
std::mutex mutex;
std::map<VkQueue, QueueState> queues;
std::vector<Pending> pending;
std::atomic<bool> any_pending{false};

template <class F> F host(VkDevice d, const char* name) {
  return reinterpret_cast<F>(raw_device_proc(d, name));
}
// Marks finished fences complete; caller holds `mutex`.
void poll(QueueState& q) {
  auto status = host<PFN_vkGetFenceStatus>(q.device, "vkGetFenceStatus");
  while (!q.fences.empty() && status(q.device, q.fences.front().second) == VK_SUCCESS) {
    q.completed = std::max(q.completed, q.fences.front().first);
    q.spare.push_back(q.fences.front().second);
    q.fences.pop_front();
  }
}
// Takes the frees no longer waiting, for freeing outside the lock; caller
// holds `mutex`. Every queue a free waits for is polled.
std::vector<Pending> ready(VkDevice device) {
  std::vector<Pending> out;
  if (pending.empty()) return out;
  for (auto& [queue, q] : queues)
    if (q.device == device) poll(q);
  std::erase_if(pending, [&](Pending& p) {
    if (p.device != device) return false;
    for (auto& [queue, serial] : p.waits) {
      auto it = queues.find(queue);
      if (it != queues.end() && it->second.completed < serial) return false;
    }
    out.push_back(std::move(p));
    return true;
  });
  any_pending.store(!pending.empty(), std::memory_order_relaxed);
  return out;
}
void release(std::vector<Pending> freed) {
  for (auto& p : freed) p.release();
}
// Makes a fence follow the queue's latest guest submission, unless one does
// already: an empty batch signals it once all earlier work has completed.
void cover(VkQueue queue) {
  VkFence fence{};
  std::uint64_t serial = 0;
  std::shared_ptr<std::recursive_mutex> submission;
  VkDevice device{};
  {
    std::lock_guard lock(mutex);
    auto it = queues.find(queue);
    if (it == queues.end()) return;
    auto& q = it->second;
    if (q.submitted <= q.completed || (!q.fences.empty() && q.fences.back().first >= q.submitted)) return;
    if (!q.spare.empty()) {
      fence = q.spare.back();
      q.spare.pop_back();
      host<PFN_vkResetFences>(q.device, "vkResetFences")(q.device, 1, &fence);
    } else {
      VkFenceCreateInfo info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
      if (host<PFN_vkCreateFence>(q.device, "vkCreateFence")(q.device, &info, nullptr, &fence) != VK_SUCCESS) return;
    }
    serial = q.submitted;
    submission = q.submission;
    device = q.device;
  }
  VkResult r;
  {
    // Submissions after `serial` may slip in before this batch; the fence
    // then covers more than recorded, which is harmless.
    std::unique_lock<std::recursive_mutex> queue_lock;
    if (submission) queue_lock = std::unique_lock(*submission);
    r = host<PFN_vkQueueSubmit>(device, "vkQueueSubmit")(queue, 0, nullptr, fence);
  }
  std::lock_guard lock(mutex);
  if (auto it = queues.find(queue); it != queues.end()) {
    if (r == VK_SUCCESS) it->second.fences.emplace_back(serial, fence);
    else it->second.spare.push_back(fence);
  }
}

void deferred_release(VkDevice d, std::function<void()> release_resource) {
  std::vector<VkQueue> busy;
  {
    std::lock_guard lock(mutex);
    Pending p{d, std::move(release_resource), {}};
    for (auto& [queue, q] : queues) {
      if (q.device != d) continue;
      poll(q);
      if (q.submitted > q.completed) p.waits.emplace_back(queue, q.submitted);
    }
    if (!p.waits.empty()) {
      for (auto& [queue, serial] : p.waits) busy.push_back(queue);
      pending.push_back(std::move(p));
      any_pending.store(true, std::memory_order_relaxed);
    } else release_resource = std::move(p.release);
  }
  if (busy.empty()) release_resource();
  // Queues that stay idle afterwards must not hold the free indefinitely.
  for (auto queue : busy) cover(queue);
  std::vector<Pending> freed;
  {
    std::lock_guard lock(mutex);
    freed = ready(d);
  }
  release(std::move(freed));
}
void VKAPI_CALL free_memory(VkDevice d, VkDeviceMemory memory, const VkAllocationCallbacks* alloc) {
  if (!memory) return;
  deferred_release(d, [=] { host<PFN_vkFreeMemory>(d, "vkFreeMemory")(d, memory, alloc); });
}
}  // namespace

void defer_gpu_release(VkDevice device, std::function<void()> release) {
  deferred_release(device, std::move(release));
}

void deferred_free_submitting(VkQueue queue) {
  std::lock_guard lock(mutex);
  auto it = queues.find(queue);
  if (it == queues.end()) {
    auto binding = queue_binding(queue);
    if (!binding) return;
    it = queues.emplace(queue, QueueState{binding->device, binding->submission_mutex}).first;
  }
  ++it->second.submitted;
}
void deferred_free_submitted(VkQueue queue) {
  if (!any_pending.load(std::memory_order_relaxed)) return;
  VkDevice device{};
  {
    std::lock_guard lock(mutex);
    auto it = queues.find(queue);
    if (it == queues.end()) return;
    device = it->second.device;
  }
  // Pending frees already have fences after the submissions they wait for;
  // only finished ones are released here.
  std::vector<Pending> freed;
  {
    std::lock_guard lock(mutex);
    freed = ready(device);
  }
  release(std::move(freed));
}
void deferred_free_queue_idle(VkQueue queue) {
  std::vector<Pending> freed;
  {
    std::lock_guard lock(mutex);
    auto it = queues.find(queue);
    if (it == queues.end()) return;
    it->second.completed = it->second.submitted;
    freed = ready(it->second.device);
  }
  release(std::move(freed));
}
// vkDestroyDevice requires all of the device's work to have completed.
void release_deferred_free_device(VkDevice device) {
  std::vector<Pending> freed;
  {
    std::lock_guard lock(mutex);
    std::erase_if(pending, [&](Pending& p) {
      if (p.device != device) return false;
      freed.push_back(std::move(p));
      return true;
    });
    any_pending.store(!pending.empty(), std::memory_order_relaxed);
    std::erase_if(queues, [&](auto& entry) {
      auto& q = entry.second;
      if (q.device != device) return false;
      auto destroy = host<PFN_vkDestroyFence>(device, "vkDestroyFence");
      for (auto& [serial, fence] : q.fences) destroy(device, fence, nullptr);
      for (auto fence : q.spare) destroy(device, fence, nullptr);
      return true;
    });
  }
  release(std::move(freed));
}
std::size_t deferred_free_pending(VkDevice device) {
  std::lock_guard lock(mutex);
  return std::size_t(std::ranges::count_if(pending, [&](const Pending& p) { return p.device == device; }));
}
PFN_vkVoidFunction deferred_free_proc(std::string_view name) {
  if (name == "vkFreeMemory") return reinterpret_cast<PFN_vkVoidFunction>(free_memory);
  return nullptr;
}
}  // namespace venture::vulkan

#pragma once
#include "binary_semaphore_order.h"
#include "serialized_queues.h"
#include <deque>
#include <map>
#include <span>

namespace venture::vulkan {
// Caller-thread readiness dispatch across every guest queue on a device.
// Every producer must pass through this dispatcher: accepting an untracked
// future wait on another native queue would invalidate its progress proof.
// Submitted arrays/pNext data are owned until the native API consumes them.
// WSI, external payload operations and sparse work share the same readiness
// order; their callbacks own every input that can survive the API caller.
class DeferredQueueSubmissions {
 public:
  struct Binding {
    std::shared_ptr<SerializedQueues> backend;
    std::uint32_t index;
  };
  DeferredQueueSubmissions(VkDevice, std::shared_ptr<SerializedQueues>, std::uint32_t count,
      PFN_vkQueueSubmit, PFN_vkGetSemaphoreCounterValue, PFN_vkQueueSubmit2 = nullptr);
  DeferredQueueSubmissions(VkDevice, std::vector<Binding>,
      PFN_vkQueueSubmit, PFN_vkGetSemaphoreCounterValue, PFN_vkQueueSubmit2 = nullptr);
  ~DeferredQueueSubmissions();
  void add_semaphore(VkSemaphore, bool timeline);
  void remove_semaphore(VkSemaphore);
  VkResult submit(std::uint32_t logical, std::uint32_t count, const VkSubmitInfo*, VkFence);
  VkResult submit2(std::uint32_t logical, std::uint32_t count, const VkSubmitInfo2*, VkFence);
  VkResult submit_sparse(std::uint32_t logical, std::uint32_t count,
      const VkBindSparseInfo*, VkFence, PFN_vkQueueBindSparse);
  VkResult flush();
  VkResult external_signal(VkSemaphore);
  VkResult native_work(std::uint32_t logical, std::span<const VkSemaphore> waits,
      std::span<const VkSemaphore> signals, const std::function<VkResult(VkQueue)>&,
      const bool* waits_issued = nullptr);

  // Issue earlier work before a native image consumer takes the physical
  // queue. This is a submission boundary, not a GPU completion wait.
  VkResult wait_submitted(std::uint32_t logical, std::uint64_t timeout_ns = UINT64_MAX);
  VkResult wait_idle(std::uint32_t logical);
  std::size_t pending(std::uint32_t logical) const;
  std::uint32_t queue_count() const { return static_cast<std::uint32_t>(bindings_.size()); }
 private:
  struct Job;
  struct Semaphore {
    bool timeline;
    std::uint64_t payload, issued_value{};
  };
  bool ready(const Job&);
  std::shared_ptr<Job> copy(std::uint32_t, const VkSubmitInfo&, VkFence, bool own = false);
  std::shared_ptr<Job> copy2(std::uint32_t, const VkSubmitInfo2&, VkFence, bool own = false);
  VkResult enqueue(std::uint32_t, const std::vector<std::shared_ptr<Job>>&);
  VkDevice device_;
  std::vector<Binding> bindings_;
  PFN_vkQueueSubmit submit_;
  PFN_vkQueueSubmit2 submit2_;
  PFN_vkGetSemaphoreCounterValue counter_;
  mutable std::recursive_mutex mutex_;
  std::map<VkSemaphore, Semaphore> semaphores_;
  BinarySemaphoreOrder binary_;
  std::vector<std::deque<std::shared_ptr<Job>>> pending_;
  std::uint64_t next_payload_{};
  std::size_t next_queue_{};
  VkResult failure_ = VK_SUCCESS;
};
} // namespace venture::vulkan

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>
#include <vulkan/vulkan_core.h>

namespace venture::vulkan {

// Binary-semaphore queue backend. Native calls, including any application
// allocator callbacks they provoke, execute synchronously on the API caller.
// Direct forwarding requires binary-only work. A deferred readiness dispatcher
// can also admit timeline work after its producer has actually been issued:
// an unsatisfied future wait must never enter the native FIFO first.
class SerializedQueues {
 public:
  struct Driver {
    PFN_vkCreateFence create_fence;
    PFN_vkDestroyFence destroy_fence;
    PFN_vkResetFences reset_fences;
    PFN_vkGetFenceStatus fence_status;
    PFN_vkWaitForFences wait_fences;
    PFN_vkQueueSubmit submit;
    PFN_vkQueueWaitIdle queue_wait_idle{};
  };
  SerializedQueues(VkDevice, VkQueue, std::uint32_t logical_count,
                   std::shared_ptr<std::recursive_mutex>, Driver, bool completion_markers = true);
  ~SerializedQueues();
  SerializedQueues(const SerializedQueues&) = delete;
  SerializedQueues& operator=(const SerializedQueues&) = delete;

  // The callback submits the original operation, with its original guest
  // fence/semaphores. The backend adds a separate native completion fence.
  VkResult submit(std::uint32_t logical,
                  const std::function<VkResult(VkQueue)>& operation, bool wait_for_slot = true);
  VkResult wait_idle(std::uint32_t logical);
  VkQueue native_queue() const { return queue_; }
  bool completion_markers() const { return completion_markers_; }
  std::uint64_t issued(std::uint32_t logical) const;
  std::uint64_t completed(std::uint32_t logical) const;

 private:
  struct Logical { std::uint64_t issued{}, completed{}; };
  struct Slot {
    VkFence fence{};
    std::uint32_t logical{};
    std::uint64_t serial{}, order{};
  };
  VkResult reserve(Slot*&, bool wait_for_slot);
  void retire(Slot&);
  VkResult error(VkResult);
  VkDevice device_;
  VkQueue queue_;
  std::shared_ptr<std::recursive_mutex> mutex_;
  Driver driver_;
  bool completion_markers_;
  std::vector<Logical> logical_;
  std::array<Slot, 64> slots_{};
  std::uint64_t next_order_{};
  VkResult failure_ = VK_SUCCESS;
};
}  // namespace venture::vulkan

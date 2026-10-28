#include "serialized_queues.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace venture::vulkan {
SerializedQueues::SerializedQueues(VkDevice device, VkQueue queue,
    std::uint32_t count, std::shared_ptr<std::recursive_mutex> mutex, Driver driver, bool completion_markers)
    : device_(device), queue_(queue), mutex_(std::move(mutex)), driver_(driver),
      completion_markers_(completion_markers), logical_(count) {
  if (!device || !queue || !count || !mutex_ || !driver.create_fence ||
      !driver.destroy_fence || !driver.reset_fences || !driver.fence_status ||
      !driver.wait_fences || !driver.submit || (!completion_markers && !driver.queue_wait_idle))
    throw std::invalid_argument("Incomplete serialized Vulkan queue backend");
}
SerializedQueues::~SerializedQueues() {
  // Destruction follows normal device-idle/lost-device teardown. No driver
  // call or guest allocator callback is delegated to a worker thread.
  std::lock_guard lock(*mutex_);
  for (auto& slot : slots_) {
    if (slot.fence) driver_.destroy_fence(device_, slot.fence, nullptr);
  }
}
VkResult SerializedQueues::error(VkResult result) {
  if (result != VK_SUCCESS) failure_ = result;
  return result;
}
void SerializedQueues::retire(Slot& slot) {
  auto& logical = logical_.at(slot.logical);
  logical.completed = std::max(logical.completed, slot.serial);
  slot.serial = 0;
}
VkResult SerializedQueues::reserve(Slot*& out, bool wait_for_slot) {
  for (auto& slot : slots_) {
    if (slot.serial) {
      const auto status = driver_.fence_status(device_, slot.fence);
      if (status == VK_SUCCESS) retire(slot);
      else if (status != VK_NOT_READY) return error(status);
    }
    if (!slot.serial) {
      if (!slot.fence) {
        const VkFenceCreateInfo info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        auto result = driver_.create_fence(device_, &info, nullptr, &slot.fence);
        if (result != VK_SUCCESS) return result;
      } else {
        auto result = driver_.reset_fences(device_, 1, &slot.fence);
        if (result != VK_SUCCESS) return error(result);
      }
      out = &slot;
      return VK_SUCCESS;
    }
  }
  if (!wait_for_slot) return VK_NOT_READY;
  // Direct submit callers permit backpressure. Readiness flushes use the
  // nonblocking path above so status polling cannot wait for a host producer.
  auto& oldest = *std::ranges::min_element(slots_, {}, &Slot::order);
  auto result = driver_.wait_fences(device_, 1, &oldest.fence, VK_TRUE,
                                    std::numeric_limits<std::uint64_t>::max());
  if (result != VK_SUCCESS) return error(result);
  retire(oldest);
  result = driver_.reset_fences(device_, 1, &oldest.fence);
  if (result != VK_SUCCESS) return error(result);
  out = &oldest;
  return VK_SUCCESS;
}
VkResult SerializedQueues::submit(std::uint32_t index,
    const std::function<VkResult(VkQueue)>& operation, bool wait_for_slot) {
  std::lock_guard lock(*mutex_);
  if (index >= logical_.size()) return VK_ERROR_INITIALIZATION_FAILED;
  if (failure_ != VK_SUCCESS) return failure_;
  if (!completion_markers_) {
    const auto result = operation(queue_);
    if (result >= VK_SUCCESS) ++logical_[index].issued;
    return result;
  }
  Slot* slot{};
  auto result = reserve(slot, wait_for_slot);
  if (result != VK_SUCCESS) return result;
  result = operation(queue_);
  if (result < VK_SUCCESS) return result;
  const auto operation_result = result;
  // Preserve the guest fence: this empty submission owns a different fence.
  // Native queue exclusion covers both calls, so another logical queue cannot
  // insert work between the guest operation and its completion marker.
  result = driver_.submit(queue_, 0, nullptr, slot->fence);
  if (result != VK_SUCCESS) return error(result);
  slot->logical = index;
  slot->serial = ++logical_[index].issued;
  slot->order = ++next_order_;
  return operation_result;
}
VkResult SerializedQueues::wait_idle(std::uint32_t index) {
  std::lock_guard lock(*mutex_);
  if (index >= logical_.size()) return VK_ERROR_INITIALIZATION_FAILED;
  if (failure_ != VK_SUCCESS) return failure_;
  if (!completion_markers_) {
    const auto result = driver_.queue_wait_idle(queue_);
    if (result == VK_SUCCESS)
      for (auto& queue : logical_) queue.completed = queue.issued;
    return result;
  }
  auto& logical = logical_[index];
  const auto target = logical.issued;
  if (logical.completed >= target) return VK_SUCCESS;
  auto marker = std::ranges::find_if(slots_, [=](const Slot& slot) {
    return slot.logical == index && slot.serial == target;
  });
  if (marker == slots_.end()) return error(VK_ERROR_INITIALIZATION_FAILED);
  auto result = driver_.wait_fences(device_, 1, &marker->fence, VK_TRUE,
                                    std::numeric_limits<std::uint64_t>::max());
  if (result != VK_SUCCESS) return error(result);
  retire(*marker);
  return VK_SUCCESS;
}
std::uint64_t SerializedQueues::issued(std::uint32_t index) const {
  std::lock_guard lock(*mutex_);
  return logical_.at(index).issued;
}
std::uint64_t SerializedQueues::completed(std::uint32_t index) const {
  std::lock_guard lock(*mutex_);
  return logical_.at(index).completed;
}
}  // namespace venture::vulkan

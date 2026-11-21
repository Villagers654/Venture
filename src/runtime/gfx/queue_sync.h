#pragma once
#include "gfx/gpu_device.h"
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace venture::gfx {
// All native users of the same VkQueue share one mutex. Unrelated queues must
// remain independent: a native submit can wait for another queue to signal it.
std::shared_ptr<std::recursive_mutex> queue_mutex(VkQueue queue);
void register_queue(VkDevice device, VkQueue queue);
void forget_device_queues(VkDevice device);
// Stall diagnostics: the queues whose lock another thread holds now, with its
// owner where the platform can tell ("unknown" otherwise). Only try-locks.
std::vector<std::pair<VkQueue, std::string>> held_queue_locks();

// vkDeviceWaitIdle externally synchronizes every queue of that device. Never
// block acquiring one queue while holding another (including ANGLE's queue).
class DeviceQueueLock {
  std::vector<std::shared_ptr<std::recursive_mutex>> mutexes_;
  std::vector<std::unique_lock<std::recursive_mutex>> locks_;
public:
  explicit DeviceQueueLock(VkDevice device);
};
}

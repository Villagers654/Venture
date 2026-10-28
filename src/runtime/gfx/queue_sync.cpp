#include "gfx/queue_sync.h"
#include "common/log.h"
#include "platform/process_info.h"
#include <algorithm>
#include <map>
#include <thread>
#include <chrono>

namespace venture::gfx {
namespace {
std::mutex registry_mutex;
struct Queue { VkDevice device{}; std::shared_ptr<std::recursive_mutex> mutex; };
std::map<VkQueue, Queue> queues;
}
std::shared_ptr<std::recursive_mutex> queue_mutex(VkQueue queue) {
  auto& gpu = gpu_device();
  if (queue == gpu.queue)
    return {&gpu.queue_lock, [](auto*) {}};
  std::lock_guard lock(registry_mutex);
  auto& record = queues[queue];
  if (!record.mutex) record.mutex = std::make_shared<std::recursive_mutex>();
  return record.mutex;
}
void register_queue(VkDevice device, VkQueue queue) {
  if (!queue) return;
  auto mutex = queue_mutex(queue);
  std::lock_guard lock(registry_mutex);
  queues[queue] = {device, std::move(mutex)};
}
void forget_device_queues(VkDevice device) {
  std::lock_guard lock(registry_mutex);
  std::erase_if(queues, [=](const auto& item) { return item.second.device == device; });
}
DeviceQueueLock::DeviceQueueLock(VkDevice device) {
  auto& gpu = gpu_device();
  {
    std::lock_guard lock(registry_mutex);
    for (const auto& [queue, record] : queues)
      if (record.device == device) mutexes_.push_back(record.mutex);
  }
  if (device == gpu.device) mutexes_.push_back(queue_mutex(gpu.queue));
  std::ranges::sort(mutexes_, std::less<>{}, [](auto& mutex) { return mutex.get(); });
  auto unique = std::ranges::unique(mutexes_, {}, [](auto& mutex) { return mutex.get(); });
  mutexes_.erase(unique.begin(), unique.end());
  for (auto& mutex : mutexes_) locks_.emplace_back(*mutex, std::defer_lock);
  // Valid device-idle callers exclude concurrent guest queue access themselves
  // (Vulkan Host Synchronization). These locks additionally exclude XR/ANGLE.
  // Release every partial acquisition before retrying so a blocked submit on
  // Q1 cannot prevent a signal on Q2 during acquisition.
  const auto start = std::chrono::steady_clock::now();
  bool reported = false;
  for (;;) {
    bool acquired = true;
    for (auto& lock : locks_)
      if (!lock.try_lock()) { acquired = false; break; }
    if (acquired) return;
    for (auto& lock : locks_) if (lock.owns_lock()) lock.unlock();
    if (!reported && std::chrono::steady_clock::now() - start > std::chrono::milliseconds(500)) {
      // Device idle is a quit and teardown path; name whoever keeps it waiting.
      reported = true;
      for (std::size_t i = 0; i < mutexes_.size(); ++i) {
        if (!mutexes_[i]->try_lock()) {
          log::warn("vulkan", "device queue lock {} of {} still busy after 500 ms; owner thread {}", i + 1,
                       mutexes_.size(), platform::mutex_owner_thread(*mutexes_[i]));
        } else {
          mutexes_[i]->unlock();
        }
      }
    }
    std::this_thread::sleep_for(std::chrono::microseconds(50));
  }
}
}

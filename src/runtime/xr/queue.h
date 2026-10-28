#pragma once
#include "gfx/gpu_device.h"
#include "gfx/queue_sync.h"
#include "xr/xr_images.h"
#include "xr/session.h"

namespace venture::xr {
class HostQueueLock {
  std::shared_ptr<std::recursive_mutex> mutex_;
  std::unique_lock<std::recursive_mutex> lock_;
  XrImages *provider_ = nullptr;
  bool locked_ = true;

public:
  explicit HostQueueLock(bool shared) {
    mutex_ = gfx::queue_mutex(shared ? gfx::gpu_device().queue : session().queue);
    lock_ = std::unique_lock(*mutex_);
    auto *images = xr_images();
    if (shared && images && images->external_queue_active()) {
      provider_ = images;
      locked_ = provider_->lock_external_queue();
    }
  }
  ~HostQueueLock() {
    if (provider_ && locked_)
      provider_->unlock_external_queue();
  }
  explicit operator bool() const { return locked_; }
};
} // namespace venture::xr

#pragma once
#include "xr/capture.h"
#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
namespace mock {
using venture::xr::capture_eye;
// Eye dumps are diagnostics: the frame thread only records and submits each
// copy; writer threads wait for the GPU and encode the PNGs, so a capture
// frame does not stall the application being observed. One encoder falls
// behind full-size eyes (about 1.3 PNG/s against 2 per second at --every 144),
// which filled the queue, blocked xrEndFrame and made swapchain teardown wait
// most of a minute.
// The runtime DLL never joins it from a static destructor (loader lock at
// process exit); session/swapchain/instance teardown drains it instead.
class CaptureWriter {
 public:
  // One frame-thread producer checks the whole batch before submitting copies.
  // Skip periodic diagnostics under pressure, rather than blocking a producer
  // that may need to submit the GPU work those copies are waiting for.
  bool can_accept(std::size_t count) {
    std::lock_guard lock(mutex_);
    const auto outstanding = queue_.size() + busy_;
    return !outstanding || outstanding + count <= 64;
  }
  void push(venture::xr::PendingCapturePtr capture) {
    {
      std::lock_guard lock(mutex_);
      if (!started_) {
        for (unsigned i = 0; i < writer_count(); ++i) std::thread([this] { run(); }).detach();
        started_ = true;
      }
      queue_.push_back(std::move(capture));
    }
    ready_.notify_one();
  }
  // Swapchain images and the device must outlive every submitted copy.
  void drain() {
    std::unique_lock lock(mutex_);
    idle_.wait(lock, [&] { return queue_.empty() && !busy_; });
  }
  bool idle() {
    std::lock_guard lock(mutex_);
    return queue_.empty() && !busy_;
  }
 private:
  static unsigned writer_count() {
    return std::clamp(std::thread::hardware_concurrency() / 4, 2u, 4u);
  }
  void run() {
    for (;;) {
      venture::xr::PendingCapturePtr capture;
      {
        std::unique_lock lock(mutex_);
        ready_.wait(lock, [&] { return !queue_.empty(); });
        capture = std::move(queue_.front());
        queue_.pop_front();
        ++busy_;
      }
      std::string error;
      if (!venture::xr::write_pending_eye(*capture, &error))
        std::fprintf(stderr, "[mock-xr] layer image failed: %s\n", error.c_str());
      capture.reset();
      {
        std::lock_guard lock(mutex_);
        --busy_;
      }
      idle_.notify_all();
    }
  }
  std::mutex mutex_;
  std::condition_variable ready_, idle_;
  std::deque<venture::xr::PendingCapturePtr> queue_;
  unsigned busy_ = 0;
  bool started_ = false;
};
}

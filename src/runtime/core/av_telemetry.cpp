#include "common/environment.h"
#include "core/av_telemetry.h"
#include "core/wait.h"
#include "platform/profile.h"
#include "common/log.h"
#include <condition_variable>
#include <atomic>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

namespace venture::av {
namespace {
struct Writer {
  std::filesystem::path path;
  std::mutex mutex;
  std::condition_variable changed;
  std::deque<nlohmann::json> queue;
  bool stopping = false;
  std::atomic<uint64_t> dropped{0};
  std::thread worker;
  std::atomic<unsigned> worker_tid{0};
  Writer() {
    if (auto p = venture::environment::getenv("VENTURE_AV_TELEMETRY"); p && *p) {
      path = std::filesystem::u8path(p);
      worker = std::thread([this] { run(); });
    }
  }
  ~Writer() { finish(); }
  void finish() {
    { std::lock_guard lock(mutex); stopping = true; }
    changed.notify_one();
    if (worker.joinable()) {
      log::info("launch", "AV flush waiting for host writer={} queued_events={}", worker_tid.load(), queue_size());
      worker.join();
    }
  }
  std::size_t queue_size() { std::lock_guard lock(mutex); return queue.size(); }
  void run() noexcept {
    worker_tid.store(platform::profile_thread_id());
    try {
      std::ofstream out(path, std::ios::app | std::ios::binary);
      std::unique_lock lock(mutex);
      for (;;) {
        changed.wait(lock, [this] { return stopping || !queue.empty(); });
        if (queue.empty() && stopping) break;
        std::deque<nlohmann::json> batch;
        batch.swap(queue);
        lock.unlock();
        for (auto &event : batch) out << event.dump() << '\n';
        out.flush();
        lock.lock();
      }
    } catch (...) { /* Optional diagnostics never affect runtime behavior. */ }
  }
};
Writer &writer() { static Writer w; return w; }
}
void finish() noexcept { try { writer().finish(); } catch (...) {} }
bool enabled() { return !writer().path.empty(); }
void emit(nlohmann::json event) noexcept {
  try {
    auto &w = writer();
    if (w.path.empty()) return;
    event["t_ns"] = monotonic_ns();
    event["writer_dropped"] = w.dropped.load();
    // Producers are explicitly non-realtime. A short queue-only critical
    // section avoids losing evidence just because audio and haptics coincide;
    // neither this lock nor the writer lock is ever held over filesystem work.
    std::unique_lock lock(w.mutex);
    if (w.stopping) return;
    if (w.queue.size() >= 4096) { ++w.dropped; return; }
    w.queue.push_back(std::move(event));
    lock.unlock();
    w.changed.notify_one();
  } catch (...) {}
}
}

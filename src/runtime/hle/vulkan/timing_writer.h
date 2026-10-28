#pragma once
#include <array>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <type_traits>

namespace venture::vulkan {
// AV telemetry's single consumer/short admission lock, with bounded storage.
// Sink and final-status callbacks run only on the writer, never a producer.
template<class Record, std::size_t Capacity, class Sink, class Final>
class TimingWriter {
  static_assert(Capacity > 0 && std::is_trivially_copyable_v<Record>);
public:
  struct Status {
    std::uint64_t accepted{}, written{}, lost{}, write_failures{};
    bool final_drain{};
  };
  TimingWriter(Sink sink, Final final) : sink_(std::move(sink)), final_(std::move(final)),
      worker_([this] { run(); }) {}
  ~TimingWriter() { finish(); }
  bool emit(const Record& record) noexcept {
    std::unique_lock lock(mutex_);
    if (stopping_ || failed_ || size_ == Capacity) { ++status_.lost; return false; }
    const bool wake = size_ == 0;
    records_[(head_ + size_) % Capacity] = record;
    ++size_; ++status_.accepted;
    lock.unlock();
    if (wake) changed_.notify_one();
    return true;
  }
  // Finish is serialized with other finish callers; producers can race it.
  void finish() noexcept {
    std::lock_guard join(join_mutex_);
    { std::lock_guard lock(mutex_); stopping_ = true; }
    changed_.notify_one();
    if (worker_.joinable()) worker_.join();
  }
  Status status() { std::lock_guard lock(mutex_); return status_; }
private:
  void run() noexcept {
    for (;;) {
      Record record;
      {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [this] { return stopping_ || size_ != 0; });
        if (!size_ && stopping_) break;
        record = records_[head_]; head_ = (head_ + 1) % Capacity; --size_;
      }
      bool ok = false;
      try { ok = sink_(record); } catch (...) {}
      {
        std::lock_guard lock(mutex_);
        if (ok) ++status_.written;
        else {
          ++status_.write_failures;
          status_.lost += 1 + size_; size_ = 0; failed_ = true;
        }
      }
    }
    Status final;
    { std::lock_guard lock(mutex_); status_.final_drain = !failed_; final = status_; }
    try { final_(final); } catch (...) {}
  }
  std::array<Record, Capacity> records_{};
  std::size_t head_{}, size_{};
  std::mutex mutex_, join_mutex_;
  std::condition_variable changed_;
  bool stopping_{}, failed_{};
  Status status_;
  Sink sink_;
  Final final_;
  std::thread worker_;
};
}

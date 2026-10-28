#pragma once
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace venture::launcher {
// Single-owner tasks return completions that are applied only by the UI thread.
class Worker {
public:
  using Completion = std::function<void()>;
  using Task = std::function<Completion(std::stop_token)>;
  explicit Worker(std::function<void()> wake)
      : wake_(std::move(wake)), thread_([this](std::stop_token stop) {
          while (!stop.stop_requested()) {
            Task task;
            {
              std::unique_lock lock(mutex_);
              cv_.wait(lock, stop, [&] { return !tasks_.empty(); });
              if (stop.stop_requested())
                return;
              task = std::move(tasks_.front());
              tasks_.pop_front();
            }
            auto result = task(stop);
            {
              std::lock_guard lock(mutex_);
              if (result)
                results_.push_back(std::move(result));
            }
            wake_();
          }
        }) {}
  ~Worker() { stop(); }
  void stop() {
    thread_.request_stop();
    cv_.notify_all();
    if (thread_.joinable())
      thread_.join();
  }
  void submit(Task task) {
    {
      std::lock_guard lock(mutex_);
      tasks_.push_back(std::move(task));
    }
    cv_.notify_one();
  }
  void drain() {
    std::deque<Completion> results;
    {
      std::lock_guard lock(mutex_);
      results.swap(results_);
    }
    for (auto &result : results)
      result();
  }

private:
  std::mutex mutex_;
  std::condition_variable_any cv_;
  std::deque<Task> tasks_;
  std::deque<Completion> results_;
  std::function<void()> wake_;
  std::jthread thread_;
};
} // namespace venture::launcher

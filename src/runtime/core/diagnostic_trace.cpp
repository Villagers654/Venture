#include "core/diagnostic_trace.h"
#include "core/diagnostic_buffer.h"
#include "common/environment.h"
#include "common/log.h"
#include "common/paths.h"
#include <miniz.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <thread>

namespace venture {
namespace {
class Trace {
 public:
  Trace() {
    const auto* value = environment::getenv("VENTURE_DIAGNOSTIC_TRACE_ZLIB");
    if (!value) { start_worker(); return; }
    auto path = paths::from_utf8(value);
    bool dev_run = false;
    for (auto it = path.begin(); it != path.end(); ++it)
      if (*it == "dev-cache" && std::next(it) != path.end() && *std::next(it) == "runs") dev_run = true;
    if (!dev_run) { fail("output must be under dev-cache/runs"); return; }
    file_.open(path, std::ios::binary | std::ios::trunc);
    if (!file_ || mz_deflateInit(&stream_, MZ_DEFAULT_COMPRESSION) != MZ_OK) {
      fail("cannot initialize compressed output"); return;
    }
    initialized_ = true;
    flushed_ = std::chrono::steady_clock::now();
    if (environment::getenv("VENTURE_VULKAN_SYNC_TRACE")) {
      flush_interval_ = std::chrono::milliseconds(5);
      // Bursty fence polling must not force producer I/O or synchronization.
      burst_ = std::make_unique<SizedDiagnosticBuffer<1024>[]>(queues_.size());
    }
    start_worker();
  }
  void write(std::string_view tag, std::string_view message) noexcept {
    if (stopping_.load(std::memory_order_relaxed)) return;
    thread_local std::size_t local = 256;
    if (local == queues_.size()) {
      const auto index = allocated_.fetch_add(1, std::memory_order_relaxed);
      if (index >= queues_.size()) { dropped_.fetch_add(1, std::memory_order_relaxed); return; }
      local = index;
    }
    const bool accepted = burst_ ? burst_[local].push(tag, message) : queues_[local].push(tag, message);
    if (!accepted) dropped_.fetch_add(1, std::memory_order_relaxed);
  }
  void flush(bool finish) {
    if (finish) {
      stopping_.store(true, std::memory_order_release);
      if (worker_.joinable()) worker_.join();
    } else flush_requested_.store(true, std::memory_order_release);
  }
 private:
  void start_worker() {
    worker_ = std::thread([this] { try { run(); } catch (...) { /* Optional diagnostics only. */ } });
  }
  void run() {
    for (;;) {
      const bool finish = stopping_.load(std::memory_order_acquire);
      DiagnosticBuffer::Record record;
      auto drain = [&](auto queues) {
        for (auto& queue : queues) while (queue.pop(record)) {
          if (initialized_) feed(std::string_view(record.bytes.data(), record.size));
          else log::info("diagnostic-trace", "{}", std::string_view(record.bytes.data(), record.size - 1));
        }
      };
      if (burst_) drain(std::span(burst_.get(), queues_.size()));
      else drain(std::span(queues_));
      if (auto dropped = dropped_.exchange(0, std::memory_order_relaxed)) {
        auto message = std::string("[diagnostic-trace] dropped=") + std::to_string(dropped) + "\n";
        if (initialized_) feed(message); else log::warn("diagnostic-trace", "{}", message);
      }
      const auto now = std::chrono::steady_clock::now();
      if (initialized_ && !failed_ && (flush_requested_.exchange(false) || pending_ >= 64 * 1024
          || now - flushed_ >= flush_interval_)) flush_locked(now);
      if (finish) {
        if (initialized_ && !failed_) { pump(MZ_FINISH); file_.flush(); mz_deflateEnd(&stream_); }
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
  void fail(std::string_view reason) {
    failed_ = true;
    log::error("diagnostic-trace", "compressed trace failed: {}", reason);
  }
  void feed(std::string_view text) {
    if (failed_) return;
    stream_.next_in = reinterpret_cast<const unsigned char*>(text.data());
    stream_.avail_in = static_cast<unsigned>(text.size());
    pending_ += text.size();
    pump(MZ_NO_FLUSH);
  }
  void pump(int mode) {
    int result;
    do {
      stream_.next_out = buffer_.data();
      stream_.avail_out = static_cast<unsigned>(buffer_.size());
      result = mz_deflate(&stream_, mode);
      if ((result == MZ_BUF_ERROR && stream_.avail_in) ||
          (mode == MZ_FINISH && result == MZ_BUF_ERROR) ||
          (result != MZ_OK && result != MZ_STREAM_END && result != MZ_BUF_ERROR)) {
        fail("compression error"); return;
      }
      file_.write(reinterpret_cast<const char*>(buffer_.data()), buffer_.size() - stream_.avail_out);
      if (!file_) { fail("write error"); return; }
    } while (stream_.avail_in || !stream_.avail_out || (mode == MZ_FINISH && result != MZ_STREAM_END));
  }
  void flush_locked(std::chrono::steady_clock::time_point now) {
    stream_.next_in = nullptr;
    stream_.avail_in = 0;
    pump(MZ_SYNC_FLUSH);
    file_.flush();
    pending_ = 0;
    flushed_ = now;
  }
  bool initialized_ = false, failed_ = false;
  std::array<DiagnosticBuffer, 256> queues_;
  std::unique_ptr<SizedDiagnosticBuffer<1024>[]> burst_;
  std::atomic<std::size_t> allocated_{0}, dropped_{0};
  std::atomic<bool> stopping_{false}, flush_requested_{false};
  std::thread worker_;
  mz_stream stream_{};
  std::array<unsigned char, 64 * 1024> buffer_{};
  std::ofstream file_;
  std::size_t pending_ = 0;
  std::chrono::steady_clock::time_point flushed_{};
  std::chrono::milliseconds flush_interval_{250};
};
std::atomic<Trace*> live{nullptr};
Trace& trace() {
  static auto* instance = [] {
    auto* value = new Trace;
    live.store(value, std::memory_order_release);
    std::atexit([] { if (auto* value = live.load(std::memory_order_acquire)) value->flush(true); });
    return value;
  }();
  return *instance;
}
}
void initialize_diagnostic_trace() {
  try {
    if (environment::getenv("VENTURE_DIAGNOSTIC_TRACE_ZLIB") || environment::getenv("VENTURE_TRACE_FUTEX")) (void)trace();
  } catch (...) { /* Optional diagnostics cannot prevent a launch. */ }
}
void diagnostic_trace(std::string_view tag, std::string_view message) noexcept {
  if (auto* value = live.load(std::memory_order_acquire)) value->write(tag, message);
}
void flush_diagnostic_trace() { if (auto* value = live.load(std::memory_order_acquire)) value->flush(false); }
}

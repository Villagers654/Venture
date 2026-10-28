#pragma once
// Bounded public API diagnostics. No guest pointers, names, payloads or handles.
#include "common/environment.h"
#include "core/api_result_trace.h"
#include <optional>
#include "common/log.h"
#include "platform/profile.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <vector>

namespace venture::xr_call_trace {
struct Record {
  std::uint64_t sequence{}, call{}, thread{};
  std::int64_t ns{}, display_time{}, period{};
  const char* api{};
  int result{}, render{}, state{}, layers{};
  unsigned fields{}; // 1=display time, 2=period/render, 4=session state, 8=layers
  bool returned{};
};
template<std::size_t Capacity> class Ring {
 public:
  void append(Record record) {
    std::lock_guard lock(mutex_);
    record.sequence = ++total_;
    records_[(total_ - 1) % Capacity] = record;
  }
  std::vector<Record> snapshot() {
    std::lock_guard lock(mutex_);
    std::vector<Record> result;
    const auto first = total_ > Capacity ? total_ - Capacity : 0;
    result.reserve(total_ - first);
    for (auto i = first; i < total_; ++i) result.push_back(records_[i % Capacity]);
    return result;
  }
 private:
  std::mutex mutex_;
  std::array<Record, Capacity> records_{};
  std::uint64_t total_{};
};
inline bool enabled() {
  static const bool value = environment::getenv("VENTURE_TRACE_XR_FRAMES") != nullptr;
  return value;
}
inline void flush() noexcept;
inline Ring<8192>& ring() {
  static Ring<8192> value;
  static const int registered=std::atexit(flush);
  (void)registered;
  return value;
}
inline std::int64_t now() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}
class Call {
 public:
  explicit Call(const char* api) noexcept : active_(enabled() || api_result_trace::enabled()), frames_(enabled()) {
    if (!active_) return;
    if (api_result_trace::enabled()) result_call_.emplace(api, api_result_trace::Phase::xr);
    if (!frames_) return;
    try {
    static std::atomic<std::uint64_t> serial{};
    record_.call = ++serial;
    record_.thread = platform::profile_thread_id();
    record_.api = api;
    record_.ns = now();
    ring().append(record_);
    } catch (...) { frames_=false; active_=result_call_.has_value(); }
  }
  // Called only after the API has validated the input or successfully filled output.
  void display(std::int64_t time) { if (frames_) { record_.display_time=time; record_.fields|=1; } }
  void frame(std::int64_t time, std::int64_t period, bool render) {
    if (frames_) { display(time); record_.period=period; record_.render=render; record_.fields|=2; }
  }
  void state(int state) { if (frames_) { record_.state=state; record_.fields|=4; } }
  void layers(int layers) { if (frames_) { record_.layers=layers; record_.fields|=8; } }
  template<class Result> Result finish(Result result) noexcept {
    if (!active_) return result;
    if (result_call_) result_call_->finish(static_cast<std::int32_t>(result));
    try {
      if (frames_) {
        record_.returned=true; record_.result=int(result); record_.ns=now(); ring().append(record_);
      }
    } catch (...) {}
    return result;
  }
 private:
  bool active_, frames_;
  std::optional<api_result_trace::Call> result_call_;
  Record record_{};
};
inline void flush() noexcept {
  if (!enabled()) return;
  static std::atomic<bool> flushed{};
  if (flushed.exchange(true)) return;
  try {
    const auto records=ring().snapshot();
    log::info("xr-call", "snapshot records={} capacity=8192 first_sequence={} last_sequence={}",
        records.size(), records.empty()?0:records.front().sequence, records.empty()?0:records.back().sequence);
    for (const auto& r : records)
      log::info("xr-call", "seq={} call={} thread={} ns={} api={} returned={} result={} fields={} display_time={} period={} render={} state={} layers={}",
          r.sequence,r.call,r.thread,r.ns,r.api,r.returned,r.result,r.fields,r.display_time,r.period,r.render,r.state,r.layers);
  } catch (...) { /* Diagnostics never replace the original exit. */ }
}
} // namespace venture::xr_call_trace

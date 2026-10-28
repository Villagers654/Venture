#pragma once
#include <algorithm>
#include <chrono>
#include <optional>
#include "common/quit_bound.h"

namespace venture {
// Diagnostics may run before delivery; only successful posting starts grace.
class LifecycleGrace {
 public:
  using Clock = std::chrono::steady_clock;
  explicit LifecycleGrace(std::chrono::milliseconds bound = std::chrono::seconds(5)) : bound_(bound) {}
  void posted(Clock::time_point now) { if (!posted_at_) posted_at_ = now; }
  // `saving`: a file save is the only remaining work; the window extends to
  // save_quit_bound.
  bool expired(Clock::time_point now, bool saving = false) const {
    return posted_at_ && now - *posted_at_ > (saving ? std::max<std::chrono::milliseconds>(bound_, save_quit_bound) : bound_);
  }
  bool within_save_bound(Clock::time_point now) const {
    return posted_at_ && now - *posted_at_ <= save_quit_bound;
  }
 private:
  std::optional<Clock::time_point> posted_at_;
  std::chrono::milliseconds bound_;
};
// One monitor sample of an OS quit after the lifecycle teardown was posted.
struct QuitSample {
  bool destroy_delivered = false;
  bool stages_complete = false;   // pause/stop/destroy acknowledged (0xf)
  bool files_healthy = true;      // the data activity watch kept coverage
  bool threads_quiet = false;     // no runnable application thread, census known
  bool writers_known = false;     // the data handle census succeeded
  bool writers_closed = false;    // no open writer under the data root
  bool write_active = false;      // a guest disk write is in flight
  std::chrono::nanoseconds since_write{};  // since the last guest disk write
};
// Quiet-second evidence: data activity (watch start, file notifications) and
// application thread activity. Work the watcher or the guest write counter
// cannot see (JDK or host SQLite writes, mapped stores, delayed notifications)
// shows only as a runnable thread, so a thread that ran within the last second
// blocks both the early settle and the bound; no wake is exempted.
class QuitQuietness {
 public:
  using Clock = std::chrono::steady_clock;
  explicit QuitQuietness(Clock::time_point watch_start) : data_(watch_start) {}
  void data_changed(Clock::time_point now) { data_ = now; }
  void threads(bool quiet, Clock::time_point now) { if (!quiet) threads_busy_ = now; }
  bool data_quiet(Clock::time_point now) const { return now - data_ >= std::chrono::seconds(1); }
  bool threads_quiet(Clock::time_point now) const {
    return !threads_busy_ || now - *threads_busy_ >= std::chrono::seconds(1);
  }
 private:
  Clock::time_point data_;
  std::optional<Clock::time_point> threads_busy_;
};
// Termination admission, checked whenever the quit window ends (early settle
// or bound). nullptr admits OS termination; any reason fails the quit. An idle
// open writer is the only difference between the two: it forbids the early
// settle but is admitted at the bound (a Quest kills a removed task with its
// handles open) once every other condition holds.
inline const char* quit_admission(const QuitSample& s, const QuitQuietness& q, QuitQuietness::Clock::time_point now) {
  if (!s.threads_quiet) return "save work not quiescent: runnable application thread or unknown census";
  if (!s.writers_known) return "application data handle census unavailable";
  return !s.destroy_delivered ? "destroy not delivered"
      : !s.stages_complete ? "pause/stop/destroy stages incomplete"
      : !s.files_healthy ? "data activity watch lost coverage"
      : s.write_active ? "guest write in progress"
      : s.since_write < std::chrono::seconds(1) ? "guest write within 1 s"
      : !q.data_quiet(now) ? "data changed within 1 s"
      : !q.threads_quiet(now) ? "application thread ran within 1 s" : nullptr;
}
// At the destroy bound, admission that fails only on recency (the current
// sample is quiet but a thread, write or data change fell within the last
// second) waits for its quiet second up to save_quit_bound instead of
// failing: a sporadic wake just before the bound is not unfinished work.
inline bool quit_recency_pending(const QuitSample& s, const QuitQuietness& q, QuitQuietness::Clock::time_point now) {
  return s.threads_quiet && s.writers_known && s.destroy_delivered && s.stages_complete && s.files_healthy &&
         !s.write_active && quit_admission(s, q, now);
}
// Early termination once destroy is delivered: admitted with every writer closed.
inline bool quit_settled(const QuitSample& s, const QuitQuietness& q, QuitQuietness::Clock::time_point now) {
  return s.writers_closed && !quit_admission(s, q, now);
}
}

#include "core/pipeline_stats.h"
#include "core/profile.h"
#include "platform/profile.h"
#include "common/log.h"
#include <algorithm>
#include <map>
#include <mutex>
#include <tuple>
#include <cstdlib>

namespace venture::profile {
namespace {
struct Totals {
  std::uint64_t calls = 0, pipelines = 0, wall_ns = 0, max_ns = 0, slow = 0;
  std::uint64_t feedback_valid = 0, driver_ns = 0, hits = 0, driver_slow = 0;
};
struct State {
  std::mutex mutex;
  std::map<std::tuple<std::uint32_t, PipelineSource, bool>, Totals> threads;
  Json events = Json::array();
  std::map<std::uint32_t, std::shared_ptr<std::atomic<std::uint32_t>>> activities;
};
State& state() { static auto* s = new State; return *s; }
const char* name(PipelineSource s) {
  switch (s) { case PipelineSource::guest: return "guest"; case PipelineSource::angle: return "angle"; default: return "decode"; }
}
}
void start_pipeline_telemetry() {
  static const bool registered = std::atexit([] { report_pipelines("exit"); }) == 0;
  (void)registered;
}
std::shared_ptr<std::atomic<std::uint32_t>> pipeline_thread_activity(std::uint32_t tid) {
  auto& s = state(); std::lock_guard lock(s.mutex);
  const auto it = s.activities.find(tid);
  return it == s.activities.end() ? nullptr : it->second;
}
std::uint32_t pipeline_exchange_activity(std::uint32_t value) noexcept {
  if (!enabled) return host;
  thread_local std::shared_ptr<std::atomic<std::uint32_t>> activity;
  try {
    if (!activity) {
      if (value != pipeline_create) return host;
      activity = std::make_shared<std::atomic<std::uint32_t>>(host);
      auto& s = state(); std::lock_guard lock(s.mutex);
      s.activities[platform::profile_thread_id()] = activity;
    }
    auto previous = activity->load(std::memory_order_relaxed);
    activity->store(value, std::memory_order_relaxed);
    return previous;
  } catch (...) { return host; }
}
void record_pipeline(PipelineSource source, bool compute, std::uint32_t count,
                     std::int64_t start_ns, std::uint64_t wall_ns,
                     std::span<const PipelineFeedback> feedback) noexcept {
  try {
    const auto tid = platform::profile_thread_id();
    auto& s = state();
    std::lock_guard lock(s.mutex);
    auto& t = s.threads[{tid, source, compute}];
    ++t.calls; t.pipelines += count; t.wall_ns += wall_ns;
    t.max_ns = std::max(t.max_ns, wall_ns); t.slow += wall_ns >= 20'000'000;
    for (const auto& f : feedback) if (f.flags & 1 /* VALID */) {
      ++t.feedback_valid; t.driver_ns += f.duration_ns;
      t.hits += (f.flags & 2 /* APPLICATION_PIPELINE_CACHE_HIT */) != 0;
      t.driver_slow += f.duration_ns >= 20'000'000;
    }
    if (enabled) {
      Json values = Json::array();
      for (const auto& f : feedback) values.push_back({{"duration_ns", f.duration_ns}, {"flags", f.flags}});
      s.events.push_back({{"start_ns", start_ns}, {"host_tid", tid}, {"source", name(source)},
                         {"kind", compute ? "compute" : "graphics"}, {"count", count},
                         {"wall_ms", wall_ns / 1e6}, {"feedback", std::move(values)}});
    }
  } catch (...) { /* Telemetry must never change Vulkan's result. */ }
}
Json pipeline_report() {
  auto& s = state(); std::lock_guard lock(s.mutex);
  Json threads = Json::array();
  for (const auto& [key, t] : s.threads) {
    const auto& [tid, source, compute] = key;
    threads.push_back({{"host_tid", tid}, {"source", name(source)}, {"kind", compute ? "compute" : "graphics"},
      {"calls", t.calls}, {"pipelines", t.pipelines}, {"total_ms", t.wall_ns / 1e6},
      {"max_ms", t.max_ns / 1e6}, {"creates_ge_20_ms", t.slow}, {"feedback_valid", t.feedback_valid},
      {"driver_total_ms", t.driver_ns / 1e6}, {"application_cache_hits", t.hits}, {"driver_pipelines_ge_20_ms", t.driver_slow}});
  }
  return {{"per_thread", std::move(threads)}, {"events", s.events}};
}
void report_pipelines(std::string_view phase) noexcept {
  try {
    auto& s = state(); std::lock_guard lock(s.mutex);
    std::uint64_t calls = 0, pipelines = 0, wall = 0, maximum = 0, slow = 0, valid = 0, hits = 0;
    std::string threads;
    for (const auto& [key, t] : s.threads) {
      calls += t.calls; pipelines += t.pipelines; wall += t.wall_ns; maximum = std::max(maximum, t.max_ns);
      slow += t.slow; valid += t.feedback_valid; hits += t.hits;
      const auto& [tid, source, compute] = key;
      if (!threads.empty()) threads += ";";
      threads += std::format("{}:{}:{} count={} total_ms={:.3f} max_ms={:.3f} ge20={}", tid, name(source),
          compute ? "compute" : "graphics", t.calls, t.wall_ns / 1e6, t.max_ns / 1e6, t.slow);
    }
    log::info("profile", "Pipeline creates {}: calls={} pipelines={} total_ms={:.3f} max_ms={:.3f} ge20={} feedback_valid={} application_cache_hits={} threads=[{}]",
              phase, calls, pipelines, wall / 1e6, maximum / 1e6, slow, valid, hits, threads);
  } catch (...) {}
}
}

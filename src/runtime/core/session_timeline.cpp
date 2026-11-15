#include "core/session_timeline.h"
#include "common/environment.h"
#include "common/log.h"
#include "core/memory_pressure.h"
#include "platform/process_info.h"
#include "platform/profile.h"
#include "platform/thread_priority.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <limits>
#include <map>
#include <mutex>
#include <string_view>
#include <thread>
#include <utility>

namespace venture::session_timeline {
namespace {
constexpr std::uint64_t MiB = 1ull << 20;
constexpr auto kInterval = std::chrono::seconds(60);
constexpr std::size_t kTopThreads = 3;

std::atomic<std::uint64_t> frames{0}, long_frames{0};
std::atomic<std::int64_t> worst_ns{0};

std::atomic<std::uint64_t> waits{0}, phase_frames{0}, late_frames{0};
std::atomic<std::int64_t> wait_sum{0}, app_sum{0}, prep_sum{0}, end_sum{0}, margin_sum{0};
std::atomic<std::int64_t> min_margin{std::numeric_limits<std::int64_t>::max()};
// Width in the high half, height in the low; 0 when never set.
std::atomic<std::uint64_t> eye{0}, recommended{0};

std::uint64_t pack(std::int32_t width, std::int32_t height) {
  return std::uint64_t(std::uint32_t(width)) << 32 | std::uint32_t(height);
}
void unpack(std::uint64_t packed, std::int32_t& width, std::int32_t& height) {
  width = std::int32_t(packed >> 32);
  height = std::int32_t(packed & 0xffffffffu);
}

std::atomic<std::uint64_t> timeouts{0};
std::atomic<std::int64_t> timeout_late{0}, timeout_max_late{0};

Timeouts take_timeouts() {
  return {timeouts.exchange(0, std::memory_order_relaxed), timeout_late.exchange(0, std::memory_order_relaxed),
          timeout_max_late.exchange(0, std::memory_order_relaxed)};
}

Phases take_phases() {
  Phases p;
  p.waits = waits.exchange(0, std::memory_order_relaxed);
  p.frames = phase_frames.exchange(0, std::memory_order_relaxed);
  p.late = late_frames.exchange(0, std::memory_order_relaxed);
  p.wait_ns = wait_sum.exchange(0, std::memory_order_relaxed);
  p.app_ns = app_sum.exchange(0, std::memory_order_relaxed);
  p.prep_ns = prep_sum.exchange(0, std::memory_order_relaxed);
  p.end_ns = end_sum.exchange(0, std::memory_order_relaxed);
  p.margin_ns = margin_sum.exchange(0, std::memory_order_relaxed);
  p.min_margin_ns = min_margin.exchange(std::numeric_limits<std::int64_t>::max(), std::memory_order_relaxed);
  if (!p.frames) p.min_margin_ns = 0;
  return p;
}

Frames take_frames() {
  return {frames.exchange(0, std::memory_order_relaxed), long_frames.exchange(0, std::memory_order_relaxed),
          worst_ns.exchange(0, std::memory_order_relaxed)};
}

// A thread without a name of its own (host, driver or runtime threads) is named by its id
// and the module it started in, e.g. "tid 37228 nvoglv64.dll".
std::string thread_name(const platform::ProfileThread& t) {
  if (!t.name.empty()) return t.name;
  const auto module = platform::profile_thread_start_module(t);
  return module.empty() ? std::format("tid {}", t.id) : std::format("tid {} {}", t.id, module);
}

void run() {
  platform::lower_background_thread_priority();
  using clock = std::chrono::steady_clock;
  const auto start = clock::now();
  auto window_start = start;
  // CPU time per live thread at the last sample, by OS id and creation time (ids are reused).
  std::map<std::pair<std::uint32_t, std::uint64_t>, std::uint64_t> previous;
  std::map<std::string, std::uint64_t> previous_gpu;  // GPU engine running time at the last sample
  std::vector<platform::GpuEngineTime> engines;
  for (;;) {
    std::this_thread::sleep_for(kInterval);
    const auto now = clock::now();
    Sample s;
    s.minutes = std::chrono::duration<double>(now - start).count() / 60;
    s.seconds = std::chrono::duration<double>(now - window_start).count();
    window_start = now;
    s.frames = take_frames();
    s.phases = take_phases();
    s.timeouts = take_timeouts();
    unpack(eye.load(std::memory_order_relaxed), s.eye_width, s.eye_height);
    unpack(recommended.load(std::memory_order_relaxed), s.recommended_width, s.recommended_height);
    std::map<std::pair<std::uint32_t, std::uint64_t>, std::uint64_t> current;
    std::vector<std::pair<std::uint64_t, std::size_t>> busy;  // CPU time, index into threads
    std::uint64_t total = 0;
    bool any = false;
    const auto threads = platform::profile_threads();
    s.threads = threads.size() + 1;  // the list leaves out this thread
    for (std::size_t i = 0; i < threads.size(); ++i) {
      const auto& t = threads[i];
      std::uint64_t ns = 0;
      if (!platform::profile_cpu_time(t, ns)) continue;
      const auto key = std::pair{t.id, t.creation_ticks};
      current[key] = ns;
      const auto old = previous.find(key);
      if (old == previous.end() || ns < old->second) continue;  // new this minute: no baseline
      any = true;
      const auto delta = ns - old->second;
      total += delta;
      busy.emplace_back(delta, i);
    }
    const bool baseline = !previous.empty();
    previous = std::move(current);
    if (baseline && any && s.seconds > 0) {
      const double wall_ns = s.seconds * 1e9;
      s.cpu_percent = total / wall_ns * 100;
      std::ranges::sort(busy, std::greater{}, &std::pair<std::uint64_t, std::size_t>::first);
      for (std::size_t i = 0; i < busy.size() && s.top.size() < kTopThreads; ++i)
        if (const double p = busy[i].first / wall_ns * 100; p >= 1) s.top.push_back({thread_name(threads[busy[i].second]), p});
    }
    if (platform::gpu_engine_times(engines)) {
      std::map<std::string, std::uint64_t> current_gpu;
      std::uint64_t busiest = 0;
      const std::string* busiest_engine = nullptr;
      for (const auto& e : engines) {
        current_gpu[e.engine] = e.ns;
        if (const auto old = previous_gpu.find(e.engine); old != previous_gpu.end())
          if (const auto delta = counter_delta(e.ns, old->second); delta && (*delta > busiest || !busiest_engine)) {
            busiest = *delta;
            busiest_engine = &e.engine;
          }
      }
      if (!previous_gpu.empty()) s.gpu_busy_ns = static_cast<std::int64_t>(busiest);
      // That engine's GPU clock and temperature now: a GPU that clocks down or heats up
      // over a session shows here while the work per frame stays the same.
      if (platform::GpuClock clock; busiest_engine && platform::gpu_clock(*busiest_engine, clock)) {
        s.gpu_mhz = clock.hz / 1'000'000;
        s.gpu_max_mhz = clock.max_hz / 1'000'000;
        s.gpu_celsius = clock.celsius;
      }
      previous_gpu = std::move(current_gpu);
    }
    platform::CommitCharge commit;
    if (platform::commit_charge(commit)) s.private_bytes = commit.process;
    memory_pressure::gpu_memory(s.gpu_used, s.gpu_budget);
    log::info("timeline", "{}", format(s));
  }
}

void start() {
  static std::once_flag once;
  std::call_once(once, [] {
    if (const auto* e = environment::getenv("VENTURE_SESSION_TIMELINE"); e && std::string_view(e) == "0") return;
    std::thread(run).detach();
  });
}
}  // namespace

void frame_submitted(std::int64_t now_ns, std::int64_t period_ns) {
  static std::int64_t last_ns = 0;  // only the frame-ending thread writes it
  static bool started = false;
  if (!started) { started = true; start(); }
  frames.fetch_add(1, std::memory_order_relaxed);
  if (last_ns && now_ns > last_ns) {
    const auto interval = now_ns - last_ns;
    if (period_ns > 0 && interval * 2 > period_ns * 3) long_frames.fetch_add(1, std::memory_order_relaxed);
    auto worst = worst_ns.load(std::memory_order_relaxed);
    while (interval > worst && !worst_ns.compare_exchange_weak(worst, interval, std::memory_order_relaxed)) {}
  }
  last_ns = now_ns;
}

void host_wait(std::int64_t ns) {
  waits.fetch_add(1, std::memory_order_relaxed);
  wait_sum.fetch_add(std::max<std::int64_t>(ns, 0), std::memory_order_relaxed);
}

void frame_phases(std::int64_t app_ns, std::int64_t end_ns, std::int64_t margin_ns) {
  phase_frames.fetch_add(1, std::memory_order_relaxed);
  app_sum.fetch_add(std::max<std::int64_t>(app_ns, 0), std::memory_order_relaxed);
  end_sum.fetch_add(std::max<std::int64_t>(end_ns, 0), std::memory_order_relaxed);
  margin_sum.fetch_add(margin_ns, std::memory_order_relaxed);
  if (margin_ns < 0) late_frames.fetch_add(1, std::memory_order_relaxed);
  auto least = min_margin.load(std::memory_order_relaxed);
  while (margin_ns < least && !min_margin.compare_exchange_weak(least, margin_ns, std::memory_order_relaxed)) {}
}

void end_prep(std::int64_t ns) { prep_sum.fetch_add(std::max<std::int64_t>(ns, 0), std::memory_order_relaxed); }

void timed_wait_expired(std::int64_t late_ns) {
  late_ns = std::max<std::int64_t>(late_ns, 0);
  timeouts.fetch_add(1, std::memory_order_relaxed);
  timeout_late.fetch_add(late_ns, std::memory_order_relaxed);
  auto most = timeout_max_late.load(std::memory_order_relaxed);
  while (late_ns > most && !timeout_max_late.compare_exchange_weak(most, late_ns, std::memory_order_relaxed)) {}
}

void eye_rect(std::int32_t width, std::int32_t height) {
  eye.store(pack(width, height), std::memory_order_relaxed);
}

void recommended_size(std::int32_t width, std::int32_t height) {
  recommended.store(pack(width, height), std::memory_order_relaxed);
}

std::string format(const Sample& s) {
  std::string out = std::format("t={:.0f}m fps={:.1f} worst_frame_ms={:.1f} long_frames={}", s.minutes,
                                s.seconds > 0 ? s.frames.count / s.seconds : 0., s.frames.worst_ns / 1e6,
                                s.frames.long_frames);
  if (s.cpu_percent >= 0) {
    out += std::format(" cpu={:.0f}%", s.cpu_percent);
    if (!s.top.empty()) {
      out += " top=\"";
      for (std::size_t i = 0; i < s.top.size(); ++i)
        out += std::format("{}{} {:.0f}%", i ? ", " : "", s.top[i].name, s.top[i].percent);
      out += '"';
    }
  }
  if (s.gpu_busy_ns >= 0 && s.seconds > 0) {
    out += std::format(" gpu_busy={:.0f}%", s.gpu_busy_ns / (s.seconds * 1e9) * 100);
    if (s.frames.count) out += std::format(" gpu_frame_ms={:.2f}", s.gpu_busy_ns / 1e6 / s.frames.count);
  }
  // Per frame: host wait, game frame, host end (ms), and the submission's lead on its display.
  if (const auto& p = s.phases; p.waits || p.frames) {
    if (p.waits) out += std::format(" wait_ms={:.2f}", p.wait_ns / 1e6 / p.waits);
    if (p.frames)
      out += std::format(" app_ms={:.2f} prep_ms={:.2f} end_ms={:.2f} margin_ms={:.2f} min_margin_ms={:.2f} late={}",
                         p.app_ns / 1e6 / p.frames, p.prep_ns / 1e6 / p.frames, p.end_ns / 1e6 / p.frames, p.margin_ns / 1e6 / p.frames,
                         p.min_margin_ns / 1e6, p.late);
  }
  if (const auto& t = s.timeouts; t.count)
    out += std::format(" timeouts={} timeout_late_ms={:.2f}/{:.2f}", t.count, t.late_ns / 1e6 / t.count, t.max_late_ns / 1e6);
  if (s.eye_width > 0) out += std::format(" eye={}x{}", s.eye_width, s.eye_height);
  if (s.recommended_width > 0) out += std::format(" rec={}x{}", s.recommended_width, s.recommended_height);
  if (s.gpu_mhz) out += s.gpu_max_mhz ? std::format(" gpu_mhz={}/{}", s.gpu_mhz, s.gpu_max_mhz) : std::format(" gpu_mhz={}", s.gpu_mhz);
  if (s.gpu_celsius > 0) out += std::format(" gpu_c={:.0f}", s.gpu_celsius);
  if (s.private_bytes) out += std::format(" private_mib={}", s.private_bytes / MiB);
  if (s.gpu_budget) out += std::format(" gpu_mib={}/{}", s.gpu_used / MiB, s.gpu_budget / MiB);
  out += std::format(" threads={}", s.threads);
  return out;
}
}  // namespace venture::session_timeline

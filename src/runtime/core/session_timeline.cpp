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

Frames take_frames() {
  return {frames.exchange(0, std::memory_order_relaxed), long_frames.exchange(0, std::memory_order_relaxed),
          worst_ns.exchange(0, std::memory_order_relaxed)};
}

void run() {
  platform::lower_background_thread_priority();
  using clock = std::chrono::steady_clock;
  const auto start = clock::now();
  auto window_start = start;
  // CPU time per live thread at the last sample, by OS id and creation time (ids are reused).
  std::map<std::pair<std::uint32_t, std::uint64_t>, std::uint64_t> previous;
  for (;;) {
    std::this_thread::sleep_for(kInterval);
    const auto now = clock::now();
    Sample s;
    s.minutes = std::chrono::duration<double>(now - start).count() / 60;
    s.seconds = std::chrono::duration<double>(now - window_start).count();
    window_start = now;
    s.frames = take_frames();
    std::map<std::pair<std::uint32_t, std::uint64_t>, std::uint64_t> current;
    std::vector<std::pair<std::uint64_t, std::string>> busy;
    std::uint64_t total = 0;
    bool any = false;
    const auto threads = platform::profile_threads();
    s.threads = threads.size() + 1;  // the list leaves out this thread
    for (const auto& t : threads) {
      std::uint64_t ns = 0;
      if (!platform::profile_cpu_time(t, ns)) continue;
      const auto key = std::pair{t.id, t.creation_ticks};
      current[key] = ns;
      const auto old = previous.find(key);
      if (old == previous.end() || ns < old->second) continue;  // new this minute: no baseline
      any = true;
      const auto delta = ns - old->second;
      total += delta;
      busy.emplace_back(delta, t.name.empty() ? std::format("tid {}", t.id) : t.name);
    }
    const bool baseline = !previous.empty();
    previous = std::move(current);
    if (baseline && any && s.seconds > 0) {
      const double wall_ns = s.seconds * 1e9;
      s.cpu_percent = total / wall_ns * 100;
      std::ranges::sort(busy, std::greater{}, &std::pair<std::uint64_t, std::string>::first);
      for (std::size_t i = 0; i < busy.size() && s.top.size() < kTopThreads; ++i)
        if (const double p = busy[i].first / wall_ns * 100; p >= 1) s.top.push_back({busy[i].second, p});
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
  if (s.private_bytes) out += std::format(" private_mib={}", s.private_bytes / MiB);
  if (s.gpu_budget) out += std::format(" gpu_mib={}/{}", s.gpu_used / MiB, s.gpu_budget / MiB);
  out += std::format(" threads={}", s.threads);
  return out;
}
}  // namespace venture::session_timeline

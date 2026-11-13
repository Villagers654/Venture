#include "core/memory_pressure.h"
#include "common/environment.h"
#include "common/log.h"
#include "platform/process_info.h"
#include "platform/thread_priority.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace venture::memory_pressure {
namespace {
constexpr std::uint64_t GiB = 1ull << 30, MiB = 1ull << 20;
constexpr std::uint64_t kHysteresis = 512 * MiB;

struct Releaser {
  std::string name;
  std::function<std::uint64_t(Level)> release;
};
struct State {
  std::mutex mutex;
  std::vector<Releaser> releasers;
  std::function<bool(std::uint64_t&, std::uint64_t&)> gpu_probe;
};
State& state() {
  static auto* value = new State;  // the monitor outlives static destruction
  return *value;
}
std::atomic<int> current{0};

const char* name(Level level) {
  return level == Level::critical ? "critical" : level == Level::tight ? "tight" : "normal";
}

void monitor() {
  platform::lower_background_thread_priority();
  auto& s = state();
  Level host = Level::normal, gpu = Level::normal, reported = Level::normal;
  auto released_at = std::chrono::steady_clock::time_point{};
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    platform::MemoryHeadroom room;
    const bool known = platform::memory_headroom(room);
    if (known) host = classify(room.available, room.limit, host);
    std::function<bool(std::uint64_t&, std::uint64_t&)> probe;
    {
      std::lock_guard lock(s.mutex);
      probe = s.gpu_probe;
    }
    std::uint64_t usage = 0, budget = 0;
    if (probe && probe(usage, budget)) gpu = classify_gpu(usage, budget, gpu);
    const auto level = std::max(host, gpu);
    current.store(int(level), std::memory_order_relaxed);
    if (level != reported) {
      log::info("memory", "Memory pressure {}: {} MiB of {} MiB allocatable left, GPU {} of {} MiB",
                name(level), room.available / MiB, room.limit / MiB, usage / MiB, budget / MiB);
    }
    const auto now = std::chrono::steady_clock::now();
    if (level != Level::normal && (level > reported || now - released_at >= std::chrono::seconds(5))) {
      released_at = now;
      std::vector<Releaser> releasers;
      {
        std::lock_guard lock(s.mutex);
        releasers = s.releasers;
      }
      std::uint64_t total = 0;
      std::string parts;
      for (const auto& r : releasers) {
        std::uint64_t bytes = 0;
        try { bytes = r.release(level); } catch (...) {}
        total += bytes;
        if (bytes >= MiB) parts += " " + r.name + "=" + std::to_string(bytes / MiB);
      }
      if (total >= MiB) log::info("memory", "Released {} MiB under {} memory pressure:{}", total / MiB, name(level), parts);
    }
    reported = level;
  }
}
}  // namespace

Level classify(std::uint64_t available, std::uint64_t limit, Level previous) noexcept {
  const auto tight = std::max(2 * GiB, limit / 16), critical = std::max(1 * GiB, limit / 32);
  // A raised level stays until headroom clears its threshold by the hysteresis margin.
  if (available < critical || (previous == Level::critical && available < critical + kHysteresis)) return Level::critical;
  if (available < tight || (previous != Level::normal && available < tight + kHysteresis)) return Level::tight;
  return Level::normal;
}

Level classify_gpu(std::uint64_t usage, std::uint64_t budget, Level previous) noexcept {
  if (!budget) return Level::normal;
  const double share = double(usage) / double(budget);
  if (share > 1.0 || (previous == Level::critical && share > 0.95)) return Level::critical;
  if (share > 0.95 || (previous != Level::normal && share > 0.90)) return Level::tight;
  return Level::normal;
}

Level level() noexcept { return Level(current.load(std::memory_order_relaxed)); }

void start() {
  static std::once_flag once;
  std::call_once(once, [] {
    if (const auto* e = environment::getenv("VENTURE_MEMORY_PRESSURE"); e && std::string_view(e) == "0") return;
    std::thread(monitor).detach();
  });
}

void on_pressure(const char* name, std::function<std::uint64_t(Level)> release) {
  auto& s = state();
  std::lock_guard lock(s.mutex);
  s.releasers.push_back({name, std::move(release)});
}

void set_gpu_probe(std::function<bool(std::uint64_t&, std::uint64_t&)> probe) {
  auto& s = state();
  std::lock_guard lock(s.mutex);
  s.gpu_probe = std::move(probe);
}

bool gpu_memory(std::uint64_t& usage, std::uint64_t& budget) {
  auto& s = state();
  std::function<bool(std::uint64_t&, std::uint64_t&)> probe;
  {
    std::lock_guard lock(s.mutex);
    probe = s.gpu_probe;
  }
  usage = budget = 0;
  return probe && probe(usage, budget);
}
}  // namespace venture::memory_pressure

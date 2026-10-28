#include "core/frame_cpu.h"
#include "common/environment.h"
#include "platform/profile.h"
#include "platform/thread_priority.h"
#include <algorithm>
#include <chrono>
#include <cctype>
#include <map>
#include <memory>
#include <mutex>
#include <set>

namespace venture::frame_cpu {
const bool enabled = [] {
  const auto* option = environment::getenv("VENTURE_FRAME_CPU_TIMING");
  const auto* output = environment::getenv("VENTURE_VULKAN_GPU_TIMING");
  return option && std::string_view(option) == "1" && output && *output;
}();
namespace {
struct Thread {
  platform::ProfileThread native;
  std::int32_t guest_tid{};
  std::string name;
  Delta frame, discovery;
  std::uint64_t recent{};
};
std::mutex mutex;
std::map<std::int32_t, std::shared_ptr<Thread>> guests;
struct Tracked { std::shared_ptr<Thread> thread; std::string role, selection; };
std::vector<Tracked> tracked;
std::shared_ptr<Thread> submitter;
auto refreshed = std::chrono::steady_clock::time_point{};
}
std::string role_name(std::string_view input) {
  std::string value(input);
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return std::tolower(c); });
  if (value == "unitymain" || value == "gamethread" || value == "mainthread" || value == "ue4gamethread") return "main";
  if (value.starts_with("rhithread")) return "rhi";
  if (value.starts_with("renderthread") || value == "unitygfxdevicew") return "render";
  return {};
}
void attach(std::int32_t tid) {
  if (!enabled) return;
  platform::thread_cycles_per_ns(); // Calibrate once at guest startup, never during a measured frame.
  auto thread = std::make_shared<Thread>();
  thread->native = platform::profile_current_thread(); thread->guest_tid = tid;
  thread->name = thread->native.name;
  std::lock_guard lock(mutex); guests[tid] = std::move(thread); refreshed = {};
}
void detach(std::int32_t tid) {
  if (!enabled) return;
  std::lock_guard lock(mutex);
  if (submitter && submitter->guest_tid == tid) submitter.reset();
  guests.erase(tid); refreshed = {};
}
void name(std::int32_t tid, std::string_view name) {
  if (!enabled) return;
  std::lock_guard lock(mutex);
  if (auto it = guests.find(tid); it != guests.end()) { it->second->name = name; refreshed = {}; }
}
std::vector<Sample> boundary() {
  if (!enabled) return {};
  std::lock_guard lock(mutex);
  const auto id = platform::profile_thread_id();
  if (!submitter || submitter->native.id != id) {
    submitter.reset();
    for (const auto& [tid, t] : guests) if (t->native.id == id) { submitter = t; break; }
    if (!submitter) {
      submitter = std::make_shared<Thread>(); submitter->native = platform::profile_current_thread();
      submitter->name = submitter->native.name;
    }
    refreshed = {};
  }
  const auto now = std::chrono::steady_clock::now();
  std::map<std::uint32_t, std::int64_t> native_readings;
  auto read_cycles = [&](const auto& t) {
    auto [it, inserted] = native_readings.try_emplace(t->native.id, -1);
    if (inserted) it->second = platform::thread_cpu_cycles(t->native);
    return it->second;
  };
  if (now - refreshed >= std::chrono::seconds(1)) {
    std::vector<std::shared_ptr<Thread>> candidates;
    for (const auto& [tid, t] : guests) {
      const auto cycles = read_cycles(t);
      const auto delta = t->discovery.read(cycles);
      t->recent = delta.value_or(cycles < 0 ? 0 : std::uint64_t(cycles));
      if (cycles >= 0) candidates.push_back(t);
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) { return a->recent > b->recent; });
    std::set<Thread*> previous;
    for (const auto& t : tracked) previous.insert(t.thread.get());
    tracked.clear();
    std::set<std::uint32_t> selected;
    for (const auto* role : {"main", "render", "rhi"}) {
      auto it = std::find_if(candidates.begin(), candidates.end(), [&](const auto& t) { return role_name(t->name) == role; });
      if (it != candidates.end()) { tracked.push_back({*it, role, "name"}); selected.insert((*it)->native.id); }
    }
    for (const auto* role : {"main", "render"}) {
      if (std::any_of(tracked.begin(), tracked.end(), [&](const auto& t) { return t.role == role; })) continue;
      auto it = std::find_if(candidates.begin(), candidates.end(), [&](const auto& t) { return !selected.contains(t->native.id) && role_name(t->name).empty(); });
      if (it != candidates.end()) { tracked.push_back({*it, role, "top_cycles_inferred"}); selected.insert((*it)->native.id); }
    }
    tracked.push_back({submitter, "xr_submission", "submit_boundary"});
    for (const auto& t : tracked) if (!previous.contains(t.thread.get())) t.thread->frame.previous.reset();
    refreshed = now;
  }
  std::map<Thread*, std::optional<std::uint64_t>> deltas;
  for (const auto& item : tracked) {
    auto* t = item.thread.get();
    if (deltas.contains(t)) continue;
    const auto cycles = read_cycles(item.thread);
    deltas[t] = t->frame.read(cycles);
  }
  std::vector<Sample> result;
  for (const auto& item : tracked) if (const auto delta = deltas[item.thread.get()])
    result.push_back({item.thread->native.id, item.thread->guest_tid, item.role, item.selection,
                     item.thread->name, *delta, platform::thread_cycles_per_ns()});
  return result;
}
} // namespace venture::frame_cpu

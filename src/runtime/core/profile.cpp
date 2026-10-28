#include "common/environment.h"
#include "core/profile.h"
#include "core/condition_census.h"
#include "cpu/outlined_atomics.h"
#include "core/frame_cpu.h"
#include "core/futex_metrics.h"
#include "dynarmic/interface/profiling.h"
#include "core/pipeline_stats.h"
#include "core/invalidation_stats.h"
#include "core/invalidation_report.h"
#include "core/thread.h"
#include "core/thread_runtime.h"
#include "core/shutdown_trace.h"
#include "core/hle.h"
#include "core/linker.h"
#include "core/elf.h"
#include "core/process.h"
#include "core/wait.h"
#include "common/json.h"
#include "common/milestone.h"
#include "common/log.h"
#include "common/paths.h"
#include "platform/profile.h"
#include "platform/memory.h"
#include "platform/cpu_affinity.h"
#include "platform/thread_priority.h"
#include <array>
#include <algorithm>
#include <format>
#include <atomic>
#include <chrono>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <map>
#include <cmath>
#include <limits>
#include <mutex>
#include <thread>
#include <tuple>

namespace venture::profile {
namespace {
const auto* const output = venture::environment::getenv("VENTURE_PROFILE");
const auto* const stop_at = venture::environment::getenv("VENTURE_PROFILE_STOP_AT");
const bool events_only = [] {
  const auto* value = environment::getenv("VENTURE_PROFILE_EVENTS_ONLY");
  return value && std::string_view(value) == "1";
}();
}
const bool enabled = output && *output;
const bool compile_timing_enabled = enabled && [] {
  const auto* value = venture::environment::getenv("VENTURE_PROFILE_COMPILE");
  return value && std::string_view(value) == "1";
}();
const bool futex_timing_enabled = enabled && [] {
  const auto* value = venture::environment::getenv("VENTURE_PROFILE_FUTEX");
  return value && std::string_view(value) == "1";
}();
const bool futex_handoff_enabled = futex_timing_enabled && [] {
  const auto* value = venture::environment::getenv("VENTURE_PROFILE_FUTEX_HANDOFF");
  return value && std::string_view(value) == "1";
}();
const bool futex_handoff_cpu_enabled = futex_handoff_enabled && [] {
  const auto* value = venture::environment::getenv("VENTURE_PROFILE_FUTEX_HANDOFF_CPU");
  return value && std::string_view(value) == "1";
}();
namespace {
using Clock = std::chrono::steady_clock;
std::uint64_t handoff_option(const char* name, std::uint64_t fallback, std::uint64_t maximum) {
  const auto* text = environment::getenv(name);
  if (!text) return fallback;
  const std::string_view value(text);
  std::uint64_t result = 0;
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || result > maximum)
    throw std::runtime_error(std::string(name) + " is outside its diagnostic limit");
  return result;
}
const auto handoff_capacity = futex_handoff_enabled
    ? handoff_option("VENTURE_PROFILE_FUTEX_HANDOFF_CAPACITY", 8192, 1'048'576) : 8192;
const auto handoff_minimum_ns = futex_handoff_enabled
    ? handoff_option("VENTURE_PROFILE_FUTEX_HANDOFF_MIN_NS", 1'000'000, 1'000'000'000) : 1'000'000;
const auto handoff_tids = [] {
  std::vector<std::int32_t> result;
  const auto* text = futex_handoff_enabled ? environment::getenv("VENTURE_PROFILE_FUTEX_HANDOFF_TIDS") : nullptr;
  if (!text) return result;
  std::string_view remaining(text);
  do {
    const auto comma = remaining.find(',');
    const auto token = remaining.substr(0, comma);
    std::int32_t tid = 0;
    const auto parsed = std::from_chars(token.data(), token.data() + token.size(), tid);
    if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size() || tid <= 0)
      throw std::runtime_error("VENTURE_PROFILE_FUTEX_HANDOFF_TIDS requires positive comma-separated tids");
    result.push_back(tid);
    if (comma == remaining.npos) break;
    remaining.remove_prefix(comma + 1);
  } while (true);
  return result;
}();
struct FutexRecord;
using FutexCaller = std::array<GuestAddr, 4>; // PC, LR, two frame-chain returns
struct Thread {
  std::mutex lifetime;
  platform::ProfileThread native;
  Cpu* cpu = nullptr;
  std::int32_t guest_tid = 0;
  GuestAddr stack_base = 0;
  std::uint64_t stack_size = 0;
  std::string guest_name;
  std::atomic<std::uint32_t> activity{host};
  std::atomic<std::uint64_t> activity_epoch{0};
  // Only the owning guest thread can query its current processor. Requests
  // coalesce during long JIT calls; these observations are not CPU-time shares.
  std::atomic<bool> residency_requested{false};
  std::array<std::atomic<std::uint64_t>, 4> residency{}; // P, E, homogeneous, unknown
  std::array<std::uint64_t, 4> residency_reported{};
  Clock::time_point last = Clock::now();
  std::uint64_t cpu_start = 0, cpu_last = 0;
  std::int64_t cycles_last = -1;
  std::uint64_t cache_clears = 0;  // full JIT code-cache clears on this thread's CPU
  std::uint64_t emitted_blocks = 0; // only the owning thread reads/writes this counter
  std::vector<double> cache_clear_seconds;
  bool sampled = false;
  bool live = true;
  bool waiting = false;
  bool xr_submit = false;
  std::uint32_t wait_activity = 0;
  std::uint64_t wait_epoch = 0;
  Clock::time_point resample_wait{};
  // Owner-only lookup. Published records have immutable keys and atomic counts.
  std::map<FutexCaller, FutexRecord*> futex_callers;
  std::unique_ptr<FutexHandoffs<>> futex_handoffs;
  std::int64_t cpu_probe_min = -1, cpu_probe_median = -1;
};
struct FutexRecord {
  Thread* thread;
  FutexCaller caller;
  std::uint32_t activity;
  FutexCounters counters;
  FutexCounters::Snapshot previous{}; // frame submitter, protected by state mutex
};
thread_local Thread* local = nullptr;
struct Key {
  unsigned bucket;
  std::uint32_t host_tid, activity;
  std::int32_t guest_tid;
  std::uintptr_t ip;
  GuestAddr pc;
  bool translated;
  bool block_pc;
  auto operator<=>(const Key&) const = default;
};
struct Count { std::uint64_t samples = 0; double seconds = 0, cpu_seconds = 0, cycle_seconds = 0; std::uint64_t cycle_samples = 0; };
struct State {
  Clock::time_point origin = Clock::now();
  std::int64_t monotonic_origin = monotonic_ns();
  std::mutex mutex;
  std::mutex finish_mutex;
  std::map<GuestThread*, std::shared_ptr<Thread>> guests;
  std::map<std::uint32_t, std::shared_ptr<Thread>> threads;
  std::vector<std::shared_ptr<Thread>> history;
  std::map<Key, Count> counts;
  struct Translation { std::uintptr_t end; GuestAddr pc; std::uint32_t host_tid; };
  std::map<std::uintptr_t, Translation> translations;
  std::uint64_t emitted_blocks = 0;
  struct CompileRecord { Dynarmic::CompileTiming timing; std::int32_t guest_tid; };
  std::vector<CompileRecord> compile_records;
  std::size_t compile_limit = 500000;
  std::uint64_t compile_dropped = 0;
  std::vector<platform::ProfileModule> modules;
  std::vector<platform::ProfileSymbol> symbols, waits;
  std::map<std::string, double> milestones;
  struct Frame { double seconds; std::uint64_t index; std::int32_t guest_tid; };
  std::vector<Frame> frames;
  Json sync_samples = Json::array();
  std::vector<sync_census::IoSample> io_samples;
  std::atomic<std::uint64_t> io_dropped{0};
  std::vector<std::unique_ptr<FutexRecord>> futex_records;
  struct FutexFrame { double second; std::uint64_t index, record; FutexCounters::Snapshot counts; };
  std::vector<FutexFrame> futex_frames;
  std::uint64_t futex_dropped = 0;

  struct OutlinedSample { double seconds; std::int32_t tid; std::array<std::uint64_t, 16> counts; };
  std::vector<OutlinedSample> outlined_samples;
  std::jthread sampler;
  unsigned hz = 1000;
  std::vector<std::int32_t> focus_tids;
  std::vector<std::string> focus_roles;
  bool tick_capture = false;
  std::int32_t state_tid = 0;
  std::uint64_t ticks = 0, failures = 0, missed = 0;
  double sampler_seconds = 0;
  double sampler_cpu_seconds = 0;
  double cycles_per_ns = 0;
  double finished_seconds = 0;
  double capture_begin = 0, capture_end = std::numeric_limits<double>::infinity();
  bool started = false;
  std::atomic<bool> stopped{false};
  std::vector<platform::HostCpu> processors;
  unsigned min_efficiency = 0, max_efficiency = 0;
  bool efficiency_known = false;  // every processor's class is reported by the OS
  platform::ProfilePowerThrottling power;
};
// Retained for adopted JVM/host threads that may outlive main's locals.
State& state() { static auto* s = new State; return *s; }
double elapsed(const State& s) { return std::chrono::duration<double>(Clock::now() - s.origin).count(); }
FutexRecord* futex_record() {
  if (!local || !local->cpu) return nullptr;
  FutexCaller caller{local->cpu->pc(), local->cpu->lr(), 0, 0};
  auto fp = local->cpu->isa() == GuestIsa::a64 ? local->cpu->frame_pointer() : 0;
  if (local->cpu->isa() == GuestIsa::a64) for (unsigned depth = 0; depth < 2; ++depth) {
    if ((fp & 7) || fp < local->stack_base || fp - local->stack_base > local->stack_size || local->stack_size - (fp - local->stack_base) < 16) break;
    std::array<GuestAddr, 2> frame{};
    if (!platform::read_memory(fp, frame.data(), sizeof(frame))) break;
    caller[depth + 2] = frame[1];
    if (frame[0] <= fp) break;
    fp = frame[0];
  }
  if (auto it = local->futex_callers.find(caller); it != local->futex_callers.end()) return it->second;
  auto& s = state(); std::lock_guard lock(s.mutex);
  if (s.stopped) return nullptr;
  if (s.futex_records.size() >= 10000) { ++s.futex_dropped; return nullptr; }
  auto record = std::make_unique<FutexRecord>();
  record->thread = local; record->caller = caller; record->activity = local->activity.load(std::memory_order_relaxed);
  auto* result = record.get();
  s.futex_records.push_back(std::move(record));
  local->futex_callers.emplace(caller, result);
  return result;
}
void baseline(Thread& t) {
  if (t.sampled || !platform::profile_cpu_time(t.native, t.cpu_last)) return;
  t.cpu_start = t.cpu_last;
  t.cycles_last = platform::profile_cpu_cycles(t.native);
  t.last = Clock::now();
  t.sampled = true;
}
void load_symbols(State& s) {
  auto file = platform::profile_executable(); file.replace_extension(".profile-symbols.tsv");
  std::ifstream input(file);
  std::string line;
  const auto base = platform::profile_image_base();
  while (std::getline(input, line)) {
    auto tab = line.find('\t');
    if (tab == line.npos) continue;
    // address<TAB>size<TAB>name (size 0: unknown); older indexes have no size column.
    try {
      const auto begin = base + std::stoull(line.substr(0, tab), nullptr, 16);
      const auto second = line.find('\t', tab + 1);
      if (second == line.npos) { s.symbols.push_back({begin, 0, line.substr(tab + 1)}); continue; }
      const auto size = std::stoull(line.substr(tab + 1, second - tab - 1), nullptr, 16);
      s.symbols.push_back({begin, size ? begin + size : 0, line.substr(second + 1)});
    } catch (const std::exception&) { continue; }
  }
  // A symbol ends at its own size when known, and never past the next symbol's start.
  for (std::size_t i = 0; i < s.symbols.size(); ++i) {
    const auto next = i + 1 < s.symbols.size() ? s.symbols[i + 1].begin : UINTPTR_MAX;
    auto& end = s.symbols[i].end;
    end = end ? std::min(end, next) : (i + 1 < s.symbols.size() ? next : 0);
  }
  if (s.symbols.empty()) log::warn("profile", "Host symbol index absent: {}; compiler attribution will be incomplete", file.string());
}
const platform::ProfileSymbol* symbol(const State& s, std::uintptr_t ip) {
  auto it = std::upper_bound(s.symbols.begin(), s.symbols.end(), ip, [](auto ip, const auto& v) { return ip < v.begin; });
  if (it == s.symbols.begin()) return nullptr;
  --it; return ip < it->end ? &*it : nullptr;
}
bool waiting(const State& s, std::uintptr_t ip) {
  return std::any_of(s.waits.begin(), s.waits.end(), [&](const auto& w) { return ip >= w.begin && ip < w.end; });
}
std::string category(const State& s, const Key& k, std::string& module, std::string& host_symbol) {
  if (k.activity == pipeline_create) return "pipeline_create";
  // The cache also contains Dynarmic's dispatcher/prelude. Only a registered
  // translated block establishes that the sampled IP executes guest code.
  if (k.translated) return k.block_pc ? "guest_jit" : "jit_dispatch";
  const auto* sym = symbol(s, k.ip);
  if (sym) {
    host_symbol = sym->name;
    if (host_symbol.find("Dynarmic::") != host_symbol.npos || host_symbol.find("Xbyak::") != host_symbol.npos)
      return "dynarmic_host";
  }
  for (const auto& m : s.modules) if (k.ip >= m.begin && k.ip < m.end) {
    module = m.name;
    if (host_symbol.empty()) {
      auto it = std::upper_bound(m.exports.begin(), m.exports.end(), k.ip, [](auto ip, const auto& v) { return ip < v.begin; });
      if (it != m.exports.begin()) { --it; if (k.ip < it->end) host_symbol = m.name + "!" + it->name; }
    }
    if (m.name == "libegl.dll" || m.name == "libglesv2.dll" || m.name == "libegl.so" || m.name == "libglesv2.so") return "angle";
    if (m.name == "jvm.dll" || m.name == "libjvm.so") return "jvm";
    // Vulkan loader/driver interface: classify ICDs by their standard export,
    // rather than maintaining a GPU-vendor DLL-name allowlist.
    if (m.name == "vulkan-1.dll" || m.name == "libvulkan.so.1" || m.vulkan_driver) return "vulkan";
    if (m.system) return "system";
    if (m.begin == platform::profile_image_base()) {
      if (k.activity != host && k.activity != jit) return "hle:" + hle::function(k.activity).library;
      return k.activity == jit ? "jit_host_support" : "venture";
    }
    return "other";
  }
  return "other_private_code"; // HotSpot JIT and unregistered code remain explicit.
}
void run(State& s, std::stop_token token) {
  try {
    const auto sampler_native = platform::profile_current_thread();
    std::uint64_t sampler_cpu_start = 0;
    platform::profile_cpu_time(sampler_native, sampler_cpu_start);
    const auto period = std::chrono::nanoseconds(1'000'000'000 / s.hz);
    auto next = Clock::now(), refresh = next;
    auto coarse_next = next, residency_next = next + std::chrono::seconds(5);
    std::size_t coarse_index = 0;
    std::vector<std::shared_ptr<Thread>> targets;
    std::vector<std::shared_ptr<Thread>> coarse;
    struct TickSample {
      platform::ProfileSample sample;
      std::int32_t tid;
      std::uint32_t host_tid;
      double begin, end;
      bool wait, block_pc;
    };
    std::vector<TickSample> tick_samples;
    std::ofstream tick_output;
    if (s.tick_capture) {
      tick_samples.reserve(128);
      tick_output.open(std::filesystem::path(output).concat(".ticks.jsonl"));
      if (!tick_output) throw std::runtime_error("cannot open profile tick output");
    }
    targets.reserve(128);
    while (!token.stop_requested()) {
      if (elapsed(s) >= s.capture_end) break;
      if (elapsed(s) < s.capture_begin) {
        platform::profile_delay(10'000'000);
        next = Clock::now();
        continue;
      }
      const auto work_start = Clock::now();
      tick_samples.clear();
      if (work_start >= refresh) {
        if (sync_census::enabled) {
          Json census{{"second", elapsed(s)}};
          for (unsigned i = 0; i < sync_census::metric_count; ++i)
            census[sync_census::metric_names[i]] = sync_census::metrics[i].load(std::memory_order_relaxed);
          for (unsigned i = 0; i < sync_census::wakes.size(); ++i) {
            const auto& w = sync_census::wakes[i];
            census["wakes"][sync_census::source_names[i]] = {
                {"calls", w.calls.load(std::memory_order_relaxed)}, {"empty", w.empty.load(std::memory_order_relaxed)},
                {"threads", w.threads.load(std::memory_order_relaxed)}, {"ns", w.ns.load(std::memory_order_relaxed)}};
          }
          census["condition_notifications_by_thread"] = Json::array();
          for (const auto& row : sync_census::condition_snapshot())
            census["condition_notifications_by_thread"].push_back({
                {"guest_tid", row.guest_tid}, {"signals", row.signals}, {"skipped_signals", row.skipped_signals},
                {"broadcasts", row.broadcasts}, {"skipped_broadcasts", row.skipped_broadcasts}});
          census["condition_notifications_dropped"] = sync_census::condition_dropped();
          s.sync_samples.push_back(std::move(census));
        }
        // OS enumeration and allocation happen while no target is suspended.
        auto native = platform::profile_threads(); auto modules = platform::profile_modules();
        std::lock_guard lock(s.mutex);
        s.modules = std::move(modules);
        for (auto& [id, t] : s.threads) t->live = t->cpu != nullptr;
        for (auto& n : native) {
          if (auto it = s.threads.find(n.id); it != s.threads.end() && it->second->native.creation_ticks == n.creation_ticks) {
            it->second->live = true;
            if (!n.name.empty()) it->second->native.name = std::move(n.name);
          } else {
            auto t = std::make_shared<Thread>(); t->native = std::move(n); s.threads[t->native.id] = t; s.history.push_back(t);
          }
        }
        refresh = work_start + std::chrono::seconds(1);
      }
      {
        std::lock_guard lock(s.mutex);
        targets.clear();
        coarse.clear();
        for (const auto& [id, t] : s.threads) if (t->live) {
          // Baseline every admitted thread, including the coarse pool. Waiting
          // for its first round-robin sample discards up to a whole pool cycle.
          baseline(*t);
          const auto role = frame_cpu::role_name(t->guest_name.empty() ? t->native.name : t->guest_name);
          const bool role_match = s.focus_roles.empty() ||
              std::find(s.focus_roles.begin(), s.focus_roles.end(), role) != s.focus_roles.end() ||
              (t->xr_submit && std::find(s.focus_roles.begin(), s.focus_roles.end(), "xr_submission") != s.focus_roles.end());
          if (t->cpu && role_match && (s.focus_tids.empty() || std::find(s.focus_tids.begin(), s.focus_tids.end(), t->guest_tid) != s.focus_tids.end())) targets.push_back(t);
          else coarse.push_back(t);
        }
      }
      // A fixed total background budget avoids hundreds of JVM/driver threads
      // turning a main-thread profile into thousands of suspensions per second.
      if (!coarse.empty() && Clock::now() >= coarse_next) {
        targets.push_back(coarse[coarse_index++ % coarse.size()]);
        coarse_next = Clock::now() + std::chrono::milliseconds(100);
      }
      for (const auto& t : targets) {
          platform::ProfileSample sample;
          sample.inspect_state = s.tick_capture && t->guest_tid == s.state_tid;
          const auto sample_begin = s.tick_capture ? elapsed(s) : 0.0;
          const auto pipeline_activity = t->cpu ? nullptr : pipeline_thread_activity(t->native.id);
          {
            // Pin just this Cpu. Never hold the block-registration/statistics
            // lock across SuspendThread/GetThreadContext: compiler callbacks
            // on other cores must continue making progress during sampling.
            std::lock_guard pin(t->lifetime);
            // Resuspending an unchanged blocked thread 1,000 times/sec adds
            // overhead without new information. Still capture it at 50 Hz;
            // a guest/HLE activity transition immediately restores full rate.
            if (!s.tick_capture && t->cpu && t->waiting && Clock::now() < t->resample_wait &&
                t->activity.load(std::memory_order_relaxed) == t->wait_activity &&
                t->activity_epoch.load(std::memory_order_relaxed) == t->wait_epoch) continue;
            // Read before suspension so transitions racing this observation
            // conservatively force a resample, including a wake followed by
            // another wait in the same HLE function between two ticks.
            t->wait_epoch = t->activity_epoch.load(std::memory_order_relaxed);
            if (!platform::profile_sample(t->native, t->cpu, pipeline_activity ? pipeline_activity.get() : &t->activity, sample)) { ++s.failures; continue; }
            // Requested after the sample: a thread servicing it just before
            // suspension would otherwise land samples in the residency code.
            if (t->cpu) t->residency_requested.store(true, std::memory_order_release);
          }
          std::lock_guard lock(s.mutex);
          const auto now = Clock::now();
          if (sample.outlined_atomics)
            s.outlined_samples.push_back({std::chrono::duration<double>(now - s.origin).count(),
                                         t->guest_tid, sample.outlined_counts});
          t->waiting = waiting(s, sample.ip); t->wait_activity = sample.activity;
          t->resample_wait = now + std::chrono::milliseconds(20);
          // If the admission-time CPU query failed, this observation retries
          // the baseline rather than charging time from before the capture.
          const auto dt = t->sampled ? std::chrono::duration<double>(now - t->last).count() : 0.;
          const auto cpu_dt = t->sampled && sample.cpu_ns >= t->cpu_last ? (sample.cpu_ns - t->cpu_last) / 1e9 : 0.;
          const bool cycles_valid = s.cycles_per_ns > 0 && t->cycles_last >= 0 && sample.cpu_cycles >= t->cycles_last;
          const auto cycle_dt = cycles_valid ? (sample.cpu_cycles - t->cycles_last) / s.cycles_per_ns / 1e9 : 0.;
          t->cycles_last = sample.cpu_cycles;
          t->last = now;
          if (!t->sampled) t->cpu_start = sample.cpu_ns;
          t->sampled = true;
          t->cpu_last = sample.cpu_ns;
          const auto seconds = std::chrono::duration<double>(now - s.origin).count();
          bool block_pc = false;
          if (sample.translated) {
            auto it = s.translations.upper_bound(sample.ip);
            if (it != s.translations.begin()) {
              --it;
              if (sample.ip < it->second.end) { sample.guest_pc = it->second.pc; block_pc = true; }
            }
          }
          if (s.tick_capture)
            tick_samples.push_back({sample, t->guest_tid, t->native.id, sample_begin,
                                    elapsed(s), t->waiting, block_pc});
          // Aggregate after ResumeThread. No symbol lookup or container mutation
          // can deadlock against an allocator/loader held by a suspended target.
          Key key{static_cast<unsigned>(seconds * 50), t->native.id, sample.activity, t->guest_tid, sample.ip,
                  sample.guest_pc, sample.translated, block_pc};
          auto& count = s.counts[key]; ++count.samples; count.seconds += dt; count.cpu_seconds += cpu_dt;
          count.cycle_seconds += cycle_dt; count.cycle_samples += cycles_valid;
      }
      if (s.tick_capture) {
        // All targets have resumed. These are ordered observations, not an
        // atomic process snapshot; explicit timestamps expose their skew.
        Json observations = Json::array(), futexes = Json::array();
        for (const auto& item : tick_samples) {
          const auto& sample = item.sample;
          Json row{{"guest_tid", item.tid}, {"host_tid", item.host_tid},
                   {"begin", item.begin}, {"end", item.end}, {"host_ip", sample.ip},
                   {"guest_pc", sample.guest_pc}, {"block_pc", item.block_pc},
                   {"translated", sample.translated}, {"activity", sample.activity},
                   {"host_wait", item.wait}, {"cpu_ns", sample.cpu_ns},
                   {"cpu_cycles", sample.cpu_cycles}};
          row["jit_active"] = sample.activity == jit;
          if (sample.activity && sample.activity != jit && sample.activity != pipeline_create) {
            const auto& function = hle::function(sample.activity);
            row["hle"] = function.library + "!" + function.name;
          }
          if (sample.spilled_register_count) {
            row["spilled_registers"] = std::vector<std::uint64_t>(sample.spilled_registers.begin(),
                sample.spilled_registers.begin() + sample.spilled_register_count);
            row["register_memory"] = Json::array();
            for (const auto& probe : sample.register_memory)
              row["register_memory"].push_back({{"address", probe.address},
                  {"readable", probe.readable}, {"words", probe.words}});
            row["host_rbp"] = sample.host_rbp;
            row["frame_returns"] = std::vector<std::uint64_t>(sample.frame_returns.begin(),
                sample.frame_returns.begin() + sample.frame_count);
            row["stack_words"] = std::vector<std::uint64_t>(sample.stack_words.begin(),
                sample.stack_words.begin() + sample.stack_word_count);
          }
          observations.push_back(std::move(row));
        }
        const auto futex_begin = elapsed(s);
        for (const auto& wait : futex_wait_snapshot())
          futexes.push_back({{"guest_tid", wait.tid}, {"address", wait.address},
              {"expected", wait.expected}, {"value", wait.value}, {"readable", wait.readable},
              {"woken", wait.woken}, {"deadline", wait.deadline}});
        tick_output << Json{{"tick", s.ticks}, {"samples", observations},
            {"futex_begin", futex_begin}, {"futex_end", elapsed(s)}, {"futexes", futexes}}.dump() << '\n';
      }
      ++s.ticks;
      if (Clock::now() >= residency_next) {
        const auto power = platform::profile_power_throttling();
        log::info("profile", "Process power throttling available={} control_mask={} state_mask={} error={}",
            power.available, power.control_mask, power.state_mask, power.error);
        std::lock_guard lock(s.mutex);
        for (const auto& t : s.history) {
          if (!t->guest_tid) continue;
          std::array<std::uint64_t, 4> delta{};
          for (unsigned i = 0; i < delta.size(); ++i) {
            const auto count = t->residency[i].load(std::memory_order_relaxed);
            delta[i] = count - t->residency_reported[i]; t->residency_reported[i] = count;
          }
          const auto total = delta[0] + delta[1] + delta[2] + delta[3];
          if (total) log::info("profile", "CPU residency guest tid {} ({}) P={} ({:.1f}%) E={} ({:.1f}%) homogeneous={} unknown={} observations; CPU {:.3f}s since capture admission",
              t->guest_tid, t->guest_name, delta[0], 100. * delta[0] / total, delta[1], 100. * delta[1] / total,
              delta[2], delta[3], (t->cpu_last - t->cpu_start) / 1e9);
        }
        residency_next = Clock::now() + std::chrono::seconds(5);
      }
      const auto work_end = Clock::now();
      s.sampler_seconds += std::chrono::duration<double>(work_end - work_start).count();
      next += period;
      if (next <= work_end) { ++s.missed; next = work_end + period; }
      platform::profile_delay(std::chrono::duration_cast<std::chrono::nanoseconds>(next - work_end).count());
    }
    std::uint64_t sampler_cpu_end = 0;
    if (platform::profile_cpu_time(sampler_native, sampler_cpu_end) && sampler_cpu_end >= sampler_cpu_start)
      s.sampler_cpu_seconds = (sampler_cpu_end - sampler_cpu_start) / 1e9;
  } catch (const std::exception& e) { log::error("profile", "Sampler stopped: {}", e.what()); }
}
void write(State& s) {
  Json rows = Json::array(), threads = Json::array(), frames = Json::array(), io = Json::array();
  std::map<std::string, Count> categories, guests, functions;
  double total = 0, compile = 0, blocked = 0;
  std::map<GuestAddr, std::string> guest_names;
  struct GuestSymbol { GuestAddr begin, end; std::string name; };
  struct GuestImage { GuestAddr begin, end, base; std::string name; std::vector<GuestSymbol> symbols; };
  std::vector<GuestImage> images;
  Json guest_images = Json::array();
  const bool images_available = Linker::get().try_for_each_image([&](const Library& library) {
    auto* loaded = dynamic_cast<const elf::Image*>(&library);
    if (!loaded) return; // synthetic unwind images do not own ELF symbol tables
    const auto& image = *loaded;
    if (compile_timing_enabled) {
      Json segments = Json::array();
      for (const auto& ph : image.headers) if (ph.type == 1 && (ph.flags & 1))
        segments.push_back({{"begin", image.load_bias + ph.vaddr}, {"end", image.load_bias + ph.vaddr + ph.memsz}});
      guest_images.push_back({{"name", image.soname()}, {"source_file", image.source_file().string()},
          {"base", image.load_bias}, {"begin", image.map_start}, {"end", image.map_start + image.map_size},
          {"a32", image.isa() == GuestIsa::a32}, {"executable_segments", segments}});
    }
    GuestImage entry{image.map_start, image.map_start + image.map_size, image.load_bias, image.soname(), {}};
    for (const auto& symbol : image.symbols()) {
      if (!symbol.shndx || !symbol.name || ((symbol.info & 15) != 2 && (symbol.info & 15) != 10)) continue;
      const auto begin = image.load_bias + symbol.value;
      entry.symbols.push_back({begin, begin + symbol.size, image.symbol_name(symbol)});
    }
    std::sort(entry.symbols.begin(), entry.symbols.end(), [](const auto& a, const auto& b) { return std::tie(a.begin, a.name) < std::tie(b.begin, b.name); });
    images.push_back(std::move(entry));
  });
  if (!images_available)
    log::warn("profile", "Loader metadata busy; profile retains raw guest PCs");
  // Snapshot loader metadata before taking the statistics lock: a library
  // constructor can register translations while a capture ends mid-game.
  std::lock_guard lock(s.mutex);
  const auto guest_name = [&](GuestAddr pc) {
    for (const auto& image : images) if (pc >= image.begin && pc < image.end) {
      auto it = std::upper_bound(image.symbols.begin(), image.symbols.end(), pc, [](auto p, const auto& v) { return p < v.begin; });
      if (it != image.symbols.begin()) {
        --it;
        if (pc < it->end) return image.name + "!" + it->name;
      }
      return image.name + std::format("+0x{:x}", pc - image.base);
    }
    return std::format("unknown+0x{:x}", pc);
  };
  for (const auto& [k, c] : s.counts) {
    std::string module, host_symbol, guest_symbol, function;
    auto cat = category(s, k, module, host_symbol);
    auto wait = waiting(s, k.ip);
    if (k.pc) {
      auto it = guest_names.find(k.pc);
      if (it == guest_names.end()) {
        it = guest_names.emplace(k.pc, guest_name(k.pc)).first;
      }
      guest_symbol = it->second;
    }
    if (k.activity && k.activity != jit && k.activity != pipeline_create) { const auto& f = hle::function(k.activity); function = f.library + "!" + f.name; }
    const bool guest = k.guest_tid != 0;
    if (guest) {
      total += c.seconds;
      if (wait) blocked += c.seconds;
      if (k.activity == jit && cat != "guest_jit" && !wait) compile += c.seconds;
      auto& v = categories[cat]; v.samples += c.samples; v.seconds += c.seconds;
      if (cat == "guest_jit") { auto& v = guests[guest_symbol]; v.samples += c.samples; v.seconds += c.seconds; }
      if (!function.empty()) { auto& v = functions[function]; v.samples += c.samples; v.seconds += c.seconds; }
    }
    rows.push_back({{"second", k.bucket / 50.0}, {"host_tid", k.host_tid}, {"guest_tid", k.guest_tid}, {"category", cat},
                    {"module", module}, {"host_symbol", host_symbol}, {"host_ip", k.ip}, {"guest_pc", k.pc},
                    {"guest_symbol", guest_symbol}, {"guest_pc_source", k.block_pc ? "translated_block" : "dispatcher_spill"},
                    {"hle", function}, {"jit_active", k.activity == jit},
                    {"state", wait ? "host_wait" : "running_or_runnable"}, {"samples", c.samples}, {"thread_seconds", c.seconds}, {"cpu_seconds", c.cpu_seconds},
                    {"cpu_estimate_seconds", c.cycle_samples ? Json(c.cycle_seconds) : Json(nullptr)}, {"cycle_samples", c.cycle_samples}});
  }
  for (const auto& t : s.history)
    threads.push_back({{"host_tid", t->native.id}, {"guest_tid", t->guest_tid}, {"name", t->native.name}, {"guest_name", t->guest_name},
                       {"guest_stack_base", t->stack_base}, {"guest_stack_size", t->stack_size}, {"attached_at_capture", t->cpu != nullptr},
                       {"cpu_seconds", (t->cpu_last - t->cpu_start) / 1e9}, {"jit_cache_clears", t->cache_clears}, {"jit_cache_clear_seconds", t->cache_clear_seconds},
                       {"cpu_residency", {{"p", t->residency[0].load()}, {"e", t->residency[1].load()},
                          {"homogeneous", t->residency[2].load()}, {"unknown", t->residency[3].load()}}}});
  // Load ranges let host_ip be symbolized offline against each module's own debug info.
  Json modules = Json::array();
  for (const auto& m : s.modules) modules.push_back({{"name", m.name}, {"path", m.path}, {"begin", m.begin}, {"end", m.end}});
  for (const auto& f : s.frames)
    frames.push_back({{"second", f.seconds}, {"index", f.index}, {"guest_tid", f.guest_tid}});
  for (const auto& event : s.io_samples) {
    const auto seconds = [&](std::int64_t ns) { return (ns - s.monotonic_origin) / 1e9; };
    io.push_back({{"source", sync_census::io_names[static_cast<unsigned>(event.source)]},
        {"guest_tid", event.guest_tid}, {"begin", seconds(event.begin_ns)}, {"end", seconds(event.end_ns)},
        {"ready_second", event.ready_ns < 0 ? Json{} : Json(seconds(event.ready_ns))},
        {"deadline_second", event.deadline_ns < 0 ? Json{} : Json(seconds(event.deadline_ns))},
        {"sleeps", event.sleeps}, {"sleep_ns", event.sleep_ns}, {"ready", event.ready}, {"timed_out", event.timed_out}});
  }
  // Existing emitted-range records identify copies of the same guest PC across
  // per-thread JITs. Count distinct (thread, PC) pairs, keeping FPCR variants
  // within a thread together. This adds no work to unprofiled execution.
  std::map<GuestAddr, std::map<std::uint32_t, std::uint64_t>> copies;
  std::map<std::uint32_t, std::pair<std::uint64_t, std::uint64_t>> jit_threads;
  for (const auto& [begin, block] : s.translations) {
    if (!block.host_tid) continue;
    copies[block.pc][block.host_tid] += block.end - begin;
    auto& thread = jit_threads[block.host_tid];
    ++thread.first; thread.second += block.end - begin;
  }
  std::uint64_t pairs = 0, duplicated_pcs = 0, extra_copies = 0, extra_bytes = 0;
  std::vector<std::pair<GuestAddr, std::size_t>> duplicates;
  for (const auto& [pc, by_thread] : copies) {
    pairs += by_thread.size();
    if (by_thread.size() < 2) continue;
    ++duplicated_pcs; extra_copies += by_thread.size() - 1;
    std::uint64_t bytes = 0, largest = 0;
    for (const auto& [tid, size] : by_thread) { bytes += size; largest = std::max(largest, size); }
    extra_bytes += bytes - largest;
    duplicates.emplace_back(pc, by_thread.size());
  }
  std::sort(duplicates.begin(), duplicates.end(), [](auto a, auto b) { return a.second > b.second; });
  Json duplicate_rows = Json::array(), jit_rows = Json::array();
  for (std::size_t i = 0; i < std::min<std::size_t>(20, duplicates.size()); ++i)
    duplicate_rows.push_back({{"guest_pc", duplicates[i].first}, {"guest_symbol", guest_name(duplicates[i].first)}, {"threads", duplicates[i].second}});
  for (const auto& [tid, counts] : jit_threads)
    jit_rows.push_back({{"host_tid", tid}, {"variant_blocks", counts.first}, {"host_code_bytes", counts.second}});
  Json translation_memory{{"unique_guest_pcs", copies.size()}, {"thread_pc_pairs", pairs},
      {"guest_pcs_on_multiple_threads", duplicated_pcs}, {"extra_thread_pc_copies", extra_copies},
      {"cross_thread_extra_host_code_bytes", extra_bytes}, {"per_thread", jit_rows}, {"most_copied", duplicate_rows},
      {"notes", "Active emitted ranges at capture, not lifetime compilations. FPCR variants are grouped by guest PC within each thread. Extra bytes count other threads after retaining the largest per-PC thread copy; this is a duplication measurement, not a shared-cache savings prediction."}};
  auto sorted = [](const auto& values) {
    std::vector<std::pair<std::string, Count>> out(values.begin(), values.end());
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.second.seconds > b.second.seconds; }); return out;
  };
  Json futex_callers = Json::array(), futex_frames = Json::array();
  for (std::size_t id = 0; id < s.futex_records.size(); ++id) {
    const auto& record = *s.futex_records[id];
    Json callers = Json::array();
    for (auto pc : record.caller) {
      bool mapped = false;
      for (const auto& image : images) if (pc >= image.begin && pc < image.end) {
        callers.push_back({{"address", pc}, {"location", image.name + std::format("+0x{:x}", pc - image.base)}, {"symbol", guest_name(pc)}});
        mapped = true; break;
      }
      if (!mapped) callers.push_back({{"address", pc}, {"location", std::format("0x{:x}", pc)}, {"symbol", ""}});
    }
    std::string entry;
    if (record.activity && record.activity != jit && record.activity != pipeline_create) {
      const auto& f = hle::function(record.activity); entry = f.library + "!" + f.name;
    }
    futex_callers.push_back({{"id", id}, {"host_tid", record.thread->native.id}, {"guest_tid", record.thread->guest_tid},
                            {"name", record.thread->guest_name.empty() ? record.thread->native.name : record.thread->guest_name}, {"hle", entry}, {"callers", callers}});
  }
  for (const auto& frame : s.futex_frames)
    futex_frames.push_back(Json::array({frame.second, frame.index, frame.record, frame.counts}));
  Json futex_report{{"enabled", futex_timing_enabled}, {"fields", FutexCounters::names}, {"callers", futex_callers},
                    {"frames", futex_frames}, {"dropped", s.futex_dropped},
                    {"notes", "Exact opt-in counters. Rows: [profiler second, projection frame, caller id, deltas]. Entries and completions can occur in different frames. Duration includes validation, bucket acquisition and blocking; parked bins require a park attempt. Park attempts include spurious returns and timed-wait attempts. Notifications count matching waiter notify calls; empty wakes never notify. Atomic fields are read independently at frame boundaries. Caller locations: PC, LR, up to two guarded A64 frame-chain returns; unresolved locations retain raw addresses. Owner-thread instrumentation adds observer cost beyond sampler cost. dropped must be zero."}};
  auto location = [&](GuestAddr pc) {
    for (const auto& image : images) if (pc >= image.begin && pc < image.end)
      return image.name + std::format("+0x{:x}", pc - image.base);
    return std::format("0x{:x}", pc);
  };
  auto entry_name = [](std::uint32_t activity) {
    if (!activity || activity == jit || activity == pipeline_create) return std::string{};
    const auto& f = hle::function(activity);
    return f.library + "!" + f.name;
  };
  std::map<std::uint64_t, unsigned> words;
  Json handoff_threads = Json::array();
  for (const auto& thread : s.history) if (thread->futex_handoffs) {
    Json rows = Json::array();
    for (const auto& row : thread->futex_handoffs->snapshot()) {
      auto [word, inserted] = words.try_emplace(row.address, static_cast<unsigned>(words.size()));
      rows.push_back(Json::array({word->second, location(row.waiter_lr), entry_name(row.waiter_activity),
          row.waker_tid, location(row.waker_lr), entry_name(row.waker_activity),
          row.begin_ns, row.enqueue_ns, row.park_ns, row.wake_request_ns, row.wake_lock_ns,
          row.notify_begin_ns, row.notify_end_ns, row.resumed_ns, row.end_ns, row.spin_ns,
          row.deadline_ns, row.expected, row.bitset, row.parks, row.result,
          row.wake_cpu, row.bucket_cpu, row.notify_cpu, row.wait_cpu}));
    }
    handoff_threads.push_back({{"guest_tid", thread->guest_tid}, {"host_tid", thread->native.id},
        {"name", thread->guest_name}, {"rows", rows},
        {"cpu_probe_min_units", thread->cpu_probe_min}, {"cpu_probe_median_units", thread->cpu_probe_median},
        {"dropped", thread->futex_handoffs->dropped.load(std::memory_order_relaxed)}});
  }
  futex_report["handoffs"] = Json{{"enabled", futex_handoff_enabled}, {"minimum_wait_ns", handoff_minimum_ns},
      {"capacity_per_thread", handoff_capacity}, {"selected_tids", handoff_tids},
      {"cpu_enabled", futex_handoff_cpu_enabled},
#ifdef _WIN32
      {"cpu_clock", "QueryThreadCycleTime"}, {"cpu_units_per_ns", s.cycles_per_ns},
#else
      {"cpu_clock", "CLOCK_THREAD_CPUTIME_ID"}, {"cpu_units_per_ns", 1.0},
#endif
      {"columns", {"word", "waiter_lr", "waiter_hle", "waker_tid", "waker_lr", "waker_hle",
                   "begin_ns", "enqueue_ns", "park_ns", "wake_request_ns", "wake_lock_ns",
                   "notify_begin_ns", "notify_end_ns", "resumed_ns", "end_ns", "spin_ns",
                   "deadline_ns", "expected", "bitset", "parks", "result",
                   "wake_cpu_prefix_units", "bucket_cpu_units", "notify_cpu_units", "wait_cpu_units"}}, {"threads", handoff_threads},
      {"notes", "Completed waits begun inside the capture window, filtered by minimum_wait_ns and selected_tids. Word IDs identify equal guest addresses within this run; storage reuse is not distinguished. Waker tid 0 means an unattached host thread. Wall timestamps include scheduling; resumed is bucket-lock reacquisition, not exact OS ready-to-running. CPU fields use the calling thread's accounting clock, -1 unavailable; divide by cpu_units_per_ns only when positive. wait_cpu covers the waiter primitive including park/reacquire CPU, excluding descheduled wall time. wake_cpu_prefix ends after this notify, before bucket unlock; multi-notify prefixes overlap and must not be summed. Per-caller wake_cpu_units includes bucket unlock but excludes the diagnostic completion hook. CPU intervals include tracing/read overhead and are conservative inclusive costs, not recoverable savings; per-thread probes bracket two consecutive CPU-clock reads for 33 trials. Cross-window unfinished waits are absent. Rows are owner-published once; capacity_per_thread bounds storage and dropped must be zero. Optional tracing/counters add observer cost."}};
  Json processors = Json::array();
  for (const auto& cpu : s.processors)
    processors.push_back({{"index", cpu.index},
                          {"efficiency_class", cpu.efficiency_known ? Json(cpu.efficiency_class) : Json(nullptr)}});
  Json compile_rows = Json::array();
  std::array<std::uint64_t, Dynarmic::compile_phase_count> compile_totals{};
  for (const auto& r : s.compile_records) {
    const auto& t = r.timing;
    for (std::size_t i = 0; i < compile_totals.size(); ++i) compile_totals[i] += t.phase_ns[i];
    compile_rows.push_back(Json::array({t.begin_ns, t.end_ns, r.guest_tid, t.guest_pc,
                                      t.a32, t.completed, t.phase_ns, t.location_descriptor}));
  }
  Json compile_report{{"enabled", compile_timing_enabled}, {"dropped_blocks", s.compile_dropped},
      {"phase_names", Dynarmic::compile_phase_names}, {"phase_ns", compile_totals},
      {"record_limit", s.compile_limit},
      {"guest_images", guest_images},
      {"columns", {"begin_monotonic_ns", "end_monotonic_ns", "guest_tid", "guest_pc", "a32", "completed", "phase_ns", "location_descriptor"}},
      {"blocks", compile_rows},
      {"notes", "Opt-in cold compilation wall-clock timings, not CPU time. Exclusive phases include scheduling delays and timer overhead. Completed=false identifies failed compilation; new-block counts use completed=true. Per-block begin/end timestamps share frames' monotonic origin; all threads may overlap, so their sum is not critical-path frame time. Code registration includes the profiler's existing range-map callback. Events are bounded to record_limit blocks and the capture window; dropped_blocks must be zero for complete counts."}};
  Json report{{"schema_version", 3}, {"bucket_seconds", .02}, {"focus_guest_tid", s.focus_tids.size() == 1 ? s.focus_tids.front() : 0},
              {"focus_guest_tids", s.focus_tids}, {"focus_roles", s.focus_roles}, {"package", process_config().package}, {"requested_hz", s.hz}, {"elapsed_seconds", s.finished_seconds},
              {"capture_begin_seconds", s.capture_begin}, {"capture_end_seconds", std::min(s.capture_end, s.finished_seconds)}, {"frames", frames},
              {"monotonic_origin_ns", s.monotonic_origin}, {"ticks", s.ticks}, {"missed_deadlines", s.missed}, {"sample_failures", s.failures},
              {"sampling_enabled", !events_only}, {"sampler_work_seconds", s.sampler_seconds}, {"sampler_cpu_seconds", s.sampler_cpu_seconds}, {"milestones", s.milestones}, {"threads", threads}, {"modules", modules}, {"samples", rows},
              {"cycles_per_ns", s.cycles_per_ns}, {"cpu_estimate_notes", "QueryThreadCycleTime deltas divided by startup TSC/QPC calibration, matching frame CPU timing. Estimates, not exact CPU nanoseconds. cycle_samples reports successful delta coverage; null cpu_estimate_seconds means unavailable. OS cpu_seconds remain separately available."},
              {"emitted_blocks", s.emitted_blocks}, {"jit_compilation", compile_report},
              {"sync_census", s.sync_samples},
              {"io_census", io}, {"io_census_dropped", s.io_dropped.load()},
              {"power_throttling", {{"available", s.power.available}, {"control_mask", s.power.control_mask}, {"state_mask", s.power.state_mask}, {"error", s.power.error}}},
              {"processors", processors},
              {"cpu_residency_notes", "Owner-thread processor observations requested at profiler ticks and collected at the next guest/JIT/HLE activity transition. Long calls coalesce ticks; blocked threads are observed after waking. P means highest EfficiencyClass on a hybrid topology; E means lower classes. Homogeneous topology is reported separately. Observation percentages are not CPU-time residency or clock-speed measurements."},
              {"translation_memory", translation_memory},
              {"pipeline_creation", pipeline_report()},
              {"invalidation", invalidation_tracing ? invalidation_report() : Json(nullptr)},
              {"futex", futex_report},
              {"notes", "Thread-seconds include blocked/runnable time, not CPU time. cpu_seconds are OS thread CPU-time deltas; total thread CPU time is measured, attribution to the interval's final sampled PC is statistical, not exact call timing. Windows CPU accounting is coarser than the sampling period. Wait states identify Windows blocking functions; unchanged waits are resampled at 50 Hz and guest/HLE activity transitions restore full rate. Translated blocks use the emitted host-range map; prelude/dispatcher samples fall back to the last spilled guest PC, which may lag linked blocks. jit_host_inclusive includes compilation, memory callbacks, cache publication and dispatcher work. Non-guest host threads, including JVM threads, share a round-robin budget of 10 samples/sec. VENTURE_PROFILE_TID optionally focuses the requested rate on comma-separated guest tids, putting other guest threads in that coarse pool. Twenty-millisecond buckets permit interval analysis; sparse/background samples have lower temporal resolution. Frames timestamp successful projection submissions, not GPU completion; eye dumps may add stalls."},
              {"guest_thread_seconds", total}, {"blocked_thread_seconds", blocked}, {"jit_host_inclusive_seconds", compile}};
  if (!s.outlined_samples.empty()) {
    report["outlined_atomic_helpers"] = outlined_atomics::names;
    report["outlined_atomic_samples"] = Json::array();
    for (const auto& sample : s.outlined_samples)
      report["outlined_atomic_samples"].push_back({{"second", sample.seconds}, {"guest_tid", sample.tid}, {"counts", sample.counts}});
    report["outlined_atomic_notes"] = "Cumulative owner-CPU instruction counts read while suspended; pairs are helper entries and direct BL calls. Entry minus BL includes indirect/tail entries. Exact complete 8-byte compiler-rt/libgcc ldadd/cas bodies only. Instrumented census, not a timing baseline.";
  }
  auto path = std::filesystem::absolute(paths::from_utf8(output));
  std::filesystem::create_directories(path.parent_path());
  std::ofstream json_file(path, std::ios::binary);
  if (!json_file) throw std::runtime_error("Cannot open VENTURE_PROFILE output");
  json_file << report.dump(2) << '\n';
  if (!json_file) throw std::runtime_error("Cannot write VENTURE_PROFILE output");
  path.replace_extension(".txt"); std::ofstream summary(path);
  if (!summary) throw std::runtime_error("Cannot open profile text summary");
  summary << "Wall seconds: " << s.finished_seconds << "\nGuest thread seconds: " << total << "\nHost waits: " << blocked
          << "\nJIT host inclusive: " << compile << " (" << (total ? 100 * compile / total : 0) << "%)\n";
  summary << "\nGuest CPU processor observations (P / E / homogeneous / unknown)\n";
  for (const auto& t : s.history) if (t->guest_tid)
    summary << t->guest_tid << ' ' << t->guest_name << ": " << t->residency[0].load() << " / "
            << t->residency[1].load() << " / " << t->residency[2].load() << " / " << t->residency[3].load() << '\n';
  for (const auto& [label, values] : {std::pair{"Categories", &categories}, {"Guest functions (block PC where available)", &guests}, {"HLE functions (inclusive)", &functions}}) {
    summary << '\n' << label << '\n'; unsigned n = 0;
    for (const auto& [name, value] : sorted(*values)) { if (n++ == 20) break; summary << value.seconds << "s " << value.samples << " samples " << name << '\n'; }
  }
  if (!summary) throw std::runtime_error("Cannot write profile text summary");
  log::info("profile", "Wrote {} sampling ticks over {:.3f}s; JIT host inclusive {:.3f} thread-s, host waits {:.3f} thread-s", s.ticks, s.finished_seconds, compile, blocked);
}
}
void start() {
  start_pipeline_telemetry();
  if (!enabled) return;
  if (!platform::profile_supported()) { log::error("profile", "VENTURE_PROFILE: POSIX signal sampling is not implemented"); return; }
  auto& s = state();
  if (s.started) return;
  if (const auto* hz = venture::environment::getenv("VENTURE_PROFILE_HZ")) {
    unsigned value = 0;
    try { value = std::stoul(hz); } catch (...) {}
    if (!value || value > 10000) throw std::runtime_error("VENTURE_PROFILE_HZ must be in [1, 10000]");
    s.hz = value;
  }
  if (const auto* window = venture::environment::getenv("VENTURE_PROFILE_WINDOW")) {
    const std::string value(window);
    const auto colon = value.find(':');
    std::size_t first = 0, last = 0;
    if (colon == value.npos) throw std::runtime_error("VENTURE_PROFILE_WINDOW must be start:end seconds since process start");
    s.capture_begin = std::stod(value.substr(0, colon), &first);
    s.capture_end = std::stod(value.substr(colon + 1), &last);
    if (first != colon || last != value.size() - colon - 1 || !std::isfinite(s.capture_begin) ||
        !std::isfinite(s.capture_end) || s.capture_begin < 0 || s.capture_end <= s.capture_begin)
      throw std::runtime_error("VENTURE_PROFILE_WINDOW requires finite 0 <= start < end");
  }
  s.processors = platform::host_cpus();
  s.efficiency_known = !s.processors.empty() &&
      std::all_of(s.processors.begin(), s.processors.end(), [](const auto& cpu) { return cpu.efficiency_known; });
  if (!s.processors.empty()) {
    s.min_efficiency = s.max_efficiency = s.processors.front().efficiency_class;
    for (const auto& cpu : s.processors) {
      s.min_efficiency = std::min<unsigned>(s.min_efficiency, cpu.efficiency_class);
      s.max_efficiency = std::max<unsigned>(s.max_efficiency, cpu.efficiency_class);
    }
  }
  s.power = platform::profile_power_throttling();
  s.cycles_per_ns = platform::thread_cycles_per_ns();
  log::info("profile", "Process power throttling available={} control_mask={} state_mask={} error={}; topology {} processors EfficiencyClass {}",
      s.power.available, s.power.control_mask, s.power.state_mask, s.power.error, s.processors.size(),
      s.efficiency_known ? std::format("{}..{}", s.min_efficiency, s.max_efficiency) : std::string("unknown"));
  load_symbols(s); s.waits = platform::profile_wait_symbols();
  if (const auto* ticks = venture::environment::getenv("VENTURE_PROFILE_TICKS"))
    s.tick_capture = std::string_view(ticks) == "1";
  if (s.tick_capture) {
    if (const auto* tid = venture::environment::getenv("VENTURE_PROFILE_STATE_TID")) {
      const std::string_view value(tid);
      const auto parsed = std::from_chars(value.data(), value.data() + value.size(), s.state_tid);
      if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || s.state_tid <= 0)
        throw std::runtime_error("VENTURE_PROFILE_STATE_TID requires a positive guest tid");
    }
  }
  milestone::hook.store(&mark, std::memory_order_release);  // Platform services' startup milestones
  if (const auto* tid = venture::environment::getenv("VENTURE_PROFILE_TID")) {
    std::string_view remaining(tid);
    do {
      const auto comma = remaining.find(',');
      const auto part = remaining.substr(0, comma);
      std::int32_t value = 0;
      const auto parsed = std::from_chars(part.data(), part.data() + part.size(), value);
      if (parsed.ec != std::errc{} || parsed.ptr != part.data() + part.size() || value <= 0)
        throw std::runtime_error("VENTURE_PROFILE_TID requires comma-separated positive guest tids");
      s.focus_tids.push_back(value);
      if (comma == remaining.npos) break;
      remaining.remove_prefix(comma + 1);
    } while (true);
  }
  if (const auto* roles = venture::environment::getenv("VENTURE_PROFILE_ROLES")) {
    std::string_view remaining(roles);
    do {
      const auto comma = remaining.find(',');
      const auto part = remaining.substr(0, comma);
      if (part != "main" && part != "render" && part != "rhi" && part != "xr_submission")
        throw std::runtime_error("VENTURE_PROFILE_ROLES requires main,render,rhi,xr_submission roles");
      s.focus_roles.emplace_back(part);
      if (comma == remaining.npos) break;
      remaining.remove_prefix(comma + 1);
    } while (true);
  }
  // Keep vector growth/copies out of measured cold-compilation callbacks.
  if (compile_timing_enabled) {
    if (const auto value = environment::getenv("VENTURE_PROFILE_COMPILE_LIMIT")) {
      std::size_t limit = 0;
      const std::string_view text(value);
      const auto parsed = std::from_chars(text.data(), text.data() + text.size(), limit);
      if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !limit || limit > 2000000)
        throw std::runtime_error("VENTURE_PROFILE_COMPILE_LIMIT must be between 1 and 2000000");
      s.compile_limit = limit;
    }
    s.compile_records.reserve(s.compile_limit);
  }
  s.started = true; s.milestones.emplace("process_start", 0);
  if (!events_only) {
    s.sampler = std::jthread([&](std::stop_token token) { run(s, token); });
    log::info("profile", "Sampling guest threads at {} Hz ({} focused tids, {} focused roles; empty filters mean all), background pool at 10 samples/sec", s.hz, s.focus_tids.size(), s.focus_roles.size());
  } else log::info("profile", "Event-only capture: no sampler thread or suspended-thread samples");
}
void stop() noexcept {
  if (!enabled) return;
  try {
    auto& s = state();
    // A milestone on a rendering thread may race main's final scope exit.
    std::lock_guard finish(s.finish_mutex);
    if (!s.started || s.stopped) return;
    if (s.sampler.joinable()) { s.sampler.request_stop(); s.sampler.join(); }
    {
      std::lock_guard lock(s.mutex); s.stopped = true;
      Dynarmic::SetCompileTimingCallback(nullptr);
      s.finished_seconds = elapsed(s);
      for (const auto& t : s.history) t->residency_requested.store(false, std::memory_order_release);
    }
    write(s);
    report_pipelines("capture");
    if (invalidation_tracing) report_invalidation("capture");
  } catch (const std::exception& e) { log::error("profile", "Could not write profile: {}", e.what()); }
}
void shutdown_burst() noexcept {
  if (!enabled || !shutdown_trace::enabled) return;
  const auto* text = environment::getenv("VENTURE_SHUTDOWN_BOUND_SAMPLES");
  if (!text) return;
  unsigned count = 0;
  const auto parsed = std::from_chars(text, text + std::char_traits<char>::length(text), count);
  if (parsed.ec != std::errc{} || *parsed.ptr || !count || count > 10) return;
  try {
    auto& s = state();
    std::vector<std::int32_t> tids;
    { std::lock_guard lock(s.mutex); if (!s.stopped) return; tids = s.focus_tids; }
    const auto modules = platform::profile_modules();
    const auto start = Clock::now();
    for (unsigned index = 0; index < count; ++index) {
      std::this_thread::sleep_until(start + std::chrono::milliseconds(100 * index));
      for_each_guest_thread([&](GuestThread& thread) {
        if (tids.empty() || std::ranges::find(tids, thread.tid()) != tids.end())
          thread.request_shutdown_sample();
      });
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      for_each_guest_thread([&](GuestThread& thread) {
        if (!tids.empty() && std::ranges::find(tids, thread.tid()) == tids.end()) return;
        std::shared_ptr<Thread> target;
        { std::lock_guard lock(s.mutex); auto it = s.guests.find(&thread); if (it != s.guests.end()) target = it->second; }
        if (!target) return;
        platform::ProfileSample sample;
        bool valid = false;
        {
          // Same per-CPU lifetime guard and OS sampler as the normal profiler.
          // Never hold the statistics mutex while a target is suspended.
          std::unique_lock pin(target->lifetime, std::try_to_lock);
          if (pin.owns_lock() && target->cpu && target->native.id != platform::profile_thread_id())
            valid = platform::profile_sample(target->native, target->cpu, &target->activity, sample);
        }
        const auto published = shutdown_trace::read(thread);
        if (index == 0 && published.futex && target->futex_handoffs) {
          // Reuse the existing handoff census to identify observed producers of
          // this exact word; do not introduce another futex event recorder.
          std::map<std::pair<std::int32_t, GuestAddr>, unsigned> wakers;
          for (const auto& row : target->futex_handoffs->snapshot())
            if (row.address == published.futex && row.wake_request_ns)
              ++wakers[{row.waker_tid, row.waker_lr}];
          log::info("shutdown-dependency", "guest_tid={} futex={:#x} recorded_wakers={} census_dropped={}",
              thread.tid(), published.futex, wakers.size(), target->futex_handoffs->dropped.load());
          unsigned emitted = 0;
          for (const auto& [waker, calls] : wakers) {
            if (++emitted > 16) { log::info("shutdown-dependency", "TRUNCATED waker groups"); break; }
            log::info("shutdown-dependency", "guest_tid={} futex={:#x} observed_waker_tid={} observed_waker_LR={:#x} calls={}",
                thread.tid(), published.futex, waker.first, waker.second, calls);
          }
        }
        std::string host_frame = "unmapped";
        for (const auto& m : modules) if (sample.ip >= m.begin && sample.ip < m.end) {
          host_frame = std::format("{}+{:#x}", m.name, sample.ip - m.begin); break;
        }
        log::info("shutdown-burst", "index={} guest_tid={} host_tid={} valid={} cpu_ns={} PC={:#x} pc_source={} published_LR={:#x} published_age_ms={} host_frame={}",
            index, thread.tid(), target->native.id, valid, sample.cpu_ns,
            valid && sample.guest_pc ? sample.guest_pc : published.pc,
            valid && sample.guest_pc ? "native-profiler" : "owner-published",
            published.lr, published.sampled_ns ? (monotonic_ns() - published.sampled_ns) / 1000000 : -1, host_frame);
      });
    }
    log::flush();
  } catch (...) { log::info("shutdown-burst", "capture unavailable"); }
}
void attach(GuestThread& thread) {
  if (!enabled || !platform::profile_supported()) return;
  auto& s = state(); auto t = std::make_shared<Thread>();
  t->native = platform::profile_current_thread(); t->cpu = &thread.cpu(); t->guest_tid = thread.tid();
  if (futex_handoff_cpu_enabled) {
    std::array<std::int64_t, 33> probes;
    bool valid = true;
    for (auto& probe : probes) {
      const auto first = futex_cpu_clock(), last = futex_cpu_clock();
      valid &= first >= 0 && last >= first;
      probe = last - first;
    }
    if (valid) {
      std::sort(probes.begin(), probes.end());
      t->cpu_probe_min = probes.front(); t->cpu_probe_median = probes[probes.size() / 2];
    }
  }
  if (futex_handoff_enabled && (handoff_tids.empty() ||
      std::find(handoff_tids.begin(), handoff_tids.end(), thread.tid()) != handoff_tids.end()))
    t->futex_handoffs = std::make_unique<FutexHandoffs<>>(handoff_capacity);
  t->stack_base = thread.stack_base; t->stack_size = thread.stack_size;
  std::lock_guard lock(s.mutex); s.guests.emplace(&thread, t); s.threads[t->native.id] = t; s.history.push_back(t); local = t.get();
  if (s.started && !s.stopped && elapsed(s) >= s.capture_begin && elapsed(s) < s.capture_end) baseline(*t);
}
void detach(GuestThread& thread) {
  if (!enabled || !platform::profile_supported()) return;
  auto& s = state(); std::lock_guard lock(s.mutex);
  if (auto it = s.guests.find(&thread); it != s.guests.end()) {
    // The owning per-thread JIT is being released. Historical samples already
    // hold their guest PC; its ranges must not inflate a later live-cache census.
    if (!s.stopped) std::erase_if(s.translations, [&](const auto& block) { return block.second.host_tid == it->second->native.id; });
    std::lock_guard pin(it->second->lifetime);
    it->second->cpu = nullptr;
    // A joined worker is destroyed on its joining thread. Its detach must not
    // clear that caller's activity pointer, including when already detached.
    if (local == it->second.get()) local = nullptr;
    s.guests.erase(it);
  }
}
void set_thread_name(std::int32_t tid, std::string_view name) {
  if (!enabled) return;
  auto& s = state(); std::lock_guard lock(s.mutex);
  for (const auto& [guest, t] : s.guests) if (t->guest_tid == tid) { t->guest_name = name; break; }
}
void mark(std::string_view name) {
  if (!enabled || !platform::profile_supported()) return;
  auto& s = state(); double seconds = 0; bool inserted = false;
  {
    std::lock_guard lock(s.mutex);
    seconds = elapsed(s); inserted = s.milestones.try_emplace(std::string(name), seconds).second;
  }
  if (inserted) {
    log::info("profile", "Milestone {} at {:.6f}s", name, seconds);
    if (stop_at && name == stop_at) stop();
  }
}
FutexEvent futex_begin(GuestThread& thread) {
  if (!futex_timing_enabled || GuestThread::current() != &thread) return {};
  const auto begin = monotonic_ns();
  auto* record = futex_record();
  if (!record) return {};
  record->counters.add(FutexCounters::waits);
  FutexEvent event{&record->counters, begin, 0};
  // Window settings are immutable after start(), before guest threads run.
  if (futex_handoff_enabled && local->futex_handoffs) {
    const auto& s = state();
    const auto second = (begin - s.monotonic_origin) / 1e9;
    event.handoff = second >= s.capture_begin && second < s.capture_end;
  }
  if (event.handoff) {
    if (futex_handoff_cpu_enabled) event.cpu_begin = futex_cpu_clock();
    event.timing.begin_ns = monotonic_ns();
    event.timing.waiter_lr = thread.cpu().lr();
    event.timing.waiter_activity = local->activity.load(std::memory_order_relaxed);
  }
  return event;
}
void futex_end(const FutexEvent& event, int result) {
  if (!event.counters) return;
  auto& counters = *static_cast<FutexCounters*>(event.counters);
  const auto end = monotonic_ns();
  const auto cpu_end = event.cpu_begin >= 0 ? futex_cpu_clock() : -1;
  counters.complete(std::max<std::int64_t>(0, end - event.begin_ns), event.parks, result);
  counters.bucket_lock(event.lock_kind, event.lock_contended, event.lock_ns);
  if (event.handoff && local && end - event.timing.begin_ns >= static_cast<std::int64_t>(handoff_minimum_ns)) {
    auto row = event.timing;
    if (event.cpu_begin >= 0 && cpu_end >= event.cpu_begin) row.wait_cpu = cpu_end - event.cpu_begin;
    row.end_ns = end; row.parks = event.parks; row.result = result;
    local->futex_handoffs->append(row);
  }
}
FutexEvent scheduling_begin(GuestThread& thread, std::int64_t requested_ns, bool yield) {
  if (!futex_timing_enabled || GuestThread::current() != &thread) return {};
  auto* record = futex_record();
  if (!record) return {};
  FutexEvent event{&record->counters, monotonic_ns(), 0};
  event.sleep_bin = yield ? -3 : FutexCounters::sleep_bin(requested_ns);
  return event;
}
void scheduling_end(const FutexEvent& event) {
  if (!event.counters) return;
  auto& counters = *static_cast<FutexCounters*>(event.counters);
  counters.schedule(event.sleep_bin, std::max<std::int64_t>(0, monotonic_ns() - event.begin_ns));
  counters.bucket_lock(event.lock_kind, event.lock_contended, event.lock_ns);
}
void futex_notifying(FutexEvent& event, std::int64_t requested_ns, std::int64_t locked_ns) {
  auto& row = event.timing;
  row.wake_request_ns = requested_ns;
  row.wake_lock_ns = locked_ns;
  if (local) {
    row.waker_tid = local->guest_tid;
    row.waker_activity = local->activity.load(std::memory_order_relaxed);
    row.waker_lr = local->cpu ? local->cpu->lr() : 0;
  }
  row.notify_begin_ns = monotonic_ns();
}
std::int64_t futex_cpu_clock() {
#ifdef _WIN32
  return platform::thread_cpu_cycles();
#else
  return platform::thread_cpu_time_ns();
#endif
}
std::int64_t futex_wake_cpu_begin() {
  const auto& s = state();
  const auto second = elapsed(s);
  return s.started && !s.stopped && second >= s.capture_begin && second < s.capture_end ? futex_cpu_clock() : -1;
}
void futex_wake(unsigned notifications, sync_census::WakeSource source, std::int64_t request_ns, std::int64_t cpu_units) {
  if (sync_census::enabled)
    sync_census::wake(source, notifications, std::max<std::int64_t>(0, monotonic_ns() - request_ns));
  if (!futex_timing_enabled) return;
  auto* record = futex_record();
  if (!record) return;
  record->counters.add(FutexCounters::wakes);
  record->counters.add(FutexCounters::notifications, notifications);
  if (!notifications) record->counters.add(FutexCounters::empty_wakes);
  if (cpu_units >= 0) {
    record->counters.add(FutexCounters::wake_cpu_units, cpu_units);
    record->counters.add(FutexCounters::wake_cpu_samples);
  }
}
void frame(std::uint64_t index) {
  if (!enabled || !platform::profile_supported()) return;
  auto& s = state(); bool finish = false;
  if (s.stopped.load(std::memory_order_acquire)) return;
  {
    std::lock_guard lock(s.mutex);
    if (s.stopped) return;
    const auto seconds = elapsed(s);
    if (local) local->xr_submit = true;
    if (seconds >= s.capture_begin && seconds <= s.capture_end)
      s.frames.push_back({seconds, index, local ? local->guest_tid : 0});
    if (futex_timing_enabled) for (std::size_t id = 0; id < s.futex_records.size(); ++id) {
      auto& record = *s.futex_records[id];
      auto counts = record.counters.snapshot();
      bool changed = false;
      for (unsigned i = 0; i < counts.size(); ++i) {
        const auto value = counts[i];
        counts[i] -= record.previous[i]; record.previous[i] = value;
        changed |= counts[i] != 0;
      }
      if (changed && seconds >= s.capture_begin && seconds <= s.capture_end)
        s.futex_frames.push_back({seconds, index, id, counts});
    }
    finish = seconds >= s.capture_end;
  }
  // Never join the sampler while holding its statistics mutex.
  if (finish) stop();
}
void record_io(const sync_census::IoSample& sample) noexcept {
  if (!sync_census::enabled) return;
  try {
    auto& s = state();
    const auto seconds = (sample.end_ns - s.monotonic_origin) / 1e9;
    std::lock_guard lock(s.mutex);
    if (!s.started || s.stopped || seconds < s.capture_begin || seconds >= s.capture_end) return;
    // Bounded even when a title unexpectedly polls every millisecond on hundreds
    // of workers. Dropped samples make the census explicitly incomplete.
    if (s.io_samples.size() >= 500000) { ++s.io_dropped; return; }
    s.io_samples.push_back(sample);
  } catch (...) {
    state().io_dropped.fetch_add(1, std::memory_order_relaxed);
  }
}
void register_compile_timing(const Dynarmic::CompileTiming& timing) noexcept {
  if (!compile_timing_enabled || !local) return;
  auto& s = state();
  if (s.stopped.load(std::memory_order_acquire)) return;
  std::lock_guard lock(s.mutex);
  if (s.stopped) return;
  const auto seconds = (std::int64_t(timing.end_ns) - s.monotonic_origin) / 1e9;
  if (seconds < s.capture_begin || seconds > s.capture_end) return;
  if (s.compile_records.size() >= s.compile_limit) { ++s.compile_dropped; return; }
  try { s.compile_records.push_back({timing, local->guest_tid}); }
  catch (...) { ++s.compile_dropped; }
}
void register_translation(const void* begin, const void* end, std::string_view name) {
  auto& s = state();
  if (name == "cache_clear") {
    if (s.stopped.load(std::memory_order_acquire)) return;
    // A full per-thread code cache is wiped and its blocks are recompiled on demand.
    std::uint64_t clears = 0; std::int32_t tid = 0; double seconds = 0;
    {
      std::lock_guard lock(s.mutex);
      if (s.stopped || !local) return;
      clears = ++local->cache_clears; tid = local->guest_tid; seconds = elapsed(s);
      local->cache_clear_seconds.push_back(seconds);
      std::erase_if(s.translations, [&](const auto& block) { return block.second.host_tid == local->native.id; });
    }
    log::info("profile", "JIT code cache cleared on guest tid {} at {:.3f}s (clear {} on this thread)", tid, seconds, clears);
    return;
  }
  // a64_<16 hex PC>_fpcr..., a32_<t|a><8 hex PC>_... ; prelude helpers have descriptive names.
  const bool a64 = name.starts_with("a64_");
  const std::size_t digits = a64 ? 16 : 8, offset = a64 ? 4 : 5;
  if ((!a64 && !name.starts_with("a32_")) || name.size() < offset + digits) return;
  GuestAddr pc = 0;
  const auto parsed = std::from_chars(name.data() + offset, name.data() + offset + digits, pc, 16);
  if (parsed.ec != std::errc{} || parsed.ptr != name.data() + offset + digits) return;
  const auto first = reinterpret_cast<std::uintptr_t>(begin), last = reinterpret_cast<std::uintptr_t>(end);
  if (first >= last) return;
  if (local) ++local->emitted_blocks;
  // A stopped capture may be serializing a large report under this mutex.
  // Guest JITs (including audio callbacks) must keep running independently.
  if (s.stopped.load(std::memory_order_acquire)) return;
  std::lock_guard lock(s.mutex);
  if (s.stopped) return;
  // Code-cache reuse must retire overlapping old mappings. Historical samples
  // already contain the guest PC, so invalidation cannot relabel old samples.
  auto it = s.translations.lower_bound(first);
  if (it != s.translations.begin() && std::prev(it)->second.end > first) --it;
  while (it != s.translations.end() && it->first < last) it = s.translations.erase(it);
  s.translations.emplace(first, State::Translation{last, pc, local ? local->native.id : 0}); ++s.emitted_blocks;
}
std::int64_t thread_emitted_blocks() noexcept {
  return local ? std::int64_t(local->emitted_blocks) : -1;
}

std::uint32_t exchange_activity(std::uint32_t value) noexcept {
  if (!local) return pipeline_exchange_activity(value);
  if (local->residency_requested.load(std::memory_order_acquire) &&
      local->residency_requested.exchange(false, std::memory_order_acquire)) {
    const auto& s = state();
    const auto processor = platform::current_cpu(); // GetCurrentProcessorNumberEx on Windows
    unsigned bucket = 3;
    for (const auto& cpu : s.processors) if (s.efficiency_known && static_cast<int>(cpu.index) == processor) {
      bucket = s.min_efficiency == s.max_efficiency ? 2 : cpu.efficiency_class == s.max_efficiency ? 0 : 1;
      break;
    }
    local->residency[bucket].fetch_add(1, std::memory_order_relaxed);
  }
  // Only the owning guest thread writes its activity. Sampling only reads it,
  // so a locked read/modify/write instruction would add needless HLE overhead.
  const auto previous = local->activity.load(std::memory_order_relaxed);
  local->activity.store(value, std::memory_order_relaxed);
  if (previous != value) local->activity_epoch.store(local->activity_epoch.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
  return previous;
}
} // namespace venture::profile

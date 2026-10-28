#include "core/shutdown_trace.h"
#include "core/mutex_watch.h"
#include <algorithm>
#include <climits>
#include "core/thread_runtime.h"
#include "core/thread_api.h"
#include "core/hle_runtime.h"
#include "core/wait.h"
#include "core/diagnostic_trace.h"
#include "core/linker.h"
#include "common/environment.h"
#include "common/log.h"
#include "platform/memory.h"
#include "platform/runtime_child.h"
#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <limits>
#include <map>
#include <string_view>
#include <thread>

namespace venture::shutdown_trace {
const bool enabled = [] {
  const char* flag = environment::getenv("VENTURE_SHUTDOWN_SNAPSHOTS");
  const char* menu = environment::getenv("VENTURE_MENU_QUIT");
  return (flag && std::string_view(flag) == "1") || (menu && std::string_view(menu) == "1");
}();
const bool word_watch_enabled = enabled && environment::getenv("VENTURE_SHUTDOWN_WATCH");
const bool mutex_watch_enabled = enabled && environment::getenv("VENTURE_MUTEX_WATCH_CALLER");
namespace {
const unsigned stack_words = [] {
  const char* flag = environment::getenv("VENTURE_SHUTDOWN_STACK_WORDS");
  unsigned count{};
  if (flag) {
    const std::string_view value(flag);
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), count);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) return 0u;
  }
  return std::min(count, 128u);
}();
std::atomic<const char*> current_phase{"running"};
std::mutex sources_mutex;
std::vector<void (*)(const char*)> snapshot_sources;
std::atomic<GuestAddr> watched_address{}, watched_value{};
DiagnosticBudget word_output_budget;
void watch_transition(GuestThread& thread, std::uint32_t id) noexcept {
  const auto address = watched_address.load(std::memory_order_relaxed);
  if (!address) return;
  GuestAddr value{};
  if (!platform::read_memory(address, &value, sizeof(value))) return;
  const auto previous = watched_value.exchange(value, std::memory_order_relaxed);
  if (previous == value || !thread.word_watch_records) return;
  WordTransitionEvent event{monotonic_ns(), previous, value, thread.cpu().pc(), thread.cpu().lr(), thread.tid(), id};
  auto fp = thread.cpu().isa() == GuestIsa::a64 ? thread.cpu().x(29) : 0;
  for (unsigned depth = 0; depth < event.callers.size() && fp && !(fp & 15); ++depth) {
    GuestAddr frame[2]{};
    if (!platform::read_memory(fp, frame, sizeof(frame))) break;
    event.callers[depth] = frame[1];
    if (frame[0] <= fp || frame[0]-fp > 1024*1024) break;
    fp = frame[0];
  }
  thread.word_watch_records->append(event);
}
void snapshot_word_transitions() {
  if (!word_watch_enabled) return;
  static std::mutex consumer;
  std::unique_lock consume(consumer, std::try_to_lock);
  if (!consume.owns_lock()) return;
  std::vector<WordTransitionEvent> rows;
  std::uint64_t dropped = 0;
  for_each_guest_thread([&](GuestThread& thread) {
    if (!thread.word_watch_records) return;
    auto& records = *thread.word_watch_records;
    const auto prefix = records.snapshot();
    rows.insert(rows.end(), prefix.begin()+records.emitted, prefix.end());
    records.emitted = prefix.size();
    dropped += records.dropped.load();
  });
  std::ranges::sort(rows, {}, &WordTransitionEvent::ns);
  for (const auto& row : rows) {
    if (!word_output_budget.take()) continue;
    const auto& hle = hle::function(row.hle);
    log::info("shutdown-watch", "transition ns={} old={:#x} value={:#x} guest_tid={} next_hle={}!{} PC={:#x} LR={:#x}",
              row.ns, row.previous, row.value, row.tid, hle.library, hle.name, row.pc, row.lr);
    for (unsigned depth = 0; depth < row.callers.size() && row.callers[depth]; ++depth)
      log::info("shutdown-watch", "transition ns={} guest_tid={} frame={} return_pc={:#x}", row.ns, row.tid, depth, row.callers[depth]);
  }
  if (word_output_budget.take()) log::info("shutdown-watch", "records_dropped={}", dropped);
  if (word_output_budget.claim_exhaustion()) log::warn("shutdown-watch", "TRUNCATED output event budget exhausted");
}

struct Monitor { std::atomic<bool> started{false}; std::jthread worker; };
Monitor& monitor() { static auto* value = new Monitor; return *value; }
const char* wait_name(Wait value) {
  switch (value) {
    case Wait::futex: return "futex";
    case Wait::sleep: return "sleep";
    case Wait::condition: return "condition";
    case Wait::join: return "join";
    default: return "none";
  }
}
}
void publish(GuestThread& thread) {
  if (!enabled) return;
  const auto pc = thread.cpu().pc(), lr = thread.cpu().lr();
  const auto fp = thread.cpu().isa() == GuestIsa::a64 ? thread.cpu().x(29) : 0;
  std::lock_guard lock(thread.shutdown_trace.mutex);
  auto& r = thread.shutdown_trace.record;
  r.pc = pc; r.lr = lr; r.fp = fp; r.sampled_ns = monotonic_ns();
  if (stack_words) {
    if (thread.cpu().isa() == GuestIsa::a64) {
      r.sp = thread.cpu().sp();
      for (unsigned i = 0; i < r.saved.size(); ++i) r.saved[i] = thread.cpu().x(19 + i);
    }
  }
}
Record read(GuestThread& thread) {
  std::lock_guard lock(thread.shutdown_trace.mutex);
  return thread.shutdown_trace.record;
}
HleScope::HleScope(GuestThread& thread, std::uint32_t id) : thread_(thread) {
  if (!enabled) return;
  watch_transition(thread, id);
  publish(thread);
  std::lock_guard lock(thread.shutdown_trace.mutex);
  auto& r = thread.shutdown_trace.record;
  previous_ = r.last_hle; nested_ = r.entered;
  r.last_hle = id; r.entered = true;
}
HleScope::~HleScope() {
  if (!enabled) return;
  publish(thread_);
  std::lock_guard lock(thread_.shutdown_trace.mutex);
  auto& r = thread_.shutdown_trace.record;
  if (nested_) r.last_hle = previous_;
  r.entered = nested_;
}
WaitScope::WaitScope(GuestThread& thread, Wait kind, GuestAddr object, GuestAddr mutex, std::int32_t target) : thread_(thread) {
  if (!enabled) return;
  publish(thread);
  std::lock_guard lock(thread.shutdown_trace.mutex);
  auto& r = thread.shutdown_trace.record;
  previous_ = r;
  r.wait = kind; r.object = object; r.associated_mutex = mutex; r.target_tid = target;
}
WaitScope::~WaitScope() {
  if (!enabled) return;
  std::lock_guard lock(thread_.shutdown_trace.mutex);
  auto& r = thread_.shutdown_trace.record;
  r.wait = previous_.wait; r.object = previous_.object;
  r.associated_mutex = previous_.associated_mutex; r.target_tid = previous_.target_tid;
}
FutexScope::FutexScope(GuestThread& thread, GuestAddr address, std::uint32_t expected, bool sleeping) : thread_(thread) {
  if (!enabled) return;
  publish(thread);
  std::lock_guard lock(thread.shutdown_trace.mutex);
  auto& r = thread.shutdown_trace.record;
  previous_ = r;
  r.futex = address; r.expected = expected;
  if (r.wait == Wait::none) r.wait = sleeping ? Wait::sleep : Wait::futex;
}
FutexScope::~FutexScope() {
  if (!enabled) return;
  std::lock_guard lock(thread_.shutdown_trace.mutex);
  auto& r = thread_.shutdown_trace.record;
  r.futex = previous_.futex; r.expected = previous_.expected; r.wait = previous_.wait;
}
void phase(const char* name) {
  if (!enabled) return;
  current_phase.store(name);
  log::info("shutdown", "phase={}", name);
  sample_watch(name);
}
std::optional<WatchLocation> parse_watch_location(std::string_view text) {
  const auto split = text.find('+');
  if (split == std::string_view::npos || split == 0) return {};
  const auto module = text.substr(0, split);
  if (module.find_first_of("/\\ \t\r\n") != std::string_view::npos) return {};
  auto number = text.substr(split + 1);
  int base = 10;
  if (number.starts_with("0x")) { number.remove_prefix(2); base = 16; }
  GuestAddr offset{};
  const auto [end, error] = std::from_chars(number.data(), number.data() + number.size(), offset, base);
  if (error != std::errc{} || end != number.data() + number.size() || offset % sizeof(GuestAddr)) return {};
  return WatchLocation{std::string(module), offset};
}
void sample_watch(std::string_view step) noexcept {
  if (!enabled || std::string_view(current_phase.load()) == "running") return;
  try {
    static const auto watch = [] {
      const auto* text = environment::getenv("VENTURE_SHUTDOWN_WATCH");
      return parse_watch_location(text ? text : "");
    }();
    if (!watch || !word_output_budget.take()) return;
    GuestAddr address{};
    const bool available = Linker::get().try_for_each_image([&](const Library& image) {
      if (image.soname() != watch->module || image.is_hle()) return;
      if (watch->offset > std::numeric_limits<GuestAddr>::max() - image.load_bias) return;
      const auto candidate = image.load_bias + watch->offset;
      if (candidate >= image.map_start && candidate - image.map_start < image.map_size &&
          image.map_size - (candidate - image.map_start) >= sizeof(GuestAddr)) address = candidate;
    });
    GuestAddr value{};
    const bool readable = available && address && platform::read_memory(address, &value, sizeof(value));
    if (readable) {
      watched_value.store(value, std::memory_order_relaxed);
      watched_address.store(address, std::memory_order_relaxed);
    }
    log::info("shutdown-watch", "step={} module={} offset={:#x} value={:#x} readable={} loader_available={}",
              step, watch->module, watch->offset, value, readable, available);
  } catch (...) { /* Diagnostic failure must not change guest execution. */ }
}
void trace_destructor(std::string_view operation, GuestAddr callback, GuestAddr dso) noexcept {
  if (!enabled) return;
  try {
    const auto* text = environment::getenv("VENTURE_SHUTDOWN_WATCH");
    const auto watch = parse_watch_location(text ? text : "");
    if (!watch) return;
    GuestAddr bias{};
    bool selected = false;
    Linker::get().try_for_each_image([&](const Library& image) {
      if (image.soname() == watch->module && callback >= image.map_start &&
          callback - image.map_start < image.map_size) { selected = true; bias = image.load_bias; }
    });
    if (!selected || !word_output_budget.take()) return;
    auto* thread = GuestThread::current();
    log::info("shutdown-handler", "operation={} callback={}+{:#x} dso={:#x} guest_tid={} caller_lr={:#x}",
              operation, watch->module, callback - bias, dso, thread ? thread->tid() : 0,
              thread ? thread->cpu().lr() : 0);
    sample_watch(operation);
  } catch (...) { /* Optional diagnostics never control finalization. */ }
}
namespace {
struct MutexWatch {
  std::atomic<GuestAddr> caller{0}, selected{0};
  DiagnosticBudget capture_budget, output_budget;
  std::mutex consumer;
  std::optional<WatchLocation> location;
  MutexWatch() {
    const std::string_view text = environment::getenv("VENTURE_MUTEX_WATCH_CALLER");
    if (!text.starts_with("0x")) location = parse_watch_location(text);
    else {
      GuestAddr value{};
      const auto [end, error] = std::from_chars(text.data()+2, text.data()+text.size(), value, 16);
      if (error == std::errc{} && end == text.data()+text.size() && value && !(value%4))
        caller.store(value);
    }
  }
};
// Provision before any guest call, not on the first mutex operation.
const auto mutex_watch_state = mutex_watch_enabled ? std::make_unique<MutexWatch>() : nullptr;
void emit_mutex_event(const MutexWatchEvent& event) {
  log::info("mutex-watch", "ns={} mutex={:#x} tid={} operation={} result={} state={:#x} owner={} depth={} readable={}",
    event.ns, event.address, event.tid, event.operation, event.result,
    event.words[0], event.words[1], event.words[2], event.readable);
  for (std::size_t i = 0; i < event.callers.size() && event.callers[i]; ++i)
    log::info("mutex-watch", "ns={} tid={} caller={} return_pc={:#x}", event.ns, event.tid, i, event.callers[i]);
}
}
void mutex_watch_image(std::string_view module, GuestAddr bias, GuestAddr begin, std::uint64_t size) noexcept {
  if (!mutex_watch_enabled) return;
  auto& watch = *mutex_watch_state;
  if (watch.caller.load() || !watch.location || module != watch.location->module) return;
  const auto offset = watch.location->offset;
  if (offset > std::numeric_limits<GuestAddr>::max()-bias) return;
  const auto address = bias+offset;
  if (address >= begin && address-begin < size) watch.caller.store(address);
}
void trace_mutex_event(void* mutex, const char* operation, std::int32_t result) noexcept {
  if (!mutex_watch_enabled || !mutex) return;
  auto* thread = GuestThread::current();
  if (!thread || thread->cpu().isa() != GuestIsa::a64 || !thread->mutex_watch_records) return;
  auto& watch = *mutex_watch_state;
  const auto address = h2g(mutex), selected = watch.selected.load(std::memory_order_relaxed);
  if (selected && selected != address) return;
  if (!watch.capture_budget.take()) return;
  MutexWatchEvent event{monotonic_ns(), address, thread->tid(), result, operation};
  event.readable = platform::read_memory(address, event.words.data(), sizeof(event.words));
  event.callers[0] = thread->cpu().lr();
  auto fp = thread->cpu().x(29);
  for (std::size_t i = 1; i < event.callers.size() && fp && !(fp & 15); ++i) {
    GuestAddr frame[2]{};
    if (!platform::read_memory(fp, frame, sizeof(frame))) break;
    event.callers[i] = frame[1];
    if (frame[0] <= fp || frame[0]-fp > 1024*1024) break;
    fp = frame[0];
  }
  thread->mutex_watch_records->append(event);
  const auto caller = watch.caller.load(std::memory_order_relaxed);
  if (!selected && caller && std::find(event.callers.begin(), event.callers.end(), caller) != event.callers.end()) {
    GuestAddr expected = 0;
    watch.selected.compare_exchange_strong(expected, address);
  }
}
void snapshot_mutex_watch(const char* label) noexcept {
  if (!mutex_watch_enabled) return;
  try {
    auto& watch = *mutex_watch_state;
    std::unique_lock consume(watch.consumer, std::try_to_lock);
    if (!consume.owns_lock()) return;
    const auto selected = watch.selected.load();
    std::vector<MutexWatchEvent> rows;
    std::uint64_t dropped = 0;
    // Copy under registry lifetime protection; never log while holding it.
    for_each_guest_thread([&](GuestThread& thread) {
      if (!thread.mutex_watch_records) return;
      auto& records = *thread.mutex_watch_records;
      const auto prefix = records.snapshot();
      if (selected) {
        for (auto i = records.emitted; i < prefix.size(); ++i)
          if (prefix[i].address == selected) rows.push_back(prefix[i]);
        records.emitted = prefix.size();
      }
      dropped += records.dropped.load();
    });
    std::ranges::sort(rows, {}, &MutexWatchEvent::ns);
    for (const auto& row : rows) if (watch.output_budget.take()) emit_mutex_event(row);
    if (watch.output_budget.claim_exhaustion()) log::warn("mutex-watch", "TRUNCATED output event budget exhausted");
    if (watch.output_budget.take()) {
      MutexWatchEvent event{monotonic_ns(), selected, 0, 0, label};
      event.readable = selected && platform::read_memory(selected, event.words.data(), sizeof(event.words));
      log::info("mutex-watch", "snapshot={} selected={:#x} readable={} state={:#x} owner={} depth={} records_dropped={} capture_budget_exhausted={}",
          label, selected, event.readable, event.words[0], event.words[1], event.words[2], dropped,
          watch.capture_budget.used.load() > DiagnosticBudget::limit);
    }
  } catch (...) {}
}
void add_snapshot_source(void (*source)(const char*)) {
  if (!enabled) return;
  std::lock_guard lock(sources_mutex);
  if (std::ranges::find(snapshot_sources, source) == snapshot_sources.end())
    snapshot_sources.push_back(source);
}
void snapshot(const char* label) {
  if (!enabled) return;
  // Halt only the CPU to refresh owner-published registers; never interrupt a
  // blocking HLE wait or deliver a guest signal for a diagnostic snapshot.
  for_each_guest_thread([](GuestThread& thread) { thread.request_shutdown_sample(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  std::map<std::int32_t, std::string> names;
  for (auto& [tid, name] : threads::list()) names.emplace(tid, name);
  log::info("shutdown", "snapshot={} phase={}", label, current_phase.load());
  snapshot_mutex_watch(label);
  snapshot_word_transitions();
  const bool images_available = Linker::get().try_for_each_image([](const Library& image) {
    log::info("shutdown", "image={} bias={:#x} start={:#x} size={:#x}", image.soname(), image.load_bias, image.map_start, image.map_size);
  });
  if (!images_available) log::info("shutdown", "images unavailable: loader lock busy");
  for_each_guest_thread([&](GuestThread& thread) {
    const auto r = read(thread);
    std::string hle = "none";
    if (r.last_hle) { const auto& f = hle::function(r.last_hle); hle = f.library + "!" + f.name; }
    std::uint32_t word{};
    const bool readable = r.futex && platform::read_memory(r.futex, &word, sizeof(word));
    log::info("shutdown", "tid={} name=\"{}\" owner_index={} PC={:#x} LR={:#x} sample_age_ms={} last_hle={} entered={} wait={} object={:#x} mutex={:#x} target_tid={} futex={:#x} expected={:#x} value={:#x} readable={}",
      thread.tid(), names[thread.tid()], thread.mutex_owner_index(), r.pc, r.lr,
      r.sampled_ns ? (monotonic_ns() - r.sampled_ns) / 1000000 : -1,
      hle, r.entered, wait_name(r.wait), r.object, r.associated_mutex, r.target_tid, r.futex, r.expected, word, readable);
    log::info("shutdown", "tid={} pending_signals={:#x} blocked_signals={:#x}",
      thread.tid(), thread.pending_signals.load(), thread.signal_mask_snapshot.load());
    if (stack_words && r.sp) {
      const auto& x = r.saved;
      log::info("shutdown", "tid={} SP={:#x} x19={:#x} x20={:#x} x21={:#x} x22={:#x} x23={:#x} x24={:#x} x25={:#x} x26={:#x} x27={:#x} x28={:#x}",
        thread.tid(), r.sp, x[0], x[1], x[2], x[3], x[4], x[5], x[6], x[7], x[8], x[9]);
      // Frameless guest code can hide intermediate callers from the FP chain.
      // Retain bounded raw words for offline ELF analysis, without interpreting
      // arbitrary values as return addresses or changing guest execution.
      for (unsigned offset = 0; offset < stack_words; offset += 8) {
        GuestAddr words[8]{};
        if (!platform::read_memory(r.sp + offset * sizeof(GuestAddr), words, sizeof(words))) break;
        log::info("shutdown", "tid={} stack_word={} [{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x}]",
          thread.tid(), offset, words[0], words[1], words[2], words[3], words[4], words[5], words[6], words[7]);
      }
    }
    auto fp = r.fp;
    for (unsigned depth = 0; depth < 12 && fp && !(fp & 15); ++depth) {
      GuestAddr frame[2]{};
      if (!platform::read_memory(fp, frame, sizeof(frame))) break;
      log::info("shutdown", "tid={} frame={} return_pc={:#x}", thread.tid(), depth, frame[1]);
      if (frame[0] <= fp || frame[0] - fp > 1024 * 1024) break;
      fp = frame[0];
    }
    const bool mutex_call = r.last_hle && hle::function(r.last_hle).library == "libc.so" &&
      hle::function(r.last_hle).name.starts_with("pthread_mutex_");
    const auto mutex = r.associated_mutex ? r.associated_mutex : (mutex_call ? r.futex : 0);
    if (mutex) {
      std::uint32_t data[3]{};
      const bool valid = platform::read_memory(mutex, data, sizeof(data));
      log::info("shutdown", "tid={} wait_mutex={:#x} state={:#x} owner_word={} auxiliary_word={} readable={}",
        thread.tid(), mutex, data[0], data[1], data[2], valid);
    }
  });
  std::vector<void (*)(const char*)> sources;
  { std::lock_guard lock(sources_mutex); sources = snapshot_sources; }
  for (auto source : sources) {
    try { source(label); } catch (...) { log::info("shutdown", "optional snapshot source unavailable"); }
  }
  log::flush();
  flush_diagnostic_trace();
}
void begin() {
  if (!enabled || monitor().started.exchange(true)) return;
  const auto requested = std::chrono::steady_clock::now();
  snapshot("request");
  monitor().worker = std::jthread([requested](std::stop_token token) {
    for (const int second : {1, 3}) {
      const auto target = requested + std::chrono::seconds(second);
      while (!token.stop_requested() && std::chrono::steady_clock::now() < target)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      if (token.stop_requested()) return;
      snapshot(second == 1 ? "+1s" : "+3s");
      // A worker may end at its own backstop before the supervisor times out.
      // Capture its host waits while it is still alive when explicitly asked.
      // The existing supervisor helper confines the dump and bounds capture.
      const char* native = environment::getenv("VENTURE_SHUTDOWN_NATIVE_SNAPSHOT");
      if (second == 1 && native && std::string_view(native) == "1")
        platform::request_runtime_stall_dump();
    }
  });
}
void stop() {
  if (!enabled || !monitor().worker.joinable()) return;
  monitor().worker.request_stop(); monitor().worker.join();
}
}

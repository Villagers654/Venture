#include "common/environment.h"
#include "core/hle_runtime.h"
#include "core/address_space.h"
#include "core/linker.h"
#include "core/process.h"
#include "core/profile.h"
#include "core/dev_game_control.h"
#include "core/signals.h"
#include "core/thread.h"
#include "core/hle_unwind.h"
#include "common/log.h"
#include "common/json.h"
#include "common/paths.h"
#include "platform/memory.h"
#include "platform/thread_priority.h"
#include "platform/profile.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <cmath>
#include <tuple>

namespace venture::hle {
namespace { const auto* const census_path = venture::environment::getenv("VENTURE_HLE_CENSUS"); }
const bool census_enabled = census_path && *census_path;
namespace {
// Immutable entries never move or disappear. A release/acquire slot publication makes dispatch lock-free.
constexpr std::size_t capacity = 1 << 20;
struct Registry {
  std::mutex mutex;
  std::array<std::atomic<FunctionInfo*>, capacity> functions{};
  std::uint32_t next = 1;
  std::atomic<std::uint64_t> generation{0};
  GuestAddr page = 0;
  std::uint64_t used = 4096;
  std::map<std::pair<std::string, std::string>, GuestAddr> missing;
};
Registry& registry() { static auto* r = new Registry; return *r; }
using CensusClock = std::chrono::steady_clock;
struct HitCount {
  std::atomic<std::uint64_t> hits{0};
  std::atomic<std::int64_t> first_ns{-1};
};
// Only an explicitly timed census substitutes wrappers at registration.
// Ordinary dispatch, handler pointers and guest stubs remain unchanged.
struct OriginalHandler { Handler handler{}; TryHandler fast{}; bool guest_end{}; };
thread_local unsigned timing_end_depth = 0;
thread_local bool timing_end_returned = false;
struct ApiTiming {
  std::uint64_t calls{}, exceptions{}, wall_ns{}, max_wall_ns{}, cpu_ns{}, cpu_samples{}, cycles{}, cycle_samples{};
  std::int64_t first_ns{}, last_ns{};
};
const bool timing_requested = [] {
  const auto* value = environment::getenv("VENTURE_HLE_TIMING");
  return value && std::string_view(value) == "1";
}();
bool timed_api(std::string_view library, std::string_view name) {
  if (library == "libopenxr_loader.so") return true;
  if (library == "libc.so") return name == "sched_yield" || name == "nanosleep" ||
      name == "clock_nanosleep" || name == "clock_gettime" || name == "gettimeofday";
  return library == "libvulkan.so" &&
      (name.starts_with("vkQueueSubmit") || name == "vkQueueWaitIdle" || name == "vkDeviceWaitIdle" ||
       name == "vkWaitForFences" || name == "vkGetFenceStatus" || name.starts_with("vkWaitSemaphores") ||
       name.starts_with("vkGetSemaphoreCounterValue") || name == "vkResetFences");
}
struct Census {
  std::filesystem::path output = std::filesystem::absolute(paths::from_utf8(census_path));
  CensusClock::time_point started = CensusClock::now();
  std::unique_ptr<HitCount[]> counts = std::make_unique<HitCount[]>(capacity);
  std::atomic<std::size_t> thread_peak{0};
  std::mutex mutex;
  Json mmaps = Json::array();
  std::unique_ptr<OriginalHandler[]> originals;
  std::map<std::tuple<std::int64_t, std::uint32_t, FunctionId, unsigned>, ApiTiming> api_timings;
  std::uint64_t timing_dropped{};
  double timing_begin = 0, timing_end = 0, cycles_per_ns = 0;
  std::int64_t timing_begin_ns = 0, timing_end_ns = 0;
  Census() {
    if (!timing_requested) return;
    const auto* text = environment::getenv("VENTURE_HLE_TIMING_WINDOW");
    if (!text) throw std::invalid_argument("VENTURE_HLE_TIMING requires VENTURE_HLE_TIMING_WINDOW=begin:end seconds since census initialization");
    char* separator = nullptr; timing_begin = std::strtod(text, &separator);
    if (!separator || separator == text || *separator != ':') throw std::invalid_argument("invalid HLE timing window");
    char* end = nullptr; timing_end = std::strtod(separator + 1, &end);
    if (*end || !std::isfinite(timing_begin) || !std::isfinite(timing_end) || timing_begin < 0 || timing_end <= timing_begin || timing_end > 9e9)
      throw std::invalid_argument("invalid HLE timing window");
    timing_begin_ns = static_cast<std::int64_t>(timing_begin * 1e9);
    timing_end_ns = static_cast<std::int64_t>(timing_end * 1e9);
    originals = std::make_unique<OriginalHandler[]>(capacity);
    cycles_per_ns = platform::thread_cycles_per_ns();
  }
};
Census& census() {
  // Like the registry, retained until process exit: adopted host threads may
  // outlive main. Registration establishes time zero before any guest dispatch.
  static auto* c = [] {
    auto* value = new Census;
    std::atexit(write_census);
    return value;
  }();
  return *c;
}
std::int64_t census_elapsed_ns(const Census& c) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(CensusClock::now() - c.started).count();
}
struct TimedCall {
  Census& c = census();
  FunctionId id;
  std::int64_t begin = census_elapsed_ns(c), cpu = -1, cycles = -1;
  bool active, guest_end, completed = false;
  unsigned scope;
  // Integer-only: an InJit integer callback must not alter guest FP flags.
  explicit TimedCall(FunctionId value) : id(value), active(begin >= c.timing_begin_ns && begin < c.timing_end_ns),
      guest_end(c.originals[id].guest_end), scope(guest_end ? 3 : timing_end_depth ? 2 : timing_end_returned ? 1 : 0) {
    if (guest_end) ++timing_end_depth;
    if (active) { cpu = platform::thread_cpu_time_ns(); cycles = platform::thread_cpu_cycles(); }
  }
  ~TimedCall() { if (guest_end) { --timing_end_depth; timing_end_returned |= completed; } }
  void finish(bool exception = false) noexcept {
    completed = true;
    if (!active) return;
    try {
      const auto end_cycles = platform::thread_cpu_cycles(), end_cpu = platform::thread_cpu_time_ns();
      const auto end = census_elapsed_ns(c);
      if (end > c.timing_end_ns) return;
      std::lock_guard lock(c.mutex);
      const auto key = std::tuple{begin / 1'000'000'000, platform::profile_thread_id(), id, scope};
      auto it = c.api_timings.find(key);
      if (it == c.api_timings.end()) {
        if (c.api_timings.size() >= 65536) { ++c.timing_dropped; return; }
        it = c.api_timings.emplace(key, ApiTiming{}).first;
      }
      auto& row = it->second;
      row.first_ns = row.calls ? std::min(row.first_ns, begin) : begin;
      ++row.calls; row.exceptions += exception; row.last_ns = std::max(row.last_ns, end);
      const auto wall = static_cast<std::uint64_t>(end - begin);
      row.wall_ns += wall; row.max_wall_ns = std::max(row.max_wall_ns, wall);
      if (cpu >= 0 && end_cpu >= cpu) { row.cpu_ns += end_cpu - cpu; ++row.cpu_samples; }
      if (cycles >= 0 && end_cycles >= cycles) { row.cycles += end_cycles - cycles; ++row.cycle_samples; }
    } catch (...) {}
  }
};
void timed_handler(CallContext& ctx) {
  TimedCall timing(ctx.info().id);
  try { timing.c.originals[timing.id].handler(ctx); }
  catch (...) { timing.finish(true); throw; }
  timing.finish();
}
bool timed_fast_handler(CallContext& ctx) {
  TimedCall timing(ctx.info().id);
  bool completed;
  try { completed = timing.c.originals[timing.id].fast(ctx); }
  catch (...) { timing.finish(true); throw; }
  if (completed) timing.finish(); // declined fast cases replay through the normal handler
  return completed;
}
void census_hit(FunctionId id) {
  auto& c = census();
  auto& count = c.counts[id];
  if (count.hits.fetch_add(1, std::memory_order_relaxed) == 0)
    count.first_ns.store(census_elapsed_ns(c), std::memory_order_release);
}
GuestAddr emit(Registry& r, std::span<const std::uint32_t> code) {
  auto bytes = code.size_bytes();
  if (bytes > 4096) throw std::runtime_error("HLE stub exceeds page size");
  if (r.used + bytes > 4096) {
    r.page = AddressSpace::get().allocate(4096, kProtRead | kProtWrite | kProtExec, "[hle thunks]");
    if (!r.page) throw std::bad_alloc();
    // Reserve a readable ELF header at the module base for unwind discovery.
    r.used = 256;
  }
  auto p = r.page + r.used; r.used += align_up(bytes, 16);
  std::memcpy(g2h(p), code.data(), bytes);
  cpu_backend().invalidate(p, bytes, InvalidationCause::code_patch);
  return p;
}
void unimplemented(CallContext& ctx) {
  auto& logged = *static_cast<std::atomic<bool>*>(ctx.user());
  if (!logged.exchange(true)) {
    DlInfo caller;
    auto pc = ctx.cpu().lr();
    Linker::get().try_address_info(pc, &caller);
    log::error("hle", "unimplemented {}!{} called from {}+{:#x} ({}+{:#x})", ctx.info().library, ctx.info().name,
               caller.path.empty() ? "unknown" : caller.path, pc - caller.base,
               caller.symbol, pc - (caller.symbol_address ? caller.symbol_address : caller.base));
  }
  signals::deliver_synchronous(ctx.thread(), {signals::kSigSys, 1, ctx.cpu().pc() - ctx.cpu().trap_instruction_size()});
}
std::uint32_t svc_for(Execution execution) {
  switch (execution) {
    case Execution::RunLoop: return kSvcHleCall;
    case Execution::InJit: return kSvcHleCallFast;
    case Execution::InJitFp: return kSvcHleCallFastFp;
    case Execution::InJitGuestFp:
      return process_config().isa() == GuestIsa::a64 ? kSvcHleCallFastGuestFp : kSvcHleCallFastFp;
  }
  return kSvcHleCall;
}
FunctionId add(Registry& r, std::string_view library, std::string_view name, Handler handler, void* user,
               Execution execution = Execution::RunLoop, TryHandler try_handler = nullptr) {
  if (!handler || r.next == capacity) throw std::runtime_error("invalid or exhausted HLE registration");
  auto id = r.next;
  if (census_enabled) (void)census();
  const auto svc = svc_for(execution);
  const bool condition_wait = library == "libc.so" && name.starts_with("pthread_cond_") && name.find("wait") != std::string_view::npos;
  auto code = guest_stub(process_config().isa(), GuestStub::hle_call, id, svc);
  if (condition_wait) {
    // A real frame keeps this cleanup distinct from the caller's catch frame.
    const auto leaf = code.words;
    if (process_config().isa() == GuestIsa::a64)
      code = {{{leaf[0], leaf[1], leaf[2], leaf[3], leaf[4], 0xa9bf7bfdu, leaf[5], 0xa8c17bfdu, leaf[6]}}, 9};
    else
      code = {{{0xe92d4010u, leaf[0], leaf[1], leaf[2], 0xe8bd8010u, 0}}, 5};
  }
  auto info = std::make_unique<FunctionInfo>(FunctionInfo{handler, user, std::string(library), std::string(name), emit(r, code.instructions())});
  record_unwind_stub(info->thunk, std::uint32_t(code.instructions().size_bytes()), GuestStub::hle_call, condition_wait ? 1 : 0);
  info->execution = execution;
  info->try_handler = try_handler;
  info->id = id;
  if (census_enabled && timing_requested && timed_api(library, name)) {
    census().originals[id] = {info->handler, info->try_handler, library == "libopenxr_loader.so" && name == "xrEndFrame"};
    info->handler = timed_handler;
    if (info->try_handler) info->try_handler = timed_fast_handler;
  }
  if (process_config().isa() == GuestIsa::a32) a32::prepare(*info);
  r.functions[id].store(info.release(), std::memory_order_release);
  ++r.next;
  r.generation.fetch_add(1, std::memory_order_release);
  return id;
}
}
FunctionId register_function(std::string_view library, std::string_view name, Handler handler, void* user, Execution execution, TryHandler try_handler) {
  auto& r = registry(); std::lock_guard lock(r.mutex); return add(r, library, name, handler, user, execution, try_handler);
}
const FunctionInfo& function(FunctionId id) {
  if (id >= capacity) throw std::runtime_error("invalid HLE function id");
  auto* f = registry().functions[id].load(std::memory_order_acquire);
  if (!f) throw std::runtime_error("unregistered HLE function id");
  return *f;
}
GuestAddr thunk_address(FunctionId id) { return function(id).thunk; }
const FunctionInfo* resolve_thunk(GuestAddr entry, std::uint32_t svc) {
  if (entry & 15) return nullptr;
  std::array<std::uint32_t, 7> actual{};
  const bool arm = process_config().isa() == GuestIsa::a32;
  if (!platform::read_memory(entry, actual.data(), (arm ? 4 : 7) * sizeof(std::uint32_t))) return nullptr;
  const auto immediate = [arm](std::uint32_t word) {
    return arm ? ((word >> 4) & 0xf000u) | (word & 0xfffu) : (word >> 5) & 0xffffu;
  };
  const auto id = immediate(actual[0]) | (immediate(actual[1]) << 16);
  if (!id || id >= capacity) return nullptr;
  // Registration publishes immutable entries. Do not take its mutex while
  // translation holds an invalidation admission: registration invalidates.
  const auto* found = registry().functions[id].load(std::memory_order_acquire);
  if (!found || found->thunk != entry) return nullptr;
  const auto& info = *found;
  if (info.execution == Execution::RunLoop) return nullptr;
  const auto expected_svc = svc_for(info.execution);
  if (svc != expected_svc) return nullptr;
  const auto expected = guest_stub(process_config().isa(), GuestStub::hle_call, info.id, svc);
  const auto words = expected.instructions();
  if (!std::equal(words.begin(), words.end(), actual.begin())) return nullptr;
  return &info;
}
std::vector<FunctionInfo> registered_functions() {
  auto& r = registry(); std::lock_guard lock(r.mutex); std::vector<FunctionInfo> result;
  for (FunctionId id = 1; id < r.next; ++id) result.push_back(*r.functions[id].load(std::memory_order_acquire));
  return result;
}
std::uint64_t registered_functions_generation() noexcept {
  return registry().generation.load(std::memory_order_acquire);
}
GuestAddr make_thunk(std::string_view library, std::string_view name, Handler handler, void* user, Execution execution, TryHandler try_handler) {
  return thunk_address(register_function(library, name, handler, user, execution, try_handler));
}
GuestAddr code_stub(std::span<const std::uint32_t> instructions, std::string_view) {
  auto& r = registry(); std::lock_guard lock(r.mutex); return emit(r, instructions);
}
GuestAddr code_stub(GuestStub kind, std::string_view name, std::uint32_t argument) {
  auto code = guest_stub(process_config().isa(), kind, argument);
  if (kind == GuestStub::return_to_host || kind == GuestStub::call_return) {
    // Unwinders look up a saved return PC minus one, including our root LR.
    std::array<std::uint32_t, 3> terminal{process_config().isa() == GuestIsa::a32 ? 0xe1a00000u : 0xd503201fu,
        code.words[0], code.words[1]};
    auto words = std::span(terminal).first(code.count + 1);
    auto address = code_stub(words, name);
    record_unwind_stub(address, std::uint32_t(words.size_bytes()), kind, argument);
    return address + 4;
  }
  if (kind == GuestStub::sigreturn && process_config().isa() == GuestIsa::a32) {
    // EHABI looks up a return PC minus one. Leave an ARM NOP before the
    // kernel-recognized restorer so that lookup stays inside its unwind range.
    std::array<std::uint32_t, 3> restorer{0xe1a00000u, code.words[0], code.words[1]};
    auto address = code_stub(restorer, name);
    record_unwind_stub(address, sizeof(restorer), kind, argument);
    return address + 4;
  }
  auto address = code_stub(code.instructions(), name);
  record_unwind_stub(address, std::uint32_t(code.instructions().size_bytes()), kind, argument);
  return address;
}
GuestAddr return_to_host_trampoline() {
  static const GuestAddr p = code_stub(GuestStub::return_to_host, "return to host");
  return p;
}
GuestAddr call_return_trampoline(std::uint32_t depth) {
  if (!depth || depth > 65535) throw std::runtime_error("guest call nesting exhausted");
  static std::mutex mutex;
  static std::map<std::uint32_t, GuestAddr> trampolines;
  std::lock_guard lock(mutex);
  if (auto it = trampolines.find(depth); it != trampolines.end()) return it->second;
  auto address = code_stub(GuestStub::call_return, "nested return to host", depth);
  trampolines.emplace(depth, address); return address;
}
namespace {
const bool trace_xr = venture::environment::getenv("VENTURE_TRACE_XR") != nullptr;
const bool trace_audio = venture::environment::getenv("VENTURE_TRACE_AUDIO") != nullptr;
const bool trace_gl = venture::environment::getenv("VENTURE_TRACE_GL") != nullptr;
void invoke_handler(GuestThread& thread, FunctionId id, const FunctionInfo& f, Registers registers = {}) {
  if (census_enabled) [[unlikely]] census_hit(id);
  CallContext ctx(thread, thread.cpu(), f, registers);
  startup_diagnostics::HleScope startup_scope(thread, thread.cpu(), id);
  shutdown_trace::HleScope shutdown_call(thread, id);
  static std::atomic<unsigned> gl_events{0};
  if (trace_gl && (f.name == "glViewport" || f.name == "glScissor" ||
                   f.name == "glBindFramebuffer" || f.name.starts_with("glFramebufferTexture") ||
                   f.name.starts_with("glDrawElements") || f.name.starts_with("glDrawArrays")) &&
      gl_events.fetch_add(1) < 2000)
    log::info("gl-trace", "{} x0={:#x} x1={:#x} x2={:#x} x3={:#x} x4={:#x} x5={:#x} x6={:#x}",
              f.name, ctx.x(0), ctx.x(1), ctx.x(2), ctx.x(3), ctx.x(4), ctx.x(5), ctx.x(6));
  const bool trace = (trace_xr && (f.library == "libopenxr_loader.so" || f.library == "libvrapi.so" ||
                                  f.library == "libvulkan.so")) ||
                     (trace_audio && (f.library == "libaaudio.so" || f.library == "libOpenSLES.so"));
  // Function names and scalar results only; never emit guest buffers or credentials.
  if (trace) log::info("hle-trace", "enter {}!{}", f.library, f.name);
  f.handler(ctx);
  if (trace) log::info("hle-trace", "leave {}!{} result={:#x}", f.library, f.name, ctx.x(0));
}
}
void deliver_memory_fault(GuestThread& thread, FunctionId id, const MemoryFault& fault) {
  const auto& f = function(id);
  MemoryRegion region{};
  if (AddressSpace::get().find(fault.address, &region))
    log::error("hle", "{}!{}: guest memory fault at {:#x} in {:#x}-{:#x} prot={} {}", f.library, f.name, fault.address,
               region.start, region.start + region.size, region.prot, region.name);
  else
    log::error("hle", "{}!{}: guest memory fault at {:#x} (unmapped)", f.library, f.name, fault.address);
  thread.cpu().set_pc(thread.cpu().pc() - thread.cpu().trap_instruction_size());
  signals::Info info{fault.bus ? signals::kSigBus : signals::kSigSegv, fault.bus ? 2 : 1, fault.address};
  info.syndrome = fault.write ? 1ull << 6 : 0;
  signals::deliver_synchronous(thread, info);
}
void dispatch(GuestThread& thread, FunctionId id) {
  try {
    const auto& f = function(id);
    if (dev_control::enabled) [[unlikely]] {
      dev_control::boundary(thread, f.name, false);
      invoke_handler(thread, id, f);
      dev_control::boundary(thread, f.name, true);
    } else invoke_handler(thread, id, f);
  }
  catch (const MemoryFault& fault) { deliver_memory_fault(thread, id, fault); }
}
bool dispatch_in_jit(GuestThread& thread, FunctionId id) {
  const auto& f = function(id);
  if (f.execution == Execution::RunLoop) return false;
  return dispatch_registered_in_jit(thread, f);
}
bool dispatch_registered_in_jit(GuestThread& thread, const FunctionInfo& f, Registers registers) {
  if (thread.interrupted.load()) return false;
  const auto id = f.id;
  // Diagnostic tracing uses the original single entry/leave boundary.
  if (f.try_handler && (trace_gl || trace_xr || trace_audio)) return false;
  auto invoke = [&] {
    if (f.try_handler) {
      startup_diagnostics::HleScope startup_scope(thread, thread.cpu(), id);
      shutdown_trace::HleScope shutdown_call(thread, id);
      CallContext ctx(thread, thread.cpu(), f, registers);
      if (!f.try_handler(ctx)) return false;
      if (census_enabled) [[unlikely]] census_hit(id);
    } else invoke_handler(thread, id, f, registers);
    return true;
  };
  bool completed;
  if (profile::enabled) { profile::Activity activity(id); completed = invoke(); }
  else completed = invoke();
  if (!completed) return false;
  // The unresolved-fault guard treats a completed HLE call as guest progress,
  // even when it no longer makes Run return to the thread loop.
  ++thread.cpu_exits;
  return true;
}
void census_guest_threads(std::size_t live) {
  if (!census_enabled) return;
  auto& peak = census().thread_peak;
  auto previous = peak.load(std::memory_order_relaxed);
  while (previous < live && !peak.compare_exchange_weak(previous, live, std::memory_order_relaxed)) {}
}
void census_file_mmap(std::string_view path, int fd, std::uint64_t size, int prot, int flags,
                      std::int64_t offset, std::int64_t result) {
  if (!census_enabled) return;
  auto& c = census();
  Json event{{"path", path}, {"fd", fd}, {"size", size}, {"prot", prot}, {"flags", flags},
             {"sharing", flags & kMapShared ? "shared" : "private"}, {"offset", offset},
             {"result", result}, {"seconds", census_elapsed_ns(c) / 1e9}};
  std::lock_guard lock(c.mutex);
  c.mmaps.push_back(std::move(event));
}
void write_census() noexcept {
  if (!census_enabled) return;
  try {
    auto& c = census();
    std::lock_guard lock(c.mutex);  // serializes snapshots and mmap events, never dispatch
    std::vector<Json> functions;
    auto& r = registry();
    for (FunctionId id = 1; id < capacity; ++id) {
      const auto first = c.counts[id].first_ns.load(std::memory_order_acquire);
      if (first < 0) continue;  // an in-flight first call can appear in the next snapshot
      const auto* f = r.functions[id].load(std::memory_order_acquire);
      functions.push_back({{"function_id", id}, {"library", f->library}, {"symbol", f->name},
                           {"hits", c.counts[id].hits.load(std::memory_order_relaxed)},
                           {"first_hit_seconds", first / 1e9}});
    }
    std::sort(functions.begin(), functions.end(), [](const Json& a, const Json& b) {
      if (a["first_hit_seconds"] != b["first_hit_seconds"])
        return a["first_hit_seconds"] < b["first_hit_seconds"];
      return a["function_id"] < b["function_id"];
    });
    Json report{{"schema_version", 1}, {"time_origin", "census initialization before guest execution (steady clock)"},
                {"elapsed_seconds", census_elapsed_ns(c) / 1e9}, {"functions", functions},
                {"peak_live_guest_threads", c.thread_peak.load(std::memory_order_relaxed)},
                {"file_mmaps_scope", "guest mmap/mmap64 and raw mmap syscall attempts; excludes host ELF loader"},
                {"file_mmaps", c.mmaps}};
    if (timing_requested) {
      Json rows = Json::array();
      for (const auto& [key, t] : c.api_timings) {
        const auto [second, tid, id, scope] = key;
        const auto* f = r.functions[id].load(std::memory_order_acquire);
        rows.push_back({{"second", second}, {"host_tid", tid}, {"library", f->library}, {"symbol", f->name},
            {"scope", std::array{"before_first_guest_End", "between_guest_End_calls", "inside_guest_End", "guest_End"}[scope]},
            {"calls", t.calls}, {"exceptions", t.exceptions}, {"first_ns", t.first_ns}, {"last_ns", t.last_ns},
            {"wall_ns", t.wall_ns}, {"max_wall_ns", t.max_wall_ns}, {"cpu_ns", t.cpu_ns}, {"cpu_samples", t.cpu_samples},
            {"cycles", t.cycles}, {"cycle_samples", t.cycle_samples}});
      }
      report["api_timing"] = {{"window_seconds", {c.timing_begin, c.timing_end}},
          {"origin_monotonic_ns", std::chrono::duration_cast<std::chrono::nanoseconds>(c.started.time_since_epoch()).count()},
          {"cycles_per_ns", c.cycles_per_ns}, {"cycles_method", platform::thread_cycles_method()},
          {"rows", std::move(rows)}, {"dropped", c.timing_dropped}, {"row_limit", 65536},
          {"notes", "Opt-in guest HLE entry durations for OpenXR and Vulkan queue/fence/semaphore calls. One-second entry-time buckets; first/last bound complete calls. CPU counters are cumulative calling-thread counters, coarse on Windows; cycle conversion is provisional. Timers and aggregation add overhead. Declined fast attempts are excluded; completed replay counted once. Nested calls overlap; never sum as critical-path time. No sampling, guest pointers, arguments or stacks."}};
    }
    paths::atomic_write(c.output, report.dump(2) + '\n');
  } catch (const std::exception& e) {
    log::error("hle-census", "could not write census: {}", e.what());
  }
}
GuestAddr missing_symbol(std::string_view library, std::string_view name) {
  auto& r = registry(); std::lock_guard lock(r.mutex);
  auto key = std::make_pair(std::string(library), std::string(name));
  if (auto it = r.missing.find(key); it != r.missing.end()) return it->second;
  auto flag = std::make_unique<std::atomic<bool>>(false);
  auto id = add(r, library, name, unimplemented, flag.get());
  flag.release();
  auto p = function(id).thunk;
  r.missing.emplace(std::move(key), p); return p;
}
}

namespace venture {
bool Cpu::dispatch_hle_in_jit() {
  if (!hle_thread_) return false;
  try { return hle::dispatch_in_jit(*hle_thread_, static_cast<hle::FunctionId>(hle_call_id())); }
  catch (...) { hle_exception_ = std::current_exception(); return false; }
}
bool Cpu::dispatch_hle_in_jit(const hle::FunctionInfo& info, hle::Registers registers) {
  if (!hle_thread_) return false;
  try { return hle::dispatch_registered_in_jit(*hle_thread_, info, registers); }
  catch (...) { hle_exception_ = std::current_exception(); return false; }
}
void Cpu::rethrow_hle_exception() {
  if (!hle_exception_) return;
  auto exception = std::exchange(hle_exception_, {});
  if (exception) std::rethrow_exception(exception);
}
}

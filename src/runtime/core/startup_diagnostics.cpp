#include "core/startup_diagnostics.h"
#include "common/log.h"
#include "core/hle_runtime.h"
#include "core/linker.h"
#include "core/thread_runtime.h"
#include "core/wait.h"
#include "platform/runtime_child.h"
#include <chrono>
#include <filesystem>
#include <format>
#include <vector>
#include <thread>

namespace venture::startup_diagnostics {
namespace {
std::function<void()> sources;
std::string location(GuestAddr pc) {
  DlInfo info;
  if (!Linker::get().try_address_info(pc, &info)) return "unknown";
  // Guest module basename and symbol only, never a host/profile path.
  return std::format("{}+{:#x} symbol={}", std::filesystem::path(info.path).filename().string(),
                     pc - info.base, info.symbol.empty() ? "unknown" : info.symbol);
}
}
void set_sources(std::function<void()> value) { sources = std::move(value); }
void sample(GuestThread& thread, const Cpu& cpu) {
  auto& state = thread.startup_state;
  std::lock_guard lock(state.mutex);
  state.sample.pc = cpu.pc(); state.sample.lr = cpu.lr(); state.sample.sp = cpu.sp();
  state.sample.sampled_ns = monotonic_ns(); state.sample.pending = false;
}
std::uint32_t enter_hle(GuestThread& thread, const Cpu& cpu, std::uint32_t id) {
  sample(thread, cpu);
  std::lock_guard lock(thread.startup_state.mutex);
  auto previous = thread.startup_state.sample.hle;
  thread.startup_state.sample.hle = id;
  return previous;
}
void leave_hle(GuestThread& thread, const Cpu& cpu, std::uint32_t previous) {
  sample(thread, cpu);
  std::lock_guard lock(thread.startup_state.mutex);
  thread.startup_state.sample.hle = previous;
}
void poll() {
  if (!enabled) return;
  const auto reason = take_snapshot_reason();
  if (reason == Reason::none) return;
  log::info("startup-snapshot", "begin reason={} timeout_seconds={}",
            reason == Reason::stall ? "stall" : "entitlement-progress", timeout_seconds());
  // Ask the independent supervisor before consulting any guest-owned locks.
  if (reason == Reason::stall) platform::request_runtime_stall_dump();
  // Do not interrupt a futex waiter: that could repair a missed wake and destroy
  // the evidence. Running CPUs halt cooperatively and publish from their owner.
  bool requested = false;
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(10);
  do {
    requested = try_for_each_guest_thread([](GuestThread& thread) { thread.request_startup_sample(); });
    if (requested) break;
    std::this_thread::yield();
  } while (std::chrono::steady_clock::now() < until);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  struct Row { int tid; unsigned owner; ThreadSample sample; };
  std::vector<Row> rows;
  const bool sampled = try_for_each_guest_thread([&](GuestThread& thread) {
    std::lock_guard lock(thread.startup_state.mutex);
    rows.push_back({thread.tid(), thread.mutex_owner_index(), thread.startup_state.sample});
  });
  log::info("startup-snapshot", "thread_registry request_lock_available={} sample_lock_available={}", requested, sampled);
  if (!requested || !sampled) {
    const auto owner = thread_registry_owner();
    log::info("startup-snapshot", "registry_owner stable={} native_tid={} site={}:{} function={}",
              owner.stable, owner.tid, std::filesystem::path(owner.file).filename().string(), owner.line, owner.function);
  }
  for (const auto& row : rows) {
    const auto& s = row.sample;
    std::string active_hle = "none";
    if (s.hle) { const auto& f = hle::function(s.hle); active_hle = f.library + "!" + f.name; }
    log::info("startup-snapshot", "thread tid={} mutex_owner={} pc={:#x} [{}] lr={:#x} [{}] sp={:#x} hle={} sample_age_ms={} sample_pending={}",
              row.tid, row.owner, s.pc, location(s.pc), s.lr, location(s.lr), s.sp, active_hle,
              (monotonic_ns() - s.sampled_ns) / 1'000'000, s.pending);
  }
  for (const auto& t : managed_thread_snapshot())
    log::info("startup-snapshot", "managed tid={} pthread={:#x} done_word={:#x} done={} detached={} joining={} live={}",
              t.tid, t.pthread, t.done_word, t.done, t.detached, t.joining, t.live);
  for (const auto& w : futex_wait_snapshot())
    log::info("startup-snapshot", "wait tid={} word={:#x} current={} readable={} expected={} woken={} bitset={:#x} deadline_ns={}",
              w.tid, w.address, w.value, w.readable, w.expected, w.woken, w.bitset, w.deadline);
  if (sources) sources();
  log::info("startup-snapshot", "end");
}
}

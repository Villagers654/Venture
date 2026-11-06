#include "core/alloc_failure.h"
#include "common/log.h"
#include "platform/process_info.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <format>
#include <mutex>
#include <utility>

namespace venture::alloc_failure {
namespace {
constexpr std::uint64_t mib = 1ull << 20;
// VkMemoryPropertyFlagBits / VkMemoryHeapFlagBits; the core layer has no Vulkan headers.
constexpr std::uint32_t device_local = 0x1, host_visible = 0x2, host_coherent = 0x4, host_cached = 0x8,
                        lazily_allocated = 0x10;
std::string type_flags(std::uint32_t flags) {
  std::string out;
  const auto add = [&](std::uint32_t bit, std::string_view name) {
    if (!(flags & bit)) return;
    if (!out.empty()) out += '|';
    out += name;
  };
  add(device_local, "DEVICE_LOCAL");
  add(host_visible, "HOST_VISIBLE");
  add(host_coherent, "HOST_COHERENT");
  add(host_cached, "HOST_CACHED");
  add(lazily_allocated, "LAZILY_ALLOCATED");
  return out.empty() ? "none" : out;
}
std::string size_text(std::uint64_t bytes) {
  if (bytes >= mib) return std::format("{:.1f} MiB", double(bytes) / double(mib));
  return std::format("{} bytes", bytes);
}
std::string_view kind_text(Kind kind) {
  switch (kind) {
    case Kind::gpu_memory: return "GPU memory";
    case Kind::gpu_object: return "GPU object";
    case Kind::jit_code: return "JIT code";
    case Kind::guest_heap: return "guest heap";
    case Kind::guest_mmap: return "guest mapping";
  }
  return "memory";
}
std::mutex report_mutex;
struct KindState {
  std::uint64_t lines = 0, skipped = 0;
  std::chrono::steady_clock::time_point last{};
};
std::array<KindState, 5> kind_states;
std::atomic<std::uint64_t> reports{0}, lines{0};
std::array<std::atomic<Relief>, 8> reliefs{};
thread_local bool relieving = false;
}  // namespace

Commit commit() {
  platform::CommitCharge charge;
  if (!platform::commit_charge(charge)) return {};
  return {true, charge.enforced, charge.system_used, charge.system_limit, charge.process};
}

Limit likely_limit(Kind kind, std::uint64_t requested, const Commit& commit, const Gpu* gpu) {
  // The process's count is Venture's own tally (guest and texture memory, VMA
  // blocks); allocations it does not see only bring the true count closer.
  if (gpu && gpu->known && gpu->max_allocations && gpu->allocations >= gpu->max_allocations / 100 * 95)
    return Limit::allocation_count;
  // Commit moves in large steps (a driver block, a heap segment) and a page
  // file that grows on demand lags the request: a few percent of headroom
  // left is as good as none.
  if (commit.known && commit.enforced && commit.system_limit) {
    const auto margin = std::max<std::uint64_t>(commit.system_limit / 32, 256 * mib);
    if (commit.system_used + requested + margin >= commit.system_limit) return Limit::system_commit;
  }
  if (gpu && gpu->known) {
    // Without Resizable BAR the CPU-visible window of VRAM is 256 MiB.
    if ((gpu->heap_flags & device_local) && (gpu->type_flags & device_local) && (gpu->type_flags & host_visible) &&
        gpu->heap_size && gpu->heap_size <= 512 * mib)
      return Limit::bar_heap;
    if (gpu->heap_budget && gpu->heap_usage + requested > gpu->heap_budget) return Limit::gpu_budget;
  }
  if (kind == Kind::jit_code) return Limit::jit_space;
  if (kind == Kind::guest_heap) return Limit::guest_arena;
  return Limit::unknown;
}

std::string_view limit_text(Limit limit) {
  switch (limit) {
    case Limit::system_commit: return "system commit limit (RAM + page file)";
    case Limit::allocation_count: return "Vulkan maxMemoryAllocationCount";
    case Limit::bar_heap: return "small CPU-visible VRAM heap (no Resizable BAR)";
    case Limit::gpu_budget: return "GPU memory budget";
    case Limit::jit_space: return "JIT code space";
    case Limit::guest_arena: return "guest heap arena";
    case Limit::unknown: break;
  }
  return "none near; the driver or OS refused";
}

std::string describe(Kind kind, std::string_view what, std::uint64_t requested, const Commit& commit,
                     const Gpu* gpu) {
  std::string line = std::format("{} allocation failed: {}: {} requested", kind_text(kind), what, size_text(requested));
  if (gpu && gpu->known) {
    line += std::format("; memory type {} ({}) heap {} ({}, {} MiB)", gpu->type, type_flags(gpu->type_flags),
                        gpu->heap, gpu->heap_flags & device_local ? "DEVICE_LOCAL" : "system",
                        gpu->heap_size / mib);
    if (gpu->heap_budget)
      line += std::format(" using {} of {} MiB budget", gpu->heap_usage / mib, gpu->heap_budget / mib);
    else
      line += ", no memory budget";
    if (gpu->max_allocations)
      line += std::format("; {} live allocations of maxMemoryAllocationCount {}", gpu->allocations,
                          gpu->max_allocations);
  }
  if (commit.known)
    line += std::format("; commit {} of {} MiB system{}, {} MiB this process", commit.system_used / mib,
                        commit.system_limit / mib, commit.enforced ? "" : " (overcommit, not enforced)",
                        commit.process / mib);
  else
    line += "; commit unknown";
  line += std::format("; likely limit: {}", limit_text(likely_limit(kind, requested, commit, gpu)));
  return line;
}

Limit report(Kind kind, std::string_view what, std::uint64_t requested, const Gpu* gpu) {
  reports.fetch_add(1, std::memory_order_relaxed);
  const auto state = commit();
  const auto limit = likely_limit(kind, requested, state, gpu);
  std::uint64_t skipped = 0;
  {
    std::lock_guard lock(report_mutex);
    auto& s = kind_states[static_cast<std::size_t>(kind)];
    const auto now = std::chrono::steady_clock::now();
    // The first lines show the pattern; later ones only whether it continues.
    if (s.lines >= 3 && now - s.last < std::chrono::seconds(10)) {
      ++s.skipped;
      return limit;
    }
    ++s.lines;
    s.last = now;
    skipped = std::exchange(s.skipped, 0);
  }
  auto line = describe(kind, what, requested, state, gpu);
  if (skipped) line += std::format(" ({} similar failures not logged)", skipped);
  lines.fetch_add(1, std::memory_order_relaxed);
  log::error("memory", "{}", line);
  return limit;
}

void jit_code_refused(std::size_t requested, std::size_t committed, std::size_t cache_size) noexcept {
  try {
    report(Kind::jit_code,
           std::format("translated code cache ({} of {} MiB committed); cleared it and continued", committed / mib,
                       cache_size / mib),
           requested);
  } catch (...) {
  }
}

void add_relief(Relief relief) {
  for (auto& slot : reliefs) {
    Relief expected = nullptr;
    if (slot.load() == relief || slot.compare_exchange_strong(expected, relief)) return;
  }
}
std::uint64_t relieve() {
  if (relieving) return 0;
  relieving = true;
  std::uint64_t released = 0;
  for (auto& slot : reliefs)
    if (auto relief = slot.load()) released += relief();
  relieving = false;
  return released;
}

Counts counts() { return {reports.load(), lines.load()}; }
void reset_counts() {
  reports = 0;
  lines = 0;
  std::lock_guard lock(report_mutex);
  kind_states = {};
}
}  // namespace venture::alloc_failure

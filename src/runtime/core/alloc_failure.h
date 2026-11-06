#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// One clear log line per failed host allocation, naming the limit that was
// likely hit, and a registry of Venture-owned spare memory to give back before
// a retry. A refused allocation with most RAM and VRAM free is usually the
// Windows commit limit: NVIDIA charges VRAM to process commit, so a game that
// commits 13-16 GB on a 16 GB machine fails GPU, JIT and heap allocations
// alike. The launcher's diagnosis (launcher/diagnosis.cpp) reads these lines.
namespace venture::alloc_failure {
enum class Kind { gpu_memory, gpu_object, jit_code, guest_heap, guest_mmap };
// System commit charge and limit, and this process's commit (Windows), or
// Committed_AS and CommitLimit with the process's private memory (Linux, where
// the limit is enforced only with vm.overcommit_memory=2).
struct Commit {
  bool known = false, enforced = true;
  std::uint64_t system_used = 0, system_limit = 0, process = 0;
};
Commit commit();
// The memory type and heap of a failed Vulkan allocation, filled by the layer.
struct Gpu {
  bool known = false;
  std::uint32_t type = 0, type_flags = 0, heap = 0, heap_flags = 0;
  std::uint64_t heap_size = 0;
  std::uint64_t heap_usage = 0, heap_budget = 0;  // VK_EXT_memory_budget; budget 0 = unknown
  std::uint64_t allocations = 0, max_allocations = 0;  // live vkAllocateMemory count, maxMemoryAllocationCount
};
enum class Limit { system_commit, allocation_count, bar_heap, gpu_budget, jit_space, guest_arena, unknown };
// The likeliest limit for a refused request of `requested` bytes.
Limit likely_limit(Kind kind, std::uint64_t requested, const Commit& commit, const Gpu* gpu = nullptr);
// The fixed text the log line ends with ("likely limit: <text>"); the launcher matches it.
std::string_view limit_text(Limit limit);
// The full line without the rate limit: "<what>: <size> requested; <gpu>; <commit>; likely limit: <text>".
std::string describe(Kind kind, std::string_view what, std::uint64_t requested, const Commit& commit,
                     const Gpu* gpu = nullptr);
// Logs the line as an error, at most a few per kind and then one per 10 s with
// the count of lines left out. Returns the limit it named.
Limit report(Kind kind, std::string_view what, std::uint64_t requested, const Gpu* gpu = nullptr);
// The JIT's code_allocation_failed callback: reports the refused commit; the
// JIT then clears its translated code and compiles again. Never throws.
void jit_code_refused(std::size_t requested, std::size_t committed, std::size_t cache_size) noexcept;

// Venture-owned memory that can be given back on demand (decode scratch,
// staging pools, cached decoded data). A relief must not throw and must not
// wait on guest threads; it may run on any thread, inside any Venture lock
// except its own. Returns bytes released (an estimate).
using Relief = std::uint64_t (*)();
void add_relief(Relief relief);
// Runs every relief once; nested calls on the same thread return 0.
std::uint64_t relieve();
// Tests: the number of report() calls and lines written since the last reset.
struct Counts { std::uint64_t reports = 0, lines = 0; };
Counts counts();
void reset_counts();
}  // namespace venture::alloc_failure

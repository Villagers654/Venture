#include "core/alloc_failure.h"
#include "core/guest_heap.h"
#include "launcher/diagnosis.h"
#include <doctest/doctest.h>
#include <atomic>

using namespace venture;
using alloc_failure::Kind;
using alloc_failure::Limit;

namespace {
constexpr std::uint64_t mib = 1ull << 20, gib = 1ull << 30;
std::atomic<int> reliefs{0}, nested{0};
std::uint64_t counting_relief() {
  ++reliefs;
  // A nested request (a relief that itself allocates and fails) gives nothing back.
  nested += alloc_failure::relieve() == 0;
  return 4096;
}
}  // namespace

TEST_CASE("alloc failure: the likely limit of a refused allocation") {
  alloc_failure::Commit roomy{true, true, 8 * gib, 32 * gib, 4 * gib};
  alloc_failure::Commit full{true, true, 18 * gib + 900 * mib, 19 * gib, 13 * gib};
  // RE4 on an 8 GB RTX 3060 Ti: VRAM barely used, commit at its limit.
  alloc_failure::Gpu vram{true, 3, 0x1, 0, 0x1, 8 * gib, 1184 * mib, 7256 * mib, 900, 4096};
  CHECK(alloc_failure::likely_limit(Kind::gpu_memory, 64 * mib, full, &vram) == Limit::system_commit);
  CHECK(alloc_failure::likely_limit(Kind::guest_heap, 43 * mib, full) == Limit::system_commit);
  CHECK(alloc_failure::likely_limit(Kind::jit_code, mib, full) == Limit::system_commit);
  // Overcommitting Linux never refuses on commit.
  auto overcommit = full;
  overcommit.enforced = false;
  CHECK(alloc_failure::likely_limit(Kind::guest_heap, 43 * mib, overcommit) == Limit::guest_arena);
  CHECK(alloc_failure::likely_limit(Kind::jit_code, mib, roomy) == Limit::jit_space);
  CHECK(alloc_failure::likely_limit(Kind::gpu_memory, 64 * mib, roomy, &vram) == Limit::unknown);
  auto counted = vram;
  counted.allocations = 4000;
  CHECK(alloc_failure::likely_limit(Kind::gpu_memory, 64 * mib, full, &counted) == Limit::allocation_count);
  // The 256 MiB CPU-visible window of a card without Resizable BAR.
  alloc_failure::Gpu bar{true, 2, 0x1 | 0x2 | 0x4, 2, 0x1, 256 * mib, 200 * mib, 240 * mib, 900, 4096};
  CHECK(alloc_failure::likely_limit(Kind::gpu_memory, 64 * mib, roomy, &bar) == Limit::bar_heap);
  auto budget = vram;
  budget.heap_usage = 7200 * mib;
  CHECK(alloc_failure::likely_limit(Kind::gpu_memory, 64 * mib, roomy, &budget) == Limit::gpu_budget);
}

TEST_CASE("alloc failure: one line names every figure and the launcher turns it into advice") {
  alloc_failure::Commit full{true, true, 18 * gib + 900 * mib, 19 * gib, 13 * gib};
  alloc_failure::Gpu vram{true, 3, 0x1, 0, 0x1, 8 * gib, 1184 * mib, 7256 * mib, 900, 4096};
  const auto line = alloc_failure::describe(Kind::gpu_memory, "texture scratch buffer", 64 * mib, full, &vram);
  CAPTURE(line);
  CHECK(line.find("GPU memory allocation failed: texture scratch buffer: 64.0 MiB requested") == 0);
  CHECK(line.find("memory type 3 (DEVICE_LOCAL) heap 0 (DEVICE_LOCAL, 8192 MiB) using 1184 of 7256 MiB budget") !=
        std::string::npos);
  CHECK(line.find("900 live allocations of maxMemoryAllocationCount 4096") != std::string::npos);
  CHECK(line.find("commit 19332 of 19456 MiB system, 13312 MiB this process") != std::string::npos);
  CHECK(line.ends_with("likely limit: system commit limit (RAM + page file)"));
  auto diagnosis = launcher::diagnose("E [memory] " + line + "\nE [vulkan] vkCmdCopyBufferToImage skipped\n");
  REQUIRE(diagnosis);
  CHECK(diagnosis->cause.find("commit memory") != std::string::npos);
  CHECK(diagnosis->advice.find("page file") != std::string::npos);
  // Each limit has its own plain cause.
  for (auto limit : {Limit::allocation_count, Limit::bar_heap, Limit::gpu_budget, Limit::jit_space,
                     Limit::guest_arena, Limit::unknown}) {
    const auto text = std::string("E [memory] JIT code allocation failed: x: 1.0 MiB requested; likely limit: ") +
                      std::string(alloc_failure::limit_text(limit));
    CAPTURE(text);
    auto cause = launcher::diagnose(text);
    REQUIRE(cause);
    CHECK(cause->cause.find("Android function") == std::string::npos);
  }
  CHECK(launcher::diagnose(std::string("likely limit: ") + std::string(alloc_failure::limit_text(Limit::bar_heap)))
            ->advice.find("Resizable BAR") != std::string::npos);
  // A guest crash after a refusal: the refusal is the cause.
  CHECK(launcher::diagnose("E [memory] guest heap allocation failed: guest malloc: 43.0 MiB requested; likely limit: "
                           "system commit limit (RAM + page file)\nE [libc] Fatal signal 5\n")
            ->cause.find("commit memory") != std::string::npos);
}

TEST_CASE("alloc failure: reports are rate-limited per kind and commit is measured") {
  alloc_failure::reset_counts();
  for (int i = 0; i < 10; ++i) alloc_failure::report(Kind::guest_mmap, "test mapping", 4096);
  alloc_failure::report(Kind::gpu_object, "test object", 4096);
  const auto counts = alloc_failure::counts();
  CHECK(counts.reports == 11);
  CHECK(counts.lines == 4);  // three of the first kind, then one a 10 s; one of the second
  alloc_failure::reset_counts();
#if defined(_WIN32)
  const auto commit = alloc_failure::commit();
  REQUIRE(commit.known);
  CHECK(commit.enforced);
  CHECK(commit.system_used > 0);
  CHECK(commit.system_used <= commit.system_limit);
  CHECK(commit.process > 0);
#endif
}

TEST_CASE("alloc failure: a refused guest allocation is reported, retried after relief, and still null") {
  alloc_failure::add_relief(counting_relief);
  alloc_failure::reset_counts();
  reliefs = nested = 0;
  // No host can back this; malloc's null is the guest's to handle.
  CHECK(guest_heap::allocate64(1ull << 62, 16) == nullptr);
  CHECK(guest_heap::zero_allocate64(1ull << 31, 1ull << 31, 16) == nullptr);
  CHECK(reliefs == 2);
  CHECK(nested == 2);
  CHECK(alloc_failure::counts().reports == 2);
  // Overflowing calloc arguments are no refusal and are not reported.
  CHECK(guest_heap::zero_allocate64(1ull << 40, 1ull << 40, 16) == nullptr);
  CHECK(alloc_failure::counts().reports == 2);
  auto* ok = guest_heap::allocate64(64, 16);
  CHECK(ok != nullptr);
  auto* grown = guest_heap::reallocate64(ok, 4096, 16);
  CHECK(grown != nullptr);  // 4 KiB left to the process heap: free may be the A32 arena's here
  CHECK(alloc_failure::counts().reports == 2);
  alloc_failure::reset_counts();
}

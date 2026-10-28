// Standalone CPU-only injected-driver tests. No Vulkan loader/device is used.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "hle/vulkan/deferred_queue_submissions.h"
#include <cstdlib>
#include <new>

// Local to this standalone executable; never linked into the runtime or broad suite.
static thread_local long allocation_budget = -1;
void* operator new(std::size_t size) {
  if (allocation_budget == 0) throw std::bad_alloc();
  if (allocation_budget > 0) --allocation_budget;
  if (auto* p = std::malloc(size ? size : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

using namespace venture;
namespace {
struct Fixture {
  static Fixture* active;
  unsigned calls{}, successes{}, fail_at{};
  bool throw_on_failure = false;
  std::uint64_t counter{};
  VkResult injected = VK_ERROR_OUT_OF_DEVICE_MEMORY;
  VkResult counter_result = VK_SUCCESS;
  VkDevice device = reinterpret_cast<VkDevice>(0x5100);
  VkSemaphore binary = reinterpret_cast<VkSemaphore>(0x5300);
  VkSemaphore timeline = reinterpret_cast<VkSemaphore>(0x5400);
  std::shared_ptr<vulkan::SerializedQueues> backend;
  std::unique_ptr<vulkan::DeferredQueueSubmissions> queues;
  static VkResult issue() {
    auto& f = *active;
    if (++f.calls == f.fail_at) {
      if (f.throw_on_failure) throw std::bad_alloc();
      return f.injected;
    }
    ++f.successes;
    return VK_SUCCESS;
  }
  Fixture() {
    active = this;
    vulkan::SerializedQueues::Driver driver{
      +[](VkDevice, const VkFenceCreateInfo*, const VkAllocationCallbacks*, VkFence*) -> VkResult { return VK_ERROR_UNKNOWN; },
      +[](VkDevice, VkFence, const VkAllocationCallbacks*) {},
      +[](VkDevice, uint32_t, const VkFence*) -> VkResult { return VK_SUCCESS; },
      +[](VkDevice, VkFence) -> VkResult { return VK_NOT_READY; },
      +[](VkDevice, uint32_t, const VkFence*, VkBool32, uint64_t) -> VkResult { return VK_TIMEOUT; },
      +[](VkQueue, uint32_t, const VkSubmitInfo*, VkFence) -> VkResult { return VK_SUCCESS; },
      +[](VkQueue) -> VkResult { return VK_SUCCESS; }};
    backend = std::make_shared<vulkan::SerializedQueues>(device, reinterpret_cast<VkQueue>(0x5200),
        2, std::make_shared<std::recursive_mutex>(), driver, false);
    queues = std::make_unique<vulkan::DeferredQueueSubmissions>(device, backend, 2,
      +[](VkQueue, uint32_t, const VkSubmitInfo*, VkFence) -> VkResult { return issue(); },
      +[](VkDevice, VkSemaphore, uint64_t* out) -> VkResult { *out = active->counter; return active->counter_result; },
      +[](VkQueue, uint32_t, const VkSubmitInfo2*, VkFence) -> VkResult { return issue(); });
    queues->add_semaphore(binary, false);
    queues->add_semaphore(timeline, true);
  }
  VkResult blocked(unsigned logical = 1) {
    const uint64_t value = 1;
    VkTimelineSemaphoreSubmitInfo times{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    times.waitSemaphoreValueCount = 1; times.pWaitSemaphoreValues = &value;
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo input{VK_STRUCTURE_TYPE_SUBMIT_INFO, &times};
    input.waitSemaphoreCount = 1; input.pWaitSemaphores = &timeline; input.pWaitDstStageMask = &stage;
    return queues->submit(logical, 1, &input, VK_NULL_HANDLE);
  }
  VkResult batches(unsigned kind, bool consume = false) {
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    if (kind == 0) {
      VkSubmitInfo input[2]{{VK_STRUCTURE_TYPE_SUBMIT_INFO}, {VK_STRUCTURE_TYPE_SUBMIT_INFO}};
      input[0].signalSemaphoreCount = 1; input[0].pSignalSemaphores = &binary;
      if (consume) { input[1].waitSemaphoreCount=1; input[1].pWaitSemaphores=&binary; input[1].pWaitDstStageMask=&stage; }
      return queues->submit(0, 2, input, VK_NULL_HANDLE);
    }
    if (kind == 1) {
      VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
      signal.semaphore = binary; signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
      VkSubmitInfo2 input[2]{{VK_STRUCTURE_TYPE_SUBMIT_INFO_2}, {VK_STRUCTURE_TYPE_SUBMIT_INFO_2}};
      input[0].signalSemaphoreInfoCount = 1; input[0].pSignalSemaphoreInfos = &signal;
      return queues->submit2(0, 2, input, VK_NULL_HANDLE);
    }
    VkBindSparseInfo input[2]{{VK_STRUCTURE_TYPE_BIND_SPARSE_INFO}, {VK_STRUCTURE_TYPE_BIND_SPARSE_INFO}};
    input[0].signalSemaphoreCount = 1; input[0].pSignalSemaphores = &binary;
    return queues->submit_sparse(0, 2, input, VK_NULL_HANDLE,
      +[](VkQueue, uint32_t, const VkBindSparseInfo*, VkFence) -> VkResult { return issue(); });
  }
  void terminal(unsigned expected_calls) {
    CHECK(queues->flush() == VK_ERROR_DEVICE_LOST);
    CHECK(queues->submit(0, 0, nullptr, VK_NULL_HANDLE) == VK_ERROR_DEVICE_LOST);
    CHECK(queues->wait_submitted(1, 0) == VK_ERROR_DEVICE_LOST);
    CHECK(calls == expected_calls);
  }
};
Fixture* Fixture::active;
}
TEST_CASE("Deferred error contract partial issue is terminal across submission forms") {
  for (unsigned kind = 0; kind != 3; ++kind) {
    for (auto error : {VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY}) {
      Fixture f; f.fail_at = 2; f.injected = error;
      CHECK(f.batches(kind) == VK_ERROR_DEVICE_LOST);
      CHECK(f.calls == 2); CHECK(f.successes == 1);
      CHECK(f.queues->pending(0) == 1);
      f.terminal(2);
    }
  }
}
TEST_CASE("Deferred error contract admission allocation failure preserves previous calls") {
  unsigned failures = 0; bool completed = false;
  for (long budget = 0; budget < 128; ++budget) {
    Fixture f;
    REQUIRE(f.blocked() == VK_SUCCESS);
    REQUIRE(f.queues->pending(1) == 1);
    allocation_budget = budget;
    const auto result = f.batches(0, true);
    allocation_budget = -1;
    if (result == VK_SUCCESS) { completed = true; break; }
    REQUIRE(result == VK_ERROR_OUT_OF_HOST_MEMORY);
    ++failures;
    CHECK(f.calls == 0); CHECK(f.queues->pending(0) == 0);
    CHECK(f.queues->pending(1) == 1);
    // Reusing the binary proves all failed admission plans were rolled back.
    REQUIRE(f.batches(0, true) == VK_SUCCESS);
    CHECK(f.calls == 2); CHECK(f.queues->pending(1) == 1);
    f.counter = 1;
    CHECK(f.queues->flush() == VK_SUCCESS);
    CHECK(f.calls == 3); CHECK(f.queues->pending(1) == 0);
  }
  MESSAGE("pre-admission allocation failpoints recovered: ", failures);
  CHECK(failures > 1); CHECK(completed);
}
TEST_CASE("Deferred error contract previously accepted work cannot become recoverable OOM") {
  Fixture f;
  REQUIRE(f.blocked() == VK_SUCCESS);
  f.counter = 1; f.fail_at = 1;
  CHECK(f.queues->flush() == VK_ERROR_DEVICE_LOST);
  CHECK(f.queues->pending(1) == 1); CHECK(f.successes == 0);
  f.terminal(1);
}
TEST_CASE("Deferred error contract shared flush preserves original accepted work on failure") {
  Fixture f;
  REQUIRE(f.blocked() == VK_SUCCESS);
  f.counter = 1; f.fail_at = 2;
  // New queue0 batch issues first; queue1 belongs to the earlier accepted call.
  CHECK(f.batches(0) == VK_ERROR_DEVICE_LOST);
  CHECK(f.successes == 1); CHECK(f.queues->pending(0) == 1);
  CHECK(f.queues->pending(1) == 1);
  f.terminal(2);
}
TEST_CASE("Deferred error contract dispatch allocation exceptions remain terminal") {
  Fixture f; f.fail_at = 2; f.throw_on_failure = true;
  CHECK(f.batches(0) == VK_ERROR_DEVICE_LOST);
  CHECK(f.successes == 1);
  f.terminal(2);
}

TEST_CASE("Deferred error contract readiness errors cannot lose previously accepted work") {
  for (auto error : {VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY}) {
    Fixture f;
    REQUIRE(f.blocked() == VK_SUCCESS);
    f.counter_result = error;
    CHECK(f.queues->flush() == VK_ERROR_DEVICE_LOST);
    CHECK(f.queues->pending(1) == 1);
    f.terminal(0);
  }
}

TEST_CASE("Deferred error contract stops other queues immediately on readiness failure") {
  Fixture f;
  REQUIRE(f.blocked(0) == VK_SUCCESS);
  f.counter_result = VK_ERROR_OUT_OF_HOST_MEMORY;
  CHECK(f.queues->submit(1, 0, nullptr, VK_NULL_HANDLE) == VK_ERROR_DEVICE_LOST);
  CHECK(f.queues->pending(0) == 1);
  CHECK(f.queues->pending(1) == 1);
  f.terminal(0);
}

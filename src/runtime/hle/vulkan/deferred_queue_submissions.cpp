#include "deferred_queue_submissions.h"
#include "chain.h"
#include <algorithm>
#include <chrono>
#include <thread>

namespace venture::vulkan {
namespace {
// Once work reaches dispatch, it may belong to an earlier call that already
// returned success, or an earlier batch may already have changed native state.
// We cannot promise vkQueueSubmit's unchanged-state guarantee for an OOM here.
VkResult dispatch_error(VkResult result) {
  return result == VK_ERROR_OUT_OF_HOST_MEMORY || result == VK_ERROR_OUT_OF_DEVICE_MEMORY
      ? VK_ERROR_DEVICE_LOST : result;
}
std::vector<DeferredQueueSubmissions::Binding> bindings(
    const std::shared_ptr<SerializedQueues>& backend, std::uint32_t count) {
  std::vector<DeferredQueueSubmissions::Binding> result;
  result.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) result.push_back({backend, i});
  return result;
}
}
struct DeferredQueueSubmissions::Job {
  Chain storage;
  const VkSubmitInfo* info{};
  const VkSubmitInfo2* info2{};
  VkFence fence{};
  std::vector<std::pair<VkSemaphore, std::uint64_t>> waits, signals;
  std::vector<VkSemaphore> binary_waits, binary_signals;
  std::optional<BinarySemaphoreOrder::Plan> binary;
  std::function<VkResult(VkQueue)> operation;
};
DeferredQueueSubmissions::DeferredQueueSubmissions(VkDevice device,
    std::shared_ptr<SerializedQueues> backend, std::uint32_t count,
    PFN_vkQueueSubmit submit, PFN_vkGetSemaphoreCounterValue counter, PFN_vkQueueSubmit2 submit2)
    : DeferredQueueSubmissions(device, bindings(backend, count), submit, counter, submit2) {}
DeferredQueueSubmissions::DeferredQueueSubmissions(VkDevice device,
    std::vector<Binding> bindings, PFN_vkQueueSubmit submit,
    PFN_vkGetSemaphoreCounterValue counter, PFN_vkQueueSubmit2 submit2)
    : device_(device), bindings_(std::move(bindings)), submit_(submit), submit2_(submit2),
      counter_(counter), pending_(bindings_.size()) {
  if (!device || bindings_.empty() || !submit || !counter)
    throw std::invalid_argument("incomplete deferred queue backend");
  for (const auto& binding : bindings_) {
    if (!binding.backend) throw std::invalid_argument("missing native queue backend");
    (void)binding.backend->issued(binding.index); // Validate before accepting work.
  }
}
DeferredQueueSubmissions::~DeferredQueueSubmissions() = default;
void DeferredQueueSubmissions::add_semaphore(VkSemaphore semaphore, bool timeline) {
  std::lock_guard lock(mutex_);
  if (!semaphore || semaphores_.contains(semaphore)) throw std::invalid_argument("duplicate semaphore");
  const auto payload = ++next_payload_;
  if (!payload) throw std::overflow_error("semaphore payload sequence exhausted");
  if (!timeline) binary_.add(payload);
  try { semaphores_.emplace(semaphore, Semaphore{timeline, payload}); }
  catch (...) { if (!timeline) binary_.remove(payload); throw; }
}
void DeferredQueueSubmissions::remove_semaphore(VkSemaphore semaphore) {
  std::lock_guard lock(mutex_);
  if (auto it = semaphores_.find(semaphore); it != semaphores_.end()) {
    if (!it->second.timeline) {
      binary_.remove(it->second.payload);
    }
    semaphores_.erase(it);
  }
}
std::shared_ptr<DeferredQueueSubmissions::Job> DeferredQueueSubmissions::copy(
    std::uint32_t logical, const VkSubmitInfo& source, VkFence fence, bool own) {
  if (logical >= pending_.size()) throw std::invalid_argument("unknown logical queue");
  auto job = std::make_shared<Job>();
  if (own) job->storage.retain_input_arrays();
  job->info = own ? static_cast<const VkSubmitInfo*>(job->storage.translate(&source)) : &source;
  if (job->storage.result() != VK_SUCCESS || !job->info)
    throw std::invalid_argument("unsupported queued submission structure");
  job->fence = fence;
  const VkTimelineSemaphoreSubmitInfo* values{};
  for (auto* node = static_cast<const VkBaseInStructure*>(job->info->pNext); node; node = node->pNext)
    if (node->sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO)
      values = reinterpret_cast<const VkTimelineSemaphoreSubmitInfo*>(node);
  for (std::uint32_t i = 0; i < job->info->waitSemaphoreCount; ++i) {
    const auto semaphore = job->info->pWaitSemaphores[i];
    const auto& state = semaphores_.at(semaphore);
    if (!state.timeline) job->binary_waits.push_back(semaphore);
    else {
      if (!values || values->waitSemaphoreValueCount <= i) throw std::invalid_argument("missing timeline wait value");
      job->waits.emplace_back(semaphore, values->pWaitSemaphoreValues[i]);
    }
  }
  for (std::uint32_t i = 0; i < job->info->signalSemaphoreCount; ++i) {
    const auto semaphore = job->info->pSignalSemaphores[i];
    const auto& state = semaphores_.at(semaphore);
    if (!state.timeline) job->binary_signals.push_back(semaphore);
    else {
      if (!values || values->signalSemaphoreValueCount <= i) throw std::invalid_argument("missing timeline signal value");
      job->signals.emplace_back(semaphore, values->pSignalSemaphoreValues[i]);
    }
  }
  return job;
}
std::shared_ptr<DeferredQueueSubmissions::Job> DeferredQueueSubmissions::copy2(
    std::uint32_t logical, const VkSubmitInfo2& source, VkFence fence, bool own) {
  if (logical >= pending_.size()) throw std::invalid_argument("unknown logical queue");
  auto job = std::make_shared<Job>();
  if (own) job->storage.retain_input_arrays();
  job->info2 = own ? static_cast<const VkSubmitInfo2*>(job->storage.translate(&source)) : &source;
  if (job->storage.result() != VK_SUCCESS || !job->info2)
    throw std::invalid_argument("unsupported queued submission2 structure");
  job->fence = fence;
  for (std::uint32_t i = 0; i < job->info2->waitSemaphoreInfoCount; ++i) {
    const auto& info = job->info2->pWaitSemaphoreInfos[i];
    const auto& state = semaphores_.at(info.semaphore);
    if (state.timeline) job->waits.emplace_back(info.semaphore, info.value);
    else job->binary_waits.push_back(info.semaphore);
  }
  for (std::uint32_t i = 0; i < job->info2->signalSemaphoreInfoCount; ++i) {
    const auto& info = job->info2->pSignalSemaphoreInfos[i];
    const auto& state = semaphores_.at(info.semaphore);
    if (state.timeline) job->signals.emplace_back(info.semaphore, info.value);
    else job->binary_signals.push_back(info.semaphore);
  }
  return job;
}
VkResult DeferredQueueSubmissions::enqueue(std::uint32_t logical,
    const std::vector<std::shared_ptr<Job>>& copies) {
  std::vector<BinarySemaphoreOrder::Plan> admitted;
  admitted.reserve(copies.size());
  const auto previous = pending_[logical].size();
  try {
  for (auto job : copies) {
    std::vector<std::uint64_t> waits, signals;
    for (auto semaphore : job->binary_waits) waits.push_back(semaphores_.at(semaphore).payload);
    for (auto semaphore : job->binary_signals) signals.push_back(semaphores_.at(semaphore).payload);
    auto plan = binary_.prepare(waits, signals);
    job->binary.emplace(plan);
    admitted.push_back(plan);
    if (!binary_.commit(plan)) throw std::invalid_argument("binary admission changed");
    // The first ready operation is consumed before this API call returns.
    // Retain arrays only for work that can actually survive the caller.
    if (bindings_[logical].backend->completion_markers() || !pending_[logical].empty() || !ready(*job)) {
      auto owned = job->info2 ? copy2(logical, *job->info2, job->fence, true)
                              : copy(logical, *job->info, job->fence, true);
      owned->binary = std::move(job->binary);
      owned->operation = std::move(job->operation);
      job = std::move(owned);
    }
    pending_[logical].push_back(std::move(job));
  }
  } catch (...) {
    while (pending_[logical].size() > previous) pending_[logical].pop_back();
    for (auto it = admitted.rbegin(); it != admitted.rend(); ++it) binary_.rollback(*it);
    throw;
  }
  return flush();
}
VkResult DeferredQueueSubmissions::submit(std::uint32_t logical, std::uint32_t count,
    const VkSubmitInfo* input, VkFence fence) {
  std::lock_guard lock(mutex_);
  if (failure_ != VK_SUCCESS) return failure_;
  if (logical >= pending_.size() || (count && !input)) return VK_ERROR_INITIALIZATION_FAILED;
  try {
    std::vector<std::shared_ptr<Job>> copies;
    copies.reserve(std::max(count, 1u));
    const VkSubmitInfo empty{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    for (std::uint32_t i = 0; i < std::max(count, 1u); ++i)
      copies.push_back(copy(logical, count ? input[i] : empty,
          i + 1 == std::max(count, 1u) ? fence : VK_NULL_HANDLE));
    return enqueue(logical, copies);
  } catch (const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
  catch (const std::exception&) { return VK_ERROR_INITIALIZATION_FAILED; }
}
VkResult DeferredQueueSubmissions::submit2(std::uint32_t logical, std::uint32_t count,
    const VkSubmitInfo2* input, VkFence fence) {
  std::lock_guard lock(mutex_);
  if (failure_ != VK_SUCCESS) return failure_;
  if (!submit2_) return VK_ERROR_EXTENSION_NOT_PRESENT;
  if (logical >= pending_.size() || (count && !input)) return VK_ERROR_INITIALIZATION_FAILED;
  try {
    std::vector<std::shared_ptr<Job>> copies;
    copies.reserve(std::max(count, 1u));
    const VkSubmitInfo2 empty{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    for (std::uint32_t i = 0; i < std::max(count, 1u); ++i)
      copies.push_back(copy2(logical, count ? input[i] : empty,
          i + 1 == std::max(count, 1u) ? fence : VK_NULL_HANDLE));
    return enqueue(logical, copies);
  } catch (const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
  catch (const std::exception&) { return VK_ERROR_INITIALIZATION_FAILED; }
}
VkResult DeferredQueueSubmissions::submit_sparse(std::uint32_t logical, std::uint32_t count,
    const VkBindSparseInfo* input, VkFence fence, PFN_vkQueueBindSparse bind) {
  std::lock_guard lock(mutex_);
  if (failure_ != VK_SUCCESS) return failure_;
  if (logical >= pending_.size() || (count && !input) || !bind) return VK_ERROR_INITIALIZATION_FAILED;
  struct Owned { Chain chain; const VkBindSparseInfo* info; };
  try {
    std::vector<std::shared_ptr<Job>> jobs;
    const VkBindSparseInfo empty{VK_STRUCTURE_TYPE_BIND_SPARSE_INFO};
    for (std::uint32_t i = 0; i < std::max(count, 1u); ++i) {
      auto owned = std::make_shared<Owned>();
      owned->chain.retain_input_arrays();
      owned->info = static_cast<const VkBindSparseInfo*>(owned->chain.translate(count ? &input[i] : &empty));
      if (!owned->info || owned->chain.result() != VK_SUCCESS) return VK_ERROR_INITIALIZATION_FAILED;
      const auto& info = *owned->info;
      const VkTimelineSemaphoreSubmitInfo* timeline = nullptr;
      for (auto* node = static_cast<const VkBaseInStructure*>(info.pNext); node; node = node->pNext)
        if (node->sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO)
          timeline = reinterpret_cast<const VkTimelineSemaphoreSubmitInfo*>(node);
      std::vector<VkPipelineStageFlags> stages(info.waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
      VkSubmitInfo metadata{VK_STRUCTURE_TYPE_SUBMIT_INFO, timeline};
      metadata.waitSemaphoreCount = info.waitSemaphoreCount;
      metadata.pWaitSemaphores = info.pWaitSemaphores;
      metadata.pWaitDstStageMask = stages.data();
      metadata.signalSemaphoreCount = info.signalSemaphoreCount;
      metadata.pSignalSemaphores = info.pSignalSemaphores;
      const auto last_fence = i + 1 == std::max(count, 1u) ? fence : VK_NULL_HANDLE;
      auto job = copy(logical, metadata, last_fence, true);
      job->operation = [owned, bind, last_fence](VkQueue native) {
        return bind(native, 1, owned->info, last_fence);
      };
      jobs.push_back(std::move(job));
    }
    return enqueue(logical, jobs);
  } catch (const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
  catch (const std::exception&) { return VK_ERROR_INITIALIZATION_FAILED; }
}
bool DeferredQueueSubmissions::ready(const Job& job) {
  if (!binary_.ready(*job.binary)) return false;
  for (const auto& [semaphore, value] : job.waits) {
    // Every admitted producer, on any native queue in this dispatcher, can
    // finish without a future guest API call. Retain the actual native wait.
    if (semaphores_.at(semaphore).issued_value >= value) continue;
    std::uint64_t actual{};
    auto result = counter_(device_, semaphore, &actual);
    if (result != VK_SUCCESS) { failure_ = dispatch_error(result); return false; }
    if (actual < value) return false;
  }
  return true;
}
VkResult DeferredQueueSubmissions::flush() {
  std::lock_guard lock(mutex_);
  try {
  if (failure_ != VK_SUCCESS) return failure_;
  for (;;) {
    bool issued = false;
    for (std::size_t offset = 0; offset < pending_.size(); ++offset) {
      const auto index = (next_queue_ + offset) % pending_.size();
      auto& queue = pending_[index];
      if (queue.empty()) continue;
      if (!ready(*queue.front())) {
        // A readiness error is terminal, not an unsatisfied dependency. Do
        // not issue a different queue after observing device loss.
        if (failure_ != VK_SUCCESS) return failure_;
        continue;
      }
      const auto job = queue.front();
      const auto& binding = bindings_[index];
      VkResult result;
      result = binding.backend->submit(binding.index, [&](VkQueue native) {
        if (job->operation) return job->operation(native);
        if (job->info2) return submit2_(native, 1, job->info2, job->fence);
        return submit_(native, 1, job->info, job->fence);
      }, false);
      // A status query or zero-timeout host wait may pump readiness. Do not
      // block that caller behind the bounded marker pool; retain the job and
      // retry after native completion or a later API call supplies progress.
      if (result == VK_NOT_READY) continue;
      if (result != VK_SUCCESS) return failure_ = dispatch_error(result);
      if (!binary_.issue(*job->binary)) return failure_ = VK_ERROR_INITIALIZATION_FAILED;
      for (const auto& [semaphore, value] : job->signals)
        semaphores_.at(semaphore).issued_value = std::max(semaphores_.at(semaphore).issued_value, value);
      queue.pop_front(); next_queue_ = (index + 1) % pending_.size();
      issued = true; break;
    }
    if (failure_ != VK_SUCCESS) return failure_;
    if (!issued) return VK_SUCCESS;
  }
  } catch (const std::bad_alloc&) {
    // Admission has finished. Do not let outer submission handlers turn a
    // dispatch allocation failure into a recoverable pre-admission OOM.
    return failure_ = VK_ERROR_DEVICE_LOST;
  }
}
VkResult DeferredQueueSubmissions::external_signal(VkSemaphore semaphore) {
  if (!semaphore) return VK_SUCCESS;
  std::lock_guard lock(mutex_);
  try {
    const auto& state = semaphores_.at(semaphore);
    if (state.timeline) return VK_ERROR_INITIALIZATION_FAILED;
    binary_.external_signal_issued(state.payload);
    return flush();
  } catch (const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
  catch (const std::exception&) { return VK_ERROR_INITIALIZATION_FAILED; }
}
VkResult DeferredQueueSubmissions::native_work(std::uint32_t logical,
    std::span<const VkSemaphore> waits, std::span<const VkSemaphore> signals,
    const std::function<VkResult(VkQueue)>& operation, const bool* waits_issued) {
  std::lock_guard lock(mutex_);
  if (failure_ != VK_SUCCESS) return failure_;
  if (logical >= pending_.size()) return VK_ERROR_INITIALIZATION_FAILED;
  struct Outcome { bool issued = false; VkResult result = VK_SUCCESS; };
  try {
    std::vector<VkPipelineStageFlags> stages(waits.size(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    VkSubmitInfo metadata{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    metadata.waitSemaphoreCount = static_cast<std::uint32_t>(waits.size());
    metadata.pWaitSemaphores = waits.data(); metadata.pWaitDstStageMask = stages.data();
    metadata.signalSemaphoreCount = static_cast<std::uint32_t>(signals.size());
    metadata.pSignalSemaphores = signals.data();
    auto job = copy(logical, metadata, VK_NULL_HANDLE, true);
    auto outcome = std::make_shared<Outcome>();
    // operation owns every input it may use after the caller returns. WSI
    // owns its result storage and reports a late status on the next call.
    job->operation = [operation, outcome, waits_issued](VkQueue native) {
      outcome->result = operation(native); outcome->issued = true;
      return waits_issued && *waits_issued ? VK_SUCCESS : outcome->result;
    };
    const auto result = enqueue(logical, {job});
    if (result != VK_SUCCESS) return result;
    return outcome->issued ? outcome->result : VK_SUCCESS;
  } catch (const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
  catch (const std::exception&) { return VK_ERROR_INITIALIZATION_FAILED; }
}
VkResult DeferredQueueSubmissions::wait_submitted(std::uint32_t logical, std::uint64_t timeout_ns) {
  const auto start = std::chrono::steady_clock::now();
  for (;;) {
    {
      std::lock_guard lock(mutex_);
      auto result = flush();
      if (result != VK_SUCCESS) return result;
      if (logical >= pending_.size()) return VK_ERROR_INITIALIZATION_FAILED;
      if (pending_[logical].empty()) return VK_SUCCESS;
    }
    if (timeout_ns != UINT64_MAX &&
        std::chrono::steady_clock::now() - start >= std::chrono::nanoseconds(timeout_ns)) return VK_TIMEOUT;
    // A host signal on another caller thread must be able to enter/flush.
    std::this_thread::sleep_for(std::chrono::microseconds(50));
  }
}
VkResult DeferredQueueSubmissions::wait_idle(std::uint32_t logical) {
  auto result = wait_submitted(logical);
  if (result != VK_SUCCESS) return result;
  const auto binding = bindings_[logical];
  return binding.backend->wait_idle(binding.index);
}
std::size_t DeferredQueueSubmissions::pending(std::uint32_t logical) const {
  std::lock_guard lock(mutex_);
  return pending_.at(logical).size();
}
} // namespace venture::vulkan

#include "gfx/pipeline_trace.h"
#include "core/profile.h"
#include "core/wait.h"
#include <cstring>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace venture::gfx {
namespace {
struct Device {
  PFN_vkGetDeviceProcAddr loader;
  PFN_vkCreateGraphicsPipelines graphics;
  PFN_vkCreateComputePipelines compute;
  bool feedback;
  profile::PipelineSource source;
};
struct State {
  std::mutex mutex;
  std::map<VkDevice, std::shared_ptr<const Device>> devices;
  std::atomic<PFN_vkGetInstanceProcAddr> loader{nullptr};
};
State& state() { static auto* s = new State; return *s; }
std::shared_ptr<const Device> device(VkDevice d) {
  auto& s = state(); std::lock_guard lock(s.mutex);
  const auto it = s.devices.find(d);
  return it == s.devices.end() ? nullptr : it->second;
}
template<class Info, class Fn>
VkResult create(const Device& d, profile::PipelineSource source, Fn fn, VkDevice handle,
                VkPipelineCache cache, std::uint32_t count, const Info* infos,
                const VkAllocationCallbacks* allocator, VkPipeline* out) {
  // Prepend to a shallow copy. The caller owns the entire original chain and
  // its feedback outputs; neither is rewritten, detached, or duplicated.
  std::vector<Info> copies;
  std::vector<VkPipelineCreationFeedbackCreateInfo> nodes;
  std::vector<VkPipelineCreationFeedback> values;
  std::vector<const VkPipelineCreationFeedback*> feedback;
  if (d.feedback && count && infos) {
    copies.assign(infos, infos + count);
    nodes.resize(count); values.resize(count); feedback.resize(count);
    for (std::uint32_t i = 0; i < count; ++i) {
      const VkPipelineCreationFeedbackCreateInfo* existing = nullptr;
      for (auto* node = static_cast<const VkBaseInStructure*>(infos[i].pNext); node; node = node->pNext)
        if (node->sType == VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO) {
          existing = reinterpret_cast<const VkPipelineCreationFeedbackCreateInfo*>(node); break;
        }
      if (existing) feedback[i] = existing->pPipelineCreationFeedback;
      else {
        nodes[i] = {VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO, infos[i].pNext,
                    &values[i], 0, nullptr};
        copies[i].pNext = &nodes[i]; feedback[i] = &values[i];
      }
    }
  }
  VkResult result;
  std::vector<profile::PipelineFeedback> measured(feedback.size());
  std::int64_t start;
  std::uint64_t duration;
  {
    profile::Activity activity(profile::pipeline_create);
    start = monotonic_ns();
    result = fn(handle, cache, count, copies.empty() ? infos : copies.data(), allocator, out);
    duration = static_cast<std::uint64_t>(monotonic_ns() - start);
  }
  for (std::size_t i = 0; i < feedback.size(); ++i)
    if (const auto* f = feedback[i]) {
      // Invalid feedback leaves duration undefined, including caller-owned outputs.
      measured[i] = {(f->flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT) ? f->duration : 0, f->flags};
    }
  profile::record_pipeline(source, std::is_same_v<Info, VkComputePipelineCreateInfo>, count, start, duration, measured);
  return result;
}
template<class F> VkResult guarded(F&& f) noexcept {
  try { return f(); }
  catch (const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
  catch (...) { return VK_ERROR_INITIALIZATION_FAILED; }
}
VkResult VKAPI_CALL graphics(VkDevice handle, VkPipelineCache cache, std::uint32_t count,
                             const VkGraphicsPipelineCreateInfo* infos, const VkAllocationCallbacks* a, VkPipeline* out) {
  return guarded([&] {
    const auto d = device(handle);
    if (!d || !d->graphics) return VK_ERROR_INITIALIZATION_FAILED;
    return create(*d, d->source, d->graphics, handle, cache, count, infos, a, out);
  });
}
VkResult VKAPI_CALL compute(VkDevice handle, VkPipelineCache cache, std::uint32_t count,
                            const VkComputePipelineCreateInfo* infos, const VkAllocationCallbacks* a, VkPipeline* out) {
  return guarded([&] {
    const auto d = device(handle);
    if (!d || !d->compute) return VK_ERROR_INITIALIZATION_FAILED;
    return create(*d, d->source, d->compute, handle, cache, count, infos, a, out);
  });
}
std::atomic<PFN_vkGetDeviceProcAddr> device_loader{nullptr};
PFN_vkVoidFunction VKAPI_CALL device_gdpa(VkDevice d, const char* name) {
  auto get = device_loader.load();
  // Keep each device tied to the resolver that created it. Tests and later
  // display initialization can wrap another loader while old devices live.
  if (auto entry = device(d)) get = entry->loader;
  auto fn = get ? get(d, name) : nullptr;
  if (fn) if (auto traced = pipeline_proc(name)) return traced;
  return fn;
}
PFN_vkVoidFunction VKAPI_CALL gipa(VkInstance i, const char* name) {
  auto fn = state().loader.load()(i, name);
  if (!fn) return nullptr;
  if (std::strcmp(name, "vkGetInstanceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(gipa);
  if (std::strcmp(name, "vkGetDeviceProcAddr") == 0) {
    device_loader = reinterpret_cast<PFN_vkGetDeviceProcAddr>(fn);
    return reinterpret_cast<PFN_vkVoidFunction>(device_gdpa);
  }
  if (auto traced = pipeline_proc(name)) return traced;
  return fn;
}
}
PFN_vkGetInstanceProcAddr wrap_pipeline_loader(PFN_vkGetInstanceProcAddr raw) {
  if (raw == gipa) return raw;
  state().loader = raw;
  return gipa;
}
PFN_vkGetInstanceProcAddr unwrap_pipeline_loader(PFN_vkGetInstanceProcAddr fn) { return fn == gipa ? state().loader.load() : fn; }
void register_pipeline_device(VkDevice d, PFN_vkGetDeviceProcAddr raw, bool feedback, profile::PipelineSource source) {
  profile::start_pipeline_telemetry();
  auto entry = std::make_shared<Device>(Device{
      raw,
      reinterpret_cast<PFN_vkCreateGraphicsPipelines>(raw(d, "vkCreateGraphicsPipelines")),
      reinterpret_cast<PFN_vkCreateComputePipelines>(raw(d, "vkCreateComputePipelines")), feedback, source});
  auto& s = state(); std::lock_guard lock(s.mutex); s.devices[d] = std::move(entry);
}
void release_pipeline_device(VkDevice d) { auto& s = state(); std::lock_guard lock(s.mutex); s.devices.erase(d); }
PFN_vkVoidFunction pipeline_proc(std::string_view name) {
  if (name == "vkCreateGraphicsPipelines") return reinterpret_cast<PFN_vkVoidFunction>(graphics);
  if (name == "vkCreateComputePipelines") return reinterpret_cast<PFN_vkVoidFunction>(compute);
  return nullptr;
}
VkResult create_decode_pipeline(VkDevice handle, VkPipelineCache cache, std::uint32_t count,
                                const VkComputePipelineCreateInfo* infos, const VkAllocationCallbacks* a, VkPipeline* out) {
  return guarded([&] {
    const auto d = device(handle);
    if (!d || !d->compute) return VK_ERROR_INITIALIZATION_FAILED;
    return create(*d, profile::PipelineSource::decode, d->compute, handle, cache, count, infos, a, out);
  });
}
}

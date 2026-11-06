#include "gfx/pipeline_trace.h"
#include "common/environment.h"
#include "common/log.h"
#include "common/paths.h"
#include "core/process.h"
#include "core/profile.h"
#include "core/wait.h"
#include "platform/thread_priority.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <thread>
#include <unordered_map>
#include <vector>

namespace venture::gfx {
namespace {
// Every pipeline of a device (the guest's, and ANGLE's for GLES games) goes
// through one VkPipelineCache that is saved per game and reloaded on the next
// launch, as Android's per-app EGL blob cache does for drivers there. Games
// save their own Vulkan caches only at a clean exit, if at all, and the data
// they kept from a Quest is another driver's; ANGLE skips its own cache on
// desktop drivers. The driver's disk cache does not cover this: it is keyed by
// executable name, so every game shares and evicts one budget, and a hit there
// still costs a lookup with the shaders' full hashes per pipeline.
struct PersistentCache {
  VkDevice device{};
  PFN_vkGetPipelineCacheData data{};
  PFN_vkDestroyPipelineCache destroy{};
  PFN_vkMergePipelineCaches merge{};
  VkPipelineCache cache{};
  std::filesystem::path file;
  // Creation shares the cache; merging into it and destroying it are
  // externally synchronized commands and take it exclusively.
  std::shared_mutex use;
  std::mutex write;  // file writes
  std::atomic<bool> dirty{false};
  std::atomic<std::int64_t> changed_ns{0};
  bool alive = true;  // under `use`
};
struct Device {
  VkDevice handle;
  PFN_vkGetDeviceProcAddr loader;
  PFN_vkCreateGraphicsPipelines graphics;
  PFN_vkCreateComputePipelines compute;
  PFN_vkCreatePipelineCache create_cache;
  PFN_vkCmdBindPipeline bind;
  PFN_vkDestroyPipeline destroy_pipeline;
  PFN_vkDestroyShaderModule destroy_module;
  PFN_vkDestroyPipelineLayout destroy_layout;
  PFN_vkDestroyRenderPass destroy_render_pass;
  PFN_vkSetDebugUtilsObjectNameEXT set_name;
  PFN_vkSetDebugUtilsObjectTagEXT set_tag;
  PFN_vkDebugMarkerSetObjectNameEXT marker_name;
  PFN_vkDebugMarkerSetObjectTagEXT marker_tag;
  bool feedback;
  bool async;  // guest graphics pipelines may be created in the background
  profile::PipelineSource source;
  std::shared_ptr<PersistentCache> persistent;
};
struct State {
  std::mutex mutex;
  std::map<VkDevice, std::shared_ptr<const Device>> devices;
  std::atomic<PFN_vkGetInstanceProcAddr> loader{nullptr};
  // VENTURE_COMPILE_TRACE: when each pipeline was created, until its first bind.
  std::mutex created_mutex;
  std::unordered_map<std::uint64_t, std::int64_t> created;
};
State& state() { static auto* s = new State; return *s; }

constexpr std::size_t kCacheLimit = 256u << 20;
void save(PersistentCache& p) {
  std::lock_guard write(p.write);
  if (!p.dirty.exchange(false)) return;
  std::string bytes;
  {
    std::shared_lock use(p.use);
    if (!p.alive) return;
    std::size_t size = 0;
    if (p.data(p.device, p.cache, &size, nullptr) != VK_SUCCESS || !size || size > kCacheLimit) return;
    bytes.resize(size);
    if (p.data(p.device, p.cache, &size, bytes.data()) != VK_SUCCESS) return;
    bytes.resize(size);
  }
  auto temporary = p.file;
  temporary += ".tmp";
  try {
    std::filesystem::create_directories(p.file.parent_path());
    {
      std::ofstream out(paths::io_path(temporary), std::ios::binary | std::ios::trunc);
      out.write(bytes.data(), std::streamsize(bytes.size()));
      out.flush();
      if (!out) throw std::runtime_error("short write");
    }
    paths::replace_file(temporary, p.file);
  } catch (...) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
  }
}
struct Writer {
  std::mutex lock;
  std::vector<std::weak_ptr<PersistentCache>> caches;
};
Writer& writer() {
  static auto* w = [] {
    auto* w = new Writer;
    // Writes once creation has been quiet for a few seconds, as a load ends,
    // so a game that never exits cleanly still keeps what it compiled.
    std::thread([w] {
      for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        std::vector<std::weak_ptr<PersistentCache>> caches;
        {
          std::lock_guard lock(w->lock);
          std::erase_if(w->caches, [](const auto& c) { return c.expired(); });
          caches = w->caches;
        }
        for (const auto& weak : caches)
          if (auto c = weak.lock(); c && c->dirty && monotonic_ns() - c->changed_ns > 3'000'000'000) save(*c);
      }
    }).detach();
    return w;
  }();
  return *w;
}
std::shared_ptr<PersistentCache> open_persistent(VkDevice d, PFN_vkGetDeviceProcAddr raw,
                                                 PFN_vkCreatePipelineCache create_cache, profile::PipelineSource source) {
  if (const auto* e = environment::getenv("VENTURE_VK_PIPELINE_CACHE"); e && std::string_view(e) == "0") return nullptr;
  if (source != profile::PipelineSource::guest && source != profile::PipelineSource::angle) return nullptr;
  const auto* override_dir = environment::getenv("VENTURE_VK_PIPELINE_CACHE_DIR");
  const auto directory = override_dir ? paths::from_utf8(override_dir) : process_config().cache_dir;
  if (directory.empty() || !create_cache) return nullptr;
  auto p = std::make_shared<PersistentCache>();
  p->device = d;
  p->data = reinterpret_cast<PFN_vkGetPipelineCacheData>(raw(d, "vkGetPipelineCacheData"));
  p->destroy = reinterpret_cast<PFN_vkDestroyPipelineCache>(raw(d, "vkDestroyPipelineCache"));
  p->merge = reinterpret_cast<PFN_vkMergePipelineCaches>(raw(d, "vkMergePipelineCaches"));
  if (!p->data || !p->destroy || !p->merge) return nullptr;
  p->file = directory / (source == profile::PipelineSource::angle ? "vk-pipeline-cache-gles.bin" : "vk-pipeline-cache.bin");
  std::string bytes;
  std::error_code error;
  if (std::filesystem::exists(p->file, error)) try { bytes = paths::read(p->file, kCacheLimit); } catch (...) {}
  // The driver checks the header (vendor, device, pipelineCacheUUID) and
  // starts empty on data from another driver or GPU.
  VkPipelineCacheCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, nullptr, 0, bytes.size(), bytes.data()};
  if (create_cache(d, &info, nullptr, &p->cache) != VK_SUCCESS) {
    info.initialDataSize = 0;
    info.pInitialData = nullptr;
    if (create_cache(d, &info, nullptr, &p->cache) != VK_SUCCESS) return nullptr;
  }
  log::info("vulkan", "Pipeline cache: {} KiB from {}", bytes.size() >> 10, paths::utf8(p->file.filename()));
  auto& w = writer();
  std::lock_guard lock(w.lock);
  w.caches.push_back(p);
  return p;
}
void changed(PersistentCache& p) {
  p.changed_ns = monotonic_ns();
  p.dirty = true;
}
std::shared_ptr<const Device> device(VkDevice d) {
  auto& s = state(); std::lock_guard lock(s.mutex);
  const auto it = s.devices.find(d);
  return it == s.devices.end() ? nullptr : it->second;
}
// The device of a command buffer: dispatchable handles share their device's
// loader dispatch table, which is their first word.
std::shared_ptr<const Device> command_device(VkCommandBuffer command_buffer) {
  thread_local void* key = nullptr;
  thread_local std::weak_ptr<const Device> cached;
  const auto table = *reinterpret_cast<void* const*>(command_buffer);
  if (table == key)
    if (auto d = cached.lock()) return d;
  auto& s = state(); std::lock_guard lock(s.mutex);
  for (const auto& [handle, d] : s.devices)
    if (*reinterpret_cast<void* const*>(handle) == table) {
      key = table;
      cached = d;
      return d;
    }
  return nullptr;
}

template<class Info>
std::vector<profile::PipelineFeedback> create_traced(const Device& d, profile::PipelineSource source,
    VkResult (VKAPI_PTR* fn)(VkDevice, VkPipelineCache, std::uint32_t, const Info*, const VkAllocationCallbacks*, VkPipeline*),
    VkDevice handle, VkPipelineCache cache, std::uint32_t count, const Info* infos,
    const VkAllocationCallbacks* allocator, VkPipeline* out, VkResult& result) {
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
  std::shared_lock<std::shared_mutex> use;
  if (d.persistent) {
    use = std::shared_lock(d.persistent->use);
    if (d.persistent->alive) cache = d.persistent->cache;
  }
  std::vector<profile::PipelineFeedback> measured(feedback.size());
  std::int64_t start;
  std::uint64_t duration;
  {
    profile::Activity activity(profile::pipeline_create);
    start = monotonic_ns();
    result = fn(handle, cache, count, copies.empty() ? infos : copies.data(), allocator, out);
    duration = static_cast<std::uint64_t>(monotonic_ns() - start);
  }
  if (use.owns_lock()) use.unlock();
  for (std::size_t i = 0; i < feedback.size(); ++i)
    if (const auto* f = feedback[i]) {
      // Invalid feedback leaves duration undefined, including caller-owned outputs.
      measured[i] = {(f->flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT) ? f->duration : 0, f->flags};
    }
  profile::record_pipeline(source, std::is_same_v<Info, VkComputePipelineCreateInfo>, count, start, duration, measured);
  if (d.persistent) {
    // Without feedback every creation may have added to the cache.
    const bool all_hit = !measured.empty() && std::all_of(measured.begin(), measured.end(), [](const auto& m) {
      return (m.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT) != 0;
    });
    if (!all_hit) changed(*d.persistent);
  }
  if constexpr (std::is_same_v<Info, VkGraphicsPipelineCreateInfo>) {
    // The shader modules, layout and render pass behind each guest pipeline:
    // how many distinct stages its pipelines share bounds what per-stage
    // pipeline libraries could reuse.
    if (infos && profile::compile_trace_enabled() &&
        (source == profile::PipelineSource::guest || source == profile::PipelineSource::async)) {
      std::string extra = "\"pipelines\":[";
      for (std::uint32_t i = 0; i < count; ++i) {
        extra += i ? ",[" : "[";
        for (std::uint32_t j = 0; j < infos[i].stageCount && infos[i].pStages; ++j)
          extra += std::format("{}[{},{}]", j ? "," : "", static_cast<std::uint32_t>(infos[i].pStages[j].stage),
                               reinterpret_cast<std::uint64_t>(infos[i].pStages[j].module));
        extra += "]";
      }
      extra += "],\"objects\":[";
      for (std::uint32_t i = 0; i < count; ++i)
        extra += std::format("{}[{},{},{}]", i ? "," : "", reinterpret_cast<std::uint64_t>(infos[i].layout),
                             reinterpret_cast<std::uint64_t>(infos[i].renderPass), infos[i].subpass);
      extra += "]";
      profile::trace_compile("vk_pipeline_stages", start, 0, count, 0, extra);
    }
  }
  if (out && profile::compile_trace_enabled() && result >= VK_SUCCESS) {
    auto& s = state(); std::lock_guard lock(s.created_mutex);
    for (std::uint32_t i = 0; i < count; ++i)
      if (out[i]) s.created[reinterpret_cast<std::uint64_t>(out[i])] = start + static_cast<std::int64_t>(duration);
  }
  return measured;
}

// ---------------------------------------------------------------------------
// Background graphics pipeline creation.
//
// Engines create most pipelines well before they draw with them: UE4's
// pipeline cache precompiles its recorded states in batches on the RHI
// thread at load, Unity warms its variants. On a Quest the driver's cache
// makes that cheap; here the first launch compiles every one, in series on
// the game's render thread. A guest graphics pipeline is therefore returned
// at once as a proxy handle and created by a pool of workers from a deep
// copy of its description, as DXVK and VKD3D-Proton compile off the
// submitting thread. Every command that takes a pipeline is interposed
// below Venture's layers (raw driver calls included, since they resolve
// through this loader): binding one that is not ready yet creates it on the
// binding thread if no worker has started it, or waits for that worker, so
// a draw never sees a missing pipeline. Objects a pending creation still
// reads (shader modules, layouts, render passes) are destroyed after it.
//
// Creation stays synchronous for anything this cannot reproduce exactly:
// guest allocation callbacks, creation feedback the game reads, pipeline
// libraries, failure-sensitive flags, structures outside the copied set,
// and devices with extensions whose commands take pipelines (ray tracing,
// device-generated commands, executable properties, pipeline binaries).
// VENTURE_VK_ASYNC_PIPELINES=0 disables it; VENTURE_VK_ASYNC_WORKERS sets the
// worker count. Creation is CPU-bound in the driver and a first launch can
// queue thousands, so the default is one worker per logical processor but
// one (2 to 32), at below-normal priority (first launch, frames over 25 ms,
// 6 -> 24 workers: RE4 12.5 -> 7.7 s, AW2 18.1 -> 11.3 s, AC Nexus 19.1 ->
// 15.5 s; peak memory +0.2 GiB).
bool async_enabled() {
  static const bool enabled = [] {
    const auto* e = environment::getenv("VENTURE_VK_ASYNC_PIPELINES");
    return !e || std::string_view(e) != "0";
  }();
  return enabled;
}

class Arena {
 public:
  void* allocate(std::size_t size) {
    blocks_.emplace_back(new std::max_align_t[(size + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t)]);
    return blocks_.back().get();
  }
  template<class T> T* copy(const T* source, std::size_t count) {
    if (!source || !count) return nullptr;
    auto* out = static_cast<T*>(allocate(sizeof(T) * count));
    std::memcpy(static_cast<void*>(out), source, sizeof(T) * count);
    return out;
  }
  const char* copy_string(const char* source) {
    if (!source) return nullptr;
    return copy(source, std::strlen(source) + 1);
  }
 private:
  std::vector<std::unique_ptr<std::max_align_t[]>> blocks_;
};

constexpr VkPipelineCreateFlags2 kAsyncFlags =
    VK_PIPELINE_CREATE_2_DISABLE_OPTIMIZATION_BIT | VK_PIPELINE_CREATE_2_ALLOW_DERIVATIVES_BIT |
    VK_PIPELINE_CREATE_2_DERIVATIVE_BIT | VK_PIPELINE_CREATE_2_VIEW_INDEX_FROM_DEVICE_INDEX_BIT |
    VK_PIPELINE_CREATE_2_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR |
    VK_PIPELINE_CREATE_2_RENDERING_FRAGMENT_DENSITY_MAP_ATTACHMENT_BIT_EXT |
    VK_PIPELINE_CREATE_2_COLOR_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT |
    VK_PIPELINE_CREATE_2_DEPTH_STENCIL_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT |
    VK_PIPELINE_CREATE_2_NO_PROTECTED_ACCESS_BIT | VK_PIPELINE_CREATE_2_PROTECTED_ACCESS_ONLY_BIT;

// Copies an extension chain of a graphics pipeline description. Only
// structures listed here are copied; any other makes the creation synchronous.
bool copy_chain(Arena& arena, const void*& next) {
  VkBaseOutStructure* head = nullptr;
  VkBaseOutStructure* tail = nullptr;
  unsigned length = 0;
  for (auto* in = static_cast<const VkBaseInStructure*>(next); in; in = in->pNext) {
    if (++length > 32) return false;
    VkBaseOutStructure* out = nullptr;
    auto plain = [&](auto* typed) { out = reinterpret_cast<VkBaseOutStructure*>(arena.copy(typed, 1)); };
    switch (in->sType) {
      case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT:
        plain(reinterpret_cast<const VkPipelineRasterizationDepthClipStateCreateInfoEXT*>(in)); break;
      case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_LINE_STATE_CREATE_INFO:
        plain(reinterpret_cast<const VkPipelineRasterizationLineStateCreateInfo*>(in)); break;
      case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT:
        plain(reinterpret_cast<const VkPipelineRasterizationProvokingVertexStateCreateInfoEXT*>(in)); break;
      case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_CONSERVATIVE_STATE_CREATE_INFO_EXT:
        plain(reinterpret_cast<const VkPipelineRasterizationConservativeStateCreateInfoEXT*>(in)); break;
      case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_STREAM_CREATE_INFO_EXT:
        plain(reinterpret_cast<const VkPipelineRasterizationStateStreamCreateInfoEXT*>(in)); break;
      case VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT:
        plain(reinterpret_cast<const VkPipelineViewportDepthClipControlCreateInfoEXT*>(in)); break;
      case VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_DOMAIN_ORIGIN_STATE_CREATE_INFO:
        plain(reinterpret_cast<const VkPipelineTessellationDomainOriginStateCreateInfo*>(in)); break;
      case VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_ADVANCED_STATE_CREATE_INFO_EXT:
        plain(reinterpret_cast<const VkPipelineColorBlendAdvancedStateCreateInfoEXT*>(in)); break;
      case VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO:
        plain(reinterpret_cast<const VkPipelineShaderStageRequiredSubgroupSizeCreateInfo*>(in)); break;
      case VK_STRUCTURE_TYPE_PIPELINE_ROBUSTNESS_CREATE_INFO:
        plain(reinterpret_cast<const VkPipelineRobustnessCreateInfo*>(in)); break;
      case VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR:
        plain(reinterpret_cast<const VkPipelineFragmentShadingRateStateCreateInfoKHR*>(in)); break;
      case VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO: {
        const auto* flags = reinterpret_cast<const VkPipelineCreateFlags2CreateInfo*>(in);
        if (flags->flags & ~kAsyncFlags) return false;
        plain(flags);
        break;
      }
      case VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO: {
        auto* copy = arena.copy(reinterpret_cast<const VkPipelineRenderingCreateInfo*>(in), 1);
        copy->pColorAttachmentFormats = arena.copy(copy->pColorAttachmentFormats, copy->colorAttachmentCount);
        out = reinterpret_cast<VkBaseOutStructure*>(copy);
        break;
      }
      case VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO: {
        auto* copy = arena.copy(reinterpret_cast<const VkPipelineVertexInputDivisorStateCreateInfo*>(in), 1);
        copy->pVertexBindingDivisors = arena.copy(copy->pVertexBindingDivisors, copy->vertexBindingDivisorCount);
        out = reinterpret_cast<VkBaseOutStructure*>(copy);
        break;
      }
      case VK_STRUCTURE_TYPE_PIPELINE_COLOR_WRITE_CREATE_INFO_EXT: {
        auto* copy = arena.copy(reinterpret_cast<const VkPipelineColorWriteCreateInfoEXT*>(in), 1);
        copy->pColorWriteEnables = arena.copy(copy->pColorWriteEnables, copy->attachmentCount);
        out = reinterpret_cast<VkBaseOutStructure*>(copy);
        break;
      }
      case VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO: {
        auto* copy = arena.copy(reinterpret_cast<const VkShaderModuleCreateInfo*>(in), 1);
        if (copy->codeSize % 4) return false;
        copy->pCode = arena.copy(copy->pCode, copy->codeSize / 4);
        out = reinterpret_cast<VkBaseOutStructure*>(copy);
        break;
      }
      default:
        return false;
    }
    out->pNext = nullptr;
    if (tail) tail->pNext = out; else head = out;
    tail = out;
  }
  next = head;
  return true;
}
template<class T> bool copy_state(Arena& arena, const T*& state) {
  if (!state) return true;
  auto* copy = arena.copy(state, 1);
  if (!copy_chain(arena, copy->pNext)) return false;
  state = copy;
  return true;
}
enum class RefType : std::uint8_t { shader_module, pipeline_layout, render_pass };
using Ref = std::pair<RefType, std::uint64_t>;
struct GraphicsCopy {
  Arena arena;
  VkGraphicsPipelineCreateInfo info{};
  std::vector<Ref> refs;
};
VkPipeline resolve(VkPipeline pipeline, const char* via = "other");
bool is_proxy(VkPipeline pipeline) noexcept;
std::unique_ptr<GraphicsCopy> copy_graphics(const VkGraphicsPipelineCreateInfo& in) {
  if (in.sType != VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO) return nullptr;
  if (VkPipelineCreateFlags2(in.flags) & ~kAsyncFlags) return nullptr;
  if ((in.flags & VK_PIPELINE_CREATE_DERIVATIVE_BIT) && in.basePipelineIndex != -1 && !in.basePipelineHandle) return nullptr;
  auto c = std::make_unique<GraphicsCopy>();
  auto& a = c->arena;
  auto& info = c->info;
  info = in;
  if (!copy_chain(a, info.pNext)) return nullptr;
  if (info.basePipelineHandle && is_proxy(info.basePipelineHandle)) info.basePipelineHandle = resolve(info.basePipelineHandle);
  if (info.stageCount > 8 || !info.pStages) return nullptr;
  auto* stages = a.copy(info.pStages, info.stageCount);
  for (std::uint32_t i = 0; i < info.stageCount; ++i) {
    auto& stage = stages[i];
    if (!copy_chain(a, stage.pNext)) return nullptr;
    stage.pName = a.copy_string(stage.pName);
    if (stage.pSpecializationInfo) {
      auto* special = a.copy(stage.pSpecializationInfo, 1);
      special->pMapEntries = a.copy(special->pMapEntries, special->mapEntryCount);
      special->pData = a.copy(static_cast<const std::byte*>(special->pData), special->dataSize);
      stage.pSpecializationInfo = special;
    }
    if (stage.module) c->refs.emplace_back(RefType::shader_module, reinterpret_cast<std::uint64_t>(stage.module));
    else if (!stage.pNext) return nullptr;  // a module identifier or an unsupported source
  }
  info.pStages = stages;
  bool dynamic_viewport = false, dynamic_scissor = false, dynamic_sample_mask = false;
  if (info.pDynamicState) {
    auto* dynamic = a.copy(info.pDynamicState, 1);
    if (!copy_chain(a, dynamic->pNext)) return nullptr;
    dynamic->pDynamicStates = a.copy(dynamic->pDynamicStates, dynamic->dynamicStateCount);
    for (std::uint32_t i = 0; i < dynamic->dynamicStateCount; ++i) {
      const auto s = dynamic->pDynamicStates[i];
      dynamic_viewport |= s == VK_DYNAMIC_STATE_VIEWPORT || s == VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT;
      dynamic_scissor |= s == VK_DYNAMIC_STATE_SCISSOR || s == VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT;
      dynamic_sample_mask |= s == VK_DYNAMIC_STATE_SAMPLE_MASK_EXT;
    }
    info.pDynamicState = dynamic;
  }
  if (info.pVertexInputState) {
    auto* vertex = a.copy(info.pVertexInputState, 1);
    if (!copy_chain(a, vertex->pNext)) return nullptr;
    vertex->pVertexBindingDescriptions = a.copy(vertex->pVertexBindingDescriptions, vertex->vertexBindingDescriptionCount);
    vertex->pVertexAttributeDescriptions = a.copy(vertex->pVertexAttributeDescriptions, vertex->vertexAttributeDescriptionCount);
    info.pVertexInputState = vertex;
  }
  if (!copy_state(a, info.pInputAssemblyState) || !copy_state(a, info.pTessellationState) ||
      !copy_state(a, info.pRasterizationState) || !copy_state(a, info.pDepthStencilState))
    return nullptr;
  if (info.pViewportState) {
    auto* viewport = a.copy(info.pViewportState, 1);
    if (!copy_chain(a, viewport->pNext)) return nullptr;
    // Arrays of dynamic state are ignored and need not be valid.
    viewport->pViewports = dynamic_viewport ? nullptr : a.copy(viewport->pViewports, viewport->viewportCount);
    viewport->pScissors = dynamic_scissor ? nullptr : a.copy(viewport->pScissors, viewport->scissorCount);
    info.pViewportState = viewport;
  }
  if (info.pMultisampleState) {
    auto* multisample = a.copy(info.pMultisampleState, 1);
    if (!copy_chain(a, multisample->pNext)) return nullptr;
    multisample->pSampleMask = dynamic_sample_mask ? nullptr
        : a.copy(multisample->pSampleMask, (std::size_t(multisample->rasterizationSamples) + 31) / 32);
    info.pMultisampleState = multisample;
  }
  if (info.pColorBlendState) {
    auto* blend = a.copy(info.pColorBlendState, 1);
    if (!copy_chain(a, blend->pNext)) return nullptr;
    blend->pAttachments = a.copy(blend->pAttachments, blend->attachmentCount);
    info.pColorBlendState = blend;
  }
  if (info.layout) c->refs.emplace_back(RefType::pipeline_layout, reinterpret_cast<std::uint64_t>(info.layout));
  if (info.renderPass) c->refs.emplace_back(RefType::render_pass, reinterpret_cast<std::uint64_t>(info.renderPass));
  return c;
}

enum ProxyState : std::uint32_t { free_slot, queued, running, done, failed };
struct Job {
  std::shared_ptr<const Device> device;
  std::unique_ptr<GraphicsCopy> copy;
};
struct Proxy {
  std::atomic<std::uint32_t> state{free_slot};
  std::atomic<VkPipeline> real{VK_NULL_HANDLE};
  bool destroy_requested = false;  // under Async::mutex
  std::unique_ptr<Job> job;        // until it runs
  Proxy* next_free = nullptr;
};
// Proxy handles point into one array, so recognizing one is a range check.
// Guest pipelines live until the guest destroys them; at most this many
// background-created ones can be alive, later ones are created at once.
constexpr std::size_t kProxies = 1u << 16;
struct Async {
  std::mutex mutex;
  std::condition_variable work;
  std::deque<Proxy*> queue;
  // Creations a thread waits for go first. Entries may be stale (started,
  // or the slot reused): a worker claims only a proxy still queued.
  std::deque<Proxy*> urgent;
  Proxy* free_list = nullptr;
  std::size_t used = 0;
  unsigned workers = 0;
  std::map<Ref, std::size_t> pending_refs;  // objects pending creations read
  std::map<Ref, std::vector<std::function<void()>>> deferred_destroys;
  std::map<VkDevice, std::size_t> pending_per_device;
  std::condition_variable drained;
  std::uint64_t submitted = 0, inline_runs = 0, waits = 0, synchronous = 0;
};
Proxy* proxy_array() { static auto* p = new Proxy[kProxies]; return p; }
Async& async_state() { static auto* a = new Async; return *a; }
bool is_proxy(VkPipeline pipeline) noexcept {
  const auto value = reinterpret_cast<std::uintptr_t>(pipeline);
  const auto base = reinterpret_cast<std::uintptr_t>(proxy_array());
  return value >= base && value < base + kProxies * sizeof(Proxy) && (value - base) % sizeof(Proxy) == 0;
}
Proxy& proxy(VkPipeline pipeline) { return *reinterpret_cast<Proxy*>(pipeline); }
VkPipeline handle(Proxy& p) { return reinterpret_cast<VkPipeline>(&p); }
// Under Async::mutex.
void release_proxy(Async& a, Proxy& p) {
  p.job.reset();
  p.real.store(VK_NULL_HANDLE, std::memory_order_relaxed);
  p.destroy_requested = false;
  p.state.store(free_slot, std::memory_order_release);
  p.next_free = a.free_list;
  a.free_list = &p;
}
// Under Async::mutex. Returns the destroys that were waiting for these objects.
std::vector<std::function<void()>> release_refs(Async& a, const std::vector<Ref>& refs) {
  std::vector<std::function<void()>> ready;
  for (const auto& ref : refs) {
    auto it = a.pending_refs.find(ref);
    if (it == a.pending_refs.end() || --it->second) continue;
    a.pending_refs.erase(it);
    if (auto d = a.deferred_destroys.find(ref); d != a.deferred_destroys.end()) {
      for (auto& f : d->second) ready.push_back(std::move(f));
      a.deferred_destroys.erase(d);
    }
  }
  return ready;
}
// Under Async::mutex: one background creation on the device is completely done,
// including the destroys it ran. drain() returns, and the device may be destroyed,
// only once no queued or running creation can call into it again.
void finished_on_device(Async& a, VkDevice d) {
  if (auto it = a.pending_per_device.find(d); it != a.pending_per_device.end() && !--it->second) {
    a.pending_per_device.erase(it);
    a.drained.notify_all();
  }
}
// Creates the pipeline of a proxy this thread claimed (state running).
void run(Proxy& p, const char* trace_kind, const char* via = nullptr) {
  auto& a = async_state();
  std::unique_ptr<Job> job;
  {
    std::lock_guard lock(a.mutex);
    job = std::move(p.job);
  }
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkResult result = VK_ERROR_INITIALIZATION_FAILED;
  const auto start = monotonic_ns();
  try {
    auto& d = *job->device;
    create_traced(d, profile::PipelineSource::async, d.graphics, d.handle, VK_NULL_HANDLE, 1,
                  &job->copy->info, nullptr, &pipeline, result);
  } catch (...) {
    result = VK_ERROR_OUT_OF_HOST_MEMORY;
  }
  if (trace_kind && profile::compile_trace_enabled())
    profile::trace_compile(trace_kind, start, static_cast<std::uint64_t>(monotonic_ns() - start), 1, 0,
                           via ? std::string("\"via\":\"") + via + "\"" : std::string());
  if (result < VK_SUCCESS) {
    log::error("vulkan", "Background pipeline creation failed ({}); draws with it are skipped", static_cast<int>(result));
    pipeline = VK_NULL_HANDLE;
  }
  std::vector<std::function<void()>> destroys;
  bool destroy_now = false;
  {
    std::lock_guard lock(a.mutex);
    p.real.store(pipeline, std::memory_order_relaxed);
    p.state.store(pipeline ? done : failed, std::memory_order_release);
    destroys = release_refs(a, job->copy->refs);
    destroy_now = p.destroy_requested;
    if (destroy_now) release_proxy(a, p);
  }
  p.state.notify_all();
  if (destroy_now && pipeline && job->device->destroy_pipeline)
    job->device->destroy_pipeline(job->device->handle, pipeline, nullptr);
  for (auto& f : destroys) f();
  // Last: until here this thread still calls the device's destroy entry points.
  std::lock_guard lock(a.mutex);
  finished_on_device(a, job->device->handle);
}
void worker() {
  auto& a = async_state();
  // Below normal: what nothing waits for yet yields to the game's threads;
  // a thread that does wait creates the pipelines itself at its own priority.
  platform::lower_worker_thread_priority();
  for (;;) {
    Proxy* p;
    {
      std::unique_lock lock(a.mutex);
      a.work.wait(lock, [&] { return !a.queue.empty() || !a.urgent.empty(); });
      auto& from = a.urgent.empty() ? a.queue : a.urgent;
      p = from.front();
      from.pop_front();
    }
    auto expected = std::uint32_t(queued);
    if (p->state.compare_exchange_strong(expected, running, std::memory_order_acq_rel)) run(*p, nullptr);
  }
}
// The real pipeline behind a proxy, creating it here if no worker has started.
VkPipeline resolve(VkPipeline pipeline, const char* via) {
  if (!is_proxy(pipeline)) return pipeline;
  auto& p = proxy(pipeline);
  auto s = p.state.load(std::memory_order_acquire);
  if (s == done) return p.real.load(std::memory_order_relaxed);
  if (s == queued) {
    auto expected = std::uint32_t(queued);
    if (p.state.compare_exchange_strong(expected, running, std::memory_order_acq_rel)) {
      { auto& a = async_state(); std::lock_guard lock(a.mutex); ++a.inline_runs; }
      run(p, "vk_async_inline", via);
    }
  }
  s = p.state.load(std::memory_order_acquire);
  if (s == running) {
    { auto& a = async_state(); std::lock_guard lock(a.mutex); ++a.waits; }
    const auto start = monotonic_ns();
    while ((s = p.state.load(std::memory_order_acquire)) == running) p.state.wait(running, std::memory_order_acquire);
    profile::trace_compile("vk_async_wait", start, static_cast<std::uint64_t>(monotonic_ns() - start), 1, 0,
                           profile::compile_trace_enabled() ? std::string("\"via\":\"") + via + "\"" : std::string());
  }
  return s == done ? p.real.load(std::memory_order_relaxed) : VK_NULL_HANDLE;
}
// Returns false when the whole call must be created synchronously.
bool submit(const std::shared_ptr<const Device>& d, VkPipelineCache cache, std::uint32_t count,
            const VkGraphicsPipelineCreateInfo* infos, const VkAllocationCallbacks* allocator, VkPipeline* out) {
  if (!d->async || !async_enabled() || allocator || !count || !infos || !out || count > 64) return false;
  // Workers create with the persistent cache. Without it, the game's own
  // cache (possibly externally synchronized, and what a later launch of the
  // game reloads) is used on its own thread.
  if (cache && !d->persistent) return false;
  std::vector<std::unique_ptr<GraphicsCopy>> copies;
  copies.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    auto c = copy_graphics(infos[i]);
    if (!c) return false;
    copies.push_back(std::move(c));
  }
  auto& a = async_state();
  std::lock_guard lock(a.mutex);
  if (a.used + count > kProxies && !a.free_list) return false;
  if (!a.workers) {
    const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
    a.workers = std::clamp(cores - 1, 2u, 32u);
    if (const auto* e = environment::getenv("VENTURE_VK_ASYNC_WORKERS"))
      a.workers = std::clamp(unsigned(std::strtoul(e, nullptr, 10)), 1u, 64u);
    for (unsigned i = 0; i < a.workers; ++i) std::thread(worker).detach();
    log::info("vulkan", "Background pipeline creation: {} workers", a.workers);
  }
  std::vector<Proxy*> taken;
  for (std::uint32_t i = 0; i < count; ++i) {
    Proxy* p = a.free_list;
    if (p) a.free_list = p->next_free;
    else if (a.used < kProxies) p = &proxy_array()[a.used++];
    if (!p) {
      for (auto* t : taken) { t->next_free = a.free_list; a.free_list = t; }
      return false;
    }
    taken.push_back(p);
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    auto& p = *taken[i];
    for (const auto& ref : copies[i]->refs) ++a.pending_refs[ref];
    p.job = std::make_unique<Job>(Job{d, std::move(copies[i])});
    p.destroy_requested = false;
    p.state.store(queued, std::memory_order_release);
    a.queue.push_back(&p);
    ++a.pending_per_device[d->handle];
    out[i] = handle(p);
  }
  a.submitted += count;
  a.work.notify_all();
  return true;
}
// Waits for every background creation on the device (before it is destroyed).
void drain(VkDevice d) {
  auto& a = async_state();
  std::vector<Proxy*> mine;
  {
    std::lock_guard lock(a.mutex);
    for (auto* p : a.queue)
      if (p->job && p->job->device->handle == d) mine.push_back(p);
  }
  for (auto* p : mine) resolve(handle(*p), "drain");
  std::unique_lock lock(a.mutex);
  a.drained.wait(lock, [&] { return !a.pending_per_device.contains(d); });
}
// A destroy of an object pending creations still read runs after them.
template<class Fn, class Handle>
bool defer_destroy(RefType type, Handle object, Fn fn, VkDevice d, const VkAllocationCallbacks* allocator) {
  if (!object) return false;
  auto& a = async_state();
  const Ref ref{type, reinterpret_cast<std::uint64_t>(object)};
  {
    std::lock_guard lock(a.mutex);
    if (!a.pending_refs.contains(ref)) return false;
    if (!allocator) {
      a.deferred_destroys[ref].push_back([=] { fn(d, object, nullptr); });
      return true;
    }
  }
  // Guest allocation callbacks run on the destroying thread: finish first.
  drain(d);
  return false;
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
    if (d->source == profile::PipelineSource::guest) {
      const auto start = monotonic_ns();
      if (submit(d, cache, count, infos, a, out)) {
        profile::trace_compile("vk_async_submit", start, static_cast<std::uint64_t>(monotonic_ns() - start), count);
        return VK_SUCCESS;
      }
      if (d->async && async_enabled()) { auto& s = async_state(); std::lock_guard lock(s.mutex); s.synchronous += count; }
    }
    // A derivative of a background-created pipeline names the real one.
    std::vector<VkGraphicsPipelineCreateInfo> resolved;
    for (std::uint32_t i = 0; infos && i < count; ++i)
      if (infos[i].basePipelineHandle && is_proxy(infos[i].basePipelineHandle)) {
        if (resolved.empty()) resolved.assign(infos, infos + count);
        resolved[i].basePipelineHandle = resolve(infos[i].basePipelineHandle, "base");
      }
    VkResult result;
    create_traced(*d, d->source, d->graphics, handle, cache, count, resolved.empty() ? infos : resolved.data(), a, out, result);
    return result;
  });
}
VkResult VKAPI_CALL compute(VkDevice handle, VkPipelineCache cache, std::uint32_t count,
                            const VkComputePipelineCreateInfo* infos, const VkAllocationCallbacks* a, VkPipeline* out) {
  return guarded([&] {
    const auto d = device(handle);
    if (!d || !d->compute) return VK_ERROR_INITIALIZATION_FAILED;
    std::vector<VkComputePipelineCreateInfo> resolved;
    for (std::uint32_t i = 0; infos && i < count; ++i)
      if (infos[i].basePipelineHandle && is_proxy(infos[i].basePipelineHandle)) {
        if (resolved.empty()) resolved.assign(infos, infos + count);
        resolved[i].basePipelineHandle = resolve(infos[i].basePipelineHandle, "base");
      }
    VkResult result;
    create_traced(*d, d->source, d->compute, handle, cache, count, resolved.empty() ? infos : resolved.data(), a, out, result);
    return result;
  });
}
VkResult VKAPI_CALL create_cache(VkDevice handle, const VkPipelineCacheCreateInfo* info,
                                 const VkAllocationCallbacks* a, VkPipelineCache* out) {
  return guarded([&] {
    const auto d = device(handle);
    if (!d || !d->create_cache) return VK_ERROR_INITIALIZATION_FAILED;
    const auto result = d->create_cache(handle, info, a, out);
    // A cache the game saved from an earlier run joins the persistent one
    // its pipelines are created with.
    if (result == VK_SUCCESS && d->persistent && info && info->initialDataSize) {
      std::unique_lock use(d->persistent->use);
      if (d->persistent->alive && d->persistent->merge(handle, d->persistent->cache, 1, out) == VK_SUCCESS)
        changed(*d->persistent);
    }
    return result;
  });
}
std::atomic<PFN_vkCmdBindPipeline> common_bind{nullptr};
std::atomic<bool> mixed_bind{false};
void VKAPI_CALL bind(VkCommandBuffer command_buffer, VkPipelineBindPoint point, VkPipeline pipeline) {
  if (profile::compile_trace_enabled()) {
    // The time from a pipeline's creation to its first bind bounds what
    // creating it in the background can hide.
    std::int64_t created = 0;
    {
      auto& s = state(); std::lock_guard lock(s.created_mutex);
      if (const auto it = s.created.find(reinterpret_cast<std::uint64_t>(pipeline)); it != s.created.end()) {
        created = it->second;
        s.created.erase(it);
      }
    }
    if (created)
      profile::trace_compile("vk_first_bind_gap", created, static_cast<std::uint64_t>(std::max<std::int64_t>(0, monotonic_ns() - created)));
  }
  if (is_proxy(pipeline)) {
    try { pipeline = resolve(pipeline, "bind"); } catch (...) { pipeline = VK_NULL_HANDLE; }
    if (!pipeline) return;  // its creation failed and was logged
  }
  // Every device normally resolves the same driver entry point; only a mix
  // of drivers needs the command buffer's device.
  if (auto common = common_bind.load(std::memory_order_acquire)) return common(command_buffer, point, pipeline);
  const auto d = command_device(command_buffer);
  if (d && d->bind) d->bind(command_buffer, point, pipeline);
}
void VKAPI_CALL destroy_pipeline(VkDevice handle, VkPipeline pipeline, const VkAllocationCallbacks* allocator) {
  const auto d = device(handle);
  if (!d || !d->destroy_pipeline) return;
  if (!is_proxy(pipeline)) return d->destroy_pipeline(handle, pipeline, allocator);
  auto& a = async_state();
  auto& p = proxy(pipeline);
  VkPipeline real = VK_NULL_HANDLE;
  {
    std::lock_guard lock(a.mutex);
    switch (p.state.load(std::memory_order_acquire)) {
      case queued: {
        // Never started: drop the work.
        auto expected = std::uint32_t(queued);
        if (p.state.compare_exchange_strong(expected, running)) {
          std::erase(a.queue, &p);
          std::vector<std::function<void()>> destroys;
          const VkDevice owner = p.job ? p.job->device->handle : VK_NULL_HANDLE;
          if (p.job) destroys = release_refs(a, p.job->copy->refs);
          release_proxy(a, p);
          // Run outside the lock below.
          if (!destroys.empty()) {
            a.mutex.unlock();
            for (auto& f : destroys) f();
            a.mutex.lock();
          }
          if (owner) finished_on_device(a, owner);
          return;
        }
        [[fallthrough]];
      }
      case running:
        p.destroy_requested = true;  // the worker destroys it when done
        return;
      case done:
        real = p.real.load(std::memory_order_relaxed);
        release_proxy(a, p);
        break;
      default:
        release_proxy(a, p);
        return;
    }
  }
  if (real) d->destroy_pipeline(handle, real, allocator);
}
#define VENTURE_DEFERRED_DESTROY(name, Handle, type, member)                                         \
  void VKAPI_CALL name(VkDevice handle, Handle object, const VkAllocationCallbacks* allocator) {      \
    const auto d = device(handle);                                                                   \
    if (!d || !d->member) return;                                                                    \
    if (defer_destroy(type, object, d->member, handle, allocator)) return;                           \
    d->member(handle, object, allocator);                                                            \
  }
VENTURE_DEFERRED_DESTROY(destroy_module, VkShaderModule, RefType::shader_module, destroy_module)
VENTURE_DEFERRED_DESTROY(destroy_layout, VkPipelineLayout, RefType::pipeline_layout, destroy_layout)
VENTURE_DEFERRED_DESTROY(destroy_render_pass, VkRenderPass, RefType::render_pass, destroy_render_pass)
#undef VENTURE_DEFERRED_DESTROY
// Debug names and tags of a background-created pipeline go to the real one.
VkResult VKAPI_CALL set_name(VkDevice handle, const VkDebugUtilsObjectNameInfoEXT* info) {
  const auto d = device(handle);
  if (!d || !d->set_name) return VK_SUCCESS;
  if (!info || info->objectType != VK_OBJECT_TYPE_PIPELINE || !is_proxy(reinterpret_cast<VkPipeline>(info->objectHandle)))
    return d->set_name(handle, info);
  auto copy = *info;
  copy.objectHandle = reinterpret_cast<std::uint64_t>(resolve(reinterpret_cast<VkPipeline>(info->objectHandle), "name"));
  return copy.objectHandle ? d->set_name(handle, &copy) : VK_SUCCESS;
}
VkResult VKAPI_CALL set_tag(VkDevice handle, const VkDebugUtilsObjectTagInfoEXT* info) {
  const auto d = device(handle);
  if (!d || !d->set_tag) return VK_SUCCESS;
  if (!info || info->objectType != VK_OBJECT_TYPE_PIPELINE || !is_proxy(reinterpret_cast<VkPipeline>(info->objectHandle)))
    return d->set_tag(handle, info);
  auto copy = *info;
  copy.objectHandle = reinterpret_cast<std::uint64_t>(resolve(reinterpret_cast<VkPipeline>(info->objectHandle), "name"));
  return copy.objectHandle ? d->set_tag(handle, &copy) : VK_SUCCESS;
}
VkResult VKAPI_CALL marker_name(VkDevice handle, const VkDebugMarkerObjectNameInfoEXT* info) {
  const auto d = device(handle);
  if (!d || !d->marker_name) return VK_SUCCESS;
  if (!info || info->objectType != VK_DEBUG_REPORT_OBJECT_TYPE_PIPELINE_EXT || !is_proxy(reinterpret_cast<VkPipeline>(info->object)))
    return d->marker_name(handle, info);
  auto copy = *info;
  copy.object = reinterpret_cast<std::uint64_t>(resolve(reinterpret_cast<VkPipeline>(info->object), "name"));
  return copy.object ? d->marker_name(handle, &copy) : VK_SUCCESS;
}
VkResult VKAPI_CALL marker_tag(VkDevice handle, const VkDebugMarkerObjectTagInfoEXT* info) {
  const auto d = device(handle);
  if (!d || !d->marker_tag) return VK_SUCCESS;
  if (!info || info->objectType != VK_DEBUG_REPORT_OBJECT_TYPE_PIPELINE_EXT || !is_proxy(reinterpret_cast<VkPipeline>(info->object)))
    return d->marker_tag(handle, info);
  auto copy = *info;
  copy.object = reinterpret_cast<std::uint64_t>(resolve(reinterpret_cast<VkPipeline>(info->object), "name"));
  return copy.object ? d->marker_tag(handle, &copy) : VK_SUCCESS;
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
bool pipeline_pending(VkPipeline pipeline) noexcept {
  if (!is_proxy(pipeline)) return false;
  const auto s = proxy(pipeline).state.load(std::memory_order_acquire);
  return s == queued || s == running;
}
void prioritize_pipeline(VkPipeline pipeline) {
  if (!is_proxy(pipeline)) return;
  auto& a = async_state();
  std::lock_guard lock(a.mutex);
  if (proxy(pipeline).state.load(std::memory_order_acquire) != queued) return;
  a.urgent.push_back(&proxy(pipeline));
  a.work.notify_one();
}
void await_pipelines(std::span<const VkPipeline> pipelines) {
  // Workers take these before creations nothing waits for yet; this thread
  // creates the ones no worker has started, then waits for the rest.
  {
    auto& a = async_state();
    std::lock_guard lock(a.mutex);
    for (const auto pipeline : pipelines)
      if (is_proxy(pipeline) && proxy(pipeline).state.load(std::memory_order_acquire) == queued)
        a.urgent.push_back(&proxy(pipeline));
    a.work.notify_all();
  }
  // From the end: workers take the list from its start.
  for (auto it = pipelines.rbegin(); it != pipelines.rend(); ++it) {
    const auto pipeline = *it;
    if (!is_proxy(pipeline)) continue;
    auto& p = proxy(pipeline);
    auto expected = std::uint32_t(queued);
    if (p.state.compare_exchange_strong(expected, running, std::memory_order_acq_rel)) {
      { auto& a = async_state(); std::lock_guard lock(a.mutex); ++a.inline_runs; }
      run(p, "vk_async_inline", "flush");
    }
  }
  for (const auto pipeline : pipelines) resolve(pipeline, "flush");
}
PFN_vkGetInstanceProcAddr wrap_pipeline_loader(PFN_vkGetInstanceProcAddr raw) {
  if (raw == gipa) return raw;
  state().loader = raw;
  return gipa;
}
PFN_vkGetInstanceProcAddr unwrap_pipeline_loader(PFN_vkGetInstanceProcAddr fn) { return fn == gipa ? state().loader.load() : fn; }
bool async_pipeline_extensions_allowed(const std::set<std::string_view>& enabled) {
  // Extensions whose commands or structures take pipelines this layer does
  // not translate from proxies.
  static constexpr std::string_view blocked[]{
      "VK_KHR_ray_tracing_pipeline", "VK_NV_ray_tracing", "VK_KHR_pipeline_executable_properties",
      "VK_EXT_pipeline_properties", "VK_AMD_shader_info", "VK_NV_device_generated_commands",
      "VK_NV_device_generated_commands_compute", "VK_EXT_device_generated_commands", "VK_HUAWEI_subpass_shading",
      "VK_HUAWEI_cluster_culling_shader", "VK_AMDX_shader_enqueue", "VK_KHR_pipeline_binary", "VK_ARM_data_graph",
      "VK_NV_cuda_kernel_launch"};
  for (const auto b : blocked)
    if (enabled.contains(b)) return false;
  return true;
}
void register_pipeline_device(VkDevice d, PFN_vkGetDeviceProcAddr raw, bool feedback, profile::PipelineSource source,
                              bool async) {
  profile::start_pipeline_telemetry();
  const auto create_cache = reinterpret_cast<PFN_vkCreatePipelineCache>(raw(d, "vkCreatePipelineCache"));
  auto entry = std::make_shared<Device>(Device{
      d, raw,
      reinterpret_cast<PFN_vkCreateGraphicsPipelines>(raw(d, "vkCreateGraphicsPipelines")),
      reinterpret_cast<PFN_vkCreateComputePipelines>(raw(d, "vkCreateComputePipelines")), create_cache,
      reinterpret_cast<PFN_vkCmdBindPipeline>(raw(d, "vkCmdBindPipeline")),
      reinterpret_cast<PFN_vkDestroyPipeline>(raw(d, "vkDestroyPipeline")),
      reinterpret_cast<PFN_vkDestroyShaderModule>(raw(d, "vkDestroyShaderModule")),
      reinterpret_cast<PFN_vkDestroyPipelineLayout>(raw(d, "vkDestroyPipelineLayout")),
      reinterpret_cast<PFN_vkDestroyRenderPass>(raw(d, "vkDestroyRenderPass")),
      reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(raw(d, "vkSetDebugUtilsObjectNameEXT")),
      reinterpret_cast<PFN_vkSetDebugUtilsObjectTagEXT>(raw(d, "vkSetDebugUtilsObjectTagEXT")),
      reinterpret_cast<PFN_vkDebugMarkerSetObjectNameEXT>(raw(d, "vkDebugMarkerSetObjectNameEXT")),
      reinterpret_cast<PFN_vkDebugMarkerSetObjectTagEXT>(raw(d, "vkDebugMarkerSetObjectTagEXT")),
      feedback, async && source == profile::PipelineSource::guest, source,
      open_persistent(d, raw, create_cache, source)});
  if (entry->bind && !mixed_bind.load()) {
    PFN_vkCmdBindPipeline expected = nullptr;
    if (!common_bind.compare_exchange_strong(expected, entry->bind) && expected != entry->bind) {
      mixed_bind = true;
      common_bind = nullptr;
    }
  }
  if (entry->async && async_enabled()) log::info("vulkan", "Guest graphics pipelines are created in the background");
  auto& s = state(); std::lock_guard lock(s.mutex); s.devices[d] = std::move(entry);
}
void release_pipeline_device(VkDevice d) {
  drain(d);
  std::shared_ptr<const Device> entry;
  {
    auto& s = state(); std::lock_guard lock(s.mutex);
    if (const auto it = s.devices.find(d); it != s.devices.end()) { entry = std::move(it->second); s.devices.erase(it); }
  }
  if (entry && entry->async) {
    auto& a = async_state(); std::lock_guard lock(a.mutex);
    log::info("vulkan", "Background pipelines: {} submitted, {} created at bind, {} waited for, {} created at once",
              a.submitted, a.inline_runs, a.waits, a.synchronous);
  }
  if (entry && entry->persistent) {
    auto& p = *entry->persistent;
    save(p);
    std::unique_lock use(p.use);
    if (p.alive) p.destroy(p.device, p.cache, nullptr);
    p.alive = false;
  }
}
PFN_vkVoidFunction pipeline_proc(std::string_view name) {
  if (name == "vkCreateGraphicsPipelines") return reinterpret_cast<PFN_vkVoidFunction>(graphics);
  if (name == "vkCreateComputePipelines") return reinterpret_cast<PFN_vkVoidFunction>(compute);
  if (name == "vkCreatePipelineCache") return reinterpret_cast<PFN_vkVoidFunction>(create_cache);
  const bool async = async_enabled();
  if (name == "vkCmdBindPipeline" && (async || profile::compile_trace_enabled())) return reinterpret_cast<PFN_vkVoidFunction>(bind);
  if (!async) return nullptr;
  if (name == "vkDestroyPipeline") return reinterpret_cast<PFN_vkVoidFunction>(destroy_pipeline);
  if (name == "vkDestroyShaderModule") return reinterpret_cast<PFN_vkVoidFunction>(destroy_module);
  if (name == "vkDestroyPipelineLayout") return reinterpret_cast<PFN_vkVoidFunction>(destroy_layout);
  if (name == "vkDestroyRenderPass") return reinterpret_cast<PFN_vkVoidFunction>(destroy_render_pass);
  if (name == "vkSetDebugUtilsObjectNameEXT") return reinterpret_cast<PFN_vkVoidFunction>(set_name);
  if (name == "vkSetDebugUtilsObjectTagEXT") return reinterpret_cast<PFN_vkVoidFunction>(set_tag);
  if (name == "vkDebugMarkerSetObjectNameEXT") return reinterpret_cast<PFN_vkVoidFunction>(marker_name);
  if (name == "vkDebugMarkerSetObjectTagEXT") return reinterpret_cast<PFN_vkVoidFunction>(marker_tag);
  return nullptr;
}
VkResult create_decode_pipeline(VkDevice handle, VkPipelineCache cache, std::uint32_t count,
                                const VkComputePipelineCreateInfo* infos, const VkAllocationCallbacks* a, VkPipeline* out) {
  return guarded([&] {
    const auto d = device(handle);
    if (!d || !d->compute) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result;
    create_traced(*d, profile::PipelineSource::decode, d->compute, handle, cache, count, infos, a, out, result);
    return result;
  });
}
}

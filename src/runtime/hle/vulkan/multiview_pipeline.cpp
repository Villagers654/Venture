// A float varying derived solely from ViewIndex is constant over a primitive,
// but perspective interpolation can round 1.0 below 1.0. A fragment shader that
// converts it back to an integer then reads the wrong view's data. Carry proven
// constant per-view scalar values with Flat on both sides of the interface.
// Ordinary attributes, mixed expressions and explicit interpolation are untouched.
#include <algorithm>
#include <atomic>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <tuple>

#include "common/log.h"
#include "internal.h"
#include "multiview.h"

namespace venture::vulkan {
namespace {
template <class F, ProcedureName Name>
F next(VkDevice device) {
  if (auto f = cached_layer_proc<Layer::multiview, Name>()) return reinterpret_cast<F>(f);
  return reinterpret_cast<F>(raw_device_proc(device, Name.value));
}
struct Shader {
  std::vector<std::uint32_t> words;
  std::mutex mutex;
  std::map<std::pair<std::string, bool>, std::vector<VaryingSlot>> interfaces;
  using Key = std::tuple<std::string, bool, std::vector<VaryingSlot>>;
  std::map<Key, std::shared_ptr<const std::vector<std::uint32_t>>> normalized;

  // The guest's code when `words` reads its subpass inputs as sampled images.
  std::vector<std::uint32_t> original;
  // By mask of single-sampled input attachments: the code with those inputs
  // read single-sampled, null when that changes nothing.
  std::map<std::uint32_t, std::shared_ptr<const std::vector<std::uint32_t>>> single;

  explicit Shader(const VkShaderModuleCreateInfo& info)
      : words(info.pCode, info.pCode + info.codeSize / sizeof(std::uint32_t)) {}
  std::shared_ptr<const std::vector<std::uint32_t>> single_sampled(std::uint32_t mask) {
    std::lock_guard lock(mutex);
    if (auto it = single.find(mask); it != single.end()) return it->second;
    auto code = single_sample_subpass_inputs(original.empty() ? words : original, mask);
    // A module whose subpass inputs are sampled images stays one.
    if (!code.empty() && !original.empty()) code = sample_subpass_inputs(code);
    std::shared_ptr<const std::vector<std::uint32_t>> changed;
    if (!code.empty()) {
      changed = std::make_shared<const std::vector<std::uint32_t>>(std::move(code));
      static std::atomic<unsigned> logged{0};
      if (logged.fetch_add(1, std::memory_order_relaxed) < 8)
        log::info("vulkan", "Fragment shader reads a one-sample input attachment as multisampled; read single-sampled");
    }
    single.emplace(mask, changed);
    return changed;
  }
  std::vector<VaryingSlot> slots(const char* entry, bool vertex) {
    std::lock_guard lock(mutex);
    auto key = std::pair{std::string(entry), vertex};
    if (auto it = interfaces.find(key); it != interfaces.end()) return it->second;
    auto slots = vertex ? view_index_outputs(words, entry) : scalar_varyings(words, entry, false);
    interfaces.emplace(std::move(key), slots);
    return slots;
  }
  std::shared_ptr<const std::vector<std::uint32_t>> flat(const char* entry, bool vertex,
                                                         const std::vector<VaryingSlot>& slots) {
    std::lock_guard lock(mutex);
    Key key{entry, vertex, slots};
    if (auto it = normalized.find(key); it != normalized.end()) return it->second;
    auto code = flat_varyings(words, entry, vertex, slots);
    std::shared_ptr<const std::vector<std::uint32_t>> changed;
    if (code != words)
      changed = std::make_shared<const std::vector<std::uint32_t>>(std::move(code));
    normalized.emplace(std::move(key), changed);
    return changed;
  }
};
std::mutex mutex;
std::map<std::pair<VkDevice, VkShaderModule>, std::shared_ptr<Shader>> shaders;
void release_device(VkDevice device) {
  std::lock_guard lock(mutex);
  std::erase_if(shaders, [=](const auto& item) { return item.first.first == device; });
}
VkResult VKAPI_CALL create_shader(VkDevice device, const VkShaderModuleCreateInfo* info,
                                  const VkAllocationCallbacks* alloc, VkShaderModule* out) {
  try {
    // Subpass inputs read as sampled images (input_attachments.cpp); pipelines
    // made later from this module, and the Flat rewrite, start from that code.
    std::vector<std::uint32_t> sampled;
    VkShaderModuleCreateInfo with_sampled;
    const auto* guest = info;
    if (sampled_input_reads() && info->pCode && info->codeSize >= 20) {
      sampled = sample_subpass_inputs({info->pCode, info->codeSize / sizeof(std::uint32_t)});
      if (!sampled.empty()) {
        with_sampled = *info;
        with_sampled.pCode = sampled.data();
        with_sampled.codeSize = sampled.size() * sizeof(std::uint32_t);
        info = &with_sampled;
        static std::atomic<unsigned> logged{0};
        if (logged.fetch_add(1, std::memory_order_relaxed) < 16)
          log::info("vulkan", "Fragment shader reads its subpass inputs as sampled array layers");
      }
    }
    auto shader = std::make_shared<Shader>(*info);
    if (guest != info)
      shader->original.assign(guest->pCode, guest->pCode + guest->codeSize / sizeof(std::uint32_t));
    auto result =
        next<PFN_vkCreateShaderModule, "vkCreateShaderModule">(device)(device, info, alloc, out);
    if (result != VK_SUCCESS) return result;
    try {
      std::lock_guard lock(mutex);
      shaders.emplace(std::pair{device, *out}, std::move(shader));
    } catch (const std::bad_alloc&) {
      next<PFN_vkDestroyShaderModule, "vkDestroyShaderModule">(device)(device, *out, alloc);
      *out = VK_NULL_HANDLE;
      return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    return result;
  } catch (const std::bad_alloc&) {
    *out = VK_NULL_HANDLE;
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }
}
void VKAPI_CALL destroy_shader(VkDevice device, VkShaderModule module,
                               const VkAllocationCallbacks* alloc) {
  {
    std::lock_guard lock(mutex);
    shaders.erase({device, module});
  }
  next<PFN_vkDestroyShaderModule, "vkDestroyShaderModule">(device)(device, module, alloc);
}
struct TemporaryModules {
  VkDevice device;
  std::vector<VkShaderModule> modules;
  ~TemporaryModules() {
    for (auto module : modules)
      next<PFN_vkDestroyShaderModule, "vkDestroyShaderModule">(device)(device, module, nullptr);
  }
};
VkResult VKAPI_CALL create_pipelines(VkDevice device, VkPipelineCache cache, std::uint32_t count,
                                     const VkGraphicsPipelineCreateInfo* infos,
                                     const VkAllocationCallbacks* alloc, VkPipeline* out) {
  if (!count)
    return next<PFN_vkCreateGraphicsPipelines, "vkCreateGraphicsPipelines">(device)(
        device, cache, count, infos, alloc, out);
  try {
    std::vector<VkGraphicsPipelineCreateInfo> adjusted(infos, infos + count);
    std::vector<std::vector<VkPipelineShaderStageCreateInfo>> stages(count);
    TemporaryModules owned{device, {}};
    owned.modules.reserve(std::size_t(count) * 2);
    for (std::uint32_t i = 0; i < count; ++i) {
      // No separately linked interfaces.
      if (infos[i].flags & VK_PIPELINE_CREATE_LIBRARY_BIT_KHR) continue;
      bool linked = false;
      for (auto* p = static_cast<const VkBaseInStructure*>(infos[i].pNext); p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR)
          linked |= reinterpret_cast<const VkPipelineLibraryCreateInfoKHR*>(p)->libraryCount != 0;
      if (linked || !infos[i].pStages) continue;
      int vertex = -1, fragment = -1;
      for (std::uint32_t j = 0; j < infos[i].stageCount; ++j) {
        if (infos[i].pStages[j].stage == VK_SHADER_STAGE_VERTEX_BIT) vertex = int(j);
        if (infos[i].pStages[j].stage == VK_SHADER_STAGE_FRAGMENT_BIT) fragment = int(j);
      }
      if (fragment < 0 || !infos[i].pStages[fragment].pName) continue;
      const auto& fs = infos[i].pStages[fragment];
      std::shared_ptr<Shader> v, f;
      {
        std::lock_guard lock(mutex);
        auto b = shaders.find({device, fs.module});
        if (b == shaders.end()) continue;
        f = b->second;
        if (vertex >= 0)
          if (auto a = shaders.find({device, infos[i].pStages[vertex].module}); a != shaders.end()) v = a->second;
      }
      // Input attachments with one sample read through multisampled subpass inputs.
      std::shared_ptr<const std::vector<std::uint32_t>> fc;
      if (infos[i].renderPass)
        if (const auto mask = single_sampled_subpass_inputs(infos[i].renderPass, infos[i].subpass))
          fc = f->single_sampled(mask);
      // Flat varyings: no intervening tessellation/geometry stages.
      std::shared_ptr<const std::vector<std::uint32_t>> vc;
      if (infos[i].stageCount == 2 && v && infos[i].pStages[vertex].pName) {
        const auto& vs = infos[i].pStages[vertex];
        const auto outputs = v->slots(vs.pName, true), inputs = f->slots(fs.pName, false);
        std::vector<VaryingSlot> slots;
        std::set_intersection(outputs.begin(), outputs.end(), inputs.begin(), inputs.end(),
                              std::back_inserter(slots));
        if (!slots.empty()) {
          vc = v->flat(vs.pName, true, slots);
          if (!fc) {
            fc = f->flat(fs.pName, false, slots);
          } else if (auto code = flat_varyings(*fc, fs.pName, false, slots); !code.empty() && code != *fc) {
            fc = std::make_shared<const std::vector<std::uint32_t>>(std::move(code));
          }
        }
      }
      if (!vc && !fc) continue;
      stages[i].assign(infos[i].pStages, infos[i].pStages + infos[i].stageCount);
      for (const auto& [index, code] : {std::pair{vertex, vc}, std::pair{fragment, fc}}) {
        if (!code) continue;
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = code->size() * sizeof(std::uint32_t);
        ci.pCode = code->data();
        VkShaderModule module{};
        auto result = next<PFN_vkCreateShaderModule, "vkCreateShaderModule">(device)(
            device, &ci, nullptr, &module);
        if (result != VK_SUCCESS) {
          std::fill_n(out, count, VK_NULL_HANDLE);
          return result;
        }
        owned.modules.push_back(module);
        stages[i][index].module = module;
      }
      adjusted[i].pStages = stages[i].data();
    }
    return next<PFN_vkCreateGraphicsPipelines, "vkCreateGraphicsPipelines">(device)(
        device, cache, count, adjusted.data(), alloc, out);
  } catch (const std::bad_alloc&) {
    std::fill_n(out, count, VK_NULL_HANDLE);
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }
}
}  // namespace
PFN_vkVoidFunction multiview_proc(std::string_view name) {
  static const bool registered = [] {
    add_device_release_hook(release_device);
    return true;
  }();
  (void)registered;
  if (name == "vkCreateShaderModule") return reinterpret_cast<PFN_vkVoidFunction>(create_shader);
  if (name == "vkDestroyShaderModule") return reinterpret_cast<PFN_vkVoidFunction>(destroy_shader);
  if (name == "vkCreateGraphicsPipelines")
    return reinterpret_cast<PFN_vkVoidFunction>(create_pipelines);
  return nullptr;
}
}  // namespace venture::vulkan

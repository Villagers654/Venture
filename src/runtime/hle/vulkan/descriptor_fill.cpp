// Newly allocated descriptor sets hold null descriptors.
//
// A descriptor the guest never writes is undefined in Vulkan. Adreno reads
// zero from one, so Quest titles that sample or fetch through a binding they
// left unwritten work there; desktop drivers leave whatever the pool memory
// held. (A fault near 0xbeefdeadbeef on AMD is not one of these: that is the
// driver's debug descriptor behind sets that are not bound at all, which no
// fill of allocated sets reaches; see report_device_fault_info.) When the device has nullDescriptor,
// every binding of each set is written null right after vkAllocateDescriptorSets,
// before the guest's own writes replace them: buffers VK_NULL_HANDLE, images and
// texel buffer views VK_NULL_HANDLE, samplers a shared default sampler unless
// immutable. Input attachments, inline uniform blocks, acceleration structures,
// mutable descriptors and large arrays (descriptor indexing) are left as they
// are. VENTURE_VULKAN_NULL_DESCRIPTORS=0 turns this off with the rest of the
// null descriptor handling, VENTURE_VULKAN_DESCRIPTOR_FILL=0 only this.
#include "hle/vulkan/internal.h"
#include "hle/vulkan/vulkan.h"
#include "common/environment.h"
#include "common/log.h"

#include <algorithm>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string_view>
#include <utility>
#include <vector>

namespace venture::vulkan {
namespace {
struct Fill {
  std::vector<NullFillBinding> bindings;
  std::uint32_t variable_binding = UINT32_MAX;  // index into bindings of the variable-count binding
};
struct DeviceFill {
  VkSampler sampler{};
  std::map<VkDescriptorSetLayout, std::shared_ptr<const Fill>> layouts;
};
std::shared_mutex mutex;
std::map<VkDevice, DeviceFill> devices;
// What the fill covered on each device, for a device fault at a fill address
// (descriptor_fill_fault_notes). Recorded at layout creation, never per frame.
struct Census {
  bool filling = false;
  std::uint64_t layouts = 0;
  std::map<std::string, std::uint64_t> unfilled;  // unfilled_buffer_bindings entries
};
std::map<VkDevice, Census> census;

template <class F> F host(VkDevice d, const char* name) {
  return reinterpret_cast<F>(raw_device_proc(d, name));
}
// Null descriptor storage shared by every write; never written by the driver.
constexpr std::uint32_t max_count = max_null_fill_count;
const VkDescriptorBufferInfo* null_buffers() {
  static const auto values = [] {
    std::vector<VkDescriptorBufferInfo> v(max_count, VkDescriptorBufferInfo{VK_NULL_HANDLE, 0, VK_WHOLE_SIZE});
    return v;
  }();
  return values.data();
}
const VkBufferView* null_views() {
  static const std::vector<VkBufferView> values(max_count, VK_NULL_HANDLE);
  return values.data();
}

VkResult VKAPI_CALL create_layout(VkDevice device, const VkDescriptorSetLayoutCreateInfo* info,
                                  const VkAllocationCallbacks* alloc, VkDescriptorSetLayout* out) {
  const auto result =
      host<PFN_vkCreateDescriptorSetLayout>(device, "vkCreateDescriptorSetLayout")(device, info, alloc, out);
  static const bool enabled = [] {
    const auto* value = venture::environment::getenv("VENTURE_VULKAN_DESCRIPTOR_FILL");
    return !(value && std::string_view(value) == "0");
  }();
  if (result != VK_SUCCESS || !info) return result;
  const bool filling = enabled && device_null_descriptor(device);
  {
    auto unfilled = unfilled_buffer_bindings(*info);
    std::unique_lock lock(mutex);
    auto& c = census[device];
    c.filling = filling;
    ++c.layouts;
    for (auto& entry : unfilled) ++c.unfilled[std::move(entry)];
  }
  if (!filling) return result;
  auto plan = null_fill_plan(*info);
  if (plan.empty()) return result;
  auto fill = std::make_shared<Fill>();
  const bool sampler = std::ranges::any_of(plan, [](const NullFillBinding& b) { return b.sampler; });
  fill->bindings = std::move(plan);
  std::unique_lock lock(mutex);
  auto& state = devices[device];
  if (sampler && !state.sampler) {
    VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler_info.magFilter = sampler_info.minFilter = VK_FILTER_NEAREST;
    sampler_info.addressModeU = sampler_info.addressModeV = sampler_info.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.maxLod = VK_LOD_CLAMP_NONE;
    if (host<PFN_vkCreateSampler>(device, "vkCreateSampler")(device, &sampler_info, nullptr, &state.sampler) !=
        VK_SUCCESS)
      state.sampler = VK_NULL_HANDLE;
  }
  // Without a sampler, sampler bindings stay unfilled; the others still are.
  if (!state.sampler) std::erase_if(fill->bindings, [](const NullFillBinding& b) { return b.sampler; });
  for (std::uint32_t i = 0; i < fill->bindings.size(); ++i)
    if (fill->bindings[i].variable) fill->variable_binding = i;
  if (fill->bindings.empty()) return result;
  static bool logged = false;  // under `mutex`
  if (!std::exchange(logged, true))
    log::info("vulkan", "New descriptor sets start with null descriptors (VENTURE_VULKAN_DESCRIPTOR_FILL=0 turns this off)");
  state.layouts[*out] = std::move(fill);
  return result;
}
void VKAPI_CALL destroy_layout(VkDevice device, VkDescriptorSetLayout layout, const VkAllocationCallbacks* alloc) {
  if (layout) {
    std::unique_lock lock(mutex);
    if (auto it = devices.find(device); it != devices.end()) it->second.layouts.erase(layout);
  }
  host<PFN_vkDestroyDescriptorSetLayout>(device, "vkDestroyDescriptorSetLayout")(device, layout, alloc);
}
VkResult VKAPI_CALL allocate_sets(VkDevice device, const VkDescriptorSetAllocateInfo* info, VkDescriptorSet* sets) {
  const auto result = host<PFN_vkAllocateDescriptorSets>(device, "vkAllocateDescriptorSets")(device, info, sets);
  if (result != VK_SUCCESS || !info || !info->descriptorSetCount) return result;
  const std::uint32_t* variable_counts = nullptr;
  for (auto* next = static_cast<const VkBaseInStructure*>(info->pNext); next; next = next->pNext)
    if (next->sType == VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO) {
      const auto& counts = *reinterpret_cast<const VkDescriptorSetVariableDescriptorCountAllocateInfo*>(next);
      if (counts.descriptorSetCount == info->descriptorSetCount) variable_counts = counts.pDescriptorCounts;
    }
  // Thread-local scratch: allocations run on the game's render threads, often per frame.
  thread_local std::vector<VkWriteDescriptorSet> writes;
  thread_local std::vector<VkDescriptorImageInfo> images;
  thread_local VkSampler images_sampler{};
  writes.clear();
  {
    std::shared_lock lock(mutex);
    auto state = devices.find(device);
    if (state == devices.end()) return result;
    if (images.empty() || images_sampler != state->second.sampler) {
      images_sampler = state->second.sampler;
      images.assign(max_count, {images_sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    }
    for (std::uint32_t s = 0; s < info->descriptorSetCount; ++s) {
      auto layout = state->second.layouts.find(info->pSetLayouts[s]);
      if (layout == state->second.layouts.end()) continue;
      const Fill& fill = *layout->second;
      for (std::uint32_t i = 0; i < fill.bindings.size(); ++i) {
        const auto& b = fill.bindings[i];
        std::uint32_t count = b.count;
        if (i == fill.variable_binding) count = variable_counts ? std::min(count, variable_counts[s]) : 0;
        if (!count) continue;
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = sets[s];
        w.dstBinding = b.binding;
        w.descriptorCount = count;
        w.descriptorType = b.type;
        w.pImageInfo = images.data();
        w.pBufferInfo = null_buffers();
        w.pTexelBufferView = null_views();
        writes.push_back(w);
      }
    }
  }
  if (!writes.empty())
    host<PFN_vkUpdateDescriptorSets>(device, "vkUpdateDescriptorSets")(device, std::uint32_t(writes.size()),
                                                                       writes.data(), 0, nullptr);
  return result;
}
}  // namespace

std::vector<NullFillBinding> null_fill_plan(const VkDescriptorSetLayoutCreateInfo& info, bool push) {
  std::vector<NullFillBinding> plan;
  if ((info.flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT) && !push) return plan;
  if (info.flags & (VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT |
                    VK_DESCRIPTOR_SET_LAYOUT_CREATE_HOST_ONLY_POOL_BIT_EXT))
    return plan;
  const VkDescriptorBindingFlags* flags = nullptr;
  for (auto* next = static_cast<const VkBaseInStructure*>(info.pNext); next; next = next->pNext)
    if (next->sType == VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO) {
      const auto& f = *reinterpret_cast<const VkDescriptorSetLayoutBindingFlagsCreateInfo*>(next);
      if (f.bindingCount == info.bindingCount) flags = f.pBindingFlags;
    }
  for (std::uint32_t i = 0; info.pBindings && i < info.bindingCount; ++i) {
    const auto& b = info.pBindings[i];
    if (!b.descriptorCount || b.descriptorCount > max_null_fill_count) continue;
    NullFillBinding fill{b.binding, b.descriptorType, b.descriptorCount};
    switch (b.descriptorType) {
      case VK_DESCRIPTOR_TYPE_SAMPLER:
        if (b.pImmutableSamplers) continue;  // nothing to write
        fill.sampler = true;
        break;
      case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
        fill.sampler = !b.pImmutableSamplers;
        break;
      case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
      case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
      case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
      case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
        break;
      default: continue;  // input attachments, inline uniform blocks, acceleration structures, mutable
    }
    if (flags) fill.variable = (flags[i] & VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT) != 0;
    plan.push_back(fill);
  }
  return plan;
}
std::vector<std::string> unfilled_buffer_bindings(const VkDescriptorSetLayoutCreateInfo& info) {
  std::vector<std::string> unfilled;
  const char* layout_reason = (info.flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT) ? "push descriptor layout"
      : (info.flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT) ? "descriptor buffer layout"
      : (info.flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_HOST_ONLY_POOL_BIT_EXT) ? "host-only layout" : nullptr;
  for (std::uint32_t i = 0; info.pBindings && i < info.bindingCount; ++i) {
    const auto& b = info.pBindings[i];
    const char* type = nullptr;
    switch (b.descriptorType) {
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: type = "uniform buffer"; break;
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: type = "storage buffer"; break;
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC: type = "dynamic uniform buffer"; break;
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: type = "dynamic storage buffer"; break;
      case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER: type = "uniform texel buffer"; break;
      case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: type = "storage texel buffer"; break;
      default: continue;  // image descriptors do not hold a 48-bit buffer address
    }
    if (!b.descriptorCount) continue;
    const char* reason = layout_reason ? layout_reason : b.descriptorCount > max_null_fill_count ? "array past the fill limit" : nullptr;
    if (reason) unfilled.push_back(std::format("{} array of {} ({})", type, b.descriptorCount, reason));
  }
  return unfilled;
}
std::vector<std::string> descriptor_fill_fault_notes(VkDevice device) {
  std::vector<std::string> notes;
  const bool vertex_nulls = device_null_descriptor(device);
  if (!vertex_nulls)
    notes.push_back("the device has no nullDescriptor: vertex bindings the game never binds and descriptors it never "
                    "writes hold whatever the driver left there (VENTURE_VULKAN_NULL_DESCRIPTORS)");
  std::shared_lock lock(mutex);
  auto it = census.find(device);
  if (it == census.end()) {
    notes.push_back("the game created no descriptor set layouts before the fault: the GPU work so far was uploads, "
                    "clears and Venture's own passes, or drew without descriptors");
    return notes;
  }
  const auto& c = it->second;
  if (vertex_nulls && !c.filling)
    notes.push_back(std::format("new descriptor sets of the {} layouts were not filled with null descriptors "
                                "(VENTURE_VULKAN_DESCRIPTOR_FILL=0): a binding the game never wrote is the likely culprit",
                                c.layouts));
  else if (c.filling)
    notes.push_back(std::format("descriptor sets of all {} layouts start with null descriptors{}", c.layouts,
                                c.unfilled.empty() ? ", so no unwritten binding of a bound set explains it: look for "
                                                     "a set index the draw or dispatch reads that was never bound "
                                                     "or pushed, or was disturbed by a bind with another layout"
                                                   : " except the buffer bindings below"));
  for (const auto& [entry, layouts] : c.unfilled)
    notes.push_back(std::format("left unwritten: {} in {} layout{}", entry, layouts, layouts == 1 ? "" : "s"));
  return notes;
}
PFN_vkVoidFunction descriptor_fill_proc(std::string_view name) {
  if (name == "vkCreateDescriptorSetLayout") return reinterpret_cast<PFN_vkVoidFunction>(create_layout);
  if (name == "vkDestroyDescriptorSetLayout") return reinterpret_cast<PFN_vkVoidFunction>(destroy_layout);
  if (name == "vkAllocateDescriptorSets") return reinterpret_cast<PFN_vkVoidFunction>(allocate_sets);
  return nullptr;
}
void release_descriptor_fill_device(VkDevice device) {
  VkSampler sampler{};
  {
    std::unique_lock lock(mutex);
    census.erase(device);
    auto it = devices.find(device);
    if (it == devices.end()) return;
    sampler = it->second.sampler;
    devices.erase(it);
  }
  if (sampler) host<PFN_vkDestroySampler>(device, "vkDestroySampler")(device, sampler, nullptr);
}
}  // namespace venture::vulkan

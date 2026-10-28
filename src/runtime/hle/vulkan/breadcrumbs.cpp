#include "common/environment.h"
// Optional GPU progress markers. Transfers stay outside render passes; native
// checkpoints, when exposed, also identify individual draws inside a pass.
// VENTURE_VULKAN_BREADCRUMB_DIR additionally saves SPIR-V and a bounded record-time
// snapshot of mapped descriptors. No GPU readbacks or cache flushes are added.
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>

#include "common/log.h"
#include "internal.h"

namespace venture::vulkan {
namespace {
constexpr std::uint32_t slots = 4096;
struct Device {
  VkBuffer buffer{};
  VkDeviceMemory memory{};
  std::uint32_t* mapped{};
  bool checkpoints{};
};
struct Image {
  VkFormat format{};
  VkExtent3D size{};
  VkImageUsageFlags usage{};
};
struct View {
  VkImage image{};
  VkFormat format{};
};
struct Descriptor {
  VkDescriptorType type{};
  VkImageView view{};
  VkBuffer buffer{};
  VkDeviceSize offset{}, range{};
};
struct State {
  struct Binding {
    VkPipeline pipeline{};
    std::map<std::uint32_t, VkDescriptorSet> sets;
    std::map<std::uint32_t, std::vector<std::uint32_t>> dynamic_offsets;
  };
  VkDevice device{};
  std::map<VkPipelineBindPoint, Binding> bindings;
  std::vector<unsigned char> push_constants;
  bool in_pass{}, suspended{}, will_suspend{};
};
std::recursive_mutex mutex;
std::map<VkDevice, Device> devices;
// The opt-in is read while creating a device. Command recording only needs to
// know whether any prepared device can consume breadcrumbs; scanning the host
// environment here would put a CRT lock and two linear scans on every draw.
std::atomic<bool> prepared_devices{false};
std::map<VkImage, Image> images;
std::map<VkImageView, View> views;
std::map<VkBuffer, VkDeviceSize> buffers;
struct Memory {
  VkDeviceSize size{}, offset{}, mapped_size{};
  VkMemoryPropertyFlags flags{};
  unsigned char* mapped{};
};
std::map<VkDeviceMemory, Memory> memories;
std::map<VkBuffer, std::pair<VkDeviceMemory, VkDeviceSize>> buffer_memory;
using DynamicBinding = std::pair<std::uint32_t, std::uint32_t>;
std::map<VkDescriptorSetLayout, std::vector<DynamicBinding>> layouts;
std::map<VkDescriptorSet, std::vector<DynamicBinding>> set_dynamic_bindings;
std::map<VkShaderModule, std::uint64_t> shaders;
std::map<VkPipeline, std::string> pipelines;
std::map<VkDescriptorSet,
         std::map<std::pair<std::uint32_t, std::uint32_t>, Descriptor>>
    descriptors;
std::map<VkDescriptorUpdateTemplate,
         std::vector<VkDescriptorUpdateTemplateEntry>>
    templates;
std::map<VkDescriptorSet, VkDescriptorPool> descriptor_pools;
std::map<VkCommandBuffer, State> commands;
struct Record {
  VkDevice device{};
  std::string text;
  std::map<std::string, std::vector<unsigned char>> bytes;
};
std::map<std::uint32_t, Record> records;
std::uint32_t serial{};
std::size_t retained_bytes{};
std::filesystem::path capture_directory() {
  auto p = venture::environment::getenv("VENTURE_VULKAN_BREADCRUMB_DIR");
  return p && *p ? std::filesystem::path(p) : std::filesystem::path{};
}
void write_capture(const std::string& name, const void* data,
                   std::size_t size) {
  auto dir = capture_directory();
  if (dir.empty()) return;
  std::error_code error;
  std::filesystem::create_directories(dir, error);
  if (error) return;
  std::ofstream file(dir / name, std::ios::binary);
  file.write(static_cast<const char*>(data), size);
}
template <class F>
F vk(VkDevice d, const char* n) {
  return reinterpret_cast<F>(raw_device_proc(d, n));
}
template <class F, ProcedureName Name>
F next(VkDevice d) {
  if (auto f = cached_layer_proc<Layer::breadcrumbs, Name>()) return reinterpret_cast<F>(f);
  return vk<F>(d, Name.value);
}
std::uint64_t hash(const VkShaderModuleCreateInfo& info) {
  std::uint64_t h = 14695981039346656037ull;
  auto bytes = reinterpret_cast<const unsigned char*>(info.pCode);
  for (std::size_t i = 0; i < info.codeSize; ++i)
    h = (h ^ bytes[i]) * 1099511628211ull;
  return h;
}
Record snapshot(const char* action, VkCommandBuffer cmd, State& state) {
  Record record{state.device};
  auto& binding_state =
      state.bindings[std::string_view(action).starts_with("dispatch")
                         ? VK_PIPELINE_BIND_POINT_COMPUTE
                         : VK_PIPELINE_BIND_POINT_GRAPHICS];
  std::string s = std::format("{} cmd={:#x} pipeline={:#x} shaders=[{}]",
                              action, h2g(cmd), h2g(binding_state.pipeline),
                              pipelines[binding_state.pipeline]);
  const bool capture = !capture_directory().empty();
  if (capture && !state.push_constants.empty())
    record.bytes["push.bin"] = state.push_constants;
  for (auto [set_index, set] : binding_state.sets) {
    std::map<DynamicBinding, std::uint32_t> dynamic;
    const auto& keys = set_dynamic_bindings[set];
    const auto& offsets = binding_state.dynamic_offsets[set_index];
    for (std::size_t i = 0; i < std::min(keys.size(), offsets.size()); ++i)
      dynamic[keys[i]] = offsets[i];
    for (auto& [binding, d] : descriptors[set]) {
      s += std::format(" [set={} binding={} element={} type={}", set_index,
                       binding.first, binding.second, int(d.type));
      if (d.view) {
        auto v = views.find(d.view);
        if (v != views.end()) {
          auto i = images.find(v->second.image);
          if (i != images.end())
            s += std::format(
                " view={:#x} guest_view_format={} image={:#x} "
                "guest_created_format={} size={}x{}x{} usage={:#x}",
                h2g(d.view), int(v->second.format), h2g(v->second.image),
                int(i->second.format), i->second.size.width,
                i->second.size.height, i->second.size.depth, i->second.usage);
        } else
          s += std::format(" untracked_view={:#x}", h2g(d.view));
      }
      if (d.buffer) {
        auto delta = dynamic.contains(binding) ? dynamic[binding] : 0;
        s += std::format(
            " buffer={:#x} guest_created_size={} offset={} range={} "
            "dynamic_offset={}",
            h2g(d.buffer), buffers[d.buffer], d.offset, d.range, delta);
        if (capture) {
          auto bound = buffer_memory.find(d.buffer);
          auto memory = bound == buffer_memory.end()
                            ? memories.end()
                            : memories.find(bound->second.first);
          const auto size = buffers[d.buffer];
          if (memory != memories.end()) {
            const auto& m = memory->second;
            s += std::format(
                " memory_flags={:#x} memory_binding_offset={} mapped_offset={} "
                "mapped_size={}",
                m.flags, bound->second.second, m.offset, m.mapped_size);
            // Subtractions guard every sum before indexing the returned
            // mapping.
            if (d.offset <= size && delta <= size - d.offset) {
              auto start = d.offset + delta;
              auto range = d.range == VK_WHOLE_SIZE ? size - start : d.range;
              if (range <= size - start && bound->second.second <= m.size &&
                  start <= m.size - bound->second.second) {
                auto absolute = bound->second.second + start;
                if (m.mapped && absolute >= m.offset &&
                    absolute - m.offset <= m.mapped_size &&
                    range <= m.mapped_size - (absolute - m.offset) &&
                    range <= 1024 * 1024) {
                  auto name =
                      std::format("set{}-binding{}-element{}.bin", set_index,
                                  binding.first, binding.second);
                  auto p = m.mapped + (absolute - m.offset);
                  record.bytes[name].assign(p, p + range);
                  s += std::format(" captured={} bytes={}", name, range);
                } else
                  s += " capture_unavailable=mapping-or-size";
              } else
                s += " capture_unavailable=range";
            } else
              s += " capture_unavailable=offset";
          } else
            s += " capture_unavailable=untracked-memory";
        }
      }
      s += "]";
    }
  }
  record.text = std::move(s);
  return record;
}
void mark(VkCommandBuffer cmd, const char* action) {
  std::lock_guard lock(mutex);
  auto d = command_device(cmd);
  auto it = devices.find(d);
  if (it == devices.end()) return;
  auto& state = commands[cmd];
  state.device = d;
  if (state.suspended) return;
  const auto id = ++serial;
  if (!id) return;
  records[id] = snapshot(action, cmd, state);
  for (auto& [name, bytes] : records[id].bytes) retained_bytes += bytes.size();
  while (records.size() > slots || retained_bytes > 64 * 1024 * 1024) {
    for (auto& [name, bytes] : records.begin()->second.bytes)
      retained_bytes -= bytes.size();
    records.erase(records.begin());
  }
  if (it->second.checkpoints)
    vk<PFN_vkCmdSetCheckpointNV>(d, "vkCmdSetCheckpointNV")(
        cmd, reinterpret_cast<const void*>(std::uintptr_t(id)));
  if (state.in_pass || !it->second.mapped) return;
  // Serialize the preceding work before the marker, and expose its write to
  // coherent host memory. vkCmdFillBuffer is never emitted inside a pass.
  vk<PFN_vkCmdPipelineBarrier>(d, "vkCmdPipelineBarrier")(
      cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
      0, 0, nullptr, 0, nullptr, 0, nullptr);
  vk<PFN_vkCmdFillBuffer>(d, "vkCmdFillBuffer")(cmd, it->second.buffer,
                                                (id % slots) * 4ull, 4, id);
  VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  vk<PFN_vkCmdPipelineBarrier>(d, "vkCmdPipelineBarrier")(
      cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
      &barrier, 0, nullptr, 0, nullptr);
}
VkResult VKAPI_CALL begin(VkCommandBuffer c,
                          const VkCommandBufferBeginInfo* i) {
  auto r = next<PFN_vkBeginCommandBuffer, "vkBeginCommandBuffer">(command_device(c))(c, i);
  if (r == VK_SUCCESS) {
    std::lock_guard lock(mutex);
    commands[c] = {};
    commands[c].device = command_device(c);
    commands[c].in_pass =
        i->flags & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
  }
  return r;
}
void VKAPI_CALL bind_pipeline(VkCommandBuffer c, VkPipelineBindPoint p,
                              VkPipeline pipeline) {
  next<PFN_vkCmdBindPipeline, "vkCmdBindPipeline">(command_device(c))(c, p,
                                                                      pipeline);
  std::lock_guard lock(mutex);
  commands[c].bindings[p].pipeline = pipeline;
}
void VKAPI_CALL bind_sets(VkCommandBuffer c, VkPipelineBindPoint p,
                          VkPipelineLayout l, std::uint32_t first,
                          std::uint32_t count, const VkDescriptorSet* sets,
                          std::uint32_t offsets, const std::uint32_t* data) {
  next<PFN_vkCmdBindDescriptorSets, "vkCmdBindDescriptorSets">(command_device(c))(c, p, l, first, count, sets,
                                                    offsets, data);
  std::lock_guard lock(mutex);
  auto& state = commands[c].bindings[p];
  std::size_t consumed = 0;
  for (std::uint32_t i = 0; i < count; ++i) {
    state.sets[first + i] = sets[i];
    auto& values = state.dynamic_offsets[first + i];
    values.clear();
    auto n = set_dynamic_bindings[sets[i]].size();
    if (consumed <= offsets && n <= offsets - consumed && n)
      values.assign(data + consumed, data + consumed + n);
    consumed += n;
  }
}
VkResult VKAPI_CALL create_shader(VkDevice d, const VkShaderModuleCreateInfo* i,
                                  const VkAllocationCallbacks* a,
                                  VkShaderModule* out) {
  auto r =
      next<PFN_vkCreateShaderModule, "vkCreateShaderModule">(d)(d, i, a, out);
  if (r == VK_SUCCESS) {
    std::lock_guard lock(mutex);
    shaders[*out] = hash(*i);
    write_capture(std::format("shader-{:016x}.spv", shaders[*out]), i->pCode,
                  i->codeSize);
  }
  return r;
}
void capture_specialization(VkPipeline pipeline,
                            const VkPipelineShaderStageCreateInfo& stage) {
  if (capture_directory().empty() || !stage.pSpecializationInfo) return;
  const auto& info = *stage.pSpecializationInfo;
  auto prefix =
      std::format("pipeline-{:x}-stage{}", h2g(pipeline), int(stage.stage));
  write_capture(prefix + "-specialization.bin", info.pData, info.dataSize);
  std::string entries;
  for (std::uint32_t i = 0; i < info.mapEntryCount; ++i) {
    auto& e = info.pMapEntries[i];
    entries += std::format("constant={} offset={} size={} data_size={}\n",
                           e.constantID, e.offset, e.size, info.dataSize);
  }
  write_capture(prefix + "-specialization.txt", entries.data(), entries.size());
}
VkResult VKAPI_CALL create_graphics(VkDevice d, VkPipelineCache cache,
                                    std::uint32_t n,
                                    const VkGraphicsPipelineCreateInfo* info,
                                    const VkAllocationCallbacks* a,
                                    VkPipeline* out) {
  auto r = next<PFN_vkCreateGraphicsPipelines, "vkCreateGraphicsPipelines">(d)(
      d, cache, n, info, a, out);
  std::lock_guard lock(mutex);
  for (std::uint32_t i = 0; i < n; ++i)
    if (out[i]) {
      std::string s;
      for (std::uint32_t j = 0; j < info[i].stageCount; ++j) {
        s += std::format("{}:{:016x} ", int(info[i].pStages[j].stage),
                         shaders[info[i].pStages[j].module]);
        capture_specialization(out[i], info[i].pStages[j]);
      }
      pipelines[out[i]] = s;
    }
  return r;
}
VkResult VKAPI_CALL create_compute(VkDevice d, VkPipelineCache cache,
                                   std::uint32_t n,
                                   const VkComputePipelineCreateInfo* info,
                                   const VkAllocationCallbacks* a,
                                   VkPipeline* out) {
  auto r = next<PFN_vkCreateComputePipelines, "vkCreateComputePipelines">(d)(
      d, cache, n, info, a, out);
  std::lock_guard lock(mutex);
  for (std::uint32_t i = 0; i < n; ++i)
    if (out[i]) {
      pipelines[out[i]] =
          std::format("compute:{:016x}", shaders[info[i].stage.module]);
      capture_specialization(out[i], info[i].stage);
    }
  return r;
}
VkResult VKAPI_CALL create_image(VkDevice d, const VkImageCreateInfo* i,
                                 const VkAllocationCallbacks* a, VkImage* out) {
  auto r = next<PFN_vkCreateImage, "vkCreateImage">(d)(d, i, a, out);
  if (r == VK_SUCCESS) {
    std::lock_guard lock(mutex);
    images[*out] = {i->format, i->extent, i->usage};
  }
  return r;
}
VkResult VKAPI_CALL create_view(VkDevice d, const VkImageViewCreateInfo* i,
                                const VkAllocationCallbacks* a,
                                VkImageView* out) {
  auto r = next<PFN_vkCreateImageView, "vkCreateImageView">(d)(d, i, a, out);
  if (r == VK_SUCCESS) {
    std::lock_guard lock(mutex);
    views[*out] = {i->image, i->format};
  }
  return r;
}
VkResult VKAPI_CALL create_buffer(VkDevice d, const VkBufferCreateInfo* i,
                                  const VkAllocationCallbacks* a,
                                  VkBuffer* out) {
  auto r = next<PFN_vkCreateBuffer, "vkCreateBuffer">(d)(d, i, a, out);
  if (r == VK_SUCCESS) {
    std::lock_guard lock(mutex);
    buffers[*out] = i->size;
    buffer_memory.erase(*out);
  }
  return r;
}
void VKAPI_CALL update(VkDevice d, std::uint32_t n,
                       const VkWriteDescriptorSet* w, std::uint32_t copies,
                       const VkCopyDescriptorSet* c) {
  next<PFN_vkUpdateDescriptorSets, "vkUpdateDescriptorSets">(d)(d, n, w, copies,
                                                                c);
  std::lock_guard lock(mutex);
  for (std::uint32_t i = 0; i < n; ++i)
    for (std::uint32_t j = 0; j < w[i].descriptorCount; ++j) {
      Descriptor record{};
      record.type = w[i].descriptorType;
      switch (record.type) {
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
        case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
          record.view = w[i].pImageInfo[j].imageView;
          break;
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
          record.buffer = w[i].pBufferInfo[j].buffer;
          record.offset = w[i].pBufferInfo[j].offset;
          record.range = w[i].pBufferInfo[j].range;
          break;
        default:
          break;
      }
      descriptors[w[i].dstSet][{w[i].dstBinding, w[i].dstArrayElement + j}] =
          record;
    }
  for (std::uint32_t i = 0; i < copies; ++i)
    for (std::uint32_t j = 0; j < c[i].descriptorCount; ++j)
      descriptors[c[i].dstSet][{c[i].dstBinding, c[i].dstArrayElement + j}] =
          descriptors[c[i].srcSet][{c[i].srcBinding, c[i].srcArrayElement + j}];
}
#define DESTROY(n, t, m)                                        \
  void VKAPI_CALL destroy_##m(VkDevice d, t object,             \
                              const VkAllocationCallbacks* a) { \
    next<PFN_##n, #n>(d)(d, object, a);                         \
    std::lock_guard lock(mutex);                                \
    m.erase(object);                                            \
  }
DESTROY(vkDestroyImage, VkImage, images)
DESTROY(vkDestroyImageView, VkImageView, views)
DESTROY(vkDestroyBuffer, VkBuffer, buffers)
DESTROY(vkDestroyShaderModule, VkShaderModule, shaders)
DESTROY(vkDestroyPipeline, VkPipeline, pipelines)
DESTROY(vkDestroyDescriptorUpdateTemplate, VkDescriptorUpdateTemplate,
        templates)
DESTROY(vkDestroyDescriptorSetLayout, VkDescriptorSetLayout, layouts)
#undef DESTROY
VkResult VKAPI_CALL allocate_memory(VkDevice d, const VkMemoryAllocateInfo* i,
                                    const VkAllocationCallbacks* a,
                                    VkDeviceMemory* out) {
  auto r = next<PFN_vkAllocateMemory, "vkAllocateMemory">(d)(d, i, a, out);
  if (r == VK_SUCCESS) {
    VkPhysicalDeviceMemoryProperties props{};
    reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(raw_physical_proc(
        device_physical(d), "vkGetPhysicalDeviceMemoryProperties"))(
        device_physical(d), &props);
    std::lock_guard lock(mutex);
    memories[*out] = {i->allocationSize, 0, 0,
                      props.memoryTypes[i->memoryTypeIndex].propertyFlags};
  }
  return r;
}
void VKAPI_CALL free_memory(VkDevice d, VkDeviceMemory m,
                            const VkAllocationCallbacks* a) {
  next<PFN_vkFreeMemory, "vkFreeMemory">(d)(d, m, a);
  std::lock_guard lock(mutex);
  memories.erase(m);
  std::erase_if(buffer_memory, [&](auto& p) { return p.second.first == m; });
}
void mapped_memory(VkDeviceMemory m, VkDeviceSize offset, VkDeviceSize size,
                   void* p) {
  std::lock_guard lock(mutex);
  auto& memory = memories[m];
  memory.offset = offset;
  memory.mapped_size = size == VK_WHOLE_SIZE && offset <= memory.size
                           ? memory.size - offset
                           : size;
  memory.mapped = static_cast<unsigned char*>(p);
}
VkResult VKAPI_CALL map_memory(VkDevice d, VkDeviceMemory m,
                               VkDeviceSize offset, VkDeviceSize size,
                               VkMemoryMapFlags flags, void** out) {
  auto r =
      next<PFN_vkMapMemory, "vkMapMemory">(d)(d, m, offset, size, flags, out);
  if (r == VK_SUCCESS) mapped_memory(m, offset, size, *out);
  return r;
}
VkResult VKAPI_CALL map_memory2(VkDevice d, const VkMemoryMapInfo* i,
                                void** out) {
  auto r = next<PFN_vkMapMemory2, "vkMapMemory2">(d)(d, i, out);
  if (r == VK_SUCCESS) mapped_memory(i->memory, i->offset, i->size, *out);
  return r;
}
void VKAPI_CALL unmap_memory(VkDevice d, VkDeviceMemory m) {
  std::lock_guard lock(mutex);
  memories[m].mapped = nullptr;
  next<PFN_vkUnmapMemory, "vkUnmapMemory">(d)(d, m);
}
VkResult VKAPI_CALL unmap_memory2(VkDevice d, const VkMemoryUnmapInfo* i) {
  std::lock_guard lock(mutex);
  auto r = next<PFN_vkUnmapMemory2, "vkUnmapMemory2">(d)(d, i);
  if (r == VK_SUCCESS) memories[i->memory].mapped = nullptr;
  return r;
}
VkResult VKAPI_CALL bind_memory(VkDevice d, VkBuffer b, VkDeviceMemory m,
                                VkDeviceSize offset) {
  auto r =
      next<PFN_vkBindBufferMemory, "vkBindBufferMemory">(d)(d, b, m, offset);
  if (r == VK_SUCCESS) {
    std::lock_guard lock(mutex);
    buffer_memory[b] = {m, offset};
  }
  return r;
}
VkResult VKAPI_CALL bind_memory2(VkDevice d, std::uint32_t n,
                                 const VkBindBufferMemoryInfo* info) {
  auto r = next<PFN_vkBindBufferMemory2, "vkBindBufferMemory2">(d)(d, n, info);
  if (r == VK_SUCCESS) {
    std::lock_guard lock(mutex);
    for (std::uint32_t i = 0; i < n; ++i)
      buffer_memory[info[i].buffer] = {info[i].memory, info[i].memoryOffset};
  }
  return r;
}
VkResult VKAPI_CALL create_layout(VkDevice d,
                                  const VkDescriptorSetLayoutCreateInfo* i,
                                  const VkAllocationCallbacks* a,
                                  VkDescriptorSetLayout* out) {
  auto r = next<PFN_vkCreateDescriptorSetLayout, "vkCreateDescriptorSetLayout">(d)(d, i, a, out);
  if (r == VK_SUCCESS) {
    std::lock_guard lock(mutex);
    auto& keys = layouts[*out];
    keys.clear();
    for (std::uint32_t b = 0; b < i->bindingCount; ++b) {
      auto& binding = i->pBindings[b];
      if (binding.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
          binding.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)
        for (std::uint32_t e = 0; e < binding.descriptorCount; ++e)
          keys.emplace_back(binding.binding, e);
    }
    std::sort(keys.begin(), keys.end());
  }
  return r;
}
void VKAPI_CALL push_constants(VkCommandBuffer c, VkPipelineLayout layout,
                               VkShaderStageFlags stages, std::uint32_t offset,
                               std::uint32_t size, const void* data) {
  next<PFN_vkCmdPushConstants, "vkCmdPushConstants">(command_device(c))(
      c, layout, stages, offset, size, data);
  if (capture_directory().empty() || offset > 65536 || size > 65536 - offset)
    return;
  std::lock_guard lock(mutex);
  auto& bytes = commands[c].push_constants;
  bytes.resize(std::max<std::size_t>(bytes.size(), offset + size));
  std::memcpy(bytes.data() + offset, data, size);
}
VkResult VKAPI_CALL allocate_sets(VkDevice d,
                                  const VkDescriptorSetAllocateInfo* i,
                                  VkDescriptorSet* out) {
  auto r = next<PFN_vkAllocateDescriptorSets, "vkAllocateDescriptorSets">(d)(
      d, i, out);
  if (r == VK_SUCCESS) {
    std::lock_guard lock(mutex);
    for (std::uint32_t j = 0; j < i->descriptorSetCount; ++j) {
      descriptors[out[j]].clear();
      set_dynamic_bindings[out[j]] = layouts[i->pSetLayouts[j]];
      descriptor_pools[out[j]] = i->descriptorPool;
    }
  }
  return r;
}
VkResult VKAPI_CALL free_sets(VkDevice d, VkDescriptorPool pool,
                              std::uint32_t n, const VkDescriptorSet* sets) {
  auto r = next<PFN_vkFreeDescriptorSets, "vkFreeDescriptorSets">(d)(d, pool, n,
                                                                     sets);
  if (r == VK_SUCCESS) {
    std::lock_guard lock(mutex);
    for (std::uint32_t j = 0; j < n; ++j) {
      descriptors.erase(sets[j]);
      set_dynamic_bindings.erase(sets[j]);
      descriptor_pools.erase(sets[j]);
    }
  }
  return r;
}
VkResult VKAPI_CALL create_template(
    VkDevice d, const VkDescriptorUpdateTemplateCreateInfo* i,
    const VkAllocationCallbacks* a, VkDescriptorUpdateTemplate* out) {
  auto r = next<PFN_vkCreateDescriptorUpdateTemplate, "vkCreateDescriptorUpdateTemplate">(d)(d, i, a, out);
  if (r == VK_SUCCESS &&
      i->templateType == VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET) {
    std::lock_guard lock(mutex);
    templates[*out].assign(
        i->pDescriptorUpdateEntries,
        i->pDescriptorUpdateEntries + i->descriptorUpdateEntryCount);
  }
  return r;
}
void VKAPI_CALL update_template(VkDevice d, VkDescriptorSet set,
                                VkDescriptorUpdateTemplate t,
                                const void* data) {
  next<PFN_vkUpdateDescriptorSetWithTemplate, "vkUpdateDescriptorSetWithTemplate">(d)(d, set, t, data);
  std::lock_guard lock(mutex);
  for (auto& entry : templates[t])
    for (std::uint32_t j = 0; j < entry.descriptorCount; ++j) {
      auto p = static_cast<const char*>(data) + entry.offset + entry.stride * j;
      Descriptor record{};
      record.type = entry.descriptorType;
      switch (record.type) {
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
        case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: {
          VkDescriptorImageInfo image{};
          std::memcpy(&image, p, sizeof(image));
          record.view = image.imageView;
          break;
        }
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: {
          VkDescriptorBufferInfo buffer{};
          std::memcpy(&buffer, p, sizeof(buffer));
          record.buffer = buffer.buffer;
          record.offset = buffer.offset;
          record.range = buffer.range;
          break;
        }
        default:
          break;
      }
      descriptors[set][{entry.dstBinding, entry.dstArrayElement + j}] = record;
    }
}
VkResult VKAPI_CALL reset_pool(VkDevice d, VkDescriptorPool pool,
                               VkDescriptorPoolResetFlags flags) {
  auto r = next<PFN_vkResetDescriptorPool, "vkResetDescriptorPool">(d)(d, pool,
                                                                       flags);
  if (r == VK_SUCCESS) {
    std::lock_guard lock(mutex);
    std::erase_if(descriptor_pools, [&](auto& p) {
      if (p.second != pool) return false;
      descriptors.erase(p.first);
      return true;
    });
  }
  return r;
}
void VKAPI_CALL begin_pass(VkCommandBuffer c, const VkRenderPassBeginInfo* i,
                           VkSubpassContents contents) {
  mark(c, "before render pass");
  next<PFN_vkCmdBeginRenderPass, "vkCmdBeginRenderPass">(command_device(c))(
      c, i, contents);
  {
    std::lock_guard lock(mutex);
    commands[c].in_pass = true;
  }
  mark(c, "begin render pass");
}
void VKAPI_CALL begin_pass2(VkCommandBuffer c, const VkRenderPassBeginInfo* i,
                            const VkSubpassBeginInfo* sub) {
  mark(c, "before render pass");
  next<PFN_vkCmdBeginRenderPass2, "vkCmdBeginRenderPass2">(command_device(c))(
      c, i, sub);
  {
    std::lock_guard lock(mutex);
    commands[c].in_pass = true;
  }
  mark(c, "begin render pass");
}
void VKAPI_CALL end_pass(VkCommandBuffer c) {
  next<PFN_vkCmdEndRenderPass, "vkCmdEndRenderPass">(command_device(c))(c);
  {
    std::lock_guard lock(mutex);
    commands[c].in_pass = false;
  }
  mark(c, "end render pass");
}
void VKAPI_CALL end_pass2(VkCommandBuffer c, const VkSubpassEndInfo* i) {
  next<PFN_vkCmdEndRenderPass2, "vkCmdEndRenderPass2">(command_device(c))(c, i);
  {
    std::lock_guard lock(mutex);
    commands[c].in_pass = false;
  }
  mark(c, "end render pass");
}
void VKAPI_CALL begin_rendering(VkCommandBuffer c, const VkRenderingInfo* i) {
  {
    std::lock_guard lock(mutex);
    if (i->flags & VK_RENDERING_RESUMING_BIT) commands[c].suspended = true;
  }
  mark(c, "before dynamic rendering");
  next<PFN_vkCmdBeginRendering, "vkCmdBeginRendering">(command_device(c))(c, i);
  {
    std::lock_guard lock(mutex);
    auto& s = commands[c];
    s.in_pass = true;
    s.suspended = false;
    s.will_suspend = i->flags & VK_RENDERING_SUSPENDING_BIT;
  }
  mark(c, "begin dynamic rendering");
}
void VKAPI_CALL end_rendering(VkCommandBuffer c) {
  next<PFN_vkCmdEndRendering, "vkCmdEndRendering">(command_device(c))(c);
  {
    std::lock_guard lock(mutex);
    auto& s = commands[c];
    s.in_pass = false;
    s.suspended = s.will_suspend;
  }
  mark(c, "end dynamic rendering");
}
void VKAPI_CALL draw(VkCommandBuffer c, std::uint32_t a, std::uint32_t b,
                     std::uint32_t x, std::uint32_t y) {
  next<PFN_vkCmdDraw, "vkCmdDraw">(command_device(c))(c, a, b, x, y);
  mark(c, "draw");
}
void VKAPI_CALL draw_indexed(VkCommandBuffer c, std::uint32_t a,
                             std::uint32_t b, std::uint32_t x, std::int32_t y,
                             std::uint32_t z) {
  next<PFN_vkCmdDrawIndexed, "vkCmdDrawIndexed">(command_device(c))(c, a, b, x,
                                                                    y, z);
  mark(c, "draw indexed");
}
void VKAPI_CALL draw_indirect(VkCommandBuffer c, VkBuffer b, VkDeviceSize o,
                              std::uint32_t n, std::uint32_t s) {
  next<PFN_vkCmdDrawIndirect, "vkCmdDrawIndirect">(command_device(c))(c, b, o,
                                                                      n, s);
  mark(c, "draw indirect");
}
void VKAPI_CALL draw_indexed_indirect(VkCommandBuffer c, VkBuffer b,
                                      VkDeviceSize o, std::uint32_t n,
                                      std::uint32_t s) {
  next<PFN_vkCmdDrawIndexedIndirect, "vkCmdDrawIndexedIndirect">(command_device(c))(c, b, o, n, s);
  mark(c, "draw indexed indirect");
}
void VKAPI_CALL dispatch(VkCommandBuffer c, std::uint32_t x, std::uint32_t y,
                         std::uint32_t z) {
  next<PFN_vkCmdDispatch, "vkCmdDispatch">(command_device(c))(c, x, y, z);
  mark(c, "dispatch");
}
void VKAPI_CALL dispatch_indirect(VkCommandBuffer c, VkBuffer b,
                                  VkDeviceSize o) {
  next<PFN_vkCmdDispatchIndirect, "vkCmdDispatchIndirect">(command_device(c))(
      c, b, o);
  mark(c, "dispatch indirect");
}
}  // namespace
bool breadcrumbs_enabled() {
  auto p = venture::environment::getenv("VENTURE_VULKAN_BREADCRUMBS");
  return p && std::string_view(p) == "1";
}
void release_breadcrumb_device(VkDevice d) {
  std::lock_guard lock(mutex);
  auto it = devices.find(d);
  if (it == devices.end()) return;
  auto data = it->second;
  if (data.mapped) vk<PFN_vkUnmapMemory>(d, "vkUnmapMemory")(d, data.memory);
  if (data.buffer)
    vk<PFN_vkDestroyBuffer>(d, "vkDestroyBuffer")(d, data.buffer, nullptr);
  if (data.memory)
    vk<PFN_vkFreeMemory>(d, "vkFreeMemory")(d, data.memory, nullptr);
  devices.erase(it);
  prepared_devices.store(!devices.empty(), std::memory_order_relaxed);
  invalidate_layer_procedures();
  std::erase_if(commands, [&](auto& p) { return p.second.device == d; });
  std::erase_if(records, [&](auto& p) {
    if (p.second.device != d) return false;
    for (auto& [name, bytes] : p.second.bytes) retained_bytes -= bytes.size();
    return true;
  });
}
void prepare_breadcrumb_device(VkDevice d, bool checkpoints) {
  std::lock_guard lock(mutex);
  auto& state = devices[d];
  prepared_devices.store(true, std::memory_order_relaxed);
  invalidate_layer_procedures();
  state.checkpoints = checkpoints;
  VkBufferCreateInfo b{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  b.size = slots * 4;
  b.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  if (vk<PFN_vkCreateBuffer>(d, "vkCreateBuffer")(d, &b, nullptr,
                                                  &state.buffer) != VK_SUCCESS)
    return;
  VkMemoryRequirements req{};
  vk<PFN_vkGetBufferMemoryRequirements>(d, "vkGetBufferMemoryRequirements")(
      d, state.buffer, &req);
  VkPhysicalDeviceMemoryProperties props{};
  reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(raw_physical_proc(
      device_physical(d), "vkGetPhysicalDeviceMemoryProperties"))(
      device_physical(d), &props);
  VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  a.allocationSize = req.size;
  a.memoryTypeIndex = props.memoryTypeCount;
  for (std::uint32_t i = 0; i < props.memoryTypeCount; ++i)
    if ((req.memoryTypeBits & (1u << i)) &&
        (props.memoryTypes[i].propertyFlags &
         (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
      a.memoryTypeIndex = i;
      break;
    }
  if (a.memoryTypeIndex == props.memoryTypeCount ||
      vk<PFN_vkAllocateMemory>(d, "vkAllocateMemory")(
          d, &a, nullptr, &state.memory) != VK_SUCCESS)
    return;
  if (vk<PFN_vkBindBufferMemory>(d, "vkBindBufferMemory")(
          d, state.buffer, state.memory, 0) != VK_SUCCESS)
    return;
  if (vk<PFN_vkMapMemory>(d, "vkMapMemory")(
          d, state.memory, 0, VK_WHOLE_SIZE, 0,
          reinterpret_cast<void**>(&state.mapped)) != VK_SUCCESS)
    return;
  std::fill_n(state.mapped, slots, 0);
  log::info("vulkan-breadcrumb",
            "enabled native_draw_checkpoints={} portable_boundary_markers=true",
            checkpoints);
}
void report_breadcrumbs(VkDevice d, VkQueue queue) {
  std::lock_guard lock(mutex);
  auto it = devices.find(d);
  if (it == devices.end()) return;
  if (!capture_directory().empty()) {
    std::string listing;
    for (auto& [id, record] : records) {
      if (record.device != d) continue;
      listing += std::format("marker={} {}\n", id, record.text);
      for (auto& [name, bytes] : record.bytes)
        write_capture(std::format("marker{}-{}", id, name), bytes.data(),
                      bytes.size());
    }
    write_capture("records.txt", listing.data(), listing.size());
  }
  std::uint32_t last{};
  if (it->second.mapped)
    for (std::uint32_t i = 0; i < slots; ++i)
      last = std::max(last, it->second.mapped[i]);
  auto describe = [&](std::uint32_t id) {
    auto r = records.find(id);
    return r != records.end() ? r->second.text
                              : std::string("record unavailable");
  };
  log::warn("vulkan-breadcrumb",
            "last completed portable boundary marker={} {}", last,
            describe(last));
  if (it->second.checkpoints) {
    auto get =
        vk<PFN_vkGetQueueCheckpointDataNV>(d, "vkGetQueueCheckpointDataNV");
    if (!queue) {
      if (auto q = internal_queue(d)) queue = q->queue;
    }
    if (queue) {
      std::uint32_t count{};
      get(queue, &count, nullptr);
      std::vector<VkCheckpointDataNV> data(
          count, {VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV});
      get(queue, &count, data.data());
      for (std::size_t i = 0; i < std::min<std::size_t>(count, data.size());
           ++i) {
        auto id = std::uint32_t(
            reinterpret_cast<std::uintptr_t>(data[i].pCheckpointMarker));
        log::warn("vulkan-breadcrumb", "native stage={:#x} marker={} {}",
                  std::uint32_t(data[i].stage), id, describe(id));
      }
    }
  }
}
PFN_vkVoidFunction breadcrumb_proc(std::string_view name) {
  if (!prepared_devices.load(std::memory_order_relaxed)) return nullptr;
#define FN(n, f) \
  if (name == #n) return reinterpret_cast<PFN_vkVoidFunction>(f)
  FN(vkBeginCommandBuffer, begin);
  FN(vkCmdBindPipeline, bind_pipeline);
  FN(vkCmdBindDescriptorSets, bind_sets);
  FN(vkCmdPushConstants, push_constants);
  FN(vkAllocateMemory, allocate_memory);
  FN(vkFreeMemory, free_memory);
  FN(vkMapMemory, map_memory);
  FN(vkMapMemory2, map_memory2);
  FN(vkUnmapMemory, unmap_memory);
  FN(vkUnmapMemory2, unmap_memory2);
  FN(vkBindBufferMemory, bind_memory);
  FN(vkBindBufferMemory2, bind_memory2);
  FN(vkCreateDescriptorSetLayout, create_layout);
  FN(vkDestroyDescriptorSetLayout, destroy_layouts);
  FN(vkCreateShaderModule, create_shader);
  FN(vkCreateGraphicsPipelines, create_graphics);
  FN(vkCreateComputePipelines, create_compute);
  FN(vkDestroyImage, destroy_images);
  FN(vkDestroyImageView, destroy_views);
  FN(vkDestroyBuffer, destroy_buffers);
  FN(vkDestroyShaderModule, destroy_shaders);
  FN(vkDestroyPipeline, destroy_pipelines);
  FN(vkCreateDescriptorUpdateTemplate, create_template);
  FN(vkDestroyDescriptorUpdateTemplate, destroy_templates);
  FN(vkUpdateDescriptorSetWithTemplate, update_template);
  FN(vkResetDescriptorPool, reset_pool);
  FN(vkAllocateDescriptorSets, allocate_sets);
  FN(vkFreeDescriptorSets, free_sets);
  FN(vkCreateImage, create_image);
  FN(vkCreateImageView, create_view);
  FN(vkCreateBuffer, create_buffer);
  FN(vkUpdateDescriptorSets, update);
  FN(vkCmdBeginRenderPass, begin_pass);
  FN(vkCmdBeginRenderPass2, begin_pass2);
  FN(vkCmdEndRenderPass, end_pass);
  FN(vkCmdEndRenderPass2, end_pass2);
  FN(vkCmdBeginRendering, begin_rendering);
  FN(vkCmdEndRendering, end_rendering);
  FN(vkCmdDraw, draw);
  FN(vkCmdDrawIndexed, draw_indexed);
  FN(vkCmdDrawIndirect, draw_indirect);
  FN(vkCmdDrawIndexedIndirect, draw_indexed_indirect);
  FN(vkCmdDispatch, dispatch);
  FN(vkCmdDispatchIndirect, dispatch_indirect);
#undef FN
  return nullptr;
}
}  // namespace venture::vulkan

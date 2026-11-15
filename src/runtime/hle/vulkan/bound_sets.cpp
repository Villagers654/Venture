// Descriptor sets a pipeline needs that the game never bound.
//
// Vulkan requires every set a pipeline's shaders use to be bound (or pushed)
// on that bind point before the draw or dispatch. Adreno tolerates a set the
// game never bound, so Quest titles that leave one out work there. AMD's
// Windows driver resolves the unbound set's table pointer of 0 to its debug
// descriptor at the start of the descriptor-table range, a buffer at
// 0xdeadbeefdeadbeef with unlimited range, and the read loses the device
// (faults just past 0xbeefdeadbeef: H3VR2, RUMBLE, issue 126).
//
// This layer sits above the texture layer, so it sees the game's commands
// with the game's handles, and it never hooks a draw or dispatch: those stay
// direct driver calls from the JIT. Per command buffer and bind point it keeps
// a mask of the set indices bound or pushed since vkBeginCommandBuffer, with
// the compatibility hash each was bound with. When a pipeline is bound, each
// set index its layout gives bindings that nothing bound yet gets a set of null
// descriptors bound (pushed, for a push descriptor set) right then, through the
// layers below and with the pipeline's own layout; a set the game binds later
// replaces it. A game bind that disturbs one of those null sets (an
// incompatible layout) binds it again. The null sets come from Venture's own
// pools, one per identically defined set layout, written once.
//
// A null set the game never replaced before the next pipeline bind or the end
// of recording is counted, and logged once per pipeline layout: draws and
// dispatches in between read it. Pipelines and pipeline layouts are found in
// read-mostly tables without locks, and command buffer records through a
// per-thread cache (a command buffer is recorded by one thread at a time), so
// a bind takes no lock. VENTURE_VULKAN_UNBOUND_SETS=log only counts and logs,
// =0 turns the layer off.
#include "hle/vulkan/internal.h"
#include "common/environment.h"
#include "common/log.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <deque>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace venture::vulkan {
namespace {
std::uint64_t mix(std::uint64_t h, std::uint64_t v) {
  h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
  h *= 0xff51afd7ed558ccdull;
  return h ^ (h >> 33);
}
}  // namespace

std::uint64_t set_layout_hash(const VkDescriptorSetLayoutCreateInfo& info) {
  std::uint64_t h = mix(0x5e7c0de5ull, info.flags);
  const VkDescriptorBindingFlags* flags = nullptr;
  for (auto* next = static_cast<const VkBaseInStructure*>(info.pNext); next; next = next->pNext)
    if (next->sType == VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO) {
      const auto& f = *reinterpret_cast<const VkDescriptorSetLayoutBindingFlagsCreateInfo*>(next);
      if (f.bindingCount == info.bindingCount) flags = f.pBindingFlags;
    }
  std::vector<std::uint32_t> order;
  for (std::uint32_t i = 0; info.pBindings && i < info.bindingCount; ++i) order.push_back(i);
  std::ranges::sort(order, {}, [&](std::uint32_t i) { return info.pBindings[i].binding; });
  for (const auto i : order) {
    const auto& b = info.pBindings[i];
    h = mix(h, b.binding);
    h = mix(h, std::uint64_t(b.descriptorType));
    h = mix(h, b.descriptorCount);
    h = mix(h, b.stageFlags);
    h = mix(h, flags ? flags[i] : 0);
    if (b.pImmutableSamplers && (b.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER ||
                                 b.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER))
      for (std::uint32_t j = 0; j < b.descriptorCount; ++j)
        h = mix(h, reinterpret_cast<std::uint64_t>(b.pImmutableSamplers[j]));
  }
  return h;
}
PipelineLayoutShape pipeline_layout_shape(std::span<const SetLayoutShape> sets,
                                          std::span<const VkPushConstantRange> ranges) {
  PipelineLayoutShape shape;
  std::uint64_t h = mix(0x9u, ranges.size());
  for (const auto& r : ranges) h = mix(mix(mix(h, r.stageFlags), r.offset), r.size);
  shape.count = std::uint32_t(std::min<std::size_t>(sets.size(), max_tracked_sets));
  for (std::uint32_t i = 0; i < shape.count; ++i) {
    h = mix(h, sets[i].hash);
    shape.prefix[i] = h;
    if (sets[i].used) shape.required |= 1u << i;
    if (sets[i].push) shape.push |= 1u << i;
  }
  return shape;
}
void BindPointSets::bind(const PipelineLayoutShape* layout, std::uint32_t first, std::uint32_t count) {
  const auto end = std::min<std::uint64_t>(std::uint64_t(first) + count, max_tracked_sets);
  for (std::uint64_t i = first; i < end; ++i) {
    bound |= 1u << i;
    compat[i] = layout && i < layout->count ? layout->prefix[i] : unknown;
  }
}
BindPointSets::Unmet BindPointSets::unmet(const PipelineLayoutShape& pipeline) const {
  Unmet u;
  for (auto m = pipeline.required; m; m &= m - 1) {
    const auto i = std::countr_zero(m);
    const auto bit = 1u << i;
    if (!(bound & bit)) u.missing |= bit;
    else if (compat[i] != unknown && compat[i] != pipeline.prefix[i]) u.incompatible |= bit;
  }
  return u;
}

namespace {
enum class Mode { off, log, guard };
Mode mode() {
  static const Mode value = [] {
    const auto* v = venture::environment::getenv("VENTURE_VULKAN_UNBOUND_SETS");
    if (v && std::string_view(v) == "0") return Mode::off;
    if (v && std::string_view(v) == "log") return Mode::log;
    return Mode::guard;
  }();
  return value;
}

struct SetRecord {
  VkDevice device{};
  std::uint64_t hash = 0;
  bool used = false, push = false;
  bool poolable = true;  // a null set can be allocated from a pool of Venture's
  bool sampler = false;  // the null writes need a sampler
  std::uint32_t dynamic = 0;  // dynamic buffer descriptors: offsets a bind supplies
  VkDescriptorPoolCreateFlags pool_flags = 0;
  std::vector<VkDescriptorPoolSize> sizes;
  std::vector<NullFillBinding> plan;
};
struct LayoutRecord {
  VkDevice device{};
  VkPipelineLayout handle{};
  bool null_descriptor = false;  // the device enabled nullDescriptor
  mutable std::atomic<bool> alive{true};
  mutable std::atomic<bool> logged{false}, logged_guard{false};
  mutable std::atomic<std::uint32_t> unguardable{0};  // set indices the guard cannot serve
  PipelineLayoutShape shape;
  std::array<std::shared_ptr<const SetRecord>, max_tracked_sets> sets;
  // Venture's null set for each index once made, and the sampler of push writes.
  mutable std::array<std::atomic<VkDescriptorSet>, max_tracked_sets> nulls{};
  mutable std::atomic<VkSampler> sampler{};
};
struct DeviceRecord {
  // Pipeline layout records live as long as the device: command buffer state
  // points at them, and a game may destroy a layout after creating pipelines.
  std::deque<std::unique_ptr<LayoutRecord>> layouts;
  VkSampler sampler{};
  std::vector<VkDescriptorPool> pools;
  std::map<std::uint64_t, VkDescriptorSet> null_sets;  // by set layout hash
};

template <class Handle> std::uint64_t key_of(Handle handle) {
  return reinterpret_cast<std::uint64_t>(handle);
}

// Handle to layout record, read without a lock. Writers (object creation and
// destruction) serialize on the table's mutex and publish a slot's value before
// its key. A full table is replaced by a larger one; the old one is kept, since
// a reader may still be probing it.
class HandleTable {
 public:
  const LayoutRecord* find(std::uint64_t key) const {
    const auto* t = current_.load(std::memory_order_acquire);
    if (!t || key == empty || key == tomb) return nullptr;
    for (auto i = slot_of(key, t->mask);; i = (i + 1) & t->mask) {
      const auto k = t->slots[i].key.load(std::memory_order_acquire);
      if (k == key) return t->slots[i].value.load(std::memory_order_relaxed);
      if (k == empty) return nullptr;
    }
  }
  void insert(std::uint64_t key, const LayoutRecord* value) {
    if (key == empty || key == tomb) return;
    std::lock_guard lock(mutex_);
    auto* t = current_.load(std::memory_order_relaxed);
    if (auto* slot = locate(t, key)) {
      slot->value.store(value, std::memory_order_release);
      return;
    }
    if (!t || (t->used + 1) * 2 > t->mask + 1) t = grow(t);
    place(*t, key, value);
  }
  void erase(std::uint64_t key) {
    std::lock_guard lock(mutex_);
    auto* t = current_.load(std::memory_order_relaxed);
    if (auto* slot = locate(t, key)) {
      slot->key.store(tomb, std::memory_order_release);
      --t->live;
    }
  }
  template <class F> void erase_if(F&& f) {
    std::lock_guard lock(mutex_);
    auto* t = current_.load(std::memory_order_relaxed);
    for (std::size_t i = 0; t && i <= t->mask; ++i) {
      auto& s = t->slots[i];
      const auto k = s.key.load(std::memory_order_relaxed);
      if (k != empty && k != tomb && f(s.value.load(std::memory_order_relaxed))) {
        s.key.store(tomb, std::memory_order_release);
        --t->live;
      }
    }
  }

 private:
  static constexpr std::uint64_t empty = 0, tomb = ~std::uint64_t(0);
  struct Slot {
    std::atomic<std::uint64_t> key{empty};
    std::atomic<const LayoutRecord*> value{nullptr};
  };
  struct Table {
    explicit Table(std::size_t n) : mask(n - 1), slots(std::make_unique<Slot[]>(n)) {}
    std::size_t mask, used = 0, live = 0;
    std::unique_ptr<Slot[]> slots;
  };
  static std::size_t slot_of(std::uint64_t key, std::size_t mask) {
    return std::size_t((key * 0x9e3779b97f4a7c15ull) >> 29) & mask;
  }
  static Slot* locate(Table* t, std::uint64_t key) {
    if (!t) return nullptr;
    for (auto i = slot_of(key, t->mask);; i = (i + 1) & t->mask) {
      const auto k = t->slots[i].key.load(std::memory_order_relaxed);
      if (k == key) return &t->slots[i];
      if (k == empty) return nullptr;
    }
  }
  static void place(Table& t, std::uint64_t key, const LayoutRecord* value) {
    for (auto i = slot_of(key, t.mask);; i = (i + 1) & t.mask) {
      auto& s = t.slots[i];
      const auto k = s.key.load(std::memory_order_relaxed);
      if (k != empty && k != tomb) continue;
      if (k == empty) ++t.used;
      ++t.live;
      s.value.store(value, std::memory_order_relaxed);
      s.key.store(key, std::memory_order_release);
      return;
    }
  }
  Table* grow(Table* old) {
    std::size_t n = 1024;
    while (old && n < old->live * 4) n *= 2;
    auto table = std::make_unique<Table>(n);
    for (std::size_t i = 0; old && i <= old->mask; ++i) {
      const auto k = old->slots[i].key.load(std::memory_order_relaxed);
      if (k != empty && k != tomb) place(*table, k, old->slots[i].value.load(std::memory_order_relaxed));
    }
    auto* t = table.get();
    tables_.push_back(std::move(table));
    current_.store(t, std::memory_order_release);
    return t;
  }
  std::mutex mutex_;
  std::atomic<Table*> current_{nullptr};
  std::vector<std::unique_ptr<Table>> tables_;
};

std::shared_mutex objects_mutex;  // object creation and destruction, null set creation
std::unordered_map<VkDescriptorSetLayout, std::shared_ptr<const SetRecord>> set_layouts;
std::map<std::pair<VkDevice, std::uint64_t>, std::vector<VkDescriptorSetLayout>> alive_by_hash;
std::unordered_map<VkDescriptorUpdateTemplate, VkPipelineBindPoint> push_templates;
std::map<VkDevice, DeviceRecord> devices;
HandleTable pipeline_layouts, pipelines;

struct Point {
  const LayoutRecord* pipeline = nullptr;
  BindPointSets sets;
  std::uint32_t watch = 0;     // set indices a bound pipeline needed that the game has not bound since
  std::uint32_t nulls = 0;     // of those, holding Venture's null set
  std::uint32_t reported = 0;  // of those, counted already
};
struct Command {
  std::atomic<VkCommandBuffer> owner{};  // the command buffer this record belongs to now
  Point points[2];                       // graphics, compute
  std::uint32_t venture_passes = 0;
  VkDevice device{};
  VkCommandPool pool{};
};
std::mutex commands_mutex;
std::unordered_map<VkCommandBuffer, Command*> commands;
std::deque<Command> command_storage;  // never shrinks: per-thread caches may still point at a record
std::vector<Command*> free_records;

std::atomic<std::uint64_t> hits{0}, guards{0};
std::atomic<int> log_budget{64};
std::mutex notes_mutex;
std::vector<std::string> first_notes;

Point* point_of(Command& c, VkPipelineBindPoint point) {
  if (point == VK_PIPELINE_BIND_POINT_GRAPHICS) return &c.points[0];
  if (point == VK_PIPELINE_BIND_POINT_COMPUTE) return &c.points[1];
  return nullptr;
}
void clear(Command& c) {
  for (auto& p : c.points) p = Point{};
  c.venture_passes = 0;
}
// Under commands_mutex.
Command* record_for(VkCommandBuffer command) {
  auto& slot = commands[command];
  if (!slot) {
    if (free_records.empty()) {
      slot = &command_storage.emplace_back();
    } else {
      slot = free_records.back();
      free_records.pop_back();
    }
    clear(*slot);
    slot->device = {};
    slot->pool = {};
    slot->owner.store(command, std::memory_order_release);
  }
  return slot;
}
// Under commands_mutex.
void release(std::unordered_map<VkCommandBuffer, Command*>::iterator it) {
  it->second->owner.store(VK_NULL_HANDLE, std::memory_order_release);
  free_records.push_back(it->second);
  commands.erase(it);
}
// The record of a command buffer. Only the thread recording it uses it, so a
// per-thread cache entry holds while the record still belongs to that buffer.
Command* find(VkCommandBuffer command, bool create) {
  struct Entry {
    VkCommandBuffer command{};
    Command* state{};
  };
  thread_local std::array<Entry, 4> cache;
  thread_local std::uint32_t next = 0;
  for (const auto& e : cache)
    if (e.command == command && e.state->owner.load(std::memory_order_acquire) == command) return e.state;
  std::lock_guard lock(commands_mutex);
  Command* state = nullptr;
  if (create) state = record_for(command);
  else if (auto it = commands.find(command); it != commands.end()) state = it->second;
  if (state) cache[next++ % cache.size()] = {command, state};
  return state;
}

template <class F, ProcedureName Name> F next_device(VkDevice device) {
  if (auto f = cached_layer_proc<Layer::bound_sets, Name>()) return reinterpret_cast<F>(f);
  return reinterpret_cast<F>(raw_device_proc(device, Name.value));
}
template <class F, ProcedureName Name> F next_command(VkCommandBuffer command) {
  if (auto f = cached_layer_proc<Layer::bound_sets, Name>()) return reinterpret_cast<F>(f);
  return reinterpret_cast<F>(raw_device_proc(command_device(command), Name.value));
}
template <class F> F host(VkDevice device, const char* name) {
  return reinterpret_cast<F>(raw_device_proc(device, name));
}

// Null descriptor storage shared by every write; never written by the driver.
const VkDescriptorBufferInfo* null_buffers() {
  static const std::vector<VkDescriptorBufferInfo> values(max_null_fill_count,
                                                          VkDescriptorBufferInfo{VK_NULL_HANDLE, 0, VK_WHOLE_SIZE});
  return values.data();
}
const VkBufferView* null_views() {
  static const std::vector<VkBufferView> values(max_null_fill_count, VK_NULL_HANDLE);
  return values.data();
}
// Under objects_mutex (unique).
VkSampler device_sampler(VkDevice device, DeviceRecord& record) {
  if (record.sampler) return record.sampler;
  VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  info.magFilter = info.minFilter = VK_FILTER_NEAREST;
  info.addressModeU = info.addressModeV = info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  info.maxLod = VK_LOD_CLAMP_NONE;
  if (host<PFN_vkCreateSampler>(device, "vkCreateSampler")(device, &info, nullptr, &record.sampler) != VK_SUCCESS)
    record.sampler = VK_NULL_HANDLE;
  return record.sampler;
}
// Null writes of a plan into `writes`; `images` must outlive their use.
void null_writes(const SetRecord& set, VkDescriptorSet target, VkSampler sampler,
                 std::vector<VkDescriptorImageInfo>& images, std::vector<VkWriteDescriptorSet>& writes) {
  images.assign(max_null_fill_count, {sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
  writes.clear();
  for (const auto& b : set.plan) {
    if (b.variable || !b.count || (b.sampler && !sampler)) continue;
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = target;
    w.dstBinding = b.binding;
    w.descriptorCount = b.count;
    w.descriptorType = b.type;
    w.pImageInfo = images.data();
    w.pBufferInfo = null_buffers();
    w.pTexelBufferView = null_views();
    writes.push_back(w);
  }
}
// A set of null descriptors for `set`'s definition, allocated once per device.
VkDescriptorSet null_set(VkDevice device, const SetRecord& set, std::string& why) {
  std::unique_lock lock(objects_mutex);
  auto& record = devices[device];
  if (auto it = record.null_sets.find(set.hash); it != record.null_sets.end()) return it->second;
  // Any live layout defined identically allocates a set compatible with this one.
  const auto alive = alive_by_hash.find({device, set.hash});
  if (alive == alive_by_hash.end() || alive->second.empty()) {
    why = "the game destroyed its set layout";
    return VK_NULL_HANDLE;
  }
  VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pi.flags = set.pool_flags;
  pi.maxSets = 1;
  pi.poolSizeCount = std::uint32_t(set.sizes.size());
  pi.pPoolSizes = set.sizes.data();
  VkDescriptorPool pool{};
  if (host<PFN_vkCreateDescriptorPool>(device, "vkCreateDescriptorPool")(device, &pi, nullptr, &pool) !=
      VK_SUCCESS) {
    why = "Venture could not create a descriptor pool for it";
    return VK_NULL_HANDLE;
  }
  record.pools.push_back(pool);
  VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  ai.descriptorPool = pool;
  ai.descriptorSetCount = 1;
  ai.pSetLayouts = &alive->second.front();
  VkDescriptorSet result{};
  if (host<PFN_vkAllocateDescriptorSets>(device, "vkAllocateDescriptorSets")(device, &ai, &result) != VK_SUCCESS) {
    why = "Venture could not allocate a set for it";
    return VK_NULL_HANDLE;
  }
  std::vector<VkDescriptorImageInfo> images;
  std::vector<VkWriteDescriptorSet> writes;
  null_writes(set, result, set.sampler ? device_sampler(device, record) : VK_NULL_HANDLE, images, writes);
  if (!writes.empty())
    host<PFN_vkUpdateDescriptorSets>(device, "vkUpdateDescriptorSets")(device, std::uint32_t(writes.size()),
                                                                       writes.data(), 0, nullptr);
  record.null_sets[set.hash] = result;
  return result;
}
// Binds or pushes null descriptors at set `index` of `layout` on `point`.
bool guard(VkCommandBuffer command, VkPipelineBindPoint point, const LayoutRecord& layout, std::uint32_t index,
           std::string& why) {
  const auto& set = layout.sets[index];
  if (!set) {
    why = "Venture did not see its set layout created";
    return false;
  }
  if (!layout.alive.load(std::memory_order_relaxed)) {
    why = "the game destroyed the pipeline layout";
    return false;
  }
  if (!layout.null_descriptor) {
    why = "the device has no nullDescriptor";
    return false;
  }
  if (set->push) {
    auto sampler = layout.sampler.load(std::memory_order_acquire);
    if (set->sampler && !sampler) {
      std::unique_lock lock(objects_mutex);
      sampler = device_sampler(layout.device, devices[layout.device]);
      layout.sampler.store(sampler, std::memory_order_release);
    }
    thread_local std::vector<VkDescriptorImageInfo> images;
    thread_local std::vector<VkWriteDescriptorSet> writes;
    null_writes(*set, VK_NULL_HANDLE, sampler, images, writes);
    if (writes.empty()) {
      why = "it has no bindings Venture can write null";
      return false;
    }
    next_command<PFN_vkCmdPushDescriptorSet, "vkCmdPushDescriptorSet">(command)(
        command, point, layout.handle, index, std::uint32_t(writes.size()), writes.data());
    return true;
  }
  if (!set->poolable) {
    why = "its set layout holds inline uniform blocks, acceleration structures, mutable descriptors, very large "
          "arrays or descriptor buffer bindings";
    return false;
  }
  auto target = layout.nulls[index].load(std::memory_order_acquire);
  if (!target) {
    target = null_set(layout.device, *set, why);
    if (!target) return false;
    layout.nulls[index].store(target, std::memory_order_release);
  }
  thread_local std::vector<std::uint32_t> offsets;
  offsets.assign(set->dynamic, 0);
  next_command<PFN_vkCmdBindDescriptorSets, "vkCmdBindDescriptorSets">(command)(
      command, point, layout.handle, index, 1, &target, set->dynamic, offsets.data());
  return true;
}
void note(const std::string& message) {
  if (log_budget.fetch_sub(1, std::memory_order_relaxed) > 0) log::warn("vulkan", "{}", message);
  std::lock_guard lock(notes_mutex);
  if (first_notes.size() < 4) first_notes.push_back(message);
}
std::string caller(VkPipelineBindPoint point, std::uint32_t venture_passes) {
  if (point != VK_PIPELINE_BIND_POINT_COMPUTE || !venture_passes) return "recorded by the game";
  return std::format("recorded by the game after {} Venture compute pass{} (texture decode or depth conversion) in "
                     "the same command buffer", venture_passes, venture_passes == 1 ? "" : "es");
}
// Sets the bound pipeline needed that nothing bound when it was bound: binds
// Venture's null sets there.
void guard_missing(VkCommandBuffer command, Point& p, VkPipelineBindPoint point) {
  if (!p.pipeline) return;
  const auto& layout = *p.pipeline;
  const auto missing = layout.shape.required & ~p.sets.bound;
  if (!missing) return;
  p.watch |= missing;
  if (mode() != Mode::guard) return;
  for (auto m = missing & ~layout.unguardable.load(std::memory_order_relaxed); m; m &= m - 1) {
    const auto index = std::uint32_t(std::countr_zero(m));
    std::string why;
    if (guard(command, point, layout, index, why)) {
      p.sets.bind(&layout.shape, index, 1);
      p.nulls |= 1u << index;
      guards.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    layout.unguardable.fetch_or(1u << index, std::memory_order_relaxed);
    if (!layout.logged_guard.exchange(true, std::memory_order_relaxed))
      note(std::format("Venture cannot bind null descriptors at set {} of pipeline layout {:#x}: {}", index,
                       reinterpret_cast<std::uintptr_t>(layout.handle), why));
  }
}
// Sets the bound pipeline needed that the game left unbound up to now: draws
// and dispatches since its bind read them. Counted once each, logged once per
// pipeline layout.
void count_unbound(const Command& c, Point& p, VkPipelineBindPoint point, const char* until) {
  if (!p.pipeline) return;
  const auto unbound = p.watch & ~p.reported & p.pipeline->shape.required;
  if (!unbound) return;
  p.reported |= unbound;
  hits.fetch_add(std::popcount(unbound), std::memory_order_relaxed);
  const auto& layout = *p.pipeline;
  if (layout.logged.exchange(true, std::memory_order_relaxed)) return;
  const auto index = std::countr_zero(unbound);
  note(std::format("Set {} of pipeline layout {:#x} was never bound or pushed by the game on that bind point between "
                   "the pipeline bind and {} ({}): {}",
                   index, reinterpret_cast<std::uintptr_t>(layout.handle), until, caller(point, c.venture_passes),
                   p.nulls >> index & 1
                       ? (layout.shape.push >> index & 1 ? "Venture pushed null descriptors there"
                                                         : "Venture bound null descriptors there")
                       : "it stayed unbound"));
}

void release_device(VkDevice device) {
  {
    // Buffers of the device that were never freed.
    std::lock_guard lock(commands_mutex);
    for (auto it = commands.begin(); it != commands.end();) {
      auto next = std::next(it);
      if (it->second->device == device) release(it);
      it = next;
    }
  }
  const auto of_device = [&](const LayoutRecord* r) { return r->device == device; };
  pipelines.erase_if(of_device);
  pipeline_layouts.erase_if(of_device);
  std::vector<VkDescriptorPool> pools;
  VkSampler sampler{};
  {
    std::unique_lock lock(objects_mutex);
    std::erase_if(set_layouts, [&](const auto& e) { return e.second->device == device; });
    std::erase_if(alive_by_hash, [&](const auto& e) { return e.first.first == device; });
    if (auto it = devices.find(device); it != devices.end()) {
      pools = std::move(it->second.pools);
      sampler = it->second.sampler;
      devices.erase(it);
    }
  }
  if (!pools.empty()) {
    auto destroy = host<PFN_vkDestroyDescriptorPool>(device, "vkDestroyDescriptorPool");
    for (auto pool : pools) destroy(device, pool, nullptr);
  }
  if (sampler) host<PFN_vkDestroySampler>(device, "vkDestroySampler")(device, sampler, nullptr);
}
void hook_release() {
  static std::once_flag once;
  std::call_once(once, [] { add_device_release_hook(release_device); });
}

VkResult VKAPI_CALL create_set_layout(VkDevice device, const VkDescriptorSetLayoutCreateInfo* info,
                                      const VkAllocationCallbacks* alloc, VkDescriptorSetLayout* out) {
  const auto result =
      next_device<PFN_vkCreateDescriptorSetLayout, "vkCreateDescriptorSetLayout">(device)(device, info, alloc, out);
  if (result != VK_SUCCESS || !info || !out) return result;
  hook_release();
  auto record = std::make_shared<SetRecord>();
  record->device = device;
  record->hash = set_layout_hash(*info);
  record->push = (info->flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT) != 0;
  record->poolable = !(info->flags & (VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT |
                                      VK_DESCRIPTOR_SET_LAYOUT_CREATE_HOST_ONLY_POOL_BIT_EXT));
  if (info->flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT)
    record->pool_flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
  std::map<VkDescriptorType, std::uint32_t> sizes;
  for (std::uint32_t i = 0; info->pBindings && i < info->bindingCount; ++i) {
    const auto& b = info->pBindings[i];
    if (!b.descriptorCount) continue;
    record->used = true;
    switch (b.descriptorType) {
      case VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK:
      case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
      case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_NV:
      case VK_DESCRIPTOR_TYPE_MUTABLE_EXT:
        record->poolable = false;
        break;
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
        record->dynamic += b.descriptorCount;
        break;
      default: break;
    }
    if (b.descriptorCount > 65536) record->poolable = false;
    sizes[b.descriptorType] += b.descriptorCount;
  }
  for (const auto& [type, count] : sizes) record->sizes.push_back({type, count});
  if (record->sizes.empty()) record->poolable = false;
  record->plan = null_fill_plan(*info, true);
  record->sampler = std::ranges::any_of(record->plan, [](const NullFillBinding& b) { return b.sampler; });
  std::unique_lock lock(objects_mutex);
  alive_by_hash[{device, record->hash}].push_back(*out);
  set_layouts[*out] = std::move(record);
  return result;
}
void VKAPI_CALL destroy_set_layout(VkDevice device, VkDescriptorSetLayout layout, const VkAllocationCallbacks* alloc) {
  if (layout) {
    std::unique_lock lock(objects_mutex);
    if (auto it = set_layouts.find(layout); it != set_layouts.end()) {
      if (auto alive = alive_by_hash.find({device, it->second->hash}); alive != alive_by_hash.end()) {
        std::erase(alive->second, layout);
        if (alive->second.empty()) alive_by_hash.erase(alive);
      }
      set_layouts.erase(it);
    }
  }
  next_device<PFN_vkDestroyDescriptorSetLayout, "vkDestroyDescriptorSetLayout">(device)(device, layout, alloc);
}
VkResult VKAPI_CALL create_pipeline_layout(VkDevice device, const VkPipelineLayoutCreateInfo* info,
                                           const VkAllocationCallbacks* alloc, VkPipelineLayout* out) {
  const auto result =
      next_device<PFN_vkCreatePipelineLayout, "vkCreatePipelineLayout">(device)(device, info, alloc, out);
  if (result != VK_SUCCESS || !info || !out) return result;
  hook_release();
  auto record = std::make_unique<LayoutRecord>();
  record->device = device;
  record->handle = *out;
  record->null_descriptor = device_null_descriptor(device);
  std::vector<SetLayoutShape> shapes(std::min(info->pSetLayouts ? info->setLayoutCount : 0u, max_tracked_sets));
  {
    std::shared_lock lock(objects_mutex);
    for (std::uint32_t i = 0; i < shapes.size(); ++i) {
      const auto it = info->pSetLayouts[i] ? set_layouts.find(info->pSetLayouts[i]) : set_layouts.end();
      if (it == set_layouts.end()) {
        // Unknown or VK_NULL_HANDLE (independent sets): never required.
        shapes[i].hash = reinterpret_cast<std::uint64_t>(info->pSetLayouts[i]);
        continue;
      }
      record->sets[i] = it->second;
      shapes[i] = {it->second->hash, it->second->used, it->second->push};
    }
  }
  record->shape = pipeline_layout_shape(
      shapes, std::span(info->pPushConstantRanges, info->pPushConstantRanges ? info->pushConstantRangeCount : 0));
  const auto* published = record.get();
  {
    std::unique_lock lock(objects_mutex);
    devices[device].layouts.push_back(std::move(record));
  }
  pipeline_layouts.insert(key_of(*out), published);
  return result;
}
void VKAPI_CALL destroy_pipeline_layout(VkDevice device, VkPipelineLayout layout, const VkAllocationCallbacks* alloc) {
  if (layout) {
    if (const auto* record = pipeline_layouts.find(key_of(layout)))
      record->alive.store(false, std::memory_order_relaxed);
    pipeline_layouts.erase(key_of(layout));
  }
  next_device<PFN_vkDestroyPipelineLayout, "vkDestroyPipelineLayout">(device)(device, layout, alloc);
}
template <class Info> void record_pipelines(std::uint32_t count, const Info* infos, const VkPipeline* out) {
  if (!infos || !out) return;
  for (std::uint32_t i = 0; i < count; ++i) {
    if (!out[i]) continue;
    if (const auto* layout = pipeline_layouts.find(key_of(infos[i].layout))) pipelines.insert(key_of(out[i]), layout);
    else pipelines.erase(key_of(out[i]));
  }
}
VkResult VKAPI_CALL create_graphics(VkDevice device, VkPipelineCache cache, std::uint32_t count,
                                    const VkGraphicsPipelineCreateInfo* infos, const VkAllocationCallbacks* alloc,
                                    VkPipeline* out) {
  const auto result = next_device<PFN_vkCreateGraphicsPipelines, "vkCreateGraphicsPipelines">(device)(
      device, cache, count, infos, alloc, out);
  if (result >= VK_SUCCESS) record_pipelines(count, infos, out);
  return result;
}
VkResult VKAPI_CALL create_compute(VkDevice device, VkPipelineCache cache, std::uint32_t count,
                                   const VkComputePipelineCreateInfo* infos, const VkAllocationCallbacks* alloc,
                                   VkPipeline* out) {
  const auto result = next_device<PFN_vkCreateComputePipelines, "vkCreateComputePipelines">(device)(
      device, cache, count, infos, alloc, out);
  if (result >= VK_SUCCESS) record_pipelines(count, infos, out);
  return result;
}
void VKAPI_CALL destroy_pipeline(VkDevice device, VkPipeline pipeline, const VkAllocationCallbacks* alloc) {
  if (pipeline) pipelines.erase(key_of(pipeline));
  next_device<PFN_vkDestroyPipeline, "vkDestroyPipeline">(device)(device, pipeline, alloc);
}
VkResult VKAPI_CALL create_template(VkDevice device, const VkDescriptorUpdateTemplateCreateInfo* info,
                                    const VkAllocationCallbacks* alloc, VkDescriptorUpdateTemplate* out) {
  const auto result = next_device<PFN_vkCreateDescriptorUpdateTemplate, "vkCreateDescriptorUpdateTemplate">(device)(
      device, info, alloc, out);
  if (result == VK_SUCCESS && info && out &&
      info->templateType == VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS) {
    std::unique_lock lock(objects_mutex);
    push_templates[*out] = info->pipelineBindPoint;
  }
  return result;
}
void VKAPI_CALL destroy_template(VkDevice device, VkDescriptorUpdateTemplate t, const VkAllocationCallbacks* alloc) {
  if (t) {
    std::unique_lock lock(objects_mutex);
    push_templates.erase(t);
  }
  next_device<PFN_vkDestroyDescriptorUpdateTemplate, "vkDestroyDescriptorUpdateTemplate">(device)(device, t, alloc);
}
VkPipelineBindPoint template_point(VkDescriptorUpdateTemplate t) {
  std::shared_lock lock(objects_mutex);
  const auto it = push_templates.find(t);
  return it == push_templates.end() ? VK_PIPELINE_BIND_POINT_MAX_ENUM : it->second;
}

VkResult VKAPI_CALL allocate_commands(VkDevice device, const VkCommandBufferAllocateInfo* info,
                                      VkCommandBuffer* out) {
  const auto result =
      next_device<PFN_vkAllocateCommandBuffers, "vkAllocateCommandBuffers">(device)(device, info, out);
  if (result != VK_SUCCESS || !info || !out) return result;
  std::lock_guard lock(commands_mutex);
  for (std::uint32_t i = 0; i < info->commandBufferCount; ++i) {
    auto* state = record_for(out[i]);
    clear(*state);
    state->device = device;
    state->pool = info->commandPool;
  }
  return result;
}
void VKAPI_CALL free_commands(VkDevice device, VkCommandPool pool, std::uint32_t count,
                              const VkCommandBuffer* buffers) {
  {
    std::lock_guard lock(commands_mutex);
    for (std::uint32_t i = 0; buffers && i < count; ++i)
      if (auto it = buffers[i] ? commands.find(buffers[i]) : commands.end(); it != commands.end()) release(it);
  }
  next_device<PFN_vkFreeCommandBuffers, "vkFreeCommandBuffers">(device)(device, pool, count, buffers);
}
void VKAPI_CALL destroy_command_pool(VkDevice device, VkCommandPool pool, const VkAllocationCallbacks* alloc) {
  if (pool) {
    std::lock_guard lock(commands_mutex);
    for (auto it = commands.begin(); it != commands.end();) {
      auto next = std::next(it);
      if (it->second->pool == pool) release(it);
      it = next;
    }
  }
  next_device<PFN_vkDestroyCommandPool, "vkDestroyCommandPool">(device)(device, pool, alloc);
}
VkResult VKAPI_CALL begin(VkCommandBuffer command, const VkCommandBufferBeginInfo* info) {
  const auto result = next_command<PFN_vkBeginCommandBuffer, "vkBeginCommandBuffer">(command)(command, info);
  if (result == VK_SUCCESS) clear(*find(command, true));
  return result;
}
VkResult VKAPI_CALL end(VkCommandBuffer command) {
  if (auto* state = find(command, false)) {
    count_unbound(*state, state->points[0], VK_PIPELINE_BIND_POINT_GRAPHICS, "the end of recording");
    count_unbound(*state, state->points[1], VK_PIPELINE_BIND_POINT_COMPUTE, "the end of recording");
  }
  return next_command<PFN_vkEndCommandBuffer, "vkEndCommandBuffer">(command)(command);
}
void VKAPI_CALL bind_pipeline(VkCommandBuffer command, VkPipelineBindPoint point, VkPipeline pipeline) {
  next_command<PFN_vkCmdBindPipeline, "vkCmdBindPipeline">(command)(command, point, pipeline);
  if (point != VK_PIPELINE_BIND_POINT_GRAPHICS && point != VK_PIPELINE_BIND_POINT_COMPUTE) return;
  const auto* record = pipelines.find(key_of(pipeline));
  auto* state = find(command, true);
  auto* p = point_of(*state, point);
  if (p->pipeline == record) return;
  count_unbound(*state, *p, point, "the next pipeline bind");
  p->pipeline = record;
  guard_missing(command, *p, point);
}
// The game bound or pushed sets [first, first + count) with `layout`.
void bound(VkCommandBuffer command, VkPipelineBindPoint point, VkPipelineLayout layout, std::uint32_t first,
           std::uint32_t count, const VkDescriptorSet* sets = nullptr) {
  auto* p = point_of(*find(command, true), point);
  if (!p) return;
  const auto* record = pipeline_layouts.find(key_of(layout));
  const auto* shape = record ? &record->shape : nullptr;
  std::uint32_t mask = 0;
  for (std::uint32_t i = 0; i < count && std::uint64_t(first) + i < max_tracked_sets; ++i)
    if (!sets || sets[i]) {
      p->sets.bind(shape, first + i, 1);
      mask |= 1u << (first + i);
    }
  p->watch &= ~mask;
  p->nulls &= ~mask;
  p->reported &= ~mask;
  if (!shape || !p->nulls) return;
  // A layout not compatible for one of Venture's other null sets disturbed it: bind it again.
  std::uint32_t disturbed = 0;
  for (auto m = p->nulls; m; m &= m - 1) {
    const auto i = std::uint32_t(std::countr_zero(m));
    if (i >= shape->count || p->sets.compat[i] != shape->prefix[i]) disturbed |= 1u << i;
  }
  if (!disturbed) return;
  p->sets.bound &= ~disturbed;
  p->nulls &= ~disturbed;
  guard_missing(command, *p, point);
}
template <class F> void for_stage_points(VkShaderStageFlags stages, F&& f) {
  if (stages & (VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT))
    f(VK_PIPELINE_BIND_POINT_GRAPHICS);
  if (stages & VK_SHADER_STAGE_COMPUTE_BIT) f(VK_PIPELINE_BIND_POINT_COMPUTE);
}
void VKAPI_CALL bind_sets(VkCommandBuffer command, VkPipelineBindPoint point, VkPipelineLayout layout,
                          std::uint32_t first, std::uint32_t count, const VkDescriptorSet* sets,
                          std::uint32_t dynamic_count, const std::uint32_t* dynamic) {
  next_command<PFN_vkCmdBindDescriptorSets, "vkCmdBindDescriptorSets">(command)(
      command, point, layout, first, count, sets, dynamic_count, dynamic);
  if (sets) bound(command, point, layout, first, count, sets);
}
void VKAPI_CALL bind_sets2(VkCommandBuffer command, const VkBindDescriptorSetsInfo* info) {
  next_command<PFN_vkCmdBindDescriptorSets2, "vkCmdBindDescriptorSets2">(command)(command, info);
  if (info && info->pDescriptorSets)
    for_stage_points(info->stageFlags, [&](VkPipelineBindPoint point) {
      bound(command, point, info->layout, info->firstSet, info->descriptorSetCount, info->pDescriptorSets);
    });
}
void VKAPI_CALL push_descriptors(VkCommandBuffer command, VkPipelineBindPoint point, VkPipelineLayout layout,
                                 std::uint32_t set, std::uint32_t count, const VkWriteDescriptorSet* writes) {
  next_command<PFN_vkCmdPushDescriptorSet, "vkCmdPushDescriptorSet">(command)(command, point, layout, set, count,
                                                                             writes);
  bound(command, point, layout, set, 1);
}
void VKAPI_CALL push_descriptors2(VkCommandBuffer command, const VkPushDescriptorSetInfo* info) {
  next_command<PFN_vkCmdPushDescriptorSet2, "vkCmdPushDescriptorSet2">(command)(command, info);
  if (info)
    for_stage_points(info->stageFlags,
                     [&](VkPipelineBindPoint point) { bound(command, point, info->layout, info->set, 1); });
}
void VKAPI_CALL push_template(VkCommandBuffer command, VkDescriptorUpdateTemplate t, VkPipelineLayout layout,
                              std::uint32_t set, const void* data) {
  next_command<PFN_vkCmdPushDescriptorSetWithTemplate, "vkCmdPushDescriptorSetWithTemplate">(command)(
      command, t, layout, set, data);
  bound(command, template_point(t), layout, set, 1);
}
void VKAPI_CALL push_template2(VkCommandBuffer command, const VkPushDescriptorSetWithTemplateInfo* info) {
  next_command<PFN_vkCmdPushDescriptorSetWithTemplate2, "vkCmdPushDescriptorSetWithTemplate2">(command)(command,
                                                                                                         info);
  if (info) bound(command, template_point(info->descriptorUpdateTemplate), info->layout, info->set, 1);
}
}  // namespace

void note_venture_compute_pass(VkCommandBuffer command) {
  if (mode() == Mode::off) return;
  if (auto* state = find(command, false)) ++state->venture_passes;
}
std::vector<std::string> bound_sets_fault_notes() {
  if (mode() == Mode::off) return {"the unbound descriptor set check is off (VENTURE_VULKAN_UNBOUND_SETS=0)"};
  const auto h = hits.load(std::memory_order_relaxed);
  if (!h)
    return {"no pipeline so far needed a set index its command buffer never bound or pushed"};
  std::vector<std::string> notes{std::format("{} times a pipeline needed a set index its command buffer never bound "
                                             "or pushed; Venture bound null descriptors {} times at pipeline binds",
                                             h, guards.load(std::memory_order_relaxed))};
  std::lock_guard lock(notes_mutex);
  notes.insert(notes.end(), first_notes.begin(), first_notes.end());
  return notes;
}
PFN_vkVoidFunction bound_sets_proc(std::string_view name) {
  if (mode() == Mode::off) return nullptr;
#define FN(command, function) \
  if (name == #command) return reinterpret_cast<PFN_vkVoidFunction>(function)
  FN(vkCreateDescriptorSetLayout, create_set_layout);
  FN(vkDestroyDescriptorSetLayout, destroy_set_layout);
  FN(vkCreatePipelineLayout, create_pipeline_layout);
  FN(vkDestroyPipelineLayout, destroy_pipeline_layout);
  FN(vkCreateGraphicsPipelines, create_graphics);
  FN(vkCreateComputePipelines, create_compute);
  FN(vkDestroyPipeline, destroy_pipeline);
  FN(vkCreateDescriptorUpdateTemplate, create_template);
  FN(vkDestroyDescriptorUpdateTemplate, destroy_template);
  FN(vkAllocateCommandBuffers, allocate_commands);
  FN(vkFreeCommandBuffers, free_commands);
  FN(vkDestroyCommandPool, destroy_command_pool);
  FN(vkBeginCommandBuffer, begin);
  FN(vkEndCommandBuffer, end);
  FN(vkCmdBindPipeline, bind_pipeline);
  FN(vkCmdBindDescriptorSets, bind_sets);
  FN(vkCmdBindDescriptorSets2, bind_sets2);
  FN(vkCmdPushDescriptorSet, push_descriptors);
  FN(vkCmdPushDescriptorSet2, push_descriptors2);
  FN(vkCmdPushDescriptorSetWithTemplate, push_template);
  FN(vkCmdPushDescriptorSetWithTemplate2, push_template2);
#undef FN
  return nullptr;
}
}  // namespace venture::vulkan

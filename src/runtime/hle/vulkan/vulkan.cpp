#include "common/environment.h"
#include "vulkan.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <future>
#include <thread>
#include <exception>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "android/native_window.h"
#include "callbacks.h"
#include "chain.h"
#include "common/log.h"
#include "core/linker.h"
#include "core/api_result_trace.h"
#include "core/os_quit.h"
#include "core/process.h"
#include "core/signals.h"
#include "core/thread.h"
#include "core/thread_runtime.h"
#include "gfx/gpu_device.h"
#include "gfx/pipeline_trace.h"
#include "gfx/queue_sync.h"
#include "internal.h"
#include "timing.h"
#include "external.h"
#include "window_readback.h"
#include "sync_trace.h"
#include "serialized_queues.h"
#include "deferred_queue_submissions.h"
#include "xr/vulkan_extensions.h"

namespace venture::vulkan {
std::optional<std::pair<std::uint32_t, std::uint32_t>> private_texture_queue(
    std::span<const VkQueueFamilyProperties> families,
    std::span<const VkDeviceQueueCreateInfo> requested) {
  constexpr auto flags = VK_QUEUE_COMPUTE_BIT | VK_QUEUE_SPARSE_BINDING_BIT;
  for (const auto& queue : requested) {
    if (queue.flags || queue.queueFamilyIndex >= families.size()) continue;
    const auto& family = families[queue.queueFamilyIndex];
    std::uint32_t count = 0;
    for (const auto& other : requested)
      if (other.queueFamilyIndex == queue.queueFamilyIndex) count += other.queueCount;
    if ((family.queueFlags & flags) == flags && count < family.queueCount)
      return std::pair{queue.queueFamilyIndex, queue.queueCount};
  }
  for (std::uint32_t family = 0; family < families.size(); ++family) {
    if (!families[family].queueCount || (families[family].queueFlags & flags) != flags)
      continue;
    if (std::ranges::none_of(requested, [=](const auto& queue) {
          return queue.queueFamilyIndex == family;
        }))
      return std::pair{family, 0u};
  }
  return std::nullopt;
}
std::uint32_t android_hardware_version() {
  // The native Vulkan marshaler currently implements the A64 wire ABI only.
  // PackageManager discovery must describe the guest API, not just the host
  // GPU: UE4 probes vkCreateInstance when this feature is advertised.
  if (process_config().isa() == GuestIsa::a32) return 0;
  try {
    auto& gpu = gfx::gpu_device();
    auto properties_fn = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
        gpu.get_instance_proc_addr(gpu.instance, "vkGetPhysicalDeviceProperties"));
    VkPhysicalDeviceProperties properties{};
    properties_fn(gpu.physical_device, &properties);
    // The flags describe the guest API, not the device type: a conformant CPU
    // implementation qualifies too (Android emulator images whose Vulkan runs
    // on SwiftShader declare them), so only the feature checks below apply.
    VkPhysicalDeviceFeatures features{};
    guest_texture_features(gpu.physical_device, features);
    if (!features.textureCompressionETC2) {
      if (venture::environment::getenv("VENTURE_TRACE_VULKAN"))
        log::info("vulkan", "Android Vulkan hardware version unavailable: ETC2 feature");
      return 0;
    }
    // Android PackageManager.FEATURE_VULKAN_HARDWARE_LEVEL requires ETC2 at
    // level zero. FEATURE_VULKAN_HARDWARE_VERSION >= 1.1 additionally requires
    // VK_ANDROID_external_memory_android_hardware_buffer, SYNC_FD semaphores
    // and fences, and samplerYcbcrConversion (developer.android.com/reference/
    // android/content/pm/PackageManager#FEATURE_VULKAN_HARDWARE_VERSION).
    // SYNC_FD is always translated; hardware buffers need host external memory.
    // Android's published flags are 1.0.3 and 1.1.0 (frameworks/native/data/
    // etc/android.hardware.vulkan.version-*.xml). UE4 rejects exactly 1.0.0;
    // Unreal Engine 5 requires 1.1.
    constexpr auto android_1_0 = VK_MAKE_API_VERSION(0, 1, 0, 3), android_1_1 = VK_MAKE_API_VERSION(0, 1, 1, 0);
    if (properties.apiVersion >= android_1_1 && hardware_buffers_supported()) return android_1_1;
    return properties.apiVersion >= android_1_0 ? android_1_0 : 0;
  } catch (const std::exception& e) {
    log::warn("vulkan", "Android Vulkan feature query unavailable: {}", e.what());
    return 0;
  }
}
namespace {
// Diagnostic A/B arm: preserve the original forwarding work while sharing the
// same binary, JIT, layers and driver with the optimized arm.
const bool reference_forwarding = [] {
  const auto* value = venture::environment::getenv("VENTURE_VULKAN_FORWARDING_REFERENCE");
  return value && std::string_view(value) == "1";
}();
const bool forwarding_census_enabled = [] {
  const auto* value = venture::environment::getenv("VENTURE_VULKAN_FORWARD_CENSUS");
  return value && std::string_view(value) == "1";
}();
std::array<std::atomic<std::uint64_t>, 4> lifetime_events{};
struct ForwardingCounters {
  std::int32_t tid;
  std::atomic<std::uint64_t> sequence{};
  std::unique_ptr<std::atomic<std::uint64_t>[]> calls, in_jit;
  std::vector<std::array<std::uint64_t, 2>> previous, next;
  ForwardingCounters(std::int32_t tid, std::size_t count)
      : tid(tid), calls(std::make_unique<std::atomic<std::uint64_t>[]>(count)),
        in_jit(std::make_unique<std::atomic<std::uint64_t>[]>(count)), previous(count), next(count) {}
};
struct ForwardingRegistry {
  std::mutex mutex;
  std::vector<std::unique_ptr<ForwardingCounters>> threads;
  std::atomic<std::uint64_t> dropped_threads{};
};
ForwardingRegistry& forwarding_registry() {
  // Retain records through adopted host-thread teardown, like the HLE census.
  static auto* registry = new ForwardingRegistry;
  return *registry;
}
ForwardingCounters* forwarding_counters(GuestThread& thread, bool initialize) {
  thread_local ForwardingCounters* local = nullptr;
  thread_local bool attempted = false;
  thread_local std::int32_t tid = -1;
  if (tid != thread.tid()) {
    if (!initialize) return nullptr;
    tid = thread.tid(); local = nullptr; attempted = false;
  }
  if (initialize && !attempted) {
    attempted = true;
    auto& registry = forwarding_registry();
    try {
      auto record = std::make_unique<ForwardingCounters>(thread.tid(), command_count());
      std::lock_guard lock(registry.mutex);
      registry.threads.push_back(std::move(record));
      local = registry.threads.back().get();
    } catch (const std::bad_alloc&) {
      registry.dropped_threads.fetch_add(1, std::memory_order_relaxed);
    }
  }
  return local;
}
enum class Scope { global, instance, physical, device, queue, command_buffer };
struct Command {
  std::string_view name, canonical;
  Scope scope;
  std::uint32_t version;
  std::string_view extensions;
  hle::Handler handler;
  PFN_vkVoidFunction driver_observer;
  hle::Execution execution;
  hle::TryHandler try_handler;
};
struct Instance {
  bool host_properties2 = false;
  bool guest_callbacks = false;
  std::set<std::string, std::less<>> extensions;
  std::uint32_t version = VK_API_VERSION_1_0;
};
struct Device {
  bool density_maps = false;
  bool fault_enabled = false, fault_reported = false;
  bool guest_callbacks = false;
  VkInstance instance{};
  VkPhysicalDevice physical{};
  std::set<std::string, std::less<>> extensions;
  std::uint32_t version = VK_API_VERSION_1_0;
  PFN_vkGetInstanceProcAddr procedure_loader = nullptr;
  std::shared_ptr<const std::vector<PFN_vkVoidFunction>> procedures;
  std::map<std::uint32_t, std::uint32_t> shared_queues;  // family -> host queue count
};
std::recursive_mutex state_lock;
std::map<VkInstance, Instance> instances;
std::map<VkPhysicalDevice, VkInstance> physicals;
std::map<VkDevice, Device> devices;
std::map<VkQueue, QueueBinding> queues;
struct QueueFacade {
  VkQueue native;
  VkDevice device;
  std::shared_ptr<SerializedQueues> backend;
  std::shared_ptr<DeferredQueueSubmissions> deferred;
  std::uint32_t backend_index{}, deferred_index{};
  std::shared_ptr<std::recursive_mutex> caller_mutex = std::make_shared<std::recursive_mutex>();
};
using QueueFamily = std::pair<std::uint32_t, VkDeviceQueueCreateFlags>;
std::map<VkQueue, std::shared_ptr<QueueFacade>> queue_facades;
std::map<VkDevice, std::map<QueueFamily, std::vector<VkQueue>>> facade_families;
std::atomic<bool> queue_facades_active{false};
std::map<VkDevice, std::shared_ptr<DeferredQueueSubmissions>> deferred_devices;
std::shared_ptr<QueueFacade> queue_facade(VkQueue queue) {
  if (!queue_facades_active.load(std::memory_order_acquire)) return {};
  std::lock_guard lock(state_lock);
  if (auto found = queue_facades.find(queue); found != queue_facades.end()) return found->second;
  return {};
}
std::shared_ptr<DeferredQueueSubmissions> deferred_device(VkDevice device) {
  if (!queue_facades_active.load(std::memory_order_acquire)) return {};
  std::lock_guard lock(state_lock);
  if (auto found = deferred_devices.find(device); found != deferred_devices.end()) return found->second;
  return {};
}
VkResult VKAPI_CALL queue_submit(VkQueue, std::uint32_t, const VkSubmitInfo*, VkFence);
VkResult VKAPI_CALL queue_submit2(VkQueue, std::uint32_t, const VkSubmitInfo2*, VkFence);

void install_queue_facades(VkDevice, const VkDeviceCreateInfo&,
    const std::map<std::uint32_t, std::uint32_t>&);
void forget_queue_facades(VkDevice);
bool facade_queue(VkDevice, std::uint32_t family, std::uint32_t index,
    VkDeviceQueueCreateFlags flags, VkQueue*);
std::shared_ptr<std::recursive_mutex> guest_queue_mutex(VkQueue);

std::map<GuestAddr, VkDevice> children;
std::map<VkCommandBuffer, VkCommandPool> command_pools;
// The inverse of command_pools: a pool reset finds its buffers without a scan.
std::map<VkCommandPool, std::set<VkCommandBuffer>> pool_commands;
std::map<VkCommandPool, VkQueueFlags> pool_flags;
std::map<VkCommandPool, std::pair<VkDevice, bool>> pool_callbacks;
std::map<std::string, GuestAddr, std::less<>> thunks;
PFN_vkGetInstanceProcAddr loader() { return gfx::gpu_device().get_instance_proc_addr; }
const Command* find_command(std::string_view name);
std::size_t command_slot(const Command* command);
bool device_command(const Command& command) {
  return command.scope == Scope::device || command.scope == Scope::queue ||
         command.scope == Scope::command_buffer;
}
struct DispatchCache {
  Scope scope{};
  GuestAddr handle{};
  std::uint64_t generation{};
  PFN_vkGetInstanceProcAddr loader{};
  std::shared_ptr<const std::vector<PFN_vkVoidFunction>> procedures;
  bool guest_callbacks = true;
};
// A handful of active command buffers/device queries per thread. Cold entries
// are filled at the outer boundary only; table ownership is retained across
// invalidation, avoiding reclamation races and per-call shared_ptr operations.
DispatchCache& dispatch_cache(Scope scope, GuestAddr handle) {
  thread_local std::array<DispatchCache, 4> cache;
  return cache[((handle >> 4) ^ (handle >> 16) ^ static_cast<unsigned>(scope)) % cache.size()];
}
bool cache_current(const DispatchCache& cache, Scope scope, GuestAddr handle) {
  return cache.scope == scope && cache.handle == handle && cache.procedures &&
      cache.generation == layer_procedure_generation.load(std::memory_order_acquire) &&
      cache.loader == loader();
}
void retain_device_allocator(VkDevice device, const VkAllocationCallbacks* allocator) {
  if (!allocator) return;
  // Child objects may retain their allocator for later queries/recording.
  // RunLoop-only creation quarantines this device before calling the driver;
  // invalidate warm TLS snapshots on every thread, not only this creator.
  std::lock_guard lock(state_lock);
  if (auto it = devices.find(device); it != devices.end() && !it->second.guest_callbacks) {
    it->second.guest_callbacks = true;
    invalidate_layer_procedures();
  }
}
PFN_vkVoidFunction indexed_host_proc(Scope scope, GuestAddr handle, std::size_t slot, const char* name,
                                    bool observe = false);
PFN_vkVoidFunction host_proc(Scope scope, GuestAddr handle, const char* name,
                            const Command* command = nullptr) {
  const auto gipa = loader();
  if (!gipa) return nullptr;
  if (scope == Scope::global) return gipa(nullptr, name);
  VkInstance instance = gfx::gpu_device().instance;
  VkDevice device = nullptr;
  {
    std::lock_guard lock(state_lock);
    if (scope == Scope::instance) instance = g2h<VkInstance_T>(handle);
    if (scope == Scope::physical) {
      auto it = physicals.find(g2h<VkPhysicalDevice_T>(handle));
      if (it != physicals.end()) instance = it->second;
    }
    if (scope == Scope::device) device = g2h<VkDevice_T>(handle);
    if (scope == Scope::queue || scope == Scope::command_buffer) {
      auto it = children.find(handle);
      if (it != children.end()) device = it->second;
    }
    if (device) {
      auto it = devices.find(device);
      if (it != devices.end()) {
        const auto& record = it->second;
        instance = record.instance;
        // Immutable dispatch snapshots belong to this device's lifetime. A
        // changed loader (including an interception layer) takes the original
        // resolver path; never reuse pointers from a different layer chain.
        if (record.procedure_loader == gipa && record.procedures) {
          if (!command) command = find_command(name);
          if (command && device_command(*command)) return (*record.procedures)[command_slot(command)];
        }
      }
    }
  }
  if (device) {
    auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(instance, "vkGetDeviceProcAddr"));
    return gdpa ? gdpa(device, name) : nullptr;
  }
  return gipa(instance, name);
}
// Scoped to one guest call, including translation hooks. Native failures are
// observed before translation returns; success-only calls incur no logging.
struct ResultObservation {
  ResultObservation* previous;
  std::optional<VkResult> native_result;
  const char* native_function = nullptr;
  static thread_local ResultObservation* current;
  ResultObservation() : previous(current) { current = this; }
  ~ResultObservation() { current = previous; }
  void report(const char* name, VkResult result) const {
    if (result == VK_SUCCESS) return;
    if (native_result == result)
      log::warn("vulkan-result", "{} returned VkResult={} source=host driver ({})", name,
                static_cast<int>(result), native_function);
    else
      log::warn("vulkan-result", "{} returned VkResult={} source=Venture translation", name,
                static_cast<int>(result));
    // The common sink rate-limits identical warning text and reports repeats.
  }
};
thread_local ResultObservation* ResultObservation::current = nullptr;
PFN_vkVoidFunction observed_host_proc(Scope scope, GuestAddr handle, const char* name);
void report_device_fault(Scope scope, GuestAddr handle) {
  VkDevice device{};
  {
    std::lock_guard lock(state_lock);
    if (scope == Scope::device) device = g2h<VkDevice_T>(handle);
    else if (scope == Scope::queue || scope == Scope::command_buffer) {
      if (auto it = children.find(handle); it != children.end()) device = it->second;
    }
    auto it = devices.find(device);
    if (it == devices.end() || it->second.fault_reported) return;
    it->second.fault_reported = true;
  }
  report_breadcrumbs(device, scope == Scope::queue ? native_queue(g2h<VkQueue_T>(handle)) : VK_NULL_HANDLE);
  { std::lock_guard lock(state_lock); if (!devices.at(device).fault_enabled) return; }
  auto query = reinterpret_cast<PFN_vkGetDeviceFaultInfoEXT>(
      host_proc(Scope::device, h2g(device), "vkGetDeviceFaultInfoEXT"));
  if (!query) return;
  // VUID-device-07336: call only after a native VK_ERROR_DEVICE_LOST.
  // First obtain counts, then supply arrays of exactly those sizes. Binary
  // dumps are not enabled; fault kinds and GPU addresses suffice here.
  VkDeviceFaultCountsEXT counts{VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
  auto r = query(device, &counts, nullptr);
  if (r != VK_SUCCESS) {
    log::warn("vulkan-fault", "vkGetDeviceFaultInfoEXT counts returned {}", int(r));
    return;
  }
  std::vector<VkDeviceFaultAddressInfoEXT> addresses(counts.addressInfoCount);
  std::vector<VkDeviceFaultVendorInfoEXT> vendors(counts.vendorInfoCount);
  VkDeviceFaultInfoEXT info{VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT};
  info.pAddressInfos = addresses.data();
  info.pVendorInfos = vendors.data();
  counts.vendorBinarySize = 0;
  r = query(device, &counts, &info);
  log::warn("vulkan-fault", "vkGetDeviceFaultInfoEXT returned {}: {} (addresses={}, vendor records={})",
            int(r), info.description, counts.addressInfoCount, counts.vendorInfoCount);
  if (r != VK_SUCCESS && r != VK_INCOMPLETE) return;
  for (std::size_t i = 0; i < std::min<std::size_t>(counts.addressInfoCount, addresses.size()); ++i) {
    const auto& a = addresses[i];
    log::warn("vulkan-fault", "kind={} address={:#x} precision={:#x}", int(a.addressType),
              a.reportedAddress, a.addressPrecision);
  }
  for (std::size_t i = 0; i < std::min<std::size_t>(counts.vendorInfoCount, vendors.size()); ++i) {
    const auto& v = vendors[i];
    log::warn("vulkan-fault", "{} code={:#x} data={:#x}", v.description, v.vendorFaultCode, v.vendorFaultData);
  }
}
template <class F, std::size_t Slot, class... A>
VkResult driver_result(const char* name, Scope scope, GuestAddr handle, A... args) {
  auto function = reinterpret_cast<F>(indexed_host_proc(scope, handle, Slot, name));
  if (!function) throw std::runtime_error("native Vulkan function disappeared");
  const auto trace = sync_trace::begin<F>(name, args...);
  std::unique_lock<std::recursive_mutex> physical_lock;
  auto unwrap = [&](auto value) {
    if constexpr (std::is_same_v<decltype(value), VkQueue>) {
      const auto native = native_queue(value);
      if (native != value) physical_lock = std::unique_lock(*gfx::queue_mutex(native));
      return native;
    } else return value;
  };
  auto result = api_result_trace::observe(name, api_result_trace::Phase::vulkan_native, [&](auto& observer) {
    const auto value = function(unwrap(args)...);
    observer.finish(value);
    return value;
  });
  if constexpr (sync_trace::supported<F>) sync_trace::end<F>(trace, result, args...);
  if (ResultObservation::current) {
    ResultObservation::current->native_result = result;
    ResultObservation::current->native_function = name;
  }
  if (result == VK_ERROR_DEVICE_LOST) {
    try {
      report_device_fault(scope, handle);
    } catch (const std::exception& e) {
      // Diagnostics must preserve the native failure even if allocating their
      // output arrays fails on an already distressed machine.
      log::warn("vulkan-fault", "Device fault query unavailable: {}", e.what());
    }
    end_app_after_device_loss();
  }
  return result;
}
template <class F, std::size_t Slot, class... A>
void driver_queue_void(const char* name, GuestAddr handle, A... args) {
  auto function = reinterpret_cast<F>(indexed_host_proc(Scope::queue, handle, Slot, name));
  if (!function) throw std::runtime_error("native Vulkan queue function disappeared");
  std::unique_lock<std::recursive_mutex> physical_lock;
  auto unwrap = [&](auto value) {
    if constexpr (std::is_same_v<decltype(value), VkQueue>) {
      const auto native = native_queue(value);
      if (native != value) physical_lock = std::unique_lock(*gfx::queue_mutex(native));
      return native;
    } else return value;
  };
  function(unwrap(args)...);
}
template <class F, class H>
F host(H handle, const char* name) {
  constexpr Scope scope = std::is_same_v<H, VkInstance>         ? Scope::instance
                          : std::is_same_v<H, VkPhysicalDevice> ? Scope::physical
                          : std::is_same_v<H, VkQueue>          ? Scope::queue
                          : std::is_same_v<H, VkCommandBuffer>  ? Scope::command_buffer
                                                                : Scope::device;
  return reinterpret_cast<F>(observed_host_proc(scope, h2g(handle), name));
}
const Command* find_command(std::string_view name);
PFN_vkVoidFunction override_proc(std::string_view name);
PFN_vkVoidFunction dispatch_proc(Layer, std::string_view name) { return override_proc(name); }

template <class>
struct Signature;
template <class R, class... A>
struct Signature<R(VKAPI_PTR*)(A...)> {
  using Result = R;
  using Args = std::tuple<A...>;
};
template <std::size_t I, class F>
using Argument = std::tuple_element_t<I, typename Signature<F>::Args>;

class Invocation {
 public:
  void host_context(VkPhysicalDevice physical) { chain_.set_physical(physical); }
  void host_context(VkDevice device) { chain_.set_device(device); }
  void depth_format(VkFormat& format) { chain_.translate_format(format); }
  void host_context(VkCommandBuffer command) { chain_.set_command(command); }
  void density_stages(VkPipelineStageFlags& flags) { chain_.translate_stages(flags); }
  void density_stages(VkPipelineStageFlags2& flags) { chain_.translate_stages(flags); }
  void density_layout(VkImageLayout& layout) { chain_.translate_layout(layout); }
  explicit Invocation(hle::CallContext& ctx, bool fast = false) : ctx_(ctx), cursor_(ctx), fast_(fast) {
    if (forwarding_census_enabled) counters_ = forwarding_counters(ctx.thread(), !fast);
  }
  ~Invocation() {
    if (counters_ && completed_) {
      counters_->sequence.fetch_add(1, std::memory_order_acq_rel);
      counters_->calls[slot_].fetch_add(1, std::memory_order_relaxed);
      if (fast_) counters_->in_jit[slot_].fetch_add(1, std::memory_order_relaxed);
      counters_->sequence.fetch_add(1, std::memory_order_release);
    }
  }
  template <ProcedureName Name, std::size_t Slot>
  bool admit(Scope scope, GuestAddr handle) {
    if (forwarding_census_enabled && !counters_) return false;
    if (ctx_.thread().interrupted.load(std::memory_order_relaxed)) return false;
    PFN_vkVoidFunction layer{}, audited{};
    if (!try_cached_layer_proc<Layer::dispatch, Name, dispatch_proc>(layer)) return false;
    // A layered command enters the JIT only through a fully audited chain.
    if (layer && (!try_cached_layer_proc<Layer::dispatch, Name, jit_dispatch_proc>(audited) || audited != layer))
      return false;
    const auto& cache = dispatch_cache(scope, handle);
    if (!cache_current(cache, scope, handle) || cache.guest_callbacks) return false;
    fast_function_ = layer ? layer : (*cache.procedures)[Slot];
    return fast_function_ != nullptr;
  }
  template <class T>
  T read() {
    // vk.xml uses size_t/VkDeviceSize. AArch64 and host x64 are both 64-bit;
    // avoid the libc marshal's blanket 'long' rejection on LP64 POSIX hosts.
    if constexpr (std::is_same_v<T, float>)
      return cursor_.next_float();
    else if constexpr (std::is_same_v<T, double>)
      return cursor_.next_double();
    else if constexpr (std::is_pointer_v<T>)
      return reinterpret_cast<T>(cursor_.next_gpr());
    else if constexpr (std::is_enum_v<T>)
      return static_cast<T>(static_cast<std::underlying_type_t<T>>(cursor_.next_gpr()));
    else {
      static_assert(sizeof(T) <= 8);
      return static_cast<T>(cursor_.next_gpr());
    }
  }
  template <class T, class N>
  void prepare(T& arg, N count) {
    try {
      prepare_impl(arg, count);
    } catch (const std::bad_alloc&) {
      error_ = VK_ERROR_OUT_OF_HOST_MEMORY;
    } catch (const std::exception& e) {
      if (!fast_) log::error("vulkan", "argument translation: {}", e.what());
      error_ = VK_ERROR_INITIALIZATION_FAILED;
    }
  }
  template <class T, class N>
  void prepare_impl(T& arg, N count) {
    if constexpr (std::is_same_v<T, const VkAllocationCallbacks*>) {
      if (fast_) { error_ = VK_ERROR_INITIALIZATION_FAILED; return; }
      try {
        arg = translate_allocator(arg);
      } catch (...) {
        error_ = VK_ERROR_INITIALIZATION_FAILED;
      }
    } else if constexpr (std::is_pointer_v<T> && requires(T p) {
                           p->sType;
                           p->pNext;
                         }) {
      if (!arg) return;
      if constexpr (std::is_const_v<std::remove_pointer_t<T>>) {
        if constexpr (std::is_integral_v<N>) {
          if (count > 1048576) {
            error_ = VK_ERROR_OUT_OF_HOST_MEMORY;
            return;
          }
          using S = std::remove_const_t<std::remove_pointer_t<T>>;
          if (!reference_forwarding && borrowable_array(arg, count)) return;
          if (fast_) { error_ = VK_ERROR_INITIALIZATION_FAILED; return; }
          auto data = std::shared_ptr<S[]>(new S[count]);
          for (std::size_t i = 0; i < static_cast<std::size_t>(count); ++i) {
            auto* copy = static_cast<const S*>(chain_.translate(arg + i));
            if (!copy) {
              error_ = chain_.result();
              return;
            }
            data[i] = *copy;
          }
          arg = data.get();
          storage_.push_back(std::move(data));
        }
      } else {
        // Outputs are ABI-compatible and remain in guest memory. Vulkan's
        // "Valid Usage for Structure Pointer Chains" requires implementations
        // to skip unknown sTypes, including when a known node follows one.
        unsigned length = 0;
        for (auto* p = reinterpret_cast<VkBaseOutStructure*>(arg); p; p = p->pNext) {
          if (++length > 256) {
            error_ = VK_ERROR_UNKNOWN;
            break;
          }
          if (structure_size(p->sType) && !structure_translatable(p->sType)) {
            error_ = VK_ERROR_EXTENSION_NOT_PRESENT;
            break;
          }
        }
      }
    }
  }
  template <class F, ProcedureName Name, std::size_t Slot, class... A>
  bool invoke(Scope scope, GuestAddr handle, A... args) {
    using R = typename Signature<F>::Result;
    constexpr auto name = Name.value;
    slot_ = Slot;
    if (fast_ && (error_ != VK_SUCCESS || !fast_function_)) return false;
    const auto phase = fast_ ? api_result_trace::Phase::vulkan_fast :
        pending_.active && pending_.slot == Slot && pending_.handle == handle ?
        api_result_trace::Phase::vulkan_delivery : api_result_trace::Phase::vulkan_slow;
    return api_result_trace::observe<std::is_same_v<R, VkResult>>(name, phase, [&](auto& trace) {
      return invoke_observed<F, Name, Slot>(scope, handle, trace, args...);
    });
  }
  template <class F, ProcedureName Name, std::size_t Slot, class Observer, class... A>
  bool invoke_observed(Scope scope, GuestAddr handle, Observer& trace, A... args) {
    using R = typename Signature<F>::Result;
    constexpr auto name = Name.value;
    if (fast_) {
      try {
        if constexpr (std::is_void_v<R>) reinterpret_cast<F>(fast_function_)(args...);
        else {
          auto result = reinterpret_cast<F>(fast_function_)(args...);
          if constexpr (std::is_same_v<R, VkResult>) {
            trace.finish(result);
            if (result != VK_SUCCESS) {
              pending_ = {true, Slot, handle, result, true, {}};
              return false;
            }
          }
          if constexpr (std::is_pointer_v<R>) ctx_.return_x(reinterpret_cast<GuestAddr>(result));
          else ctx_.return_x(static_cast<std::uint64_t>(result));
        }
        completed_ = true;
        return true;
      } catch (...) {
        pending_ = {true, Slot, handle, VK_ERROR_INITIALIZATION_FAILED, false, std::current_exception()};
        return false;
      }
    }
    completed_ = true;
    // A native failure has already completed the driver call in the SVC
    // callback. Report/deliver it at the outer boundary without replaying any
    // side effects or losing driver-written outputs.
    if (pending_.active && pending_.slot == Slot && pending_.handle == handle) {
      auto pending = std::exchange(pending_, {});
      if (pending.exception) {
        try { std::rethrow_exception(pending.exception); }
        catch (const std::bad_alloc&) { pending.result = VK_ERROR_OUT_OF_HOST_MEMORY; }
        catch (const std::exception& e) { log::error("vulkan", "{}: {}", name, e.what()); }
      }
      if (pending.native) {
        observation_.native_result = pending.result;
        observation_.native_function = name;
        if (pending.result == VK_ERROR_DEVICE_LOST) {
          try { report_device_fault(scope, handle); }
          catch (const std::exception& e) { log::warn("vulkan-fault", "Device fault query unavailable: {}", e.what()); }
          end_app_after_device_loss();
        }
      }
      fail<R>(name, pending.result, trace);
      return true;
    }
    try {
      auto fp = cached_layer_proc<Layer::dispatch, Name, dispatch_proc>();
      // Warm the in-JIT admission slot; admission itself never resolves.
      if (fp) cached_layer_proc<Layer::dispatch, Name, jit_dispatch_proc>();
      if (!fp) fp = indexed_host_proc(scope, handle, Slot, name, true);
      if (!fp || error_ != VK_SUCCESS) {
        fail<R>(name, error_ != VK_SUCCESS ? error_ : VK_ERROR_EXTENSION_NOT_PRESENT, trace);
        return true;
      }
      std::shared_ptr<std::recursive_mutex> queue_mutex;
      std::unique_lock<std::recursive_mutex> queue_lock;
      std::optional<gfx::DeviceQueueLock> device_lock;
      if (scope == Scope::queue) {
        queue_mutex = guest_queue_mutex(g2h<VkQueue_T>(handle));
        queue_lock = std::unique_lock(*queue_mutex);
      } else if (scope == Scope::device && std::string_view(name) == "vkDeviceWaitIdle" &&
                 !deferred_device(g2h<VkDevice_T>(handle))) {
        device_lock.emplace(g2h<VkDevice_T>(handle));
      }
      if constexpr (std::is_void_v<R>)
        reinterpret_cast<F>(fp)(args...);
      else {
        auto result = reinterpret_cast<F>(fp)(args...);
        if constexpr (std::is_same_v<R, VkResult>) observation_.report(name, result);
        if constexpr (std::is_pointer_v<R>)
          ctx_.return_x(reinterpret_cast<GuestAddr>(result));
        else
          ctx_.return_x(static_cast<std::uint64_t>(result));
        if constexpr (std::is_same_v<R, VkResult>) trace.finish(result);
      }
    } catch (const std::bad_alloc&) {
      fail<R>(name, VK_ERROR_OUT_OF_HOST_MEMORY, trace);
    } catch (const std::exception& e) {
      log::error("vulkan", "{}: {}", name, e.what());
      fail<R>(name, VK_ERROR_INITIALIZATION_FAILED, trace);
    }
    return true;
  }

 private:
  template <class R, class Observer>
  void fail(const char* name, VkResult error, Observer& trace) {
    if constexpr (std::is_same_v<R, VkResult>) observation_.report(name, error);
    else log::error("vulkan", "{} rejected ({})", name, static_cast<int>(error));
    if constexpr (std::is_same_v<R, VkResult>) {
      ctx_.return_x(static_cast<std::uint64_t>(error));
      trace.finish(error);
    }
    else if constexpr (!std::is_void_v<R>)
      ctx_.return_x(0);
    else
      signals::deliver_synchronous(ctx_.thread(), {signals::kSigSys});
  }
  ResultObservation observation_;
  hle::CallContext& ctx_;
  hle::ArgCursor cursor_;
  Chain chain_;
  VkResult error_ = VK_SUCCESS;
  std::vector<std::shared_ptr<void>> storage_;
  bool fast_;
  PFN_vkVoidFunction fast_function_{};
  struct Pending {
    bool active = false;
    std::size_t slot{};
    GuestAddr handle{};
    VkResult result{};
    bool native = false;
    std::exception_ptr exception;
  };
  static thread_local Pending pending_;
  ForwardingCounters* counters_{};
  std::size_t slot_{};
  bool completed_ = false;
};
thread_local Invocation::Pending Invocation::pending_;

#include "vk_commands.inc"

const Command* find_command(std::string_view name) {
  auto it = std::lower_bound(std::begin(commands), std::end(commands), name,
                             [](const Command& c, std::string_view n) { return c.name < n; });
  return it != std::end(commands) && it->name == name ? it : nullptr;
}
std::size_t command_slot(const Command* command) {
  return static_cast<std::size_t>(command - std::begin(commands));
}
PFN_vkVoidFunction indexed_host_proc(Scope scope, GuestAddr handle, std::size_t slot, const char* name, bool observe) {
  if (reference_forwarding) {
    const auto* command = find_command(name);
    const auto native = host_proc(scope, handle, name, command);
    return observe && native && command && command->driver_observer ? command->driver_observer : native;
  }
  const auto& command = commands[slot];
  if (device_command(command)) {
    auto& cache = dispatch_cache(scope, handle);
    if (!cache_current(cache, scope, handle)) {
      std::lock_guard lock(state_lock);
      VkDevice device{};
      if (scope == Scope::device) device = g2h<VkDevice_T>(handle);
      else if (auto it = children.find(handle); it != children.end()) device = it->second;
      if (auto it = devices.find(device); it != devices.end()) {
        const auto& record = it->second;
        bool callbacks = record.guest_callbacks;
        if (scope == Scope::command_buffer) {
          auto pool = command_pools.find(g2h<VkCommandBuffer_T>(handle));
          if (pool == command_pools.end()) callbacks = true;
          else {
            auto allocator = pool_callbacks.find(pool->second);
            callbacks |= allocator == pool_callbacks.end() || allocator->second.second;
          }
        }
        cache = {scope, handle, layer_procedure_generation.load(std::memory_order_acquire),
                 record.procedure_loader, record.procedures, callbacks};
      }
    }
    if (cache_current(cache, scope, handle)) {
      const auto native = (*cache.procedures)[slot];
      return observe && native && command.driver_observer ? command.driver_observer : native;
    }
  }
  const auto native = host_proc(scope, handle, name, &command);
  return observe && native && command.driver_observer ? command.driver_observer : native;
}
PFN_vkVoidFunction observed_host_proc(Scope scope, GuestAddr handle, const char* name) {
  auto* command = find_command(name);
  auto native = host_proc(scope, handle, name, command);
  if (!native) return nullptr;
  return command && command->driver_observer ? command->driver_observer : native;
}

template <class T>
VkResult enumerate(const std::vector<T>& values, std::uint32_t* count, T* out) {
  if (!count) return VK_ERROR_UNKNOWN;
  if (!out) {
    *count = static_cast<std::uint32_t>(values.size());
    return VK_SUCCESS;
  }
  auto n = std::min(*count, static_cast<std::uint32_t>(values.size()));
  std::copy_n(values.begin(), n, out);
  *count = n;
  return n < values.size() ? VK_INCOMPLETE : VK_SUCCESS;
}
template <class F>
std::vector<VkExtensionProperties> extensions(F query) {
  std::vector<VkExtensionProperties> values;
  for (int attempt = 0; attempt < 8; ++attempt) {
    std::uint32_t count = 0;
    if (query(&count, nullptr) != VK_SUCCESS) return {};
    values.resize(count);
    auto result = query(&count, values.data());
    values.resize(count);
    if (result == VK_SUCCESS) return values;
    if (result != VK_INCOMPLETE) return {};
  }
  return {};
}
std::vector<VkExtensionProperties> native_instance_extensions() {
  auto f = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
      observed_host_proc(Scope::global, 0, "vkEnumerateInstanceExtensionProperties"));
  if (!f) return {};
  return extensions([&](auto n, auto p) { return f(nullptr, n, p); });
}
std::vector<VkExtensionProperties> instance_extensions() {
  auto values = native_instance_extensions();
  std::erase_if(values, [](auto& p) { return !extension_translatable(p.extensionName); });
  wsi_instance_extensions(values);
  return values;
}
VkResult VKAPI_CALL enumerate_instance_extensions(const char* layer, std::uint32_t* count,
                                                  VkExtensionProperties* out) {
  if (layer) return VK_ERROR_LAYER_NOT_PRESENT;
  return enumerate(instance_extensions(), count, out);
}
VkResult VKAPI_CALL enumerate_device_extensions(VkPhysicalDevice physical, const char* layer,
                                                std::uint32_t* count, VkExtensionProperties* out) {
  if (layer) return VK_ERROR_LAYER_NOT_PRESENT;
  auto f = host<PFN_vkEnumerateDeviceExtensionProperties>(physical, "vkEnumerateDeviceExtensionProperties");
  auto values = extensions([&](auto n, auto p) { return f(physical, nullptr, n, p); });
  external_extensions(values);
  sync_fd_extensions(values);
  hardware_buffer_extensions(values);
  texture_extensions(physical, values);
  std::erase_if(values, [](auto& p) { return !extension_translatable(p.extensionName); });
  wsi_device_extensions(values);
  if (emulated_density_maps(physical)) {
    VkExtensionProperties density{};
    std::strcpy(density.extensionName, VK_EXT_FRAGMENT_DENSITY_MAP_EXTENSION_NAME);
    density.specVersion = VK_EXT_FRAGMENT_DENSITY_MAP_SPEC_VERSION;
    values.push_back(density);
  }
  return enumerate(values, count, out);
}
VkResult VKAPI_CALL enumerate_layers(std::uint32_t* count, VkLayerProperties* out) {
  return enumerate(std::vector<VkLayerProperties>{}, count, out);
}
VkResult VKAPI_CALL enumerate_device_layers(VkPhysicalDevice, std::uint32_t* count, VkLayerProperties* out) {
  return enumerate_layers(count, out);
}
VkResult VKAPI_CALL enumerate_version(std::uint32_t* version) {
  if (!version) return VK_ERROR_UNKNOWN;
  auto f = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
      observed_host_proc(Scope::global, 0, "vkEnumerateInstanceVersion"));
  VkResult r = VK_SUCCESS;
  *version = VK_API_VERSION_1_0;
  if (f) r = f(version);
  *version = std::min(*version, VK_API_VERSION_1_4);
  return r;
}
bool allowed_names(std::uint32_t count, const char* const* names) {
  for (std::uint32_t i = 0; i < count; ++i) {
    if (!names || !names[i] || !extension_translatable(names[i])) {
      log::error("vulkan", "unsupported requested extension: {}", names && names[i] ? names[i] : "<null>");
      return false;
    }
  }
  return true;
}
bool native_xr_extensions(bool device, bool enable2, std::vector<std::string>& names) {
  // enable2 already supplies its final native list through XrResolverScope.
  // Flat mode and runtimes with only enable2 need no legacy requirements.
  if (enable2) return true;
  auto& xr = venture::xr::host_xr();
  if (!xr.available || !xr.enabled_extensions.contains("XR_KHR_vulkan_enable")) return true;
  const auto result = venture::xr::vulkan_extension_requirements(xr.instance, xr.system, device, names);
  if (XR_FAILED(result))
    log::error("vulkan", "Host XR Vulkan {} extension query failed: {}", device ? "device" : "instance", int(result));
  return XR_SUCCEEDED(result);
}
VkResult create_instance_impl(const VkInstanceCreateInfo* info, const VkAllocationCallbacks* alloc,
                              VkInstance* out, bool xr) {
  if (!info || !out) return VK_ERROR_INITIALIZATION_FAILED;
  *out = VK_NULL_HANDLE;
  if (info->enabledLayerCount) return VK_ERROR_LAYER_NOT_PRESENT;
  if (!xr && !allowed_names(info->enabledExtensionCount, info->ppEnabledExtensionNames))
    return VK_ERROR_EXTENSION_NOT_PRESENT;
  std::vector<const char*> names;
  for (std::uint32_t i = 0; i < info->enabledExtensionCount; ++i) {
    auto* n = info->ppEnabledExtensionNames[i];
    if (!emulated_wsi_extension(n)) names.push_back(n);  // wsi.cpp, without the host
  }
  std::vector<std::string> required;
  if (!native_xr_extensions(false, xr, required)) return VK_ERROR_INITIALIZATION_FAILED;
  for (const auto& name : required)
    if (std::ranges::find(names, std::string_view(name)) == names.end()) names.push_back(name.c_str());
  auto copy = *info;
  // Host diagnostics also work for Vulkan 1.0 guests. Checkpoints require
  // properties2 even when verbose tracing is off. Keep the
  // internal instance extension out of the guest's advertised capabilities.
  bool host_properties2 = false;
  if ((venture::environment::getenv("VENTURE_TRACE_VULKAN") || breadcrumbs_enabled()) &&
      std::ranges::any_of(native_instance_extensions(), [](const auto& e) {
        return std::string_view(e.extensionName) == VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
      })) {
    if (std::ranges::find(names, std::string_view(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME)) == names.end())
      names.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
    host_properties2 = true;
  }
  address_log_instance_extensions(names, native_instance_extensions());
  copy.enabledExtensionCount = static_cast<std::uint32_t>(names.size());
  copy.ppEnabledExtensionNames = names.data();
  auto f = reinterpret_cast<PFN_vkCreateInstance>(observed_host_proc(Scope::global, 0, "vkCreateInstance"));
  if (!f) return VK_ERROR_INITIALIZATION_FAILED;
  auto r = f(&copy, alloc, out);
  if (r == VK_SUCCESS) {
    address_log_attach(*out);
    adopt_instance(*out, *info, alloc != nullptr);
    std::lock_guard lock(state_lock);
    instances.at(*out).host_properties2 = host_properties2;
  }
  return r;
}
VkResult VKAPI_CALL create_instance(const VkInstanceCreateInfo* info, const VkAllocationCallbacks* alloc,
                                    VkInstance* out) {
  return create_instance_impl(info, alloc, out, XrResolverScope::active());
}
void VKAPI_CALL destroy_instance(VkInstance instance, const VkAllocationCallbacks* alloc) {
  if (!instance) return;
  address_log_detach(instance);
  host<PFN_vkDestroyInstance>(instance, "vkDestroyInstance")(instance, alloc);
  std::lock_guard lock(state_lock);
  instances.erase(instance);
  invalidate_layer_procedures();
  std::erase_if(physicals, [&](auto& p) { return p.second == instance; });
}
bool same_gpu(VkInstance instance, VkPhysicalDevice physical) {
  const auto& shared = gfx::gpu_device();
  if (!shared.physical_device) return true;
  if (instance == shared.instance) return physical == shared.physical_device;
  bool core_ids = false, extension_ids = false;
  {
    std::lock_guard lock(state_lock);
    if (auto it = instances.find(instance); it != instances.end()) {
      core_ids = it->second.version >= VK_API_VERSION_1_1;
      const auto& extensions = it->second.extensions;
      extension_ids = extensions.contains("VK_KHR_get_physical_device_properties2") &&
          (extensions.contains("VK_KHR_external_memory_capabilities") ||
           extensions.contains("VK_KHR_external_fence_capabilities") ||
           extensions.contains("VK_KHR_external_semaphore_capabilities"));
    }
  }
  // A non-null loader procedure does not enable Vulkan 1.1 or its ID pNext
  // structure on a 1.0 instance. Khronos vkGetPhysicalDeviceProperties2 and
  // VkPhysicalDeviceIDProperties require 1.1 or the corresponding KHR extensions.
  // UE4's extension-free 1.0 driver probe must use the 1.0 properties below.
  auto query = core_ids || extension_ids ? reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
      loader()(instance, core_ids ? "vkGetPhysicalDeviceProperties2" : "vkGetPhysicalDeviceProperties2KHR")) : nullptr;
  auto shared_query = shared.api_version >= VK_API_VERSION_1_1 ? reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
      loader()(shared.instance, "vkGetPhysicalDeviceProperties2")) : nullptr;
  if (query && shared_query) {
    VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceIDProperties shared_id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 p{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
    VkPhysicalDeviceProperties2 sp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &shared_id};
    query(physical, &p);
    shared_query(shared.physical_device, &sp);
    return std::memcmp(id.deviceUUID, shared_id.deviceUUID, VK_UUID_SIZE) == 0;
  }
  // Vulkan 1.0 cannot disambiguate identical adapters; reject ambiguity rather
  // than accidentally binding an XR session to a different physical GPU.
  VkPhysicalDeviceProperties p{}, sp{};
  reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(loader()(instance, "vkGetPhysicalDeviceProperties"))(
      physical, &p);
  reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
      loader()(shared.instance, "vkGetPhysicalDeviceProperties"))(shared.physical_device, &sp);
  return p.vendorID == sp.vendorID && p.deviceID == sp.deviceID &&
         std::memcmp(p.pipelineCacheUUID, sp.pipelineCacheUUID, VK_UUID_SIZE) == 0;
}
VkResult VKAPI_CALL enumerate_physicals(VkInstance instance, std::uint32_t* count, VkPhysicalDevice* out) {
  auto f = host<PFN_vkEnumeratePhysicalDevices>(instance, "vkEnumeratePhysicalDevices");
  std::uint32_t n = 0;
  auto r = f(instance, &n, nullptr);
  if (r != VK_SUCCESS) return r;
  std::vector<VkPhysicalDevice> values(n);
  r = f(instance, &n, values.data());
  if (r != VK_SUCCESS) return r;
  values.resize(n);
  std::erase_if(values, [&](auto p) { return !same_gpu(instance, p); });
  if (values.size() > 1 && gfx::gpu_device().physical_device) {
    log::error("vulkan", "selected GPU is ambiguous without a unique device UUID");
    if (count) *count = 0;
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  {
    std::lock_guard lock(state_lock);
    for (auto p : values) physicals[p] = instance;
  }
  return enumerate(values, count, out);
}
VkResult VKAPI_CALL enumerate_groups(VkInstance instance, std::uint32_t* count,
                                     VkPhysicalDeviceGroupProperties* out) {
  std::uint32_t n = 0;
  auto r = enumerate_physicals(instance, &n, nullptr);
  if (r != VK_SUCCESS) return r;
  std::vector<VkPhysicalDevice> selected(n);
  r = enumerate_physicals(instance, &n, selected.data());
  if (r != VK_SUCCESS) return r;
  std::vector<VkPhysicalDeviceGroupProperties> groups;
  for (auto p : selected) {
    VkPhysicalDeviceGroupProperties group{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GROUP_PROPERTIES};
    group.physicalDeviceCount = 1;
    group.physicalDevices[0] = p;
    groups.push_back(group);
  }
  // This broker exposes only its selected adapter. A single physical device is
  // always a valid logical-device configuration, including in a linked group.
  if (!out) return enumerate(groups, count, out);
  auto written = std::min(*count, static_cast<std::uint32_t>(groups.size()));
  for (std::uint32_t i = 0; i < written; ++i) {
    out[i].physicalDeviceCount = 1;
    out[i].physicalDevices[0] = groups[i].physicalDevices[0];
    out[i].subsetAllocation = VK_FALSE;
  }
  *count = written;
  return written < groups.size() ? VK_INCOMPLETE : VK_SUCCESS;
}
VkResult create_device_impl(VkPhysicalDevice physical, const VkDeviceCreateInfo* info,
                            const VkAllocationCallbacks* alloc, VkDevice* out, bool xr) {
  if (!info || !out) return VK_ERROR_INITIALIZATION_FAILED;
  *out = VK_NULL_HANDLE;
  if (!xr && !allowed_names(info->enabledExtensionCount, info->ppEnabledExtensionNames))
    return VK_ERROR_EXTENSION_NOT_PRESENT;
  auto f = host<PFN_vkCreateDevice>(physical, "vkCreateDevice");
  auto copy = *info;
  std::vector<const char*> names;
  std::set<std::string_view> unique_names;
  auto enumerate_host = host<PFN_vkEnumerateDeviceExtensionProperties>(physical, "vkEnumerateDeviceExtensionProperties");
  const auto host_extensions = extensions([&](auto n, auto p) { return enumerate_host(physical, nullptr, n, p); });
  for (std::uint32_t i = 0; i < info->enabledExtensionCount; ++i) {
    const char* guest = info->ppEnabledExtensionNames[i];
    if (emulated_wsi_extension(guest)) continue;  // wsi.cpp, without the host
    if (std::string_view(guest) == VK_EXT_FRAGMENT_DENSITY_MAP_EXTENSION_NAME && emulated_density_maps(physical))
      continue;
    auto* name = hardware_buffer_extension(guest) ? hardware_buffer_host_extension(guest)
                                                  : external_host_extension(guest);
    if (!name) continue;
    // SYNC_FD objects are runtime sync files; the host extension is optional.
    if (sync_fd_extension(guest) &&
        !std::ranges::any_of(host_extensions, [&](auto& e) { return std::string_view(e.extensionName) == name; }))
      continue;
    if (unique_names.insert(name).second) names.push_back(name);
  }
  std::vector<std::string> required;
  if (!native_xr_extensions(true, xr, required)) return VK_ERROR_INITIALIZATION_FAILED;
  for (const auto& name : required)
    if (unique_names.insert(name).second) names.push_back(name.c_str());
  if (std::ranges::any_of(host_extensions, [](const auto& e) {
        return std::string_view(e.extensionName) == VK_EXT_PIPELINE_CREATION_FEEDBACK_EXTENSION_NAME;
      }) && unique_names.insert(VK_EXT_PIPELINE_CREATION_FEEDBACK_EXTENSION_NAME).second)
    names.push_back(VK_EXT_PIPELINE_CREATION_FEEDBACK_EXTENSION_NAME);
  copy.enabledExtensionCount = static_cast<std::uint32_t>(names.size());
  copy.ppEnabledExtensionNames = names.data();
  VkPhysicalDeviceFeatures features{};
  if (info->pEnabledFeatures) {
    features = *info->pEnabledFeatures;
    auto r = host_texture_features(physical, features);
    if (r != VK_SUCCESS) return r;
    copy.pEnabledFeatures = &features;
  }
  Chain chain;
  copy.pNext = chain.translate(info->pNext);
  if (chain.result() != VK_SUCCESS) return chain.result();
  if (emulated_density_maps(physical))
    for (auto** link = reinterpret_cast<VkBaseOutStructure**>(const_cast<void**>(&copy.pNext)); *link;) {
      if ((*link)->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_FEATURES_EXT)
        *link = (*link)->pNext;
      else
        link = &(*link)->pNext;
    }
  bool buffer_update_after_bind = false;
  VkPhysicalDeviceFeatures2* base_features{};
  VkPhysicalDeviceRobustness2FeaturesEXT* requested_robust2{};
  VkPhysicalDeviceImageRobustnessFeatures* requested_image{};
  VkPhysicalDeviceVulkan13Features* requested_v13{};
  for (auto* next = const_cast<VkBaseOutStructure*>(static_cast<const VkBaseOutStructure*>(copy.pNext)); next;
       next = next->pNext) {
    if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
      base_features = reinterpret_cast<VkPhysicalDeviceFeatures2*>(next);
      auto r = host_texture_features(physical, base_features->features);
      if (r != VK_SUCCESS) return r;
    }
    if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES) {
      const auto& d = *reinterpret_cast<const VkPhysicalDeviceDescriptorIndexingFeatures*>(next);
      buffer_update_after_bind |= d.descriptorBindingUniformBufferUpdateAfterBind || d.descriptorBindingStorageBufferUpdateAfterBind ||
          d.descriptorBindingUniformTexelBufferUpdateAfterBind || d.descriptorBindingStorageTexelBufferUpdateAfterBind;
    }
    if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
      const auto& d = *reinterpret_cast<const VkPhysicalDeviceVulkan12Features*>(next);
      buffer_update_after_bind |= d.descriptorBindingUniformBufferUpdateAfterBind || d.descriptorBindingStorageBufferUpdateAfterBind ||
          d.descriptorBindingUniformTexelBufferUpdateAfterBind || d.descriptorBindingStorageTexelBufferUpdateAfterBind;
    }
    if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT)
      requested_robust2 = reinterpret_cast<VkPhysicalDeviceRobustness2FeaturesEXT*>(next);
    if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES)
      requested_image = reinterpret_cast<VkPhysicalDeviceImageRobustnessFeatures*>(next);
    if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES)
      requested_v13 = reinterpret_cast<VkPhysicalDeviceVulkan13Features*>(next);
    if ((next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES &&
         reinterpret_cast<VkPhysicalDeviceVulkan14Features*>(next)->hostImageCopy) ||
        (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_FEATURES &&
         reinterpret_cast<VkPhysicalDeviceHostImageCopyFeatures*>(next)->hostImageCopy))
      return VK_ERROR_FEATURE_NOT_PRESENT;
  }
  if (!info->pEnabledFeatures && !base_features) {
    if (auto r = host_texture_features(physical, features); r != VK_SUCCESS) return r;
    copy.pEnabledFeatures = &features;
  }
  // A private sparse/compute queue lets first-use initialization wait for
  // completed texture producers without waiting for unrelated guest work.
  VkPhysicalDeviceFeatures native_features{};
  host<PFN_vkGetPhysicalDeviceFeatures>(physical, "vkGetPhysicalDeviceFeatures")(physical, &native_features);
  std::uint32_t family_count = 0;
  auto family_query = host<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(physical,
      "vkGetPhysicalDeviceQueueFamilyProperties");
  family_query(physical, &family_count, nullptr);
  std::vector<VkQueueFamilyProperties> families(family_count);
  family_query(physical, &family_count, families.data());
  // Quest's Adreno offers three graphics queues and titles request them
  // without checking (HITMAN: 3 on family 0); RADV offers one. Create what the
  // host has; the extra guest queues share those host queues, whose
  // submissions are serialized by the shared-queue dispatcher. It retains
  // future timeline waits until their producers can enter the native FIFO.
  std::vector<VkDeviceQueueCreateInfo> fitted_queues;
  std::map<std::uint32_t, std::uint32_t> shared_queues;
  for (std::uint32_t i = 0; i < copy.queueCreateInfoCount; ++i) {
    const auto& queue = copy.pQueueCreateInfos[i];
    if (queue.queueFamilyIndex >= families.size() ||
        queue.queueCount <= families[queue.queueFamilyIndex].queueCount) continue;
    if (fitted_queues.empty()) fitted_queues.assign(copy.pQueueCreateInfos, copy.pQueueCreateInfos + copy.queueCreateInfoCount);
    fitted_queues[i].queueCount = families[queue.queueFamilyIndex].queueCount;
    shared_queues[queue.queueFamilyIndex] = fitted_queues[i].queueCount;
    log::warn("vulkan", "Guest requested {} queues in family {}; the host has {}, which the others share",
              queue.queueCount, queue.queueFamilyIndex, fitted_queues[i].queueCount);
  }
  if (!fitted_queues.empty()) {
    copy.pQueueCreateInfos = fitted_queues.data();
    // Only a device with timeline semaphores can wait before a signal is submitted.
    bool timeline = false;
    for (auto* next = static_cast<const VkBaseInStructure*>(copy.pNext); next; next = next->pNext) {
      if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES)
        timeline |= reinterpret_cast<const VkPhysicalDeviceVulkan12Features*>(next)->timelineSemaphore;
      if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES)
        timeline |= reinterpret_cast<const VkPhysicalDeviceTimelineSemaphoreFeatures*>(next)->timelineSemaphore;
    }
    log::warn("vulkan", "Guest timeline semaphores on shared queues: {}", timeline ? "enabled" : "not enabled");
  }
  // Protected command streams cannot use the ordinary shared-queue broker.
  // Preserve native protected support on devices that need no queue sharing.
  if (!shared_queues.empty()) {
    for (std::uint32_t i = 0; i < info->queueCreateInfoCount; ++i)
      if (info->pQueueCreateInfos[i].flags & VK_DEVICE_QUEUE_CREATE_PROTECTED_BIT)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    for (auto* node = static_cast<const VkBaseInStructure*>(info->pNext); node; node = node->pNext)
      if ((node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES &&
           reinterpret_cast<const VkPhysicalDeviceProtectedMemoryFeatures*>(node)->protectedMemory) ||
          (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES &&
           reinterpret_cast<const VkPhysicalDeviceVulkan11Features*>(node)->protectedMemory))
        return VK_ERROR_FEATURE_NOT_PRESENT;
  }
  std::vector<VkDeviceQueueCreateInfo> native_queues;
  std::vector<float> private_priorities;
  std::optional<std::pair<std::uint32_t, std::uint32_t>> private_queue;
  if (native_features.sparseBinding &&
      (native_features.sparseResidencyImage2D || native_features.sparseResidencyImage3D)) {
    private_queue = private_texture_queue(families,
        {copy.pQueueCreateInfos, copy.queueCreateInfoCount});
    if (private_queue) {
      native_queues.assign(copy.pQueueCreateInfos, copy.pQueueCreateInfos + copy.queueCreateInfoCount);
      auto existing = std::ranges::find_if(native_queues, [&](const auto& queue) {
        return !queue.flags && queue.queueFamilyIndex == private_queue->first;
      });
      if (existing != native_queues.end()) {
        private_priorities.assign(existing->pQueuePriorities,
            existing->pQueuePriorities + existing->queueCount);
        private_priorities.push_back(0.5f);
        existing->queueCount++;
        existing->pQueuePriorities = private_priorities.data();
      } else {
        private_priorities.push_back(0.5f);
        VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue.queueFamilyIndex = private_queue->first;
        queue.queueCount = 1;
        queue.pQueuePriorities = private_priorities.data();
        native_queues.push_back(queue);
      }
      copy.queueCreateInfoCount = static_cast<std::uint32_t>(native_queues.size());
      copy.pQueueCreateInfos = native_queues.data();
      auto enable = [&](VkPhysicalDeviceFeatures& enabled) {
        enabled.sparseBinding = VK_TRUE;
        enabled.sparseResidencyImage2D = native_features.sparseResidencyImage2D;
        enabled.sparseResidencyImage3D = native_features.sparseResidencyImage3D;
      };
      bool feature_chain = false;
      for (auto* node = const_cast<VkBaseOutStructure*>(static_cast<const VkBaseOutStructure*>(copy.pNext));
           node; node = node->pNext) {
        if (node->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) continue;
        enable(reinterpret_cast<VkPhysicalDeviceFeatures2*>(node)->features);
        feature_chain = true;
      }
      if (!feature_chain) { enable(features); copy.pEnabledFeatures = &features; }
    }
  }
  // Unity/HLSL compute shaders declare storage images Rgba32f while binding
  // R16G16B16A16_SFLOAT or B10G11R11_UFLOAT views; such loads and stores are
  // undefined and differ between hosts. With the formatless features enabled,
  // create_shader declares them Unknown so every host follows the view format.
  const bool formatless = storage_format_rewrite_enabled() &&
      native_features.shaderStorageImageReadWithoutFormat && native_features.shaderStorageImageWriteWithoutFormat;
  // With VK_KHR_format_feature_flags2 (core in 1.3) the host also says per
  // format whether storage access without a declared format works. A storage
  // format lacking it keeps its declarations as the guest wrote them: making
  // them Unknown would break shaders whose views do match.
  std::vector<std::uint32_t> formatless_keep;
  if (formatless) {
    VkPhysicalDeviceProperties properties{};
    host<PFN_vkGetPhysicalDeviceProperties>(physical, "vkGetPhysicalDeviceProperties")(physical, &properties);
    const bool flags2 = properties.apiVersion >= VK_API_VERSION_1_3 || std::ranges::any_of(host_extensions,
        [](const auto& e) { return std::string_view(e.extensionName) == VK_KHR_FORMAT_FEATURE_FLAGS_2_EXTENSION_NAME; });
    // A 1.0 game instance has only the KHR alias (lavapipe's loader dispatch is null for the core name).
    auto query = reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties2>(
        raw_physical_proc(physical, "vkGetPhysicalDeviceFormatProperties2"));
    std::string kept;
    for (std::uint32_t declared = 1; flags2 && query && declared < 64; ++declared) {
      const auto format = storage_image_format(declared);
      if (format == VK_FORMAT_UNDEFINED) continue;
      VkFormatProperties3 features3{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
      VkFormatProperties2 features2{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, &features3};
      query(physical, format, &features2);
      constexpr VkFormatFeatureFlags2 without = VK_FORMAT_FEATURE_2_STORAGE_READ_WITHOUT_FORMAT_BIT |
                                                VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT;
      const auto supported = features3.optimalTilingFeatures;
      if ((supported & VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT) && (supported & without) != without) {
        formatless_keep.push_back(declared);
        kept += (kept.empty() ? "" : ", ") + std::to_string(int(format));
      }
    }
    if (!kept.empty())
      log::info("vulkan", "Storage images of VkFormat {} keep their declared format: the host lacks storage "
                "access without a format for them", kept);
  }
  if (formatless) {
    auto enable = [](VkPhysicalDeviceFeatures& enabled) {
      enabled.shaderStorageImageReadWithoutFormat = VK_TRUE;
      enabled.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    };
    bool feature_chain = false;
    for (auto* node = const_cast<VkBaseOutStructure*>(static_cast<const VkBaseOutStructure*>(copy.pNext));
         node; node = node->pNext) {
      if (node->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) continue;
      enable(reinterpret_cast<VkPhysicalDeviceFeatures2*>(node)->features);
      feature_chain = true;
    }
    if (!feature_chain) { enable(features); copy.pEnabledFeatures = &features; }
  }
  if (venture::environment::getenv("VENTURE_TRACE_VULKAN"))
    log::info("vulkan-robustness", "Guest requested buffer={} buffer2={} image={} image2={} nullDescriptor={}",
        bool(base_features ? base_features->features.robustBufferAccess : features.robustBufferAccess),
        bool(requested_robust2 && requested_robust2->robustBufferAccess2),
        bool((requested_image && requested_image->robustImageAccess) || (requested_v13 && requested_v13->robustImageAccess)),
        bool(requested_robust2 && requested_robust2->robustImageAccess2),
        bool(requested_robust2 && requested_robust2->nullDescriptor));
  VkPhysicalDeviceFaultFeaturesEXT fault{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT};
  PFN_vkGetPhysicalDeviceFeatures2 query_features{};
  std::uint32_t instance_version{};
  {
    std::lock_guard lock(state_lock);
    auto pi = physicals.find(physical);
    auto ii = instances.find(pi != physicals.end() ? pi->second : gfx::gpu_device().instance);
    instance_version = ii != instances.end() ? ii->second.version : gfx::gpu_device().api_version;
    const bool core = ii != instances.end() ? ii->second.version >= VK_API_VERSION_1_1
                                           : gfx::gpu_device().api_version >= VK_API_VERSION_1_1;
    const bool khr = ii != instances.end() && (ii->second.host_properties2 ||
        ii->second.extensions.contains(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME));
    if (core || khr)
      query_features = host<PFN_vkGetPhysicalDeviceFeatures2>(physical,
          core ? "vkGetPhysicalDeviceFeatures2" : "vkGetPhysicalDeviceFeatures2KHR");
  }
  if (query_features && std::ranges::any_of(host_extensions, [](const auto& e) {
        return std::string_view(e.extensionName) == VK_EXT_DEVICE_FAULT_EXTENSION_NAME;
      })) {
    VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &fault};
    query_features(physical, &query);
  }
  const bool fault_enabled = fault.deviceFault;
  VkPhysicalDeviceRobustness2FeaturesEXT robust2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
  VkPhysicalDeviceImageRobustnessFeatures image_robust{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES};
  // Opt-in diagnostic. Preserve guest-requested features when disabled, and
  // keep internal extensions and feature changes out of guest capabilities.
  if (const auto* setting = venture::environment::getenv("VENTURE_VULKAN_ROBUST_ACCESS"); setting && std::string_view(setting) == "1") {
    VkPhysicalDeviceFeatures supported{};
    host<PFN_vkGetPhysicalDeviceFeatures>(physical, "vkGetPhysicalDeviceFeatures")(physical, &supported);
    auto& enabled = base_features ? base_features->features : features;
    // VUID-VkDeviceCreateInfo-robustBufferAccess-10247: enabling robustness
    // must not make the guest's update-after-bind feature combination invalid.
    if (buffer_update_after_bind && query_features) {
      VkPhysicalDeviceDescriptorIndexingProperties indexing{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES};
      VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &indexing};
      auto query_properties = host<PFN_vkGetPhysicalDeviceProperties2>(physical,
          instance_version >= VK_API_VERSION_1_1 ? "vkGetPhysicalDeviceProperties2" : "vkGetPhysicalDeviceProperties2KHR");
      if (query_properties) query_properties(physical, &properties);
      if (!indexing.robustBufferAccessUpdateAfterBind) supported.robustBufferAccess = VK_FALSE;
    }
    enabled.robustBufferAccess |= supported.robustBufferAccess;
    if (!base_features) copy.pEnabledFeatures = &features;
    auto has_extension = [&](const char* name) {
      return std::ranges::any_of(host_extensions, [&](const auto& e) { return std::string_view(e.extensionName) == name; });
    };
    const bool has_robust2 = query_features && has_extension(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
    VkPhysicalDeviceProperties properties{};
    host<PFN_vkGetPhysicalDeviceProperties>(physical, "vkGetPhysicalDeviceProperties")(physical, &properties);
    const bool has_image = query_features && (has_extension(VK_EXT_IMAGE_ROBUSTNESS_EXTENSION_NAME) ||
        (instance_version >= VK_API_VERSION_1_3 && properties.apiVersion >= VK_API_VERSION_1_3));
    VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    if (has_robust2) { robust2.pNext = query.pNext; query.pNext = &robust2; }
    if (has_image) { image_robust.pNext = query.pNext; query.pNext = &image_robust; }
    if (query.pNext) query_features(physical, &query);
    auto add_extension = [&](const char* name) {
      if (unique_names.insert(name).second) names.push_back(name);
    };
    // robustBufferAccess2 requires robustBufferAccess (VUID-04000).
    robust2.robustBufferAccess2 &= enabled.robustBufferAccess;
    if (has_robust2 && (robust2.robustBufferAccess2 || robust2.robustImageAccess2)) {
      if (requested_robust2) {
        requested_robust2->robustBufferAccess2 |= robust2.robustBufferAccess2;
        requested_robust2->robustImageAccess2 |= robust2.robustImageAccess2;
      } else {
        robust2.nullDescriptor = VK_FALSE;
        robust2.pNext = const_cast<void*>(copy.pNext);
        copy.pNext = &robust2;
      }
      add_extension(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
    }
    if (has_image && image_robust.robustImageAccess) {
      if (requested_v13) requested_v13->robustImageAccess = VK_TRUE;
      else if (requested_image) requested_image->robustImageAccess = VK_TRUE;
      else {
        image_robust.pNext = const_cast<void*>(copy.pNext);
        copy.pNext = &image_robust;
      }
      if (has_extension(VK_EXT_IMAGE_ROBUSTNESS_EXTENSION_NAME))
        add_extension(VK_EXT_IMAGE_ROBUSTNESS_EXTENSION_NAME);
    }
    log::info("vulkan-robustness", "Host diagnostic enabled buffer={} buffer2={} image={} image2={}",
        bool(enabled.robustBufferAccess), bool(robust2.robustBufferAccess2),
        bool(image_robust.robustImageAccess), bool(robust2.robustImageAccess2));
  }
  if (fault_enabled) {
    fault.deviceFaultVendorBinary = VK_FALSE;
    VkPhysicalDeviceFaultFeaturesEXT* existing{};
    for (auto* next = const_cast<VkBaseOutStructure*>(static_cast<const VkBaseOutStructure*>(copy.pNext)); next;
         next = next->pNext)
      if (next->sType == fault.sType) existing = reinterpret_cast<VkPhysicalDeviceFaultFeaturesEXT*>(next);
    if (existing) existing->deviceFault = VK_TRUE;
    else {
      fault.pNext = const_cast<void*>(copy.pNext);
      copy.pNext = &fault;
    }
    if (unique_names.insert(VK_EXT_DEVICE_FAULT_EXTENSION_NAME).second)
      names.push_back(VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
  }
  VkPhysicalDeviceAddressBindingReportFeaturesEXT address_report{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ADDRESS_BINDING_REPORT_FEATURES_EXT};
  if (address_log_device(host_extensions) && unique_names.insert(VK_EXT_DEVICE_ADDRESS_BINDING_REPORT_EXTENSION_NAME).second) {
    names.push_back(VK_EXT_DEVICE_ADDRESS_BINDING_REPORT_EXTENSION_NAME);
    address_report.reportAddressBinding = VK_TRUE;
    address_report.pNext = const_cast<void*>(copy.pNext);
    copy.pNext = &address_report;
  }
  const bool checkpoints = breadcrumbs_enabled() && std::ranges::any_of(host_extensions, [](const auto& e) {
    return std::string_view(e.extensionName) == VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME;
  });
  if (checkpoints && unique_names.insert(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME).second)
    names.push_back(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
  copy.enabledExtensionCount = static_cast<std::uint32_t>(names.size());
  copy.ppEnabledExtensionNames = names.data();
  const bool trace = venture::environment::getenv("VENTURE_TRACE_VULKAN") != nullptr;
  if (trace) {
    for (std::uint32_t i = 0; i < copy.enabledExtensionCount; ++i)
      log::info("vulkan-device", "Host device extension: {}", copy.ppEnabledExtensionNames[i]);
    for (auto* next = static_cast<const VkBaseInStructure*>(copy.pNext); next; next = next->pNext)
      log::info("vulkan-device", "Host device structure: {}", int(next->sType));
    for (std::uint32_t i = 0; i < copy.queueCreateInfoCount; ++i)
      log::info("vulkan-device", "Host queue family {} count {} flags {}", copy.pQueueCreateInfos[i].queueFamilyIndex,
                copy.pQueueCreateInfos[i].queueCount, copy.pQueueCreateInfos[i].flags);
    log::info("vulkan-device", "Host allocator {}", alloc != nullptr);
  }
  auto r = f(physical, &copy, alloc, out);
  if (trace) log::info("vulkan-device", "Host vkCreateDevice returned {}", int(r));
  if (r != VK_SUCCESS) {
    log::error("vulkan", "vkCreateDevice returned {}", int(r));
    for (std::uint32_t i = 0; i < copy.enabledExtensionCount; ++i)
      log::error("vulkan", "Requested device extension: {}", copy.ppEnabledExtensionNames[i]);
    for (auto* next = static_cast<const VkBaseInStructure*>(copy.pNext); next; next = next->pNext)
      log::error("vulkan", "Requested device feature structure: {}", int(next->sType));
  }
  if (r == VK_SUCCESS) {
    try {
      adopt_device(physical, *out, *info, alloc != nullptr, shared_queues);
      install_queue_facades(*out, *info, shared_queues);
    } catch (...) {
      const auto created = *out;
      forget_queue_facades(created);
      {
        std::lock_guard lock(state_lock);
        std::erase_if(queues, [&](const auto& q) { return q.second.device == created; });
        std::erase_if(children, [&](const auto& child) { return child.second == created; });
        devices.erase(created);
        invalidate_layer_procedures();
      }
      gfx::forget_device_queues(created);
      host<PFN_vkDestroyDevice>(physical, "vkDestroyDevice")(created, alloc);
      *out = VK_NULL_HANDLE;
      throw;
    }
    if (trace) log::info("vulkan-device", "Guest device adopted");
    if (private_queue) {
      VkQueue queue{};
      host<PFN_vkGetDeviceQueue>(*out, "vkGetDeviceQueue")(
          *out, private_queue->first, private_queue->second, &queue);
      gfx::register_queue(*out, queue);
      {
        std::lock_guard lock(state_lock);
        children[h2g(queue)] = *out;
        const auto& device = devices.at(*out);
        queues[queue] = {device.instance, device.physical, *out, private_queue->first,
            private_queue->second, gfx::queue_mutex(queue)};
      }
      configure_lazy_texture_device(*out, queue, private_queue->first, native_features);
    }
    {
      std::lock_guard lock(state_lock);
      devices.at(*out).fault_enabled = fault_enabled;
    }
    set_storage_without_format(*out, formatless, formatless_keep);
    if (venture::environment::getenv("VENTURE_TRACE_VULKAN"))
      log::info("vulkan-fault", "Host device fault diagnostics enabled={}", fault_enabled);
    VkPhysicalDeviceProperties properties{};
    host<PFN_vkGetPhysicalDeviceProperties>(physical, "vkGetPhysicalDeviceProperties")(physical, &properties);
    std::uint32_t version;
    { std::lock_guard lock(state_lock); version = devices.at(*out).version; }
    const bool feedback = unique_names.contains(VK_EXT_PIPELINE_CREATION_FEEDBACK_EXTENSION_NAME) ||
        (properties.apiVersion >= VK_API_VERSION_1_3 && version >= VK_API_VERSION_1_3);
    auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(loader()(device_instance(*out), "vkGetDeviceProcAddr"));
    gfx::register_pipeline_device(*out, gdpa, feedback, profile::PipelineSource::guest);
    if (trace) log::info("vulkan-device", "Guest device queue configured");
    prepare_texture_device(*out);
    if (trace) log::info("vulkan-device", "Guest device textures prepared");
    if (breadcrumbs_enabled()) prepare_breadcrumb_device(*out, checkpoints);
    log::info("vulkan", "Guest device: {} (vendor=0x{:04x}, device=0x{:04x})",
              properties.deviceName, properties.vendorID, properties.deviceID);
  }
  return r;
}
VkResult VKAPI_CALL create_device(VkPhysicalDevice physical, const VkDeviceCreateInfo* info,
                                  const VkAllocationCallbacks* alloc, VkDevice* out) {
  return create_device_impl(physical, info, alloc, out, XrResolverScope::active());
}
std::mutex release_hooks_lock;
std::vector<void (*)(VkDevice)> release_hooks;
void release_device_resources(VkDevice device) {
  std::vector<void (*)(VkDevice)> hooks;
  {
    std::lock_guard lock(release_hooks_lock);
    hooks = release_hooks;
  }
  for (auto* hook : hooks) hook(device);
  release_timing_device(device);
  release_texture_device(device);
  release_breadcrumb_device(device);
  gfx::release_pipeline_device(device);
  release_sync_fd_device(device);
  release_hardware_buffer_device(device);
  release_external_device(device);
  release_wsi_device(device);
  release_deferred_free_device(device);
  forget_queue_facades(device);
}
void VKAPI_CALL destroy_device(VkDevice device, const VkAllocationCallbacks* alloc) {
  if (!device) return;
  release_device_resources(device);
  host<PFN_vkDestroyDevice>(device, "vkDestroyDevice")(device, alloc);
  gfx::forget_device_queues(device);
  std::lock_guard lock(state_lock);
  devices.erase(device);
  set_storage_without_format(device, false);
  std::erase_if(pool_callbacks, [&](const auto& pool) {
    if (pool.second.first != device) return false;
    pool_flags.erase(pool.first);
    return true;
  });
  invalidate_layer_procedures();
  std::erase_if(queues, [&](auto& p) { return p.second.device == device; });
  std::erase_if(command_pools, [&](auto& p) {
    auto it = children.find(h2g(p.first));
    if (it == children.end() || it->second != device) return false;
    if (auto owner = pool_commands.find(p.second); owner != pool_commands.end()) {
      owner->second.erase(p.first);
      if (owner->second.empty()) pool_commands.erase(owner);
    }
    return true;
  });
  std::erase_if(children, [&](auto& p) { return p.second == device; });
}
void VKAPI_CALL get_queue(VkDevice device, std::uint32_t family, std::uint32_t index, VkQueue* out) {
  if (facade_queue(device, family, index, 0, out)) return;
  index = host_queue_index(device, family, index);
  host<PFN_vkGetDeviceQueue>(device, "vkGetDeviceQueue")(device, family, index, out);
  if (out && *out) {
    std::lock_guard lock(state_lock);
    children[h2g(*out)] = device;
    const auto& d = devices.at(device);
    auto& q = queues[*out];
    q.submission_mutex = gfx::queue_mutex(*out);
    q.instance = d.instance; q.physical = d.physical; q.device = device;
    q.family = family; q.index = index;
  }
  if (out && *out) timing_queue(device, *out, family);
  if (out && *out) gfx::register_queue(device, *out);
}
void VKAPI_CALL get_queue2(VkDevice device, const VkDeviceQueueInfo2* guest, VkQueue* out) {
  if (!guest) { if (out) *out = VK_NULL_HANDLE; return; }
  if (facade_queue(device, guest->queueFamilyIndex, guest->queueIndex, guest->flags, out)) return;
  auto query = *guest;
  query.queueIndex = host_queue_index(device, query.queueFamilyIndex, query.queueIndex);
  const auto* info = &query;
  host<PFN_vkGetDeviceQueue2>(device, "vkGetDeviceQueue2")(device, info, out);
  if (out && *out) {
    std::lock_guard lock(state_lock);
    children[h2g(*out)] = device;
    const auto& d = devices.at(device);
    auto& q = queues[*out];
    q.submission_mutex = gfx::queue_mutex(*out);
    q.instance = d.instance; q.physical = d.physical; q.device = device;
    q.family = info->queueFamilyIndex; q.index = info->queueIndex;
  }
  if (out && *out && !info->flags) timing_queue(device, *out, info->queueFamilyIndex);
  if (out && *out) gfx::register_queue(device, *out);
}
VkResult VKAPI_CALL queue_submit(VkQueue queue, std::uint32_t count, const VkSubmitInfo* submits, VkFence fence) {
  if (auto facade = queue_facade(queue))
    return facade->deferred->submit(facade->deferred_index, count, submits, fence);
  deferred_free_submitting(queue);
  auto r = texture_submit(queue, count, submits, fence, host<PFN_vkQueueSubmit>(queue, "vkQueueSubmit"));
  if (r == VK_SUCCESS) sync_fd_submitted(queue, count, submits);
  if (r == VK_SUCCESS) deferred_free_submitted(queue);
  return r;
}
VkResult VKAPI_CALL queue_submit2(VkQueue queue, std::uint32_t count, const VkSubmitInfo2* submits, VkFence fence) {
  if (auto facade = queue_facade(queue))
    return facade->deferred->submit2(facade->deferred_index, count, submits, fence);
  auto fn = host<PFN_vkQueueSubmit2>(queue, "vkQueueSubmit2");
  if (!fn) fn = host<PFN_vkQueueSubmit2>(queue, "vkQueueSubmit2KHR");
  if (!fn) return VK_ERROR_EXTENSION_NOT_PRESENT;
  deferred_free_submitting(queue);
  auto r = texture_submit2(queue, count, submits, fence, fn);
  if (r == VK_SUCCESS) sync_fd_submitted(queue, count, submits);
  if (r == VK_SUCCESS) deferred_free_submitted(queue);
  return r;
}
VkResult VKAPI_CALL queue_wait_idle(VkQueue queue) {
  if (auto facade = queue_facade(queue)) return facade->deferred->wait_idle(facade->deferred_index);
  auto r = host<PFN_vkQueueWaitIdle>(queue, "vkQueueWaitIdle")(queue);
  if (r == VK_SUCCESS) texture_queue_idle(queue);
  if (r == VK_SUCCESS) deferred_free_queue_idle(queue);
  return r;
}
VkResult VKAPI_CALL queue_bind_sparse(VkQueue queue, std::uint32_t count,
    const VkBindSparseInfo* binds, VkFence fence) {
  if (auto facade = queue_facade(queue))
    return facade->deferred->submit_sparse(facade->deferred_index, count, binds, fence, queue_bind_sparse);
  texture_queue_uncertain(queue);
  // Sparse binding is queue work: later frees of the memory wait for it.
  deferred_free_submitting(queue);
  return host<PFN_vkQueueBindSparse>(queue, "vkQueueBindSparse")(queue, count, binds, fence);
}
template<class F> F next_device(VkDevice device, const char* name) {
  if (auto next = next_proc(Layer::dispatch, name)) return reinterpret_cast<F>(next);
  if (auto fn = host<F>(device, name)) return fn;
  if (std::string_view(name) == "vkWaitSemaphores" || std::string_view(name) == "vkSignalSemaphore" ||
      std::string_view(name) == "vkGetSemaphoreCounterValue") {
    const auto alias = std::string(name) + "KHR";
    if (auto fn = host<F>(device, alias.c_str())) return fn;
  }
  throw std::runtime_error("native device function unavailable");
}
VkResult VKAPI_CALL create_semaphore(VkDevice device, const VkSemaphoreCreateInfo* info,
    const VkAllocationCallbacks* allocator, VkSemaphore* out) {
  auto result = next_device<PFN_vkCreateSemaphore>(device, "vkCreateSemaphore")(device, info, allocator, out);
  if (result != VK_SUCCESS) return result;
  if (auto dispatcher = deferred_device(device)) {
    bool timeline = false;
    for (auto* node = static_cast<const VkBaseInStructure*>(info->pNext); node; node = node->pNext)
      if (node->sType == VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO)
        timeline = reinterpret_cast<const VkSemaphoreTypeCreateInfo*>(node)->semaphoreType == VK_SEMAPHORE_TYPE_TIMELINE;
    try { dispatcher->add_semaphore(*out, timeline); }
    catch (...) {
      next_device<PFN_vkDestroySemaphore>(device, "vkDestroySemaphore")(device, *out, allocator);
      *out = VK_NULL_HANDLE;
      throw;
    }
  }
  return result;
}
void VKAPI_CALL destroy_semaphore(VkDevice device, VkSemaphore semaphore, const VkAllocationCallbacks* allocator) {
  if (auto dispatcher = deferred_device(device)) dispatcher->remove_semaphore(semaphore);
  next_device<PFN_vkDestroySemaphore>(device, "vkDestroySemaphore")(device, semaphore, allocator);
}
template<class Operation> VkResult wait_with_dispatch(VkDevice device, std::uint64_t timeout, Operation operation) {
  auto dispatcher = deferred_device(device);
  if (!dispatcher) return operation(timeout);
  const auto start = std::chrono::steady_clock::now();
  for (;;) {
    auto result = dispatcher->flush();
    if (result != VK_SUCCESS) return result;
    const auto elapsed = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start).count());
    const auto remaining = timeout > elapsed ? timeout - elapsed : 0;
    result = operation(std::min(remaining, std::uint64_t{1'000'000}));
    if (result != VK_TIMEOUT) return result;
    if (timeout != UINT64_MAX && static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - start).count()) >= timeout) return VK_TIMEOUT;
  }
}
VkResult VKAPI_CALL wait_fences(VkDevice device, std::uint32_t count, const VkFence* fences,
    VkBool32 all, std::uint64_t timeout) {
  auto fn = next_device<PFN_vkWaitForFences>(device, "vkWaitForFences");
  return wait_with_dispatch(device, timeout, [&](std::uint64_t slice) { return fn(device, count, fences, all, slice); });
}
VkResult VKAPI_CALL fence_status(VkDevice device, VkFence fence) {
  if (auto dispatcher = deferred_device(device)) {
    auto result = dispatcher->flush();
    if (result != VK_SUCCESS) return result;
  }
  return next_device<PFN_vkGetFenceStatus>(device, "vkGetFenceStatus")(device, fence);
}
VkResult VKAPI_CALL wait_semaphores(VkDevice device, const VkSemaphoreWaitInfo* info, std::uint64_t timeout) {
  auto fn = next_device<PFN_vkWaitSemaphores>(device, "vkWaitSemaphores");
  return wait_with_dispatch(device, timeout, [&](std::uint64_t slice) { return fn(device, info, slice); });
}
VkResult VKAPI_CALL semaphore_value(VkDevice device, VkSemaphore semaphore, std::uint64_t* out) {
  if (auto dispatcher = deferred_device(device)) {
    auto result = dispatcher->flush();
    if (result != VK_SUCCESS) return result;
  }
  return next_device<PFN_vkGetSemaphoreCounterValue>(device, "vkGetSemaphoreCounterValue")(device, semaphore, out);
}
VkResult VKAPI_CALL signal_semaphore(VkDevice device, const VkSemaphoreSignalInfo* info) {
  auto result = next_device<PFN_vkSignalSemaphore>(device, "vkSignalSemaphore")(device, info);
  if (result == VK_SUCCESS) if (auto dispatcher = deferred_device(device)) result = dispatcher->flush();
  return result;
}
VkResult VKAPI_CALL device_wait_idle(VkDevice device) {
  auto dispatcher = deferred_device(device);
  if (dispatcher) {
    for (std::uint32_t i = 0; i < dispatcher->queue_count(); ++i) {
      auto result = dispatcher->wait_submitted(i);
      if (result != VK_SUCCESS) return result;
    }
    gfx::DeviceQueueLock lock(device);
    return next_device<PFN_vkDeviceWaitIdle>(device, "vkDeviceWaitIdle")(device);
  }
  return next_device<PFN_vkDeviceWaitIdle>(device, "vkDeviceWaitIdle")(device);
}
VkResult VKAPI_CALL allocate_commands(VkDevice device, const VkCommandBufferAllocateInfo* info,
                                      VkCommandBuffer* out) {
  auto r = host<PFN_vkAllocateCommandBuffers>(device, "vkAllocateCommandBuffers")(device, info, out);
  if (r == VK_SUCCESS) {
    std::lock_guard lock(state_lock);
    for (std::uint32_t i = 0; i < info->commandBufferCount; ++i) {
      children[h2g(out[i])] = device;
      command_pools[out[i]] = info->commandPool;
      pool_commands[info->commandPool].insert(out[i]);
    }
    lifetime_events[0].fetch_add(1, std::memory_order_relaxed);
    invalidate_layer_procedures();
  }
  return r;
}
VkResult VKAPI_CALL create_pool(VkDevice device, const VkCommandPoolCreateInfo* info,
                                const VkAllocationCallbacks* alloc, VkCommandPool* out) {
  auto r = host<PFN_vkCreateCommandPool>(device, "vkCreateCommandPool")(device, info, alloc, out);
  if (r != VK_SUCCESS) return r;
  auto physical = device_physical(device);
  auto query = host<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(physical,
                                                                  "vkGetPhysicalDeviceQueueFamilyProperties");
  std::uint32_t count = 0;
  query(physical, &count, nullptr);
  std::vector<VkQueueFamilyProperties> families(count);
  query(physical, &count, families.data());
  std::lock_guard lock(state_lock);
  pool_flags[*out] = families.at(info->queueFamilyIndex).queueFlags;
  pool_callbacks[*out] = {device, alloc != nullptr};
  lifetime_events[2].fetch_add(1, std::memory_order_relaxed);
  invalidate_layer_procedures();
  return r;
}
void VKAPI_CALL free_commands(VkDevice device, VkCommandPool pool, std::uint32_t count,
                              const VkCommandBuffer* cmds) {
  host<PFN_vkFreeCommandBuffers>(device, "vkFreeCommandBuffers")(device, pool, count, cmds);
  for (std::uint32_t i = 0; i < count; ++i) {
    release_texture_commands(cmds[i]);
    release_render_commands(cmds[i]);
  }
  std::lock_guard lock(state_lock);
  for (std::uint32_t i = 0; i < count; ++i) {
    if (!cmds[i]) continue;
    children.erase(h2g(cmds[i]));
    command_pools.erase(cmds[i]);
    if (auto owner = pool_commands.find(pool); owner != pool_commands.end()) owner->second.erase(cmds[i]);
  }
  lifetime_events[1].fetch_add(1, std::memory_order_relaxed);
  invalidate_layer_procedures();
}
VkResult VKAPI_CALL reset_pool(VkDevice device, VkCommandPool pool, VkCommandPoolResetFlags flags) {
  auto r = host<PFN_vkResetCommandPool>(device, "vkResetCommandPool")(device, pool, flags);
  if (r != VK_SUCCESS) return r;
  std::vector<VkCommandBuffer> commands;
  {
    std::lock_guard lock(state_lock);
    if (auto owner = pool_commands.find(pool); owner != pool_commands.end())
      commands.assign(owner->second.begin(), owner->second.end());
  }
  for (auto command : commands) {
    release_texture_commands(command);
    release_render_commands(command);
  }
  if (flags & VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT) trim_texture_pool(device, pool);
  return r;
}
void VKAPI_CALL trim_pool(VkDevice device, VkCommandPool pool, VkCommandPoolTrimFlags flags) {
  auto fn = host<PFN_vkTrimCommandPool>(device, "vkTrimCommandPool");
  if (!fn) fn = host<PFN_vkTrimCommandPool>(device, "vkTrimCommandPoolKHR");
  if (!fn) throw std::runtime_error("missing Vulkan command vkTrimCommandPool");
  fn(device, pool, flags);
  trim_texture_pool(device, pool);
}
void VKAPI_CALL destroy_pool(VkDevice device, VkCommandPool pool, const VkAllocationCallbacks* alloc) {
  host<PFN_vkDestroyCommandPool>(device, "vkDestroyCommandPool")(device, pool, alloc);
  std::vector<VkCommandBuffer> removed;
  {
    std::lock_guard lock(state_lock);
    pool_flags.erase(pool);
    pool_callbacks.erase(pool);
    if (auto owner = pool_commands.find(pool); owner != pool_commands.end()) {
      for (auto command : owner->second) {
        removed.push_back(command);
        command_pools.erase(command);
        children.erase(h2g(command));
      }
      pool_commands.erase(owner);
    }
    lifetime_events[3].fetch_add(1, std::memory_order_relaxed);
    invalidate_layer_procedures();
  }
  for (auto command : removed) {
    release_texture_commands(command);
    release_render_commands(command);
  }
  trim_texture_pool(device, pool);
}
// The emulated Quest's Adreno is an integrated GPU, and a CPU implementation's
// unified, host-visible memory matches that class; Unity never selects a
// CPU-type device. Hardware GPUs keep their reported type. Name, IDs, memory
// types and the other limits stay the host's.
// Uniform buffer limits are Quest's where the host is more permissive (RADV:
// 4 GiB ranges at 4-byte offsets). Applications size uniform descriptors and
// dynamic offsets from these; with RADV's values HITMAN's uniform descriptors
// overran their buffers (validation), while NVIDIA reports Quest's values.
void present_device_type(VkPhysicalDeviceProperties& properties) {
  if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU)
    properties.deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
  auto& limits = properties.limits;
  limits.maxUniformBufferRange = std::min(limits.maxUniformBufferRange, 65536u);
  limits.minUniformBufferOffsetAlignment = std::max(limits.minUniformBufferOffsetAlignment, VkDeviceSize(64));
}
void VKAPI_CALL physical_properties(VkPhysicalDevice physical, VkPhysicalDeviceProperties* out) {
  host<PFN_vkGetPhysicalDeviceProperties>(physical, "vkGetPhysicalDeviceProperties")(physical, out);
  present_device_type(*out);
}
void VKAPI_CALL physical_properties2(VkPhysicalDevice physical, VkPhysicalDeviceProperties2* out) {
  auto f = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
      raw_physical_proc(physical, "vkGetPhysicalDeviceProperties2"));
  if (f)
    f(physical, out);
  else
    host<PFN_vkGetPhysicalDeviceProperties>(physical, "vkGetPhysicalDeviceProperties")(physical, &out->properties);
  present_device_type(out->properties);
  if (emulated_density_maps(physical))
    for (auto* next = static_cast<VkBaseOutStructure*>(out->pNext); next; next = next->pNext)
      if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_PROPERTIES_EXT) {
        auto& density = *reinterpret_cast<VkPhysicalDeviceFragmentDensityMapPropertiesEXT*>(next);
        density.minFragmentDensityTexelSize = emulated_density_texel_min;
        density.maxFragmentDensityTexelSize = emulated_density_texel_max;
        density.fragmentDensityInvocations = VK_FALSE;
      }
}
bool queue_property_trace_enabled() {
  static const bool enabled = [] {
    const auto* value = venture::environment::getenv("VENTURE_VULKAN_QUEUE_TRACE");
    const bool on = value && std::string_view(value) == "1";
    if (on) log::info("vk-queue", "Guest queue family trace enabled");
    return on;
  }();
  return enabled;
}
// Quest exposes three graphics queues and titles require them (HITMAN). Requests beyond the host's
// count are fitted to it and served by distinct facades over the shared queues.
void VKAPI_CALL queue_families(VkPhysicalDevice physical, std::uint32_t* count, VkQueueFamilyProperties* out) {
  host<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(physical, "vkGetPhysicalDeviceQueueFamilyProperties")(physical, count, out);
  if (out && count) for (std::uint32_t i = 0; i < *count; ++i)
    if (out[i].queueCount && (out[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
      out[i].queueCount = std::max(out[i].queueCount, 3u);
  if (!queue_property_trace_enabled()) return;
  static std::atomic<unsigned> calls{};
  if (calls.fetch_add(1) >= 8 || !count) return;
  if (!out) log::info("vk-queue", "Guest queue families count-only result={}", *count);
  else for (std::uint32_t i = 0; i < *count; ++i)
    log::info("vk-queue", "Guest queue family={} returned_count={} returned_flags={}", i, out[i].queueCount, out[i].queueFlags);
}
void VKAPI_CALL queue_families2(VkPhysicalDevice physical, std::uint32_t* count, VkQueueFamilyProperties2* out) {
  auto fn = host<PFN_vkGetPhysicalDeviceQueueFamilyProperties2>(physical, "vkGetPhysicalDeviceQueueFamilyProperties2");
  if (!fn) fn = host<PFN_vkGetPhysicalDeviceQueueFamilyProperties2>(physical, "vkGetPhysicalDeviceQueueFamilyProperties2KHR");
  fn(physical, count, out);
  if (out && count) for (std::uint32_t i = 0; i < *count; ++i) {
    auto& family = out[i].queueFamilyProperties;
    if (family.queueCount && (family.queueFlags & VK_QUEUE_GRAPHICS_BIT))
      family.queueCount = std::max(family.queueCount, 3u);
  }
  if (!queue_property_trace_enabled()) return;
  static std::atomic<unsigned> calls{};
  if (calls.fetch_add(1) >= 8 || !count) return;
  if (!out) log::info("vk-queue", "Guest queue families2 count-only result={}", *count);
  else for (std::uint32_t i = 0; i < *count; ++i)
    log::info("vk-queue", "Guest queue family2={} returned_count={} returned_flags={}", i, out[i].queueFamilyProperties.queueCount, out[i].queueFamilyProperties.queueFlags);
}
PFN_vkVoidFunction VKAPI_CALL guest_gipa(VkInstance instance, const char* name) {
  return reinterpret_cast<PFN_vkVoidFunction>(instance_proc(instance, name));
}
PFN_vkVoidFunction VKAPI_CALL guest_gdpa(VkDevice device, const char* name) {
  return reinterpret_cast<PFN_vkVoidFunction>(device_proc(device, name));
}
PFN_vkVoidFunction override_proc(std::string_view name) {
  const auto* c = find_command(name);
  if (!c) return nullptr;
  name = c->canonical;
#define OVERRIDE(vk, fn) \
  if (name == #vk) return reinterpret_cast<PFN_vkVoidFunction>(fn)
  OVERRIDE(vkGetInstanceProcAddr, guest_gipa);
  OVERRIDE(vkGetDeviceProcAddr, guest_gdpa);
  OVERRIDE(vkCreateInstance, create_instance);
  OVERRIDE(vkDestroyInstance, destroy_instance);
  OVERRIDE(vkCreateDevice, create_device);
  OVERRIDE(vkDestroyDevice, destroy_device);
  OVERRIDE(vkEnumerateInstanceExtensionProperties, enumerate_instance_extensions);
  OVERRIDE(vkEnumerateDeviceExtensionProperties, enumerate_device_extensions);
  OVERRIDE(vkEnumerateInstanceLayerProperties, enumerate_layers);
  OVERRIDE(vkEnumerateDeviceLayerProperties, enumerate_device_layers);
  OVERRIDE(vkEnumerateInstanceVersion, enumerate_version);
  OVERRIDE(vkEnumeratePhysicalDevices, enumerate_physicals);
  OVERRIDE(vkEnumeratePhysicalDeviceGroups, enumerate_groups);
  OVERRIDE(vkGetPhysicalDeviceProperties, physical_properties);
  OVERRIDE(vkGetPhysicalDeviceProperties2, physical_properties2);
  OVERRIDE(vkGetPhysicalDeviceQueueFamilyProperties, queue_families);
  OVERRIDE(vkGetPhysicalDeviceQueueFamilyProperties2, queue_families2);
  OVERRIDE(vkGetDeviceQueue, get_queue);
  OVERRIDE(vkGetDeviceQueue2, get_queue2);
  OVERRIDE(vkQueueSubmit, queue_submit);
  OVERRIDE(vkQueueSubmit2, queue_submit2);
  OVERRIDE(vkQueueWaitIdle, queue_wait_idle);
  OVERRIDE(vkQueueBindSparse, queue_bind_sparse);
  if (queue_facades_active.load(std::memory_order_acquire)) {
    OVERRIDE(vkCreateSemaphore, create_semaphore);
    OVERRIDE(vkDestroySemaphore, destroy_semaphore);
    OVERRIDE(vkWaitForFences, wait_fences);
    OVERRIDE(vkGetFenceStatus, fence_status);
    OVERRIDE(vkWaitSemaphores, wait_semaphores);
    OVERRIDE(vkGetSemaphoreCounterValue, semaphore_value);
    OVERRIDE(vkSignalSemaphore, signal_semaphore);
    OVERRIDE(vkDeviceWaitIdle, device_wait_idle);
  }
  OVERRIDE(vkAllocateCommandBuffers, allocate_commands);
  OVERRIDE(vkFreeCommandBuffers, free_commands);
  OVERRIDE(vkResetCommandPool, reset_pool);
  OVERRIDE(vkTrimCommandPool, trim_pool);
  OVERRIDE(vkDestroyCommandPool, destroy_pool);
  OVERRIDE(vkCreateCommandPool, create_pool);
#undef OVERRIDE
  if (auto f = wsi_override(name)) return f;
  return next_proc(Layer::dispatch, name);
}
bool command_allowed(const Command& c) {
  if (c.version) return true;
  std::string_view list = c.extensions;
  while (!list.empty()) {
    auto end = list.find(',');
    if (extension_translatable(list.substr(0, end))) return true;
    if (end == list.npos) break;
    list.remove_prefix(end + 1);
  }
  return false;
}
bool enabled_command(const Command& c, const Device& device) {
  if (c.version && c.version <= device.version) return true;
  auto list = c.extensions;
  while (!list.empty()) {
    auto end = list.find(',');
    if (device.extensions.contains(list.substr(0, end))) return true;
    if (end == list.npos) break;
    list.remove_prefix(end + 1);
  }
  return false;
}
GuestAddr thunk(const Command& c) {
  std::lock_guard lock(state_lock);
  auto& addr = thunks[std::string(c.name)];
  if (!addr) {
    const auto execution = process_config().isa() == GuestIsa::a64 && !reference_forwarding
        ? c.execution : hle::Execution::RunLoop;
    addr = hle::make_thunk("libvulkan.so", c.name, c.handler, nullptr, execution,
                           execution == hle::Execution::RunLoop ? nullptr : c.try_handler);
  }
  return addr;
}
}  // namespace
bool emulated_density_maps(VkPhysicalDevice physical) {
  static const bool requested = [] {
    // A guest offered XR foveation on a device without density maps is in a state
    // Quest never presents (Deadpool rebuilt its eye framebuffers every frame).
    const auto* value = venture::environment::getenv("VENTURE_VULKAN_QUEST_DENSITY_MAPS");
    return !value || std::string_view(value) != "0";
  }();
  if (!requested || !physical) return false;
  static std::mutex cache_mutex;
  static std::map<VkPhysicalDevice, bool> cache;
  std::lock_guard lock(cache_mutex);
  if (auto it = cache.find(physical); it != cache.end()) return it->second;
  auto f = host<PFN_vkEnumerateDeviceExtensionProperties>(physical, "vkEnumerateDeviceExtensionProperties");
  if (!f) return cache[physical] = false;  // a host that cannot list extensions is offered nothing extra
  const auto values = extensions([&](auto n, auto p) { return f(physical, nullptr, n, p); });
  const bool emulated = std::ranges::none_of(values, [](const auto& e) {
    return std::string_view(e.extensionName) == VK_EXT_FRAGMENT_DENSITY_MAP_EXTENSION_NAME;
  });
  if (emulated) log::info("vulkan", "Guests are offered fragment density maps as on Quest; the host renders at full rate");
  return cache[physical] = emulated;
}
ForwardingSample forwarding_sample() {
  ForwardingSample result{layer_procedure_generation.load(std::memory_order_acquire), {}, 0, 0, {}};
  for (std::size_t i = 0; i < lifetime_events.size(); ++i)
    result.lifetime_events[i] = lifetime_events[i].load(std::memory_order_relaxed);
  if (!forwarding_census_enabled) return result;
  auto& registry = forwarding_registry();
  std::lock_guard lock(registry.mutex);
  result.dropped_census_threads = registry.dropped_threads.load(std::memory_order_relaxed);
  for (auto& thread : registry.threads) {
    // Never wait for a recording thread. If an update overlaps the snapshot,
    // retain its previous baseline and collect that thread at the next frame.
    const auto sequence = thread->sequence.load(std::memory_order_acquire);
    if (sequence & 1) { ++result.busy_census_threads; continue; }
    const auto first = result.calls.size();
    for (std::size_t slot = 0; slot < std::size(commands); ++slot) {
      const auto calls = thread->calls[slot].load(std::memory_order_relaxed);
      const auto in_jit = thread->in_jit[slot].load(std::memory_order_relaxed);
      auto& previous = thread->previous[slot];
      thread->next[slot] = {calls, in_jit};
      if (calls != previous[0] || in_jit != previous[1]) {
        result.calls.push_back({thread->tid, commands[slot].name.data(), calls - previous[0], in_jit - previous[1]});
      }
    }
    std::atomic_thread_fence(std::memory_order_acquire);
    if (thread->sequence.load(std::memory_order_relaxed) != sequence) {
      result.calls.resize(first);
      ++result.busy_census_threads;
    } else thread->previous.swap(thread->next);
  }
  return result;
}
// The render layers observe the guest's calls and forward them unchanged, so
// they sit above the layers that translate them.
constexpr std::pair<Layer, PFN_vkVoidFunction (*)(std::string_view)> layer_chain[] = {
    {Layer::breadcrumbs, breadcrumb_proc},
    {Layer::render_check, render_check_proc},
    {Layer::render_area, render_area_proc},
    {Layer::multiview, multiview_proc},
    {Layer::hardware_buffer, hardware_buffer_proc},
    {Layer::sync_fd, sync_fd_proc},
    {Layer::external, external_proc},
    {Layer::wsi, wsi_proc},
    {Layer::shader, shader_proc},
    {Layer::texture, texture_proc},
    {Layer::pipeline, gfx::pipeline_proc},
    {Layer::deferred_free, deferred_free_proc},
};
// Layer functions that run in the JIT: host bookkeeping under a short lock
// (never across a GPU wait) around the next call, no guest callbacks, no
// blocking waits. Failures throw and are reported by the outer boundary
// without replay, as for the native driver call.
constexpr std::pair<Layer, std::string_view> jit_layer_functions[] = {
    {Layer::render_area, "vkCmdBindPipeline"}, {Layer::render_area, "vkCmdSetScissor"},
    {Layer::texture, "vkCmdBindPipeline"},     {Layer::texture, "vkCmdBindDescriptorSets"},
    {Layer::texture, "vkCmdPushConstants"},    {Layer::texture, "vkUpdateDescriptorSets"},
};
PFN_vkVoidFunction next_proc(Layer self, std::string_view name) {
  for (const auto& [layer, proc] : layer_chain)
    if (layer > self)
      if (auto f = proc(name)) return f;
  return nullptr;
}
PFN_vkVoidFunction jit_dispatch_proc(Layer self, std::string_view name) {
  const auto* command = find_command(name);
  if (!command) return nullptr;
  name = command->canonical;
  // Overrides that are not layer functions (instance/device/WSI entry points) stay outside.
  const auto top = dispatch_proc(self, name);
  if (!top || top != next_proc(Layer::dispatch, name)) return nullptr;
  for (const auto& [layer, proc] : layer_chain)
    if (proc(name) && std::find(std::begin(jit_layer_functions), std::end(jit_layer_functions),
                                std::pair{layer, name}) == std::end(jit_layer_functions))
      return nullptr;
  return top;
}

bool extension_translatable(std::string_view name) {
  if (emulated_wsi_extension(name)) return true;
  if (host_wsi_extension(name)) return false;
  if (external_extension(name) || sync_fd_extension(name) || hardware_buffer_extension(name)) return true;
  // r3b report: Gorilla/Epic probe AHB, QCOM and density maps. Advertising
  // unsupported Android/Adreno paths makes Unity choose unusable render paths.
  if (name.starts_with("VK_QCOM_") || name.starts_with("VK_ANDROID_") || name == "VK_GOOGLE_display_timing" ||
      name == "VK_EXT_device_memory_report" || name == "VK_LUNARG_direct_driver_loading" ||
      name == "VK_EXT_external_memory_host" || name == "VK_NV_cuda_kernel_launch" ||
      name.starts_with("VK_NVX_") || name == "VK_EXT_descriptor_buffer" || name == "VK_EXT_descriptor_heap" ||
      name == "VK_EXT_shader_object" || name == "VK_EXT_host_image_copy" ||
      name == "VK_EXT_queue_family_foreign" || name.ends_with("_fd") || name.find("_win32") != name.npos ||
      name == "VK_EXT_external_memory_dma_buf" || name == "VK_EXT_image_drm_format_modifier")
    return false;
  return std::binary_search(std::begin(registry_extensions), std::end(registry_extensions), name);
}
void adopt_instance(VkInstance instance, const VkInstanceCreateInfo& info, bool allocation_callbacks) {
  Instance record;
  record.guest_callbacks = allocation_callbacks;
  record.version = info.pApplicationInfo && info.pApplicationInfo->apiVersion
                       ? info.pApplicationInfo->apiVersion
                       : VK_API_VERSION_1_0;
  for (std::uint32_t i = 0; i < info.enabledExtensionCount; ++i)
    record.extensions.emplace(info.ppEnabledExtensionNames[i]);
  record.guest_callbacks |= record.extensions.contains(VK_EXT_DEBUG_UTILS_EXTENSION_NAME) ||
                            record.extensions.contains(VK_EXT_DEBUG_REPORT_EXTENSION_NAME);
  std::lock_guard lock(state_lock);
  instances[instance] = std::move(record);
  invalidate_layer_procedures();
}
void adopt_device(VkPhysicalDevice physical, VkDevice device, const VkDeviceCreateInfo& info, bool allocation_callbacks,
                  const std::map<std::uint32_t, std::uint32_t>& shared) {
  Device record;
  record.shared_queues = shared;
  record.guest_callbacks = allocation_callbacks;
  record.physical = physical;
  {
    std::lock_guard lock(state_lock);
    auto it = physicals.find(physical);
    record.instance = it != physicals.end() ? it->second : gfx::gpu_device().instance;
    auto ii = instances.find(record.instance);
    record.version = ii != instances.end() ? ii->second.version : gfx::gpu_device().api_version;
    record.guest_callbacks |= ii == instances.end() || ii->second.guest_callbacks;
  }
  for (std::uint32_t i = 0; i < info.enabledExtensionCount; ++i)
    record.extensions.emplace(info.ppEnabledExtensionNames[i]);
  if (record.extensions.contains("VK_EXT_fragment_density_map") && !emulated_density_maps(physical))
    for (auto* node = static_cast<const VkBaseInStructure*>(info.pNext); node; node = node->pNext)
      if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_FEATURES_EXT)
        record.density_maps = reinterpret_cast<const VkPhysicalDeviceFragmentDensityMapFeaturesEXT*>(node)->fragmentDensityMap;
  record.procedure_loader = loader();
  auto gdpa = record.procedure_loader ? reinterpret_cast<PFN_vkGetDeviceProcAddr>(
      record.procedure_loader(record.instance, "vkGetDeviceProcAddr")) : nullptr;
  if (gdpa) {
    try {
      auto procedures = std::make_shared<std::vector<PFN_vkVoidFunction>>(std::size(commands));
      for (const auto& command : commands)
        if (device_command(command))
          (*procedures)[command_slot(&command)] = gdpa(device, command.name.data());
      record.procedures = std::move(procedures);
    } catch (const std::bad_alloc&) {
      // Optional acceleration must not turn a successfully created native
      // device into a guest allocation failure; retain ordinary resolution.
      record.procedures.reset();
    }
  }
  {
    std::lock_guard lock(state_lock);
    devices[device] = std::move(record);
    invalidate_layer_procedures();
  }
  // Device idle synchronizes even queues the application has not retrieved.
  for (std::uint32_t i = 0; i < info.queueCreateInfoCount; ++i) {
    const auto& requested = info.pQueueCreateInfos[i];
    const auto found = shared.find(requested.queueFamilyIndex);
    const auto count = found == shared.end() ? requested.queueCount : found->second;
    for (std::uint32_t index = 0; index < count; ++index) {
      VkQueue queue{};
      if (requested.flags) {
        VkDeviceQueueInfo2 query{VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2, nullptr,
            requested.flags, requested.queueFamilyIndex, index};
        host<PFN_vkGetDeviceQueue2>(device, "vkGetDeviceQueue2")(device, &query, &queue);
      } else {
        host<PFN_vkGetDeviceQueue>(device, "vkGetDeviceQueue")(
            device, requested.queueFamilyIndex, index, &queue);
      }
      gfx::register_queue(device, queue);
      std::lock_guard lock(state_lock);
      children[h2g(queue)] = device;
      const auto& d = devices.at(device);
      queues[queue] = {d.instance, d.physical, device, requested.queueFamilyIndex,
          index, gfx::queue_mutex(queue)};
    }
  }
}
std::uint32_t host_queue_index(VkDevice device, std::uint32_t family, std::uint32_t index) {
  std::lock_guard lock(state_lock);
  const auto d = devices.find(device);
  if (d == devices.end()) return index;
  const auto f = d->second.shared_queues.find(family);
  return f == d->second.shared_queues.end() ? index : index % f->second;
}
void adopt_physical_device(VkInstance instance, VkPhysicalDevice physical) {
  std::lock_guard lock(state_lock);
  physicals[physical] = instance;
}
VkPhysicalDevice shared_physical_device_in(VkInstance instance) {
  if (!instance || !gfx::gpu_device().physical_device) return VK_NULL_HANDLE;
  std::uint32_t count = 1;
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  // Filtered to the shared GPU; more than one match is refused there.
  if (enumerate_physicals(instance, &count, &physical) != VK_SUCCESS || count != 1) return VK_NULL_HANDLE;
  return physical;
}
std::optional<QueueBinding> queue_binding(VkQueue queue) {
  std::lock_guard lock(state_lock);
  if (auto it = queues.find(queue); it != queues.end()) return it->second;
  return std::nullopt;
}
// Logical identity is separate from native queue ownership. Guest callers
// synchronize per facade; actual driver calls retain the physical queue mutex.
VkQueue native_queue(VkQueue queue) {
  if (!queue_facades_active.load(std::memory_order_acquire)) return queue;
  std::lock_guard lock(state_lock);
  if (auto found = queue_facades.find(queue); found != queue_facades.end()) return found->second->native;
  return queue;
}
namespace {
std::shared_ptr<std::recursive_mutex> guest_queue_mutex(VkQueue queue) {
  if (queue_facades_active.load(std::memory_order_acquire)) {
    std::lock_guard lock(state_lock);
    if (auto found = queue_facades.find(queue); found != queue_facades.end()) return found->second->caller_mutex;
  }
  return gfx::queue_mutex(queue);
}
}  // namespace
VkResult native_queue_work(VkQueue queue, const std::function<VkResult(VkQueue)>& operation) {
  auto facade = queue_facade(queue);
  if (!facade) return operation(queue);
  auto result = facade->deferred->wait_submitted(facade->deferred_index);
  if (result != VK_SUCCESS) return result;
  return facade->backend->submit(facade->backend_index, operation);
}
VkResult native_queue_work(VkQueue queue, std::span<const VkSemaphore> waits,
    std::span<const VkSemaphore> signals, const std::function<VkResult(VkQueue)>& operation,
    const bool* waits_issued) {
  auto facade = queue_facade(queue);
  return facade ? facade->deferred->native_work(facade->deferred_index, waits, signals, operation, waits_issued)
                : operation(queue);
}
VkResult external_semaphore_signal(VkDevice device, VkSemaphore semaphore) {
  auto dispatcher = deferred_device(device);
  return dispatcher ? dispatcher->external_signal(semaphore) : VK_SUCCESS;
}
VkResult external_queue_work(VkDevice device, VkQueue queue, std::span<const VkSemaphore> waits,
    std::span<const VkSemaphore> signals, const std::function<VkResult(VkQueue)>& operation) {
  std::shared_ptr<QueueFacade> facade;
  if (queue_facades_active.load(std::memory_order_acquire)) {
    std::lock_guard lock(state_lock);
    for (const auto& [handle, candidate] : queue_facades)
      if (candidate->device == device && candidate->native == queue) { facade = candidate; break; }
  }
  return facade ? facade->deferred->native_work(facade->deferred_index, waits, signals, operation)
                : operation(queue);
}
VkResult flush_queue_submissions(VkQueue queue) {
  auto facade = queue_facade(queue);
  if (!facade) return VK_SUCCESS;
  // A guest may host-signal a timeline only after xrEndFrame returns. Avoid
  // trapping the compositor on that future host call; native waits remain.
  const auto result = facade->deferred->wait_submitted(facade->deferred_index, 100'000'000);
  if (result == VK_TIMEOUT) {
    log::warn("vulkan", "Image handoff still has deferred queue work after 100 ms");
    return VK_SUCCESS;
  }
  return result;
}
VkResult flush_queue_submissions(VkDevice device, std::uint32_t family, std::uint32_t index) {
  if (!queue_facades_active.load(std::memory_order_acquire)) return VK_SUCCESS;
  VkQueue queue{};
  {
    std::lock_guard lock(state_lock);
    if (auto d = facade_families.find(device); d != facade_families.end())
      if (auto f = d->second.find({family, 0}); f != d->second.end() && index < f->second.size())
        queue = f->second[index];
  }
  return flush_queue_submissions(queue);
}
namespace {
bool facade_queue(VkDevice device, std::uint32_t family, std::uint32_t index,
    VkDeviceQueueCreateFlags flags, VkQueue* out) {
  if (!queue_facades_active.load(std::memory_order_acquire)) return false;
  std::lock_guard lock(state_lock);
  auto d = facade_families.find(device);
  if (d == facade_families.end()) return false;
  auto f = d->second.find({family, flags});
  if (f == d->second.end()) return false;
  if (out) *out = index < f->second.size() ? f->second[index] : VK_NULL_HANDLE;
  return true;
}
void install_queue_facades(VkDevice device, const VkDeviceCreateInfo& info,
    const std::map<std::uint32_t, std::uint32_t>& shared) {
  if (shared.empty()) return;
  auto vk = [&](const char* name) { return cleanup_device_proc(device, name); };
  SerializedQueues::Driver driver{
      reinterpret_cast<PFN_vkCreateFence>(vk("vkCreateFence")),
      reinterpret_cast<PFN_vkDestroyFence>(vk("vkDestroyFence")),
      reinterpret_cast<PFN_vkResetFences>(vk("vkResetFences")),
      reinterpret_cast<PFN_vkGetFenceStatus>(vk("vkGetFenceStatus")),
      reinterpret_cast<PFN_vkWaitForFences>(vk("vkWaitForFences")),
      reinterpret_cast<PFN_vkQueueSubmit>(vk("vkQueueSubmit")),
      reinterpret_cast<PFN_vkQueueWaitIdle>(vk("vkQueueWaitIdle"))};
  std::map<VkQueue, std::shared_ptr<SerializedQueues>> backends;
  std::vector<std::shared_ptr<QueueFacade>> facades;
  std::vector<DeferredQueueSubmissions::Binding> bindings;
  std::set<VkQueue> timed;
  // Track every guest producer on this device, including unshared families:
  // their signals can release waits retained on a shared family.
  for (std::uint32_t n = 0; n < info.queueCreateInfoCount; ++n) {
    const auto& requested = info.pQueueCreateInfos[n];
    // Shared devices reject protected queues before installation; flags are
    // zero here, so vkGetDeviceQueue is sufficient for every native queue.
    for (std::uint32_t index = 0; index < requested.queueCount; ++index) {
      VkQueue native{};
      host<PFN_vkGetDeviceQueue>(device, "vkGetDeviceQueue")(
          device, requested.queueFamilyIndex, host_queue_index(device, requested.queueFamilyIndex, index), &native);
      if (timed.insert(native).second) timing_queue(device, native, requested.queueFamilyIndex);
      auto& backend = backends[native];
      if (!backend) backend = std::make_shared<SerializedQueues>(device, native,
          requested.queueCount, gfx::queue_mutex(native), driver, false);
      auto facade = std::make_shared<QueueFacade>();
      facade->native = native; facade->device = device;
      facade->backend = backend; facade->backend_index = index;
      facade->deferred_index = static_cast<std::uint32_t>(bindings.size());
      bindings.push_back({backend, index});
      const auto handle = reinterpret_cast<VkQueue>(facade.get());
      // Physical exclusion belongs to the backend; caller exclusion stays on
      // this existing facade, never in the native DeviceQueueLock registry.
      {
        std::lock_guard lock(state_lock);
        children[h2g(handle)] = device;
        queues[handle] = queues.at(native);
        queue_facades.emplace(handle, facade);
        facade_families[device][{requested.queueFamilyIndex, requested.flags}].push_back(handle);
      }
      facades.push_back(std::move(facade));
    }
  }
  auto counter = reinterpret_cast<PFN_vkGetSemaphoreCounterValue>(vk("vkGetSemaphoreCounterValue"));
  if (!counter) counter = reinterpret_cast<PFN_vkGetSemaphoreCounterValue>(vk("vkGetSemaphoreCounterValueKHR"));
  if (!counter) counter = +[](VkDevice, VkSemaphore, std::uint64_t*) -> VkResult {
    return VK_ERROR_FEATURE_NOT_PRESENT;
  };
  auto dispatcher = std::make_shared<DeferredQueueSubmissions>(device, std::move(bindings),
      queue_submit, counter, queue_submit2);
  for (auto& facade : facades) facade->deferred = dispatcher;
  {
    std::lock_guard lock(state_lock);
    deferred_devices.emplace(device, dispatcher);
    queue_facades_active.store(true, std::memory_order_release);
    invalidate_layer_procedures();
  }
  log::info("vulkan", "Deferred guest queue facades: {}", facades.size());
}
void forget_queue_facades(VkDevice device) {
  std::lock_guard lock(state_lock);
  std::erase_if(queue_facades, [&](const auto& row) { return row.second->device == device; });
  facade_families.erase(device);
  deferred_devices.erase(device);
  invalidate_layer_procedures();
  queue_facades_active.store(!queue_facades.empty(), std::memory_order_release);
}
}  // namespace
// adopt_device registers every created queue, so one exists before any vkGetDeviceQueue.
std::optional<InternalQueue> internal_queue(VkDevice device) {
  std::lock_guard lock(state_lock);
  for (auto& [queue, binding] : queues)
    if (binding.device == device && !queue_facades.contains(queue)) return InternalQueue{queue, binding.submission_mutex};
  return std::nullopt;
}
std::vector<std::uint32_t> device_queue_families(VkDevice device) {
  std::lock_guard lock(state_lock);
  std::set<std::uint32_t> families;
  for (const auto& [queue, binding] : queues)
    if (binding.device == device) families.insert(binding.family);
  return {families.begin(), families.end()};
}
GuestAddr instance_proc(VkInstance instance, const char* name) {
  if (!name) return 0;
  auto missing = [&](const char* reason) -> GuestAddr {
    if (venture::environment::getenv("VENTURE_TRACE_VULKAN"))
      log::info("vulkan", "vkGetInstanceProcAddr returned null: {} ({})", name, reason);
    return 0;
  };
  auto* c = find_command(name);
  if (!c || !command_allowed(*c)) return missing("unavailable command");
  if (!instance && c->scope != Scope::global) return missing("requires an instance");
  if (instance && c->name == "vkCreateAndroidSurfaceKHR") {
    std::lock_guard lock(state_lock);
    auto it = instances.find(instance);
    if (it == instances.end() || !it->second.extensions.contains("VK_KHR_android_surface"))
      return missing("Android surface extension not enabled");
  } else if (!override_proc(name) && (!loader() || !loader()(instance, name)))
    return missing("host resolver");
  return thunk(*c);
}
GuestAddr device_proc(VkDevice device, const char* name) {
  if (!name) return 0;
  auto missing = [&](const char* reason) -> GuestAddr {
    if (venture::environment::getenv("VENTURE_TRACE_VULKAN"))
      log::info("vulkan", "vkGetDeviceProcAddr returned null: {} ({})", name, reason);
    return 0;
  };
  if (!device) return missing("requires a device");
  auto* c = find_command(name);
  if (!c || !command_allowed(*c) ||
      (c->scope != Scope::device && c->scope != Scope::queue && c->scope != Scope::command_buffer))
    return missing("unavailable command or device scope");
  {
    std::lock_guard lock(state_lock);
    auto it = devices.find(device);
    if (it != devices.end() && !enabled_command(*c, it->second)) return missing("capability not enabled");
  }
  if (!host_proc(Scope::device, h2g(device), name) && !override_proc(name)) return missing("host resolver");
  return thunk(*c);
}
std::shared_ptr<HleLibrary> create_library() {
  auto library = std::make_shared<HleLibrary>("libvulkan.so", std::vector<std::string>{"libvulkan.so.1"});
  // Register translated entry points without creating a GPU device. The loader
  // exports dispatch stubs; vkGet*ProcAddr and each call validate host support.
  for (const auto& c : commands) {
    if (!command_allowed(c)) continue;
    library->add_function(c.name, thunk(c));
  }
  return library;
}
void register_library() {
  Linker::get().register_hle_library(create_library());
  register_hardware_buffer_allocator();
}
hle::Handler command_handler(std::string_view name) {
  auto* c = find_command(name);
  return c && command_allowed(*c) ? c->handler : nullptr;
}
std::size_t command_count() { return std::size(commands); }

void add_device_release_hook(void (*hook)(VkDevice)) {
  std::lock_guard lock(release_hooks_lock);
  if (std::find(release_hooks.begin(), release_hooks.end(), hook) == release_hooks.end())
    release_hooks.push_back(hook);
}
// Layer functions resolve their next command on every forwarded call. Resolved
// pointers are kept per thread and dropped with the dispatch snapshots, so a
// warm call takes no state_lock. The pointer only selects the entry; the
// retained text decides a hit, so a reused name buffer resolves its contents.
struct RawProcCache {
  VkDevice device{};
  const char* name{};
  std::string text;
  std::uint64_t generation{};
  PFN_vkGetInstanceProcAddr loader{};
  PFN_vkVoidFunction proc{};
};
PFN_vkVoidFunction raw_device_proc_uncached(VkDevice device, const char* name);
PFN_vkVoidFunction raw_device_proc(VkDevice device, const char* name) {
  thread_local std::array<RawProcCache, 32> cache;
  const auto key = reinterpret_cast<std::uintptr_t>(name) ^ reinterpret_cast<std::uintptr_t>(device);
  auto& entry = cache[((key >> 3) ^ (key >> 11)) % cache.size()];
  const auto generation = layer_procedure_generation.load(std::memory_order_acquire);
  const auto gipa = loader();
  if (entry.device == device && entry.name == name && entry.generation == generation && entry.loader == gipa &&
      entry.text == name)
    return entry.proc;
  auto p = raw_device_proc_uncached(device, name);
  if (p) {
    entry.device = device;
    entry.name = name;
    entry.text = name;
    entry.generation = generation;
    entry.loader = gipa;
    entry.proc = p;
  }
  return p;
}
PFN_vkVoidFunction raw_device_proc_uncached(VkDevice device, const char* name) {
  auto p = observed_host_proc(Scope::device, h2g(device), name);
  if (p) return p;
  for (const auto& c : commands)
    if (c.canonical == name && c.name != name) {
      p = observed_host_proc(Scope::device, h2g(device), c.name.data());
      if (p) return p;
    }
  return nullptr;
}
PFN_vkVoidFunction cleanup_device_proc(VkDevice device, const char* name) {
  return host_proc(Scope::device, h2g(device), name);
}
PFN_vkVoidFunction raw_physical_proc(VkPhysicalDevice physical, const char* name) {
  // Pointer availability does not permit a core 1.1 query on a 1.0 instance.
  // Our decode and resolve layers must use the enabled KHR alias there too.
  constexpr std::array<std::pair<const char*, const char*>, 7> properties2{{
      {"vkGetPhysicalDeviceFeatures2", "vkGetPhysicalDeviceFeatures2KHR"},
      {"vkGetPhysicalDeviceProperties2", "vkGetPhysicalDeviceProperties2KHR"},
      {"vkGetPhysicalDeviceFormatProperties2", "vkGetPhysicalDeviceFormatProperties2KHR"},
      {"vkGetPhysicalDeviceImageFormatProperties2", "vkGetPhysicalDeviceImageFormatProperties2KHR"},
      {"vkGetPhysicalDeviceQueueFamilyProperties2", "vkGetPhysicalDeviceQueueFamilyProperties2KHR"},
      {"vkGetPhysicalDeviceMemoryProperties2", "vkGetPhysicalDeviceMemoryProperties2KHR"},
      {"vkGetPhysicalDeviceSparseImageFormatProperties2", "vkGetPhysicalDeviceSparseImageFormatProperties2KHR"},
  }};
  for (auto [core, khr] : properties2) {
    if (std::string_view(name) != core && std::string_view(name) != khr) continue;
    std::uint32_t version = gfx::gpu_device().api_version;
    bool extension = false;
    {
      std::lock_guard lock(state_lock);
      if (auto p = physicals.find(physical); p != physicals.end())
        if (auto i = instances.find(p->second); i != instances.end()) {
          version = i->second.version;
          extension = i->second.host_properties2 || i->second.extensions.contains(
              VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
        }
    }
    if (version < VK_API_VERSION_1_1 && !extension) return nullptr;
    return observed_host_proc(Scope::physical, h2g(physical),
                              version >= VK_API_VERSION_1_1 ? core : khr);
  }
  auto p = observed_host_proc(Scope::physical, h2g(physical), name);
  if (p) return p;
  for (const auto& c : commands)
    if (c.canonical == name && c.name != name) {
      p = observed_host_proc(Scope::physical, h2g(physical), c.name.data());
      if (p) return p;
    }
  return nullptr;
}
VkDevice command_device(VkCommandBuffer command) {
  // Every children erase bumps the generation, so a warm entry is never stale.
  struct Entry {
    VkCommandBuffer command{};
    std::uint64_t generation{};
    VkDevice device{};
  };
  thread_local std::array<Entry, 8> cache;
  const auto key = reinterpret_cast<std::uintptr_t>(command);
  auto& entry = cache[((key >> 4) ^ (key >> 16)) % cache.size()];
  const auto generation = layer_procedure_generation.load(std::memory_order_acquire);
  if (entry.command == command && entry.generation == generation) return entry.device;
  std::lock_guard lock(state_lock);
  auto it = children.find(h2g(command));
  if (it == children.end()) throw std::runtime_error("untracked Vulkan command buffer");
  entry = {command, generation, it->second};
  return it->second;
}
VkCommandPool command_pool(VkCommandBuffer command) {
  std::lock_guard lock(state_lock);
  auto it = command_pools.find(command);
  if (it == command_pools.end()) throw std::runtime_error("untracked Vulkan command buffer");
  return it->second;
}
VkPhysicalDevice device_physical(VkDevice device) {
  std::lock_guard lock(state_lock);
  auto it = devices.find(device);
  if (it == devices.end()) throw std::runtime_error("untracked Vulkan device");
  return it->second.physical;
}
bool device_density_maps_enabled(VkDevice device) {
  std::lock_guard lock(state_lock);
  auto it = devices.find(device);
  return it != devices.end() && it->second.density_maps;
}
VkInstance device_instance(VkDevice device) {
  std::lock_guard lock(state_lock);
  auto it = devices.find(device);
  if (it == devices.end()) throw std::runtime_error("untracked Vulkan device");
  return it->second.instance;
}
std::uint32_t device_api_version(VkDevice device) {
  std::lock_guard lock(state_lock);
  auto it = devices.find(device);
  if (it == devices.end()) throw std::runtime_error("untracked Vulkan device");
  return it->second.version;
}
bool command_supports_compute(VkCommandBuffer command) {
  std::lock_guard lock(state_lock);
  auto it = command_pools.find(command);
  if (it == command_pools.end()) return false;
  auto flags = pool_flags.find(it->second);
  return flags != pool_flags.end() && (flags->second & VK_QUEUE_COMPUTE_BIT);
}

// XR invokes host ABI functions. Unlike generated guest handlers, these need
// their own translation scope before entering the same creation implementation.
static VkResult VKAPI_CALL xr_create_instance(const VkInstanceCreateInfo* info,
                                              const VkAllocationCallbacks* alloc, VkInstance* out) {
  try {
    if (!info) return VK_ERROR_INITIALIZATION_FAILED;
    Chain chain;
    auto copy = *info;
    copy.pNext = chain.translate(info->pNext);
    if (chain.result() != VK_SUCCESS) return chain.result();
    return create_instance_impl(&copy, translate_allocator(alloc), out, true);
  } catch (const std::bad_alloc&) {
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  } catch (const std::exception& e) {
    log::error("vulkan", "XR Vulkan instance creation: {}", e.what());
    return VK_ERROR_INITIALIZATION_FAILED;
  }
}
static VkResult VKAPI_CALL xr_create_device(VkPhysicalDevice physical, const VkDeviceCreateInfo* info,
                                            const VkAllocationCallbacks* alloc, VkDevice* out) {
  try {
    if (!info) return VK_ERROR_INITIALIZATION_FAILED;
    Chain chain;
    auto copy = *info;
    copy.pNext = chain.translate(info->pNext);
    if (chain.result() != VK_SUCCESS) return chain.result();
    return create_device_impl(physical, &copy, translate_allocator(alloc), out, true);
  } catch (const std::bad_alloc&) {
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  } catch (const std::exception& e) {
    log::error("vulkan", "XR Vulkan device creation: {}", e.what());
    return VK_ERROR_INITIALIZATION_FAILED;
  }
}
PFN_vkVoidFunction VKAPI_CALL xr_host_proc(VkInstance instance, const char* name) {
  if (!name) return nullptr;
  if (std::string_view(name) == "vkCreateInstance")
    return reinterpret_cast<PFN_vkVoidFunction>(xr_create_instance);
  if (std::string_view(name) == "vkCreateDevice")
    return reinterpret_cast<PFN_vkVoidFunction>(xr_create_device);
  if (std::string_view(name) == "vkEnumeratePhysicalDevices")
    return reinterpret_cast<PFN_vkVoidFunction>(enumerate_physicals);
  return loader() ? loader()(instance, name) : nullptr;
}

thread_local XrResolverScope* XrResolverScope::current_ = nullptr;
XrResolverScope::XrResolverScope(GuestAddr resolver) : previous_(current_), resolver_(resolver) {
  current_ = this;
}
XrResolverScope::~XrResolverScope() { current_ = previous_; }
bool XrResolverScope::active() { return current_ != nullptr; }
PFN_vkVoidFunction VKAPI_CALL XrResolverScope::proc(VkInstance instance, const char* name) {
  if (!name) return nullptr;
  // OpenXR may retain this resolver for the instance lifetime and query
  // platform commands that are intentionally absent from the Android table.
  // Only creation needs the guest application's synchronous enable2 hooks.
  if (!current_ || (std::string_view(name) != "vkCreateInstance" &&
                    std::string_view(name) != "vkCreateDevice"))
    return xr_host_proc(instance, name);
  try {
    auto fn = GuestThread::adopt_current().call(current_->resolver_, {h2g(instance), h2g(name)});
    if (!fn) return nullptr;
    if (std::string_view(name) == "vkCreateInstance") {
      current_->instance_create_ = fn;
      return reinterpret_cast<PFN_vkVoidFunction>(create_instance_callback);
    }
    if (std::string_view(name) == "vkCreateDevice") {
      current_->device_create_ = fn;
      return reinterpret_cast<PFN_vkVoidFunction>(create_device_callback);
    }
  } catch (const std::exception& e) {
    log::error("vulkan", "XR Vulkan resolver callback failed: {}", e.what());
  }
  return nullptr;
}
VkResult VKAPI_CALL XrResolverScope::create_instance_callback(const VkInstanceCreateInfo* info,
    const VkAllocationCallbacks* alloc, VkInstance* out) {
  if (!current_ || !current_->instance_create_) return VK_ERROR_INITIALIZATION_FAILED;
  try {
    return static_cast<VkResult>(GuestThread::adopt_current().call(
        current_->instance_create_, {h2g(info), h2g(alloc), h2g(out)}));
  } catch (const std::exception& e) {
    log::error("vulkan", "XR guest vkCreateInstance callback failed: {}", e.what());
    return VK_ERROR_INITIALIZATION_FAILED;
  }
}
VkResult VKAPI_CALL XrResolverScope::create_device_callback(VkPhysicalDevice physical,
    const VkDeviceCreateInfo* info, const VkAllocationCallbacks* alloc, VkDevice* out) {
  if (!current_ || !current_->device_create_) return VK_ERROR_INITIALIZATION_FAILED;
  try {
    return static_cast<VkResult>(GuestThread::adopt_current().call(
        current_->device_create_, {h2g(physical), h2g(info), h2g(alloc), h2g(out)}));
  } catch (const std::exception& e) {
    log::error("vulkan", "XR guest vkCreateDevice callback failed: {}", e.what());
    return VK_ERROR_INITIALIZATION_FAILED;
  }
}
void end_app_after_device_loss() {
  // North Star kept submitting after an amdgpu page fault lost its device; the
  // driver's GTT grew to 32 GB in a minute and the host OOM killer took other
  // desktop applications. OS quit already accepts loss in its own wait.
  // Embedded fixtures keep exception control of their simulated losses.
  auto* thread = GuestThread::current();
  if (!thread || os_quit::requested() || !terminal_guest_process_exit()) return;
  log::error("vulkan-fault", "Native Vulkan device lost on guest tid {}; ending the app as a fatal error", thread->tid());
  record_guest_process_failure(1);
  exit_guest_process(1);
}
static std::atomic<bool> quit_device_lost{false};
bool os_quit_device_lost() { return quit_device_lost.load(); }
bool wait_devices_for_os_quit(std::chrono::milliseconds bound) {
  // Guest execution must already be stopped. Snapshot the dispatch pointers;
  // never retain the registry lock while acquiring queue locks or in a driver.
  std::vector<std::pair<VkDevice, PFN_vkDeviceWaitIdle>> pending;
  {
    std::unique_lock lock(state_lock, std::try_to_lock);
    if (!lock.owns_lock()) {
      log::error("launch", "OS quit Vulkan registry is still held by a host call");
      return false;
    }
    for (const auto& [device, record] : devices)
      pending.emplace_back(device, reinterpret_cast<PFN_vkDeviceWaitIdle>(
          host_proc(Scope::device, h2g(device), "vkDeviceWaitIdle")));
    auto shared = gfx::gpu_device().device;
    if (shared && !devices.contains(shared))
      pending.emplace_back(shared, reinterpret_cast<PFN_vkDeviceWaitIdle>(
          host_proc(Scope::device, h2g(shared), "vkDeviceWaitIdle")));
  }
  // Vulkan has no cancellable DeviceWaitIdle. An OS-backed future bounds both
  // queue acquisition and driver waits. On timeout the caller records P1 and
  // ends the worker; it must never destroy a device still used by this helper.
  auto task = std::make_shared<std::packaged_task<bool()>>([pending = std::move(pending)] {
    for (const auto& [device, idle] : pending) {
      if (!idle) {
        log::error("launch", "OS quit Vulkan device={} has no native wait-idle", h2g(device));
        return false;
      }
      if (auto dispatcher = deferred_device(device)) {
        auto result = dispatcher->flush();
        if (result != VK_SUCCESS) return false;
        for (std::uint32_t i = 0; i < dispatcher->queue_count(); ++i)
          if (dispatcher->pending(i)) {
            log::error("launch", "OS quit deferred Vulkan queue {} has unissued work", i);
            return false;
          }
      }
      log::info("launch", "OS quit Vulkan device={} waiting for queue locks and device idle", h2g(device));
      VkResult result;
      {
        gfx::DeviceQueueLock queues(device);
        log::info("launch", "OS quit Vulkan device={} queue locks held; waiting for device idle", h2g(device));
        result = idle(device);
      }
      log::info("launch", "OS quit Vulkan device={} wait-idle result={} device_lost={}",
          h2g(device), int(result), result == VK_ERROR_DEVICE_LOST);
      if (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST) return false;
      if (result == VK_ERROR_DEVICE_LOST) quit_device_lost = true;
      // Device loss is already a terminal native state. Record it, without
      // invoking guest fault callbacks or trying to destroy its child objects.
    }
    return true;
  });
  auto complete = task->get_future();
  std::thread worker([task] { (*task)(); });
  if (complete.wait_for(bound) != std::future_status::ready) {
    worker.detach();
    log::error("launch", "OS quit Vulkan queue/device idle timed out after {} ms (P1)", bound.count());
    return false;
  }
  worker.join();
  try { return complete.get(); }
  catch (const std::exception& error) {
    log::error("launch", "OS quit Vulkan idle exception: {}", error.what());
    return false;
  }
}
void release_os_device_resources(VkDevice device) { release_device_resources(device); }
}  // namespace venture::vulkan

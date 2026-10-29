#pragma once
#include <vector>
#include <functional>
#include <span>
#include <atomic>
#include <mutex>
#include <array>

#include "vulkan.h"

namespace venture::vulkan {
// Optional per-thread call census plus cumulative lifetime invalidations.
// Timing samples turn the cumulative values into aligned per-frame deltas.
struct ForwardingCallSample {
  std::int32_t guest_tid;
  const char* command;
  std::uint64_t calls, in_jit;
};
struct ForwardingSample {
  std::uint64_t layer_generation;
  // allocate commands, free commands, create pool, destroy pool.
  std::array<std::uint64_t, 4> lifetime_events;
  std::uint64_t dropped_census_threads;
  std::uint64_t busy_census_threads = 0;
  std::vector<ForwardingCallSample> calls;
};
ForwardingSample forwarding_sample();
std::optional<std::pair<std::uint32_t, std::uint32_t>> private_texture_queue(
    std::span<const VkQueueFamilyProperties> families,
    std::span<const VkDeviceQueueCreateInfo> requested);
// A queue of the device for runtime-internal submissions, with the submission
// mutex shared by guest calls on that queue. Empty if the device has none.
struct InternalQueue {
  VkQueue queue;
  std::shared_ptr<std::recursive_mutex> mutex;
};
std::optional<InternalQueue> internal_queue(VkDevice device);
std::vector<std::uint32_t> device_queue_families(VkDevice device);
// The private queue has no guest submissions waiting on future guest signals.
void configure_lazy_texture_device(VkDevice device, VkQueue queue, std::uint32_t family,
                                    const VkPhysicalDeviceFeatures& features);
VkResult texture_submit(VkQueue queue, std::uint32_t count, const VkSubmitInfo* submits,
                        VkFence fence, PFN_vkQueueSubmit native);
VkResult texture_submit2(VkQueue queue, std::uint32_t count, const VkSubmitInfo2* submits,
                         VkFence fence, PFN_vkQueueSubmit2 native);
void texture_queue_idle(VkQueue queue);
void texture_queue_uncertain(VkQueue queue);

// VENTURE_VULKAN_ADDRESS_LOG diagnostic (address_log.cpp).
bool address_log_enabled();
void address_log_instance_extensions(std::vector<const char*>& names, const std::vector<VkExtensionProperties>& host);
void address_log_attach(VkInstance instance);
void address_log_detach(VkInstance instance);
bool address_log_device(const std::vector<VkExtensionProperties>& host);

// Android surfaces and swapchains without a host surface (wsi.cpp).
// Emulated extensions are never enabled on the host; host WSI ones stay hidden.
bool emulated_wsi_extension(std::string_view name);
bool host_wsi_extension(std::string_view name);
void wsi_instance_extensions(std::vector<VkExtensionProperties>& values);
void wsi_device_extensions(std::vector<VkExtensionProperties>& values);
// Entry points that replace the host's, and the layer rewriting PRESENT_SRC_KHR.
PFN_vkVoidFunction wsi_override(std::string_view name);
PFN_vkVoidFunction wsi_proc(std::string_view name);
void release_wsi_device(VkDevice device);

// Guest vkFreeMemory waits for the work submitted before it (deferred_free.cpp).
PFN_vkVoidFunction deferred_free_proc(std::string_view name);
// Before each guest submission or sparse bind reaches the driver and after a
// successful one, with the queue held; and after vkQueueWaitIdle.
void deferred_free_submitting(VkQueue queue);
void deferred_free_submitted(VkQueue queue);
void deferred_free_queue_idle(VkQueue queue);
void release_deferred_free_device(VkDevice device);
// Private suballocations cannot be reused while preceding GPU work still runs.
void defer_gpu_release(VkDevice device, std::function<void()> release);
// Tests: frees of the device still waiting for the GPU.
std::size_t deferred_free_pending(VkDevice device);

// Android SYNC_FD semaphore/fence payloads as guest sync-file descriptors.
PFN_vkVoidFunction sync_fd_proc(std::string_view name);
bool sync_fd_extension(std::string_view name);
void sync_fd_extensions(std::vector<VkExtensionProperties>& properties);
VkResult sync_fd_chain(VkBaseOutStructure* structure);
void release_sync_fd_device(VkDevice device);
// Guest semaphore waits in successful queue submissions.
void sync_fd_submitted(VkQueue queue, std::uint32_t count, const VkSubmitInfo* submits);
void sync_fd_submitted(VkQueue queue, std::uint32_t count, const VkSubmitInfo2* submits);
// Tests: records kept for the device's semaphore exports, and host signals
// issued by SYNC_FD semaphore imports (all devices).
struct SyncFdState {
  std::size_t export_records = 0;
  std::uint64_t import_signals = 0;
};
SyncFdState sync_fd_state(VkDevice device);

// VK_ANDROID_external_memory_android_hardware_buffer (hardware_buffer.cpp).
bool hardware_buffers_supported();
bool hardware_buffer_extension(std::string_view name);
void hardware_buffer_extensions(std::vector<VkExtensionProperties>& properties);
// Host extension for a guest one, or nullptr when it is translated instead.
const char* hardware_buffer_host_extension(const char* name);
void hardware_buffer_chain(VkBaseOutStructure* structure);
PFN_vkVoidFunction hardware_buffer_proc(std::string_view name);
void release_hardware_buffer_device(VkDevice device);
// Runs for each guest VkDevice before the host vkDestroyDevice, while it is still
// valid, so other HLE libraries release the objects they created on it.
void add_device_release_hook(void (*hook)(VkDevice));
void register_hardware_buffer_allocator();

PFN_vkVoidFunction raw_device_proc(VkDevice device, const char* name);
// Capture while the device resolver is alive for teardown. Unlike raw_device_proc,
// this bypasses result-observer wrappers, which resolve the driver again at call time.
PFN_vkVoidFunction cleanup_device_proc(VkDevice device, const char* name);
PFN_vkVoidFunction raw_physical_proc(VkPhysicalDevice physical, const char* name);
VkDevice command_device(VkCommandBuffer command);
VkCommandPool command_pool(VkCommandBuffer command);
VkPhysicalDevice device_physical(VkDevice device);
bool device_density_maps_enabled(VkDevice device);
// Quest devices always have VK_EXT_fragment_density_map. On a host without it the
// guest is offered it anyway (VENTURE_VULKAN_QUEST_DENSITY_MAPS=0 opts out); its
// density maps are never read (chain.cpp renders those passes at full rate).
bool emulated_density_maps(VkPhysicalDevice physical);
inline constexpr VkExtent2D emulated_density_texel_min{8, 8}, emulated_density_texel_max{32, 32};
std::uint32_t device_api_version(VkDevice device);
VkInstance device_instance(VkDevice device);
bool command_supports_compute(VkCommandBuffer command);
// The pass-through layers between the guest's dispatch and the host, top first.
// A layer forwards a command it intercepts to the next layer below it that also
// intercepts it, else to the host, so every layer sees every call it hooks.
enum class Layer { dispatch, breadcrumbs, render_check, render_area, multiview, hardware_buffer, sync_fd, external, wsi, shader, texture, pipeline, deferred_free };
PFN_vkVoidFunction shader_proc(std::string_view name);
// Empty means no change. Explicit initializers and nonnumeric types stay intact.
std::vector<std::uint32_t> initialize_shader_locals(std::span<const std::uint32_t> code);
// Declares storage images Format Unknown (adding the WithoutFormat capabilities)
// so loads and stores follow the bound view format, as Adreno does. A type whose
// rewrite would duplicate another declaration keeps its format. Empty: no change.
// Declarations whose SPIR-V Image Format is in `keep` stay as declared.
std::vector<std::uint32_t> formatless_storage_images(std::span<const std::uint32_t> code,
                                                     std::span<const std::uint32_t> keep = {});
// The VkFormat a SPIR-V storage Image Format names, or VK_FORMAT_UNDEFINED.
VkFormat storage_image_format(std::uint32_t spirv_format);
// Guest devices created with shaderStorageImage{Read,Write}WithoutFormat enabled.
bool storage_format_rewrite_enabled();
// `keep`: SPIR-V Image Formats whose VkFormat lacks STORAGE_{READ,WRITE}_WITHOUT_FORMAT
// on this device, so a declaration of that format must not become Unknown.
void set_storage_without_format(VkDevice device, bool enabled, std::vector<std::uint32_t> keep = {});
bool breadcrumbs_enabled();
PFN_vkVoidFunction breadcrumb_proc(std::string_view name);
void prepare_breadcrumb_device(VkDevice device, bool checkpoints);
void release_breadcrumb_device(VkDevice device);
void report_breadcrumbs(VkDevice device, VkQueue queue = VK_NULL_HANDLE);
// The first layer below `self` that intercepts `name`, or nullptr for the host.
// next_proc(Layer::dispatch, name) is the top of the stack.
PFN_vkVoidFunction next_proc(Layer self, std::string_view name);
// The dispatch-level entry for `name` when it is a layer function and every
// layer intercepting `name` is audited for in-JIT forwarding, else nullptr.
PFN_vkVoidFunction jit_dispatch_proc(Layer self, std::string_view name);
// Only layer entry points are cached here. Native procedures retain their
// device/loader-scoped resolver and observation wrappers.
inline std::atomic<std::uint64_t> layer_procedure_generation{1};
inline void invalidate_layer_procedures() {
  layer_procedure_generation.fetch_add(1, std::memory_order_release);
}
template <std::size_t N> struct ProcedureName {
  char value[N];
  constexpr ProcedureName(const char (&name)[N]) {
    for (std::size_t i = 0; i < N; ++i) value[i] = name[i];
  }
};
template <Layer Self, ProcedureName Name, auto Resolve = next_proc>
struct LayerProcedureSlot {
  struct Entry {
    std::uint64_t generation = 0;
    PFN_vkVoidFunction function = nullptr;
  };
  inline static std::mutex mutex;
  inline static Entry shared;
  static Entry& local() { thread_local Entry entry; return entry; }
};
template <Layer Self, ProcedureName Name,
          auto Resolve = next_proc> PFN_vkVoidFunction cached_layer_proc() {
  using Slot = LayerProcedureSlot<Self, Name, Resolve>;
  auto& local = Slot::local();
  const auto generation = layer_procedure_generation.load(std::memory_order_acquire);
  if (local.generation == generation) return local.function;
  std::lock_guard lock(Slot::mutex);
  if (Slot::shared.generation != generation)
    Slot::shared = {generation, Resolve(Self, Name.value)};
  local = Slot::shared;
  return local.function;
}
// In-JIT admission must never initialize a slot or wait for its slow owner.
template <Layer Self, ProcedureName Name, auto Resolve = next_proc>
bool try_cached_layer_proc(PFN_vkVoidFunction& function) {
  const auto& entry = LayerProcedureSlot<Self, Name, Resolve>::local();
  if (entry.generation != layer_procedure_generation.load(std::memory_order_acquire)) return false;
  function = entry.function;
  return true;
}
PFN_vkVoidFunction texture_proc(std::string_view name);
// Keeps rendering inside every attachment: clamps host framebuffers and render
// areas to the smallest attachment and dynamic scissors to the render area.
PFN_vkVoidFunction render_area_proc(std::string_view name);
// Subpass dependencies a render pass lacks for its attachment accesses and
// layout transitions between subpasses (subpass_sync.cpp); empty when the
// guest's own dependencies cover them.
std::vector<VkSubpassDependency> missing_subpass_dependencies(const VkRenderPassCreateInfo& info);
std::vector<VkSubpassDependency2> missing_subpass_dependencies(const VkRenderPassCreateInfo2& info);
// Preserve constant per-view shader values across rasterizer interpolation.
PFN_vkVoidFunction multiview_proc(std::string_view name);
void release_render_commands(VkCommandBuffer command);
// Attachment images created outside the guest's vkCreateImage (XR eye images).
void note_image(VkImage image, const VkImageCreateInfo& info);
void forget_image(VkImage image);
// Tests: the extent recorded for an attachment image, or 0x0.
VkExtent2D attachment_extent(VkImage image);
// Tests: whether resolve sources get SwiftShader's stand-ins regardless of the
// driver; nullopt restores the driver check.
void force_resolve_stand_ins(std::optional<bool> enabled);
// VENTURE_TRACE_VULKAN: render-target bounds checks wrapping the commands above.
PFN_vkVoidFunction render_check_proc(std::string_view name);
void release_texture_device(VkDevice device);
// Start the decoder on a worker at device setup. First use shares its result;
// device release joins it before destroying any Vulkan objects.
void prepare_texture_device(VkDevice device);
VkPipeline wait_texture_pipeline(VkDevice device);
void release_texture_commands(VkCommandBuffer command);
void trim_texture_pool(VkDevice device, VkCommandPool pool);
void texture_extensions(VkPhysicalDevice physical, std::vector<VkExtensionProperties>& extensions);
void guest_texture_features(VkPhysicalDevice physical, VkPhysicalDeviceFeatures& features);
VkResult host_texture_features(VkPhysicalDevice physical, VkPhysicalDeviceFeatures& requested);
// Tests: decode compressed formats even where the host samples them natively
// (SwiftShader has ASTC and ETC2), so suites exercise the emulated path.
void force_texture_emulation(bool forced);
// Tests: 1 stores every eligible decoded texture as BC7 as if GPU memory were full, 0 never, -1 by budget.
void force_texture_compression(int mode);
// Install an idle test queue only when the host has no private texture queue.
void ensure_texture_source_test_queue(VkDevice device, VkQueue queue, std::uint32_t family);
void force_pageable_texture_sources(bool forced);
// Test hooks use the same retirement path, bypassing only its idle deadline.
void trim_texture_sources(VkDevice device);
void wait_texture_sources(VkDevice device);
bool texture_source_gpu_resident(VkImage image);
void set_texture_source_worker_hook(std::function<void()> hook);
// Tests: called after the first chunk while the worker owns the source copy guard.
void set_texture_source_copy_hook(std::function<void()> hook);
std::size_t texture_source_scan_count(VkDevice device);
// Tests: the host images backing an emulated compressed image (TextureStorage
// planes), or none when the handle has no emulated-texture record.
std::vector<VkImage> emulated_texture_planes(VkImage image);
}  // namespace venture::vulkan

#pragma once
#include <chrono>

#include <vulkan/vulkan_core.h>

// Platform extension headers require Vulkan core declarations first.
#include <vulkan/vulkan_android.h>

#include <map>
#include <memory>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>

#include "core/hle.h"

namespace venture {
class HleLibrary;
}

namespace venture::vulkan {

// Guest-owned instances/devices preserve the application's enabled feature set.
// They use the GPU selected by gfx::gpu_device(), identified across instances by
// deviceUUID (VkPhysicalDevice handles are instance-local).
std::shared_ptr<HleLibrary> create_library();
void register_library();
GuestAddr instance_proc(VkInstance instance, const char* name);
GuestAddr device_proc(VkDevice device, const char* name);

// Native resolver for a host broker creating guest-compatible Vulkan objects.
// It translates allocators/pNext callbacks and tracks the resulting objects.
// Guest enable2 requests use XrResolverScope to preserve application hooks.
// Never give this native pointer to guest code.
PFN_vkVoidFunction VKAPI_CALL xr_host_proc(VkInstance instance, const char* name);

// Bridges the application's resolver during a synchronous enable2 creation
// call. Engines may intercept vkCreateDevice to return an existing device;
// bypassing that callback would create a different host device for XR images.
class XrResolverScope {
 public:
  explicit XrResolverScope(GuestAddr resolver);
  ~XrResolverScope();
  XrResolverScope(const XrResolverScope&) = delete;
  XrResolverScope& operator=(const XrResolverScope&) = delete;
  static bool active();
  static PFN_vkVoidFunction VKAPI_CALL proc(VkInstance instance, const char* name);
 private:
  static thread_local XrResolverScope* current_;
  XrResolverScope* previous_;
  GuestAddr resolver_, instance_create_ = 0, device_create_ = 0;
  static VkResult VKAPI_CALL create_instance_callback(const VkInstanceCreateInfo*,
      const VkAllocationCallbacks*, VkInstance*);
  static VkResult VKAPI_CALL create_device_callback(VkPhysicalDevice, const VkDeviceCreateInfo*,
      const VkAllocationCallbacks*, VkDevice*);
};

// For an XR broker that creates objects through its own host resolver. Enabled
// names must be the final list actually used to create the object. No ownership
// transfer: destruction still comes from the creator/guest.
void adopt_instance(VkInstance instance, const VkInstanceCreateInfo& info, bool allocation_callbacks = false);
void adopt_physical_device(VkInstance instance, VkPhysicalDevice physical);
// The GPU of Venture's shared device as seen through another instance, or null
// when it is absent or ambiguous there.
VkPhysicalDevice shared_physical_device_in(VkInstance instance);
// `shared` maps each family whose guest queue count exceeded the host's to
// the host count: guest index i of that family is host queue i % count.
void adopt_device(VkPhysicalDevice physical, VkDevice device, const VkDeviceCreateInfo& info,
                  bool allocation_callbacks = false,
                  const std::map<std::uint32_t, std::uint32_t>& shared = {});
// The host queue index behind a guest (family, index); identity unless the
// family was oversubscribed (see adopt_device).
std::uint32_t host_queue_index(VkDevice device, std::uint32_t family, std::uint32_t index);

// The queue's actual creation indices are required by native XR bindings.
// Handles must have been returned by a translated vkGetDeviceQueue{,2} call.
struct QueueBinding {
  VkInstance instance;
  VkPhysicalDevice physical;
  VkDevice device;
  std::uint32_t family, index;
  std::shared_ptr<std::recursive_mutex> submission_mutex;
};
std::optional<QueueBinding> queue_binding(VkQueue queue);
VkQueue native_queue(VkQueue);
VkResult flush_queue_submissions(VkQueue queue);
VkResult flush_queue_submissions(VkDevice device, std::uint32_t family, std::uint32_t index);
VkResult native_queue_work(VkQueue, const std::function<VkResult(VkQueue)>&);
VkResult native_queue_work(VkQueue, std::span<const VkSemaphore> waits,
    std::span<const VkSemaphore> signals, const std::function<VkResult(VkQueue)>&,
    const bool* waits_issued = nullptr);
VkResult external_semaphore_signal(VkDevice, VkSemaphore);
VkResult external_queue_work(VkDevice, VkQueue, std::span<const VkSemaphore> waits,
    std::span<const VkSemaphore> signals, const std::function<VkResult(VkQueue)>&);

// Registry metadata for dispatch generation and focused ABI tests.
hle::Handler command_handler(std::string_view name);
std::size_t command_count();
bool extension_translatable(std::string_view name);

// Android PackageManager's Vulkan level-zero version, or zero when unavailable.
// The Android 1.1 hardware-buffer/SYNC_FD contract is not implemented.
std::uint32_t android_hardware_version();
// After all guest execution stops, idle every tracked device plus the shared
// host device, taking its queue locks. Includes driver/lock time in one bound.
// Accepts and records device loss. Does not destroy guest devices or children;
// on false the worker must terminate nonzero, without further graphics cleanup.
bool wait_devices_for_os_quit(std::chrono::milliseconds bound);
// A device reported VK_ERROR_DEVICE_LOST while waiting for OS quit; its native objects are left to process exit.
bool os_quit_device_lost();
void release_os_device_resources(VkDevice);
// Called on a native VK_ERROR_DEVICE_LOST after its fault report. On a guest
// thread of an app worker, outside OS quit, this ends the app as a fatal error
// (status 1): a lost device never recovers, and a title that keeps submitting
// makes the host driver retain its allocations without bound. Host threads
// and embedded fixtures only report.
void end_app_after_device_loss();
// Logs the device-local memory in use against the driver's budget (frame
// stall captures: GPU memory past the budget stalls frames).
void log_gpu_memory();

}  // namespace venture::vulkan

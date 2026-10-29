#include "common/log.h"
#include "gfx/angle.h"
#include "gfx/angle_callbacks.h"
#include "gfx/pipeline_trace.h"
#include "gfx/gpu_device_internal.h"
#include "gfx/queue_probe.h"
#include "platform/gfx_library.h"
#include "xr/host_xr.h"
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace venture::gfx {
namespace {
GpuDevice gpu;
std::recursive_mutex creation_lock;
std::vector<std::string> instance_requirements, device_requirements;
std::vector<std::string> instance_enabled, device_enabled;
PFN_vkGetInstanceProcAddr angle_loader = nullptr;
bool unified_image_layouts = false;
PFN_vkGetInstanceProcAddr loader() {
  if (angle_loader) return angle_loader;
  static auto fn =
      reinterpret_cast<PFN_vkGetInstanceProcAddr>(platform::graphics_symbol(
          platform::open_graphics_library(platform::vulkan_library_name()),
          "vkGetInstanceProcAddr"));
  return fn;
}
template <class T>
T vk(VkInstance instance, const char* name) {
  return reinterpret_cast<T>(loader()(instance, name));
}
template <class T>
T xr_proc(const char* name) {
  auto& host = xr::host_xr();
  PFN_xrVoidFunction out = nullptr;
  auto get =
      reinterpret_cast<PFN_xrGetInstanceProcAddr>(host.get_instance_proc_addr);
  if (!get || XR_FAILED(get(host.instance, name, &out)) || !out)
    throw std::runtime_error(std::string("Missing host XR procedure: ") + name);
  return reinterpret_cast<T>(out);
}
std::vector<const char*> union_names(std::uint32_t count,
                                     const char* const* original,
                                     const std::vector<std::string>& extra) {
  std::vector<const char*> result;
  for (std::uint32_t i = 0; i < count; ++i) result.push_back(original[i]);
  for (const auto& s : extra)
    if (std::ranges::none_of(result, [&](const char* p) { return s == p; }))
      result.push_back(s.c_str());
  return result;
}
void remember(std::vector<std::string>& to, std::uint32_t count,
              const char* const* names) {
  to.clear();
  for (std::uint32_t i = 0; i < count; ++i) to.emplace_back(names[i]);
}
// OpenXR enable2 permits wrapping GIPA to observe the runtime's final extension
// union. Do not redirect these actual Vulkan creation calls back through XR.
VkResult VKAPI_CALL record_instance(const VkInstanceCreateInfo* ci,
                                    const VkAllocationCallbacks* a,
                                    VkInstance* out) {
  try {
    std::vector<std::string> names;
    remember(names, ci->enabledExtensionCount, ci->ppEnabledExtensionNames);
    auto r = vk<PFN_vkCreateInstance>(nullptr, "vkCreateInstance")(ci, a, out);
    if (r == VK_SUCCESS) instance_enabled.swap(names);
    return r;
  } catch (...) {
    *out = nullptr;
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }
}
VkResult VKAPI_CALL record_device(VkPhysicalDevice p,
                                  const VkDeviceCreateInfo* ci,
                                  const VkAllocationCallbacks* a,
                                  VkDevice* out) {
  try {
    bool unified = false;
    // Consume the enabled chain while vkCreateDevice's input is alive. An XR
    // runtime may append temporary feature nodes during device creation.
    for (auto* node = static_cast<const VkBaseInStructure*>(ci->pNext); node; node = node->pNext)
      if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFIED_IMAGE_LAYOUTS_FEATURES_KHR)
        unified = reinterpret_cast<const VkPhysicalDeviceUnifiedImageLayoutsFeaturesKHR*>(node)->unifiedImageLayouts;
    std::vector<std::string> names;
    remember(names, ci->enabledExtensionCount, ci->ppEnabledExtensionNames);
    auto r =
        vk<PFN_vkCreateDevice>(gpu.instance, "vkCreateDevice")(p, ci, a, out);
    if (r == VK_SUCCESS) {
      device_enabled.swap(names); unified_image_layouts = unified;
      VkPhysicalDeviceProperties properties{};
      vk<PFN_vkGetPhysicalDeviceProperties>(gpu.instance, "vkGetPhysicalDeviceProperties")(p, &properties);
      const bool feedback = std::ranges::find(device_enabled, VK_EXT_PIPELINE_CREATION_FEEDBACK_EXTENSION_NAME) != device_enabled.end() ||
          (gpu.api_version >= VK_API_VERSION_1_3 && properties.apiVersion >= VK_API_VERSION_1_3);
      auto get = vk<PFN_vkGetDeviceProcAddr>(gpu.instance, "vkGetDeviceProcAddr");
      register_pipeline_device(*out, get, feedback, profile::PipelineSource::angle);
    }
    return r;
  } catch (...) {
    *out = nullptr;
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }
}
PFN_vkVoidFunction VKAPI_CALL recording_gipa(VkInstance instance,
                                             const char* name) {
  if (std::strcmp(name, "vkCreateInstance") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(&record_instance);
  if (std::strcmp(name, "vkCreateDevice") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(&record_device);
  return loader()(instance, name);
}
VkResult create_instance(const VkInstanceCreateInfo* requested,
                         const VkAllocationCallbacks* alloc, VkInstance* out) {
  std::lock_guard lock(creation_lock);
  if (!loader()) return VK_ERROR_INCOMPATIBLE_DRIVER;
  if (gpu.instance) return VK_ERROR_INITIALIZATION_FAILED;
  auto ci = *requested;
  auto names = union_names(ci.enabledExtensionCount, ci.ppEnabledExtensionNames,
                           instance_requirements);
  ci.enabledExtensionCount = static_cast<std::uint32_t>(names.size());
  ci.ppEnabledExtensionNames = names.data();
  VkApplicationInfo app =
      ci.pApplicationInfo
          ? *ci.pApplicationInfo
          : VkApplicationInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  if (!app.apiVersion) app.apiVersion = VK_API_VERSION_1_0;
  ci.pApplicationInfo = &app;
  VkResult result = VK_ERROR_INITIALIZATION_FAILED;
  try {
    auto& host = xr::host_xr();
    if (host.available) {
      if (!host.enabled_extensions.contains("XR_KHR_vulkan_enable2"))
        return VK_ERROR_EXTENSION_NOT_PRESENT;
      XrGraphicsRequirementsVulkan2KHR requirements{
          XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
      auto xr_result = xr_proc<PFN_xrGetVulkanGraphicsRequirements2KHR>(
          "xrGetVulkanGraphicsRequirements2KHR")(host.instance, host.system,
                                                 &requirements);
      if (XR_FAILED(xr_result)) return VK_ERROR_INITIALIZATION_FAILED;
      auto to_vk = [](XrVersion v) {
        return VK_MAKE_API_VERSION(0, XR_VERSION_MAJOR(v), XR_VERSION_MINOR(v),
                                   0);
      };
      app.apiVersion =
          std::max(app.apiVersion, to_vk(requirements.minApiVersionSupported));
      if ((app.apiVersion & ~0xfffu) >
          to_vk(requirements.maxApiVersionSupported))
        return VK_ERROR_INCOMPATIBLE_DRIVER;
      XrVulkanInstanceCreateInfoKHR info{
          XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
      info.systemId = host.system;
      info.pfnGetInstanceProcAddr = recording_gipa;
      info.vulkanCreateInfo = &ci;
      info.vulkanAllocator = alloc;
      xr_result = xr_proc<PFN_xrCreateVulkanInstanceKHR>(
          "xrCreateVulkanInstanceKHR")(host.instance, &info, out, &result);
      if (XR_FAILED(xr_result)) {
        log::error("gfx", "xrCreateVulkanInstanceKHR failed: {}",
                   static_cast<int>(xr_result));
        return VK_ERROR_INITIALIZATION_FAILED;
      }
    } else
      result = record_instance(&ci, alloc, out);
  } catch (const std::exception& e) {
    log::error("gfx", "{}", e.what());
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  if (result == VK_SUCCESS) {
    gpu.instance = *out;
    gpu.api_version = app.apiVersion;
    gpu.get_instance_proc_addr = loader();
  }
  return result;
}
VkResult select_device(VkInstance instance, VkPhysicalDevice* out) {
  std::lock_guard lock(creation_lock);
  try {
    auto& host = xr::host_xr();
    if (host.available) {
      XrVulkanGraphicsDeviceGetInfoKHR info{
          XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
      info.systemId = host.system;
      info.vulkanInstance = instance;
      auto r = xr_proc<PFN_xrGetVulkanGraphicsDevice2KHR>(
          "xrGetVulkanGraphicsDevice2KHR")(host.instance, &info, out);
      if (XR_FAILED(r)) return VK_ERROR_INITIALIZATION_FAILED;
    } else {
      auto enumerate = vk<PFN_vkEnumeratePhysicalDevices>(
          instance, "vkEnumeratePhysicalDevices");
      std::uint32_t count = 0;
      auto result = enumerate(instance, &count, nullptr);
      if (result != VK_SUCCESS || !count) return VK_ERROR_INITIALIZATION_FAILED;
      std::vector<VkPhysicalDevice> devices(count);
      if (enumerate(instance, &count, devices.data()) != VK_SUCCESS)
        return VK_ERROR_INITIALIZATION_FAILED;
      // Prefer a discrete GPU, with no vendor assumptions.
      auto properties = vk<PFN_vkGetPhysicalDeviceProperties>(
          instance, "vkGetPhysicalDeviceProperties");
      *out = devices.front();
      for (auto p : devices) {
        VkPhysicalDeviceProperties prop{};
        properties(p, &prop);
        if (prop.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
          *out = p;
          break;
        }
      }
    }
  } catch (const std::exception& e) {
    log::error("gfx", "{}", e.what());
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  gpu.physical_device = *out;
  return VK_SUCCESS;
}
VkResult create_device(VkPhysicalDevice physical,
                       const VkDeviceCreateInfo* requested,
                       const VkAllocationCallbacks* alloc, VkDevice* out) {
  std::lock_guard lock(creation_lock);
  if (gpu.device || physical != gpu.physical_device)
    return VK_ERROR_INITIALIZATION_FAILED;
  auto ci =
      *requested;  // Preserve ANGLE's feature chain, queues, and allocator.
  auto names = union_names(ci.enabledExtensionCount, ci.ppEnabledExtensionNames,
                           device_requirements);
  auto enumerate = vk<PFN_vkEnumerateDeviceExtensionProperties>(gpu.instance, "vkEnumerateDeviceExtensionProperties");
  std::uint32_t extension_count = 0;
  if (enumerate(physical, nullptr, &extension_count, nullptr) == VK_SUCCESS) {
    std::vector<VkExtensionProperties> extensions(extension_count);
    if (enumerate(physical, nullptr, &extension_count, extensions.data()) == VK_SUCCESS &&
        std::ranges::any_of(extensions, [](const auto& e) {
          return std::strcmp(e.extensionName, VK_EXT_PIPELINE_CREATION_FEEDBACK_EXTENSION_NAME) == 0;
        }) && std::ranges::none_of(names, [](const char* n) {
          return std::strcmp(n, VK_EXT_PIPELINE_CREATION_FEEDBACK_EXTENSION_NAME) == 0;
        })) names.push_back(VK_EXT_PIPELINE_CREATION_FEEDBACK_EXTENSION_NAME);
  }
  ci.enabledExtensionCount = static_cast<std::uint32_t>(names.size());
  ci.ppEnabledExtensionNames = names.data();
  VkResult result = VK_ERROR_INITIALIZATION_FAILED;
  try {
    auto& host = xr::host_xr();
    if (host.available) {
      XrVulkanDeviceCreateInfoKHR info{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
      info.systemId = host.system;
      info.pfnGetInstanceProcAddr = recording_gipa;
      info.vulkanPhysicalDevice = physical;
      info.vulkanCreateInfo = &ci;
      info.vulkanAllocator = alloc;
      auto xr_result = xr_proc<PFN_xrCreateVulkanDeviceKHR>(
          "xrCreateVulkanDeviceKHR")(host.instance, &info, out, &result);
      if (XR_FAILED(xr_result)) {
        log::error("gfx", "xrCreateVulkanDeviceKHR failed: {}",
                   static_cast<int>(xr_result));
        return VK_ERROR_INITIALIZATION_FAILED;
      }
    } else
      result = record_device(physical, &ci, alloc, out);
  } catch (const std::exception& e) {
    log::error("gfx", "{}", e.what());
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  if (result == VK_SUCCESS) gpu.device = *out;
  return result;
}
void device_ready(VkDevice device, std::uint32_t family, std::uint32_t index) {
  std::lock_guard lock(creation_lock);
  if (device != gpu.device) return;
  gpu.queue_family = family;
  gpu.queue_index = index;
  vk<PFN_vkGetDeviceQueue>(gpu.instance, "vkGetDeviceQueue")(device, family,
                                                             index, &gpu.queue);
}
template <class F>
VkResult callback_result(F&& fn) noexcept {
  try {
    return fn();
  } catch (const std::bad_alloc&) {
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  } catch (const std::exception& e) {
    log::error("gfx", "ANGLE Vulkan callback: {}", e.what());
    return VK_ERROR_INITIALIZATION_FAILED;
  } catch (...) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
}
VkResult VKAPI_CALL angle_create_instance(void*,
                                          PFN_vkGetInstanceProcAddr source,
                                          const VkInstanceCreateInfo* ci,
                                          const VkAllocationCallbacks* alloc,
                                          VkInstance* out) noexcept {
  *out = nullptr;
  return callback_result([&] {
    std::lock_guard lock(creation_lock);
    if (!source || gpu.instance) return VK_ERROR_INITIALIZATION_FAILED;
    angle_loader = unwrap_pipeline_loader(source);
    return create_instance(ci, alloc, out);
  });
}
VkResult VKAPI_CALL angle_choose_physical(void*,
                                          PFN_vkGetInstanceProcAddr source,
                                          VkInstance instance,
                                          std::uint32_t count,
                                          const VkPhysicalDevice* devices,
                                          VkPhysicalDevice* out) noexcept {
  *out = nullptr;
  return callback_result([&] {
    if (unwrap_pipeline_loader(source) != angle_loader || instance != gpu.instance)
      return VK_ERROR_INITIALIZATION_FAILED;
    auto r = select_device(instance, out);
    if (r == VK_SUCCESS &&
        std::find(devices, devices + count, *out) == devices + count) {
      *out = nullptr;
      return VK_ERROR_INITIALIZATION_FAILED;
    }
    return r;
  });
}
VkResult VKAPI_CALL angle_create_device(void*, PFN_vkGetInstanceProcAddr source,
                                        VkInstance instance,
                                        VkPhysicalDevice physical,
                                        const VkDeviceCreateInfo* ci,
                                        const VkAllocationCallbacks* alloc,
                                        VkDevice* out) noexcept {
  *out = nullptr;
  return callback_result([&] {
    if (unwrap_pipeline_loader(source) != angle_loader || instance != gpu.instance)
      return VK_ERROR_INITIALIZATION_FAILED;
    return create_device(physical, ci, alloc, out);
  });
}
void VKAPI_CALL angle_device_ready(void*, VkInstance instance,
                                   VkPhysicalDevice physical, VkDevice device,
                                   std::uint32_t family, std::uint32_t index,
                                   VkQueue queue) noexcept {
  callback_result([&] {
    if (instance != gpu.instance || physical != gpu.physical_device ||
        device != gpu.device)
      return VK_ERROR_INITIALIZATION_FAILED;
    device_ready(device, family, index);
    if (gpu.queue != queue) {
      gpu.queue = nullptr;
      return VK_ERROR_INITIALIZATION_FAILED;
    }
    return VK_SUCCESS;
  });
}
void create_direct() {
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "Venture";
  app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ci.pApplicationInfo = &app;
  VkInstance instance{};
  auto r = create_instance(&ci, nullptr, &instance);
  if (r != VK_SUCCESS)
    throw std::runtime_error("Vulkan instance creation failed: " +
                             std::to_string(r));
  VkPhysicalDevice p{};
  if (select_device(instance, &p) != VK_SUCCESS)
    throw std::runtime_error("No Vulkan physical device");
  auto get_families = vk<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
      instance, "vkGetPhysicalDeviceQueueFamilyProperties");
  std::uint32_t count = 0;
  get_families(p, &count, nullptr);
  std::vector<VkQueueFamilyProperties> families(count);
  get_families(p, &count, families.data());
  auto it = std::ranges::find_if(families, [](auto f) {
    return f.queueCount && (f.queueFlags & VK_QUEUE_GRAPHICS_BIT);
  });
  if (it == families.end())
    throw std::runtime_error("No Vulkan graphics queue");
  float priority = 1;
  VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  q.queueFamilyIndex = static_cast<std::uint32_t>(it - families.begin());
  q.queueCount = 1;
  q.pQueuePriorities = &priority;
  VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dc.queueCreateInfoCount = 1;
  dc.pQueueCreateInfos = &q;
  VkDevice device{};
  r = create_device(p, &dc, nullptr, &device);
  if (r != VK_SUCCESS)
    throw std::runtime_error("Vulkan device creation failed: " +
                             std::to_string(r));
  device_ready(device, q.queueFamilyIndex, 0);
  VkPhysicalDeviceProperties properties{};
  vk<PFN_vkGetPhysicalDeviceProperties>(
      instance, "vkGetPhysicalDeviceProperties")(p, &properties);
  log::info("gfx", "Vulkan device: {}, queue {}:{}", properties.deviceName,
            gpu.queue_family, gpu.queue_index);
}
}  // namespace
GpuDevice& gpu_device_state() { return gpu; }
bool shutdown_for_os_quit() {
  std::lock_guard lock(creation_lock);
  if (angle_shared_device()) {
    if (!terminate_for_os_quit()) return false;
  } else {
    if (gpu.device) {
      auto get = vk<PFN_vkGetDeviceProcAddr>(gpu.instance, "vkGetDeviceProcAddr");
      auto destroy = get ? reinterpret_cast<PFN_vkDestroyDevice>(get(gpu.device, "vkDestroyDevice")) : nullptr;
      if (!destroy) return false;
      destroy(gpu.device, nullptr);
    }
    if (gpu.instance) {
      auto destroy = vk<PFN_vkDestroyInstance>(gpu.instance, "vkDestroyInstance");
      if (!destroy) return false;
      destroy(gpu.instance, nullptr);
    }
  }
  gpu.device = nullptr; gpu.queue = nullptr; gpu.instance = nullptr; gpu.physical_device = nullptr;
  return true;
}
GpuDevice& gpu_device() {
  // ANGLE initialization must precede taking creation_lock: callbacks take it.
  static std::once_flag once;
  std::call_once(once, [] {
    initialize_angle();
    std::lock_guard lock(creation_lock);
    if (!gpu.device) {
      if (angle_display())
        throw std::runtime_error(
            "ANGLE device sharing failed; refusing a second graphics device");
      create_direct();
    }
    if (!gpu.queue)
      throw std::runtime_error("Graphics device has no reported queue");
    register_queue_probe();
  });
  return gpu;
}
bool request_gpu_extensions(std::span<const std::string> instances,
                            std::span<const std::string> devices) {
  std::lock_guard lock(creation_lock);
  if (gpu.instance) return false;
  instance_requirements.insert(instance_requirements.end(), instances.begin(),
                               instances.end());
  device_requirements.insert(device_requirements.end(), devices.begin(),
                             devices.end());
  return true;
}
const std::vector<std::string>& enabled_gpu_instance_extensions() {
  return instance_enabled;
}
const std::vector<std::string>& enabled_gpu_device_extensions() {
  return device_enabled;
}
bool gpu_unified_image_layouts_enabled() { return unified_image_layouts; }
bool host_vulkan_instance_extension(std::string_view name) {
  if (!loader()) return false;
  auto enumerate = vk<PFN_vkEnumerateInstanceExtensionProperties>(
      nullptr, "vkEnumerateInstanceExtensionProperties");
  std::uint32_t count = 0;
  if (!enumerate || enumerate(nullptr, &count, nullptr) != VK_SUCCESS) return false;
  std::vector<VkExtensionProperties> extensions(count);
  if (enumerate(nullptr, &count, extensions.data()) != VK_SUCCESS) return false;
  return std::ranges::any_of(extensions, [&](const auto& e) { return name == e.extensionName; });
}
const AngleVulkanCallbacksV1& angle_vulkan_callbacks() {
  static const AngleVulkanCallbacksV1 callbacks{1,
                                                sizeof(AngleVulkanCallbacksV1),
                                                nullptr,
                                                angle_create_instance,
                                                angle_choose_physical,
                                                angle_create_device,
                                                angle_device_ready,
                                                [](void*, PFN_vkGetInstanceProcAddr raw) { return wrap_pipeline_loader(raw); }};
  return callbacks;
}
bool borrow_angle_device(VkInstance i, VkPhysicalDevice p, VkDevice d,
                         VkQueue q, std::uint32_t family,
                         PFN_vkGetInstanceProcAddr get, std::uint32_t version,
                         const char* const* ie, const char* const* de) {
  std::lock_guard lock(creation_lock);
  if (xr::host_xr().available || gpu.device || !i || !p || !d || !q || !get)
    return false;
  VkQueue first{};
  auto get_queue =
      reinterpret_cast<PFN_vkGetDeviceQueue>(get(i, "vkGetDeviceQueue"));
  get_queue(d, family, 0, &first);
  if (first != q) {
    log::error("gfx",
               "Stock ANGLE medium queue is not index zero; creation callback "
               "required");
    return false;
  }
  auto record = [](std::vector<std::string>& result, const char* const* list) {
    result.clear();
    if (list)
      for (; *list; ++list) result.emplace_back(*list);
  };
  record(instance_enabled, ie);
  record(device_enabled, de);
  for (const auto& name : instance_requirements)
    if (std::ranges::find(instance_enabled, name) == instance_enabled.end())
      return false;
  for (const auto& name : device_requirements)
    if (std::ranges::find(device_enabled, name) == device_enabled.end())
      return false;
  gpu.instance = i;
  gpu.physical_device = p;
  gpu.device = d;
  gpu.queue = q;
  // Stock EGL has no queue-index query; accept only the verified index-zero
  // case.
  gpu.queue_family = family;
  gpu.queue_index = 0;
  gpu.get_instance_proc_addr = get;
  gpu.api_version = version;
  return true;
}
void discard_angle_device() {
  std::lock_guard lock(creation_lock);
  if (gpu.device) release_pipeline_device(gpu.device);
  gpu.instance = nullptr;
  gpu.physical_device = nullptr;
  gpu.device = nullptr;
  gpu.queue = nullptr;
  gpu.api_version = 0;
  gpu.get_instance_proc_addr = nullptr;
  unified_image_layouts = false;
  instance_enabled.clear();
  device_enabled.clear();
}
}  // namespace venture::gfx

#include "common/injected_modules.h"
#include "common/log.h"
#include "gfx/foveation_vulkan.h"
#include "gfx/angle.h"
#include "gfx/anisotropy.h"
#include "gfx/angle_callbacks.h"
#include "gfx/pipeline_trace.h"
#include "gfx/gpu_device_internal.h"
#include "gfx/queue_probe.h"
#include "gfx/shared_image.h"
#include "gfx/vulkan_enumerate.h"
#include "gfx/xr_vulkan_version.h"
#include "platform/gfx_library.h"
#include "xr/host_xr.h"
#include "xr/vulkan_extensions.h"
#include "xr/vulkan_layers.h"
#include "xr/display_override.h"
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <stdexcept>
#include <string>

namespace venture::gfx {
namespace {
GpuDevice gpu;
std::recursive_mutex creation_lock;
std::vector<std::string> instance_requirements, device_requirements;
std::vector<std::string> instance_enabled, device_enabled;
// False when an XR runtime created the object without calling the GIPA it was given
// (Virtual Desktop); the requested extensions, which the runtime must enable, stand in.
bool instance_recorded = false, device_recorded = false;
// Set while xrCreateVulkanInstanceKHR runs, which reaches record_instance.
bool instance_through_xr = false;
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
// XR_KHR_vulkan_enable2 says the runtime adds its own requirements inside
// xrCreateVulkanInstanceKHR/xrCreateVulkanDeviceKHR. The Oculus PC runtime
// relies on the legacy lists instead (Meta: a device without the
// xrGetVulkanDeviceExtensionsKHR extensions faults inside LibOVRRT): GLES
// titles on the shared device crashed with a host call through null in
// xrDestroySwapchain (Job Simulator, Beat Saber; issue 46). Guest Vulkan
// devices already merge these lists (hle/vulkan native_xr_extensions); the
// shared device does the same, keeping only names this Vulkan lists, so a
// runtime that does add them, or lists an absent one, changes nothing.
void add_legacy_xr_requirements(bool device, std::vector<const char*>& names,
                                std::vector<std::string>& storage,
                                const std::vector<VkExtensionProperties>& available) {
  auto& host = xr::host_xr();
  if (!host.enabled_extensions.contains("XR_KHR_vulkan_enable")) return;
  std::vector<std::string> required;
  if (XR_FAILED(xr::vulkan_extension_requirements(host.instance, host.system, device, required))) return;
  storage.reserve(required.size());
  for (auto& name : required) {
    if (std::ranges::any_of(names, [&](const char* n) { return name == n; })) continue;
    if (std::ranges::none_of(available, [&](const auto& e) { return name == e.extensionName; })) {
      log::warn("gfx", "Host XR runtime requires Vulkan {} extension {}, which this Vulkan driver does not list",
                device ? "device" : "instance", name);
      continue;
    }
    storage.push_back(std::move(name));
  }
  for (const auto& name : storage) names.push_back(name.c_str());
}
const char* vk_result_name(VkResult r) {
  switch (r) {
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    default: return "VkResult";
  }
}
std::string joined(std::uint32_t count, const char* const* names) {
  std::string text;
  for (std::uint32_t i = 0; i < count; ++i) text += (i ? " " : "") + std::string(names[i]);
  return text.empty() ? "none" : text;
}
// Why the last shared instance creation failed, for the launch error a report shows.
std::string instance_failure;
// Why the host runtime's GPU could not be found, for the launch error.
std::string device_failure;
// The system's hardware display adapters that no Vulkan device matches by LUID,
// by name ("A and B"); empty when Vulkan lists them all or the system has no
// adapter list (platform::display_adapters).
std::string adapters_vulkan_misses() {
  const auto system = platform::display_adapters();
  if (system.empty()) return {};
  std::vector<std::array<std::uint8_t, 8>> listed;
  try {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Venture adapter probe";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    const auto create = vk<PFN_vkCreateInstance>(nullptr, "vkCreateInstance");
    VkInstance probe{};
    if (!create || create(&ci, nullptr, &probe) != VK_SUCCESS) return {};
    const auto enumerate = vk<PFN_vkEnumeratePhysicalDevices>(probe, "vkEnumeratePhysicalDevices");
    const auto properties2 = vk<PFN_vkGetPhysicalDeviceProperties2>(probe, "vkGetPhysicalDeviceProperties2");
    std::vector<VkPhysicalDevice> devices;
    bool known = enumerate && properties2 &&
                 enumerate_vulkan(devices, [&](std::uint32_t* n, VkPhysicalDevice* p) { return enumerate(probe, n, p); }) >= 0;
    for (auto device : devices) {
      if (!known) break;
      VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
      VkPhysicalDeviceProperties2 p{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
      p.pNext = &id;
      properties2(device, &p);
      // Without LUIDs nothing can be matched: name no adapter.
      if (!id.deviceLUIDValid) known = false;
      else {
        std::array<std::uint8_t, 8> luid{};
        std::copy(id.deviceLUID, id.deviceLUID + 8, luid.begin());
        listed.push_back(luid);
      }
    }
    if (auto destroy = vk<PFN_vkDestroyInstance>(probe, "vkDestroyInstance")) destroy(probe, nullptr);
    if (!known) return {};
  } catch (...) {
    return {};
  }
  std::string names;
  for (const auto& a : system)
    if (std::find(listed.begin(), listed.end(), a.luid) == listed.end())
      names += (names.empty() ? "" : " and ") + (a.name.empty() ? std::string("a display adapter") : a.name);
  return names;
}
// On a failed instance creation, what the plain loader sees: its version and
// every adapter a bare Vulkan 1.0 instance enumerates, with its driver's
// Vulkan version, so a report names the GPU and driver (hybrid PCs list two).
std::string describe_vulkan_adapters() {
  try {
    std::uint32_t loader_version = VK_API_VERSION_1_0;
    if (auto version = vk<PFN_vkEnumerateInstanceVersion>(nullptr, "vkEnumerateInstanceVersion")) version(&loader_version);
    std::string text = "loader Vulkan " + vulkan_version_text(loader_version);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Venture adapter probe";
    app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    VkInstance probe{};
    const auto create = vk<PFN_vkCreateInstance>(nullptr, "vkCreateInstance");
    if (!create) return text + "; no vkCreateInstance";
    if (auto r = create(&ci, nullptr, &probe); r != VK_SUCCESS)
      return text + std::format("; a plain Vulkan 1.0 instance fails too ({} {}): no Vulkan driver loads", static_cast<int>(r), vk_result_name(r));
    const auto enumerate = vk<PFN_vkEnumeratePhysicalDevices>(probe, "vkEnumeratePhysicalDevices");
    const auto properties = vk<PFN_vkGetPhysicalDeviceProperties>(probe, "vkGetPhysicalDeviceProperties");
    std::vector<VkPhysicalDevice> devices;
    // VK_INCOMPLETE still returns valid handles: a count that changed between
    // the count and fill calls once made this report "no adapters".
    VkResult listed = VK_ERROR_INITIALIZATION_FAILED;
    if (enumerate && properties)
      listed = enumerate_vulkan(devices, [&](std::uint32_t* n, VkPhysicalDevice* p) { return enumerate(probe, n, p); });
    if (!enumerate || !properties) text += "; no vkEnumeratePhysicalDevices";
    else if (listed < 0) text += std::format("; vkEnumeratePhysicalDevices failed ({})", static_cast<int>(listed));
    else if (listed == VK_INCOMPLETE) text += "; the adapter count kept changing between calls (VK_INCOMPLETE)";
    text += devices.empty() ? "; no adapters" : "; adapters:";
    for (auto device : devices) {
      VkPhysicalDeviceProperties p{};
      properties(device, &p);
      text += std::format(" [{} ({}, vendor 0x{:x}, Vulkan {}, driver 0x{:x})]", p.deviceName,
                          p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? "discrete"
                          : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? "integrated"
                                                                                   : "other",
                          p.vendorID, vulkan_version_text(vulkan_major_minor(p.apiVersion)), p.driverVersion);
    }
    if (auto destroy = vk<PFN_vkDestroyInstance>(probe, "vkDestroyInstance")) destroy(probe, nullptr);
    return text;
  } catch (const std::exception& e) {
    return e.what();
  }
}
void log_instance_failure(std::string cause, const VkInstanceCreateInfo& ci) {
  static bool probed = false;
  log::error("gfx", "Vulkan instance creation failed: {}; extensions: {}", cause,
             joined(ci.enabledExtensionCount, ci.ppEnabledExtensionNames));
  if (!probed) {
    probed = true;
    log::error("gfx", "Vulkan on this PC: {}", describe_vulkan_adapters());
  }
  instance_failure = std::move(cause);
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
    // Venture asks for no layers here: any are the host XR stack's.
    auto final = *ci;
    xr::HostAddedVulkanLayers layers;
    if (instance_through_xr) layers = xr::leave_out_host_added_vulkan_layers(final, "Venture's shared Vulkan instance");
    auto r = vk<PFN_vkCreateInstance>(nullptr, "vkCreateInstance")(&final, a, out);
    if (r == VK_SUCCESS) { instance_enabled.swap(names); instance_recorded = true; }
    return r;
  } catch (...) {
    *out = nullptr;
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }
}
void adopt_device(VkPhysicalDevice p, VkDevice device, std::vector<std::string> names) {
  device_recorded = true;
  device_enabled.swap(names);
  VkPhysicalDeviceProperties properties{};
  vk<PFN_vkGetPhysicalDeviceProperties>(gpu.instance, "vkGetPhysicalDeviceProperties")(p, &properties);
  const bool feedback = std::ranges::find(device_enabled, VK_EXT_PIPELINE_CREATION_FEEDBACK_EXTENSION_NAME) != device_enabled.end() ||
      (gpu.api_version >= VK_API_VERSION_1_3 && properties.apiVersion >= VK_API_VERSION_1_3);
  auto get = vk<PFN_vkGetDeviceProcAddr>(gpu.instance, "vkGetDeviceProcAddr");
  register_pipeline_device(device, get, feedback, profile::PipelineSource::angle);
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
      unified_image_layouts = unified;
      adopt_device(p, *out, std::move(names));
    }
    return r;
  } catch (...) {
    *out = nullptr;
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }
}
// The XR runtime finds the HMD's GPU on the shared instance with procedures
// from the GIPA it was given (xrGetVulkanGraphicsDevice2KHR). Each call answers
// from one complete or accepted list (gfx/vulkan_enumerate.h), so a count that
// changes between the loader's count and fill calls reaches the runtime as one
// consistent count and fill instead of VK_INCOMPLETE.
VkResult VKAPI_CALL steady_enumerate_physicals(VkInstance instance, std::uint32_t* count, VkPhysicalDevice* out) {
  try {
    auto next = vk<PFN_vkEnumeratePhysicalDevices>(instance, "vkEnumeratePhysicalDevices");
    if (!next) return VK_ERROR_INITIALIZATION_FAILED;
    std::vector<VkPhysicalDevice> devices;
    if (auto r = enumerate_vulkan(devices, [&](std::uint32_t* n, VkPhysicalDevice* p) { return next(instance, n, p); });
        r < 0)
      return r;
    return fill_vulkan_enumeration(devices, count, out);
  } catch (const std::bad_alloc&) {
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }
}
VkResult VKAPI_CALL steady_enumerate_groups(VkInstance instance, std::uint32_t* count,
                                            VkPhysicalDeviceGroupProperties* out, const char* name) {
  try {
    auto next = vk<PFN_vkEnumeratePhysicalDeviceGroups>(instance, name);
    if (!next || !count) return VK_ERROR_INITIALIZATION_FAILED;
    std::vector<VkPhysicalDeviceGroupProperties> groups;
    if (auto r = enumerate_vulkan(
            groups, [&](std::uint32_t* n, VkPhysicalDeviceGroupProperties* p) { return next(instance, n, p); },
            VkPhysicalDeviceGroupProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GROUP_PROPERTIES});
        r < 0)
      return r;
    if (!out) {
      *count = static_cast<std::uint32_t>(groups.size());
      return VK_SUCCESS;
    }
    // The caller's sType and pNext stay as given.
    const auto n = std::min(*count, static_cast<std::uint32_t>(groups.size()));
    for (std::uint32_t i = 0; i < n; ++i) {
      out[i].physicalDeviceCount = groups[i].physicalDeviceCount;
      std::copy_n(groups[i].physicalDevices, VK_MAX_DEVICE_GROUP_SIZE, out[i].physicalDevices);
      out[i].subsetAllocation = groups[i].subsetAllocation;
    }
    *count = n;
    return n < groups.size() ? VK_INCOMPLETE : VK_SUCCESS;
  } catch (const std::bad_alloc&) {
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }
}
VkResult VKAPI_CALL steady_enumerate_groups_core(VkInstance instance, std::uint32_t* count,
                                                 VkPhysicalDeviceGroupProperties* out) {
  return steady_enumerate_groups(instance, count, out, "vkEnumeratePhysicalDeviceGroups");
}
VkResult VKAPI_CALL steady_enumerate_groups_khr(VkInstance instance, std::uint32_t* count,
                                                VkPhysicalDeviceGroupProperties* out) {
  return steady_enumerate_groups(instance, count, out, "vkEnumeratePhysicalDeviceGroupsKHR");
}
PFN_vkVoidFunction VKAPI_CALL recording_gipa(VkInstance instance,
                                             const char* name) {
  if (std::strcmp(name, "vkCreateInstance") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(&record_instance);
  if (std::strcmp(name, "vkCreateDevice") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(&record_device);
  if (instance && loader()(instance, name)) {
    if (std::strcmp(name, "vkEnumeratePhysicalDevices") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&steady_enumerate_physicals);
    if (std::strcmp(name, "vkEnumeratePhysicalDeviceGroups") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&steady_enumerate_groups_core);
    if (std::strcmp(name, "vkEnumeratePhysicalDeviceGroupsKHR") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&steady_enumerate_groups_khr);
  }
  auto fn = loader()(instance, name);
  if (!fn) xr::note_unresolved_vulkan_proc(name);
  return fn;
}
VkResult create_instance(const VkInstanceCreateInfo* requested,
                         const VkAllocationCallbacks* alloc, VkInstance* out) {
  std::lock_guard lock(creation_lock);
  if (!loader()) {
    instance_failure = "the Vulkan loader (" + std::string(platform::vulkan_library_name()) + ") did not load";
    log::error("gfx", "Vulkan instance creation failed: {}", instance_failure);
    return VK_ERROR_INCOMPATIBLE_DRIVER;
  }
  if (gpu.instance) return VK_ERROR_INITIALIZATION_FAILED;
  auto ci = *requested;
  auto names = union_names(ci.enabledExtensionCount, ci.ppEnabledExtensionNames,
                           instance_requirements);
  // The desktop mirror window presents from this device (xr/desktop_mirror.h).
  if (const auto& mirror = xr::mirror_instance_extensions();
      !mirror.empty() && std::ranges::all_of(mirror, [](const char* n) { return host_vulkan_instance_extension(n); }))
    for (const char* name : mirror)
      if (std::ranges::none_of(names, [&](const char* n) { return std::strcmp(n, name) == 0; })) names.push_back(name);
  std::vector<std::string> legacy;
  ci.enabledExtensionCount = static_cast<std::uint32_t>(names.size());
  ci.ppEnabledExtensionNames = names.data();
  VkApplicationInfo app =
      ci.pApplicationInfo
          ? *ci.pApplicationInfo
          : VkApplicationInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  if (!app.apiVersion) app.apiVersion = VK_API_VERSION_1_0;
  ci.pApplicationInfo = &app;
  VkResult result = VK_ERROR_INITIALIZATION_FAILED;
  instance_failure.clear();
  try {
    auto& host = xr::host_xr();
    if (host.available) {
      if (!host.enabled_extensions.contains("XR_KHR_vulkan_enable2")) {
        log_instance_failure(host.runtime_name + " does not offer XR_KHR_vulkan_enable2", ci);
        return VK_ERROR_EXTENSION_NOT_PRESENT;
      }
      XrGraphicsRequirementsVulkan2KHR requirements{
          XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
      auto xr_result = xr_proc<PFN_xrGetVulkanGraphicsRequirements2KHR>(
          "xrGetVulkanGraphicsRequirements2KHR")(host.instance, host.system,
                                                 &requirements);
      if (XR_FAILED(xr_result)) {
        log_instance_failure(std::format("{} xrGetVulkanGraphicsRequirements2KHR failed: {}", host.runtime_name,
                                         static_cast<int>(xr_result)), ci);
        return VK_ERROR_INITIALIZATION_FAILED;
      }
      // The maximum is only what the runtime was tested with (gfx/xr_vulkan_version.h).
      const auto plan = plan_xr_vulkan_version(app.apiVersion, requirements.minApiVersionSupported,
                                               requirements.maxApiVersionSupported,
                                               std::min(app.apiVersion, VK_API_VERSION_1_1));
      const auto bounds = std::format("{} requires Vulkan {} and was tested up to {} (XrVersion {} to {})",
                                      host.runtime_name, vulkan_version_text(plan.min_version),
                                      vulkan_version_text(plan.max_version),
                                      xr_version_text(requirements.minApiVersionSupported),
                                      xr_version_text(requirements.maxApiVersionSupported));
      const std::uint32_t asked = app.apiVersion;
      app.apiVersion = plan.api_version;
      if (plan.above_tested_max || !plan.min_version || !plan.max_version)
        log::warn("gfx", "Vulkan {} requested; {}; trying it", vulkan_version_text(vulkan_major_minor(asked)), bounds);
      std::vector<VkExtensionProperties> available;
      if (auto enumerate = vk<PFN_vkEnumerateInstanceExtensionProperties>(
              nullptr, "vkEnumerateInstanceExtensionProperties"))
        enumerate_vulkan(available, [&](std::uint32_t* n, VkExtensionProperties* p) { return enumerate(nullptr, n, p); });
      add_legacy_xr_requirements(false, names, legacy, available);
      ci.enabledExtensionCount = static_cast<std::uint32_t>(names.size());
      ci.ppEnabledExtensionNames = names.data();
      XrVulkanInstanceCreateInfoKHR info{
          XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
      info.systemId = host.system;
      info.pfnGetInstanceProcAddr = recording_gipa;
      info.vulkanCreateInfo = &ci;
      info.vulkanAllocator = alloc;
      const auto create = xr_proc<PFN_xrCreateVulkanInstanceKHR>("xrCreateVulkanInstanceKHR");
      auto attempt = [&](std::uint32_t version) {
        app.apiVersion = version;  // ci.pApplicationInfo points at app
        instance_recorded = false;
        result = VK_ERROR_INITIALIZATION_FAILED;
        *out = nullptr;
        instance_through_xr = true;
        const auto created = create(host.instance, &info, out, &result);
        instance_through_xr = false;
        return created;
      };
      xr_result = attempt(plan.api_version);
      if (XR_SUCCEEDED(xr_result) && result == VK_ERROR_INCOMPATIBLE_DRIVER && plan.retry_version) {
        log::warn("gfx", "Vulkan {} instance refused as incompatible; {}; retrying with Vulkan {}",
                  vulkan_version_text(vulkan_major_minor(plan.api_version)), bounds,
                  vulkan_version_text(plan.retry_version));
        xr_result = attempt(plan.retry_version);
      }
      if (XR_FAILED(xr_result)) {
        log_instance_failure(std::format("{} xrCreateVulkanInstanceKHR failed: {} (requested Vulkan {}; {})",
                                         host.runtime_name, static_cast<int>(xr_result),
                                         vulkan_version_text(vulkan_major_minor(app.apiVersion)), bounds),
                             ci);
        return VK_ERROR_INITIALIZATION_FAILED;
      }
      if (result != VK_SUCCESS)
        log_instance_failure(std::format("{} from {} xrCreateVulkanInstanceKHR (requested Vulkan {}; {})",
                                         vk_result_name(result), host.runtime_name,
                                         vulkan_version_text(vulkan_major_minor(app.apiVersion)), bounds),
                             ci);
      if (result == VK_SUCCESS && !instance_recorded) {
        log::info("gfx", "The XR runtime created the Vulkan instance itself; using the requested extensions");
        remember(instance_enabled, ci.enabledExtensionCount, ci.ppEnabledExtensionNames);
      }
    } else {
      result = record_instance(&ci, alloc, out);
      if (result != VK_SUCCESS)
        log_instance_failure(std::format("{} from the Vulkan loader (requested Vulkan {})", vk_result_name(result),
                                         vulkan_version_text(vulkan_major_minor(app.apiVersion))),
                             ci);
    }
  } catch (const std::exception& e) {
    log_instance_failure(e.what(), ci);
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
      if (XR_FAILED(r)) {
        const auto adapters = describe_vulkan_adapters();
        log::error("gfx", "{} xrGetVulkanGraphicsDevice2KHR failed: {}; Vulkan on this PC: {}", host.runtime_name,
                   static_cast<int>(r), adapters);
        // A GPU the system has but Vulkan does not list (a driver without
        // Vulkan, or a laptop's dedicated GPU while Windows runs Venture on the
        // power-saving one) is likely the one the runtime renders on.
        if (const auto missing = adapters_vulkan_misses(); !missing.empty())
          device_failure = std::format(
              "Vulkan does not list {}, which {} likely renders on. Install the current driver from its maker; "
              "on a laptop, also set venture-runtime.exe to High performance in Windows' graphics settings",
              missing, host.runtime_name);
        return VK_ERROR_INITIALIZATION_FAILED;
      }
    } else {
      auto enumerate = vk<PFN_vkEnumeratePhysicalDevices>(
          instance, "vkEnumeratePhysicalDevices");
      std::vector<VkPhysicalDevice> devices;
      if (!enumerate ||
          enumerate_vulkan(devices, [&](std::uint32_t* n, VkPhysicalDevice* p) { return enumerate(instance, n, p); }) < 0 ||
          devices.empty())
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
  // Which GPU renders, and its VRAM, for reports: PCs with an integrated GPU
  // beside a discrete one run on whichever the XR runtime names.
  try {
    VkPhysicalDeviceProperties prop{};
    vk<PFN_vkGetPhysicalDeviceProperties>(instance, "vkGetPhysicalDeviceProperties")(*out, &prop);
    VkPhysicalDeviceMemoryProperties memory{};
    vk<PFN_vkGetPhysicalDeviceMemoryProperties>(instance, "vkGetPhysicalDeviceMemoryProperties")(*out, &memory);
    VkDeviceSize vram = 0;
    for (std::uint32_t i = 0; i < memory.memoryHeapCount; ++i)
      if (memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) vram = std::max(vram, memory.memoryHeaps[i].size);
    log::info("gfx", "Host GPU: {} ({}, {} MiB device memory), chosen by {}", prop.deviceName,
              prop.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? "discrete"
              : prop.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? "integrated"
                                                                          : "other",
              vram >> 20, xr::host_xr().available ? "the XR runtime" : "Venture (first discrete GPU)");
    // The driver by name and version ([facts]: kept whole in debug reports).
    // One GPU family runs under different drivers (Adreno: Qualcomm's or
    // Mesa's Turnip), whose capabilities pick different ANGLE paths.
    const auto properties2 = vk<PFN_vkGetPhysicalDeviceProperties2>(instance, "vkGetPhysicalDeviceProperties2");
    if (properties2 && prop.apiVersion >= VK_API_VERSION_1_2) {
      VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
      VkPhysicalDeviceProperties2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &driver};
      properties2(*out, &query);
      log::info("facts", "Host Vulkan driver: {} ({}), driver id {}, Vulkan {}.{}.{}, GPU {} (vendor {:#06x} device {:#x})",
                driver.driverName, driver.driverInfo, int(driver.driverID), VK_API_VERSION_MAJOR(prop.apiVersion),
                VK_API_VERSION_MINOR(prop.apiVersion), VK_API_VERSION_PATCH(prop.apiVersion), prop.deviceName,
                prop.vendorID, prop.deviceID);
    }
  } catch (const std::exception&) {
  }
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
  std::vector<VkExtensionProperties> extensions;
  if (enumerate_vulkan(extensions, [&](std::uint32_t* n, VkExtensionProperties* p) {
        return enumerate(physical, nullptr, n, p);
      }) >= 0) {
    if (std::ranges::any_of(extensions, [](const auto& e) {
          return std::strcmp(e.extensionName, VK_EXT_PIPELINE_CREATION_FEEDBACK_EXTENSION_NAME) == 0;
        }) && std::ranges::none_of(names, [](const char* n) {
          return std::strcmp(n, VK_EXT_PIPELINE_CREATION_FEEDBACK_EXTENSION_NAME) == 0;
        })) names.push_back(VK_EXT_PIPELINE_CREATION_FEEDBACK_EXTENSION_NAME);
    // Imports decoded video pictures from the media decoder's device on this
    // GPU (gfx/shared_image.h); enabling it changes nothing else.
    if (std::ranges::any_of(extensions, [](const auto& e) { return std::strcmp(e.extensionName, shared_image_extension) == 0; }) &&
        std::ranges::none_of(names, [](const char* n) { return std::strcmp(n, shared_image_extension) == 0; }))
      names.push_back(shared_image_extension);
  }
  if (const char* swapchain = xr::mirror_device_extension();
      std::ranges::find(instance_enabled, "VK_KHR_surface") != instance_enabled.end() &&
      std::ranges::any_of(extensions, [&](const auto& e) { return std::strcmp(e.extensionName, swapchain) == 0; }) &&
      std::ranges::none_of(names, [&](const char* n) { return std::strcmp(n, swapchain) == 0; }))
    names.push_back(swapchain);
  std::vector<std::string> legacy;
  if (xr::host_xr().available) add_legacy_xr_requirements(true, names, legacy, extensions);
  ci.enabledExtensionCount = static_cast<std::uint32_t>(names.size());
  ci.ppEnabledExtensionNames = names.data();
  // The desktop mirror presents on a queue of its own (xr/desktop_mirror.h).
  xr::MirrorQueueRequest mirror_queue;
  if (xr::mirror_mode() != xr::MirrorMode::off &&
      std::ranges::any_of(names, [](const char* n) { return std::strcmp(n, xr::mirror_device_extension()) == 0; })) {
    auto get_families = vk<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(gpu.instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    std::uint32_t family_count = 0;
    get_families(physical, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    get_families(physical, &family_count, families.data());
    families.resize(family_count);
    mirror_queue.add(families, ci);
  }
  // Foveated rendering's shading-rate attachments (gfx/foveation_vulkan.h).
  foveation::DeviceAdditions foveated;
  foveation::add_to_device(loader(), gpu.instance, physical, ci, foveated);
  ci = foveated.info;
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
      device_recorded = false;
      auto xr_result = xr_proc<PFN_xrCreateVulkanDeviceKHR>(
          "xrCreateVulkanDeviceKHR")(host.instance, &info, out, &result);
      if (XR_FAILED(xr_result)) {
        log::error("gfx", "xrCreateVulkanDeviceKHR failed: {}",
                   static_cast<int>(xr_result));
        return VK_ERROR_INITIALIZATION_FAILED;
      }
      if (result == VK_SUCCESS && !device_recorded) {
        log::info("gfx", "The XR runtime created the Vulkan device itself; using the requested extensions");
        std::vector<std::string> requested;
        remember(requested, ci.enabledExtensionCount, ci.ppEnabledExtensionNames);
        adopt_device(physical, *out, std::move(requested));
      }
    } else
      result = record_device(physical, &ci, alloc, out);
  } catch (const std::exception& e) {
    log::error("gfx", "{}", e.what());
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  if (result == VK_SUCCESS) {
    gpu.device = *out;
    mirror_queue.created(*out);
    foveation::register_device(*out, physical, loader(), gpu.instance,
                               vk<PFN_vkGetDeviceProcAddr>(gpu.instance, "vkGetDeviceProcAddr"), foveated.added);
    // ANGLE enables samplerAnisotropy where the GPU has it (for
    // GL_EXT_texture_filter_anisotropic); its samplers may then be raised.
    bool anisotropy = ci.pEnabledFeatures && ci.pEnabledFeatures->samplerAnisotropy;
    for (auto* next = static_cast<const VkBaseInStructure*>(ci.pNext); next; next = next->pNext)
      if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2)
        anisotropy |= reinterpret_cast<const VkPhysicalDeviceFeatures2*>(next)->features.samplerAnisotropy == VK_TRUE;
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceFeatures features{};
    vk<PFN_vkGetPhysicalDeviceProperties>(gpu.instance, "vkGetPhysicalDeviceProperties")(physical, &properties);
    features.samplerAnisotropy = anisotropy;
    set_angle_anisotropy(anisotropy_level(properties, features));
  }
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
  if (r != VK_SUCCESS) {
    if (auto message = missing_vulkan_surface_support(); !message.empty()) throw std::runtime_error(message);
    throw std::runtime_error("Vulkan instance creation failed: " + std::to_string(r) +
                             (instance_failure.empty() ? "" : " (" + instance_failure + ")"));
  }
  VkPhysicalDevice p{};
  if (select_device(instance, &p) != VK_SUCCESS)
    throw std::runtime_error(device_failure.empty() ? "No Vulkan physical device"
                                                    : "No Vulkan physical device: " + device_failure);
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
// Media decoders place their shared images on this physical device.
void publish_host_gpu() {
  if (gpu.api_version < VK_API_VERSION_1_1) return;
  auto properties = vk<PFN_vkGetPhysicalDeviceProperties2>(gpu.instance, "vkGetPhysicalDeviceProperties2");
  if (!properties) return;
  VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
  VkPhysicalDeviceProperties2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
  properties(gpu.physical_device, &query);
  HostGpu host;
  std::memcpy(host.device_uuid.data(), id.deviceUUID, VK_UUID_SIZE);
  std::memcpy(host.driver_uuid.data(), id.driverUUID, VK_UUID_SIZE);
  set_host_gpu(host);
  note_shared_image_device(gpu.device,
                           std::ranges::find(device_enabled, shared_image_extension) != device_enabled.end());
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
    xr::log_loaded_vulkan_layers();
    injected_modules::log_loaded();
    publish_host_gpu();
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
  std::vector<VkExtensionProperties> extensions;
  if (!enumerate ||
      enumerate_vulkan(extensions, [&](std::uint32_t* n, VkExtensionProperties* p) { return enumerate(nullptr, n, p); }) < 0)
    return false;
  return std::ranges::any_of(extensions, [&](const auto& e) { return name == e.extensionName; });
}
std::string missing_vulkan_surface_support() {
#ifdef _WIN32
  // Every Windows GPU driver with Vulkan provides both; the loader lists neither
  // when no driver manifest it finds loads (a damaged or partial driver install).
  std::string missing;
  for (auto name : {"VK_KHR_surface", "VK_KHR_win32_surface"})
    if (!host_vulkan_instance_extension(name)) missing += std::string(missing.empty() ? "" : ", ") + name;
  if (!missing.empty())
    return "Your graphics driver's Vulkan support is not working (Vulkan lists no " + missing +
           "). Reinstall the latest driver for your GPU, then press Play again.";
#endif
  return {};
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
  if (gpu.device) foveation::release_device(gpu.device);
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
bool supports_filtered_blit(std::int64_t format) {
  // Never creates the device: without one there is nothing to present with.
  auto& device = gpu_device_state();
  if (!device.instance || !device.physical_device || !device.get_instance_proc_addr) return false;
  const auto query = reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(
      device.get_instance_proc_addr(device.instance, "vkGetPhysicalDeviceFormatProperties"));
  if (!query) return false;
  VkFormatProperties properties{};
  query(device.physical_device, VkFormat(format), &properties);
  constexpr VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
                                          VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
  return (properties.optimalTilingFeatures & needed) == needed;
}
}  // namespace venture::gfx

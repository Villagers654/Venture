#include "common/environment.h"
#include "gfx/angle.h"

#include <cstdlib>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/log.h"
#include "gfx/angle_callbacks.h"
#include "gfx/blob_cache.h"
#include "gfx/gpu_device_internal.h"
#include "platform/gfx_library.h"
#include "xr/host_xr.h"

namespace venture::gfx {
namespace {
std::mutex config_lock;
std::filesystem::path angle_directory;
bool started = false, shared = false;
bool general_layout = false;
void* egl_library = nullptr;
void* gles_library = nullptr;
EGLDisplay display = EGL_NO_DISPLAY;
std::string failure;
std::once_flag libraries_once, display_once;
PFNEGLGETPROCADDRESSPROC get_proc = nullptr;
using QueueLock = void (*)(EGLDisplay);
QueueLock queue_lock = nullptr, queue_unlock = nullptr;
void load_libraries() {
  std::lock_guard lock(config_lock);
  started = true;
  if (angle_directory.empty()) {
    if (const auto* path = venture::environment::getenv("VENTURE_ANGLE_DIR"))
      angle_directory = std::filesystem::absolute(path);
  }
  egl_library = platform::open_graphics_library(angle_directory /
                                                platform::egl_library_name());
  gles_library = platform::open_graphics_library(angle_directory /
                                                 platform::gles_library_name());
  get_proc = reinterpret_cast<PFNEGLGETPROCADDRESSPROC>(
      platform::graphics_symbol(egl_library, "eglGetProcAddress"));
  if (!egl_library || !gles_library || !get_proc)
    failure =
        "ANGLE libraries unavailable; set VENTURE_ANGLE_DIR to the pinned ANGLE "
        "directory";
}
void init_display() {
  std::call_once(libraries_once, load_libraries);
  if (!failure.empty()) {
    log::warn("gfx", "{}", failure);
    return;
  }
  auto query_string = angle_function<PFNEGLQUERYSTRINGPROC>("eglQueryString");
  const char* client_extensions =
      query_string ? query_string(EGL_NO_DISPLAY, EGL_EXTENSIONS) : nullptr;
  const bool branded_callbacks =
      client_extensions &&
      has_extension(client_extensions,
                    "EGL_VENTURE_vulkan_creation_callbacks");
  // The verified pre-rebrand ANGLE build has the identical numeric/layout ABI.
  constexpr auto legacy_callbacks_extension = "EGL_QUESTPORT_vulkan_creation_callbacks"; // rebrand: legacy ANGLE ABI
  const bool callbacks = branded_callbacks || (client_extensions &&
      has_extension(client_extensions, legacy_callbacks_extension));
  if (xr::host_xr().available && !callbacks) {
    failure =
        "XR requires ANGLE's Venture Vulkan creation callbacks; stock ANGLE "
        "is flat-only";
    log::error("gfx", "{}", failure);
    return;
  }
  auto get_display =
      angle_function<PFNEGLGETPLATFORMDISPLAYPROC>("eglGetPlatformDisplay");
  auto init = angle_function<PFNEGLINITIALIZEPROC>("eglInitialize");
  if (!get_display || !init) {
    failure = "ANGLE EGL platform entry points unavailable";
    return;
  }
  // EGL_ANGLE_platform_angle: explicitly Vulkan, never the Windows D3D default.
  constexpr EGLint platform_angle = 0x3202, type = 0x3203, vulkan = 0x3450;
  // The private callback pointer needs EGLAttrib-sized entries. EXT's EGLint
  // list truncates a host pointer on both supported 64-bit platforms.
  std::vector<EGLAttrib> attributes{type, vulkan};
  if (callbacks) {
    attributes.push_back(kAngleVulkanCallbacksAttribute);
    attributes.push_back(reinterpret_cast<EGLAttrib>(&angle_vulkan_callbacks()));
  }
  // ANGLE's Vulkan backend implements EXT_multisampled_render_to_texture with
  // transient multisample attachments and resolves. Its default emulation
  // policy selects mobile GPUs. Permit that implementation on desktop too,
  // retaining ANGLE's Vulkan depth/stencil resolve capability checks.
  // EGL_ANGLE_feature_control uses a null-terminated array of feature names.
  static const char* feature_overrides[]{"allowMultisampledRenderToTextureEmulation", nullptr};
  // warmUpPipelineCacheAtLink builds placeholder pipelines after every link,
  // and saving a program binary waits for them. Engines that save each
  // program (UE4's binary cache) then serialize that work on their render
  // thread. Pipelines are created at first use instead; the driver keeps
  // its own pipeline cache on the desktop GPUs where ANGLE skips its own.
  static const char* disabled_features[]{"warmUpPipelineCacheAtLink", nullptr};
  if (client_extensions && has_extension(client_extensions, "EGL_ANGLE_feature_control")) {
    attributes.push_back(0x3466 /* EGL_FEATURE_OVERRIDES_ENABLED_ANGLE */);
    attributes.push_back(reinterpret_cast<EGLAttrib>(feature_overrides));
    attributes.push_back(0x3467 /* EGL_FEATURE_OVERRIDES_DISABLED_ANGLE */);
    attributes.push_back(reinterpret_cast<EGLAttrib>(disabled_features));
  }
  attributes.push_back(EGL_NONE);
  auto candidate = get_display(platform_angle, EGL_DEFAULT_DISPLAY, attributes.data());
  EGLint major{}, minor{};
  if (!candidate || !init(candidate, &major, &minor)) {
    auto error = angle_function<PFNEGLGETERRORPROC>("eglGetError")();
    failure =
        std::format("ANGLE Vulkan initialization failed (EGL 0x{:x})", error);
    if (callbacks) discard_angle_device();
    log::error("gfx", "{}", failure);
    return;
  }
  display = candidate;
  queue_lock = angle_function<QueueLock>("eglLockVulkanQueueANGLE");
  queue_unlock = angle_function<QueueLock>("eglUnlockVulkanQueueANGLE");
  if (callbacks)
    shared = gpu_device_state().device && gpu_device_state().queue;
  else {
    auto query_display = angle_function<PFNEGLQUERYDISPLAYATTRIBEXTPROC>(
        "eglQueryDisplayAttribEXT");
    auto query_device = angle_function<PFNEGLQUERYDEVICEATTRIBEXTPROC>(
        "eglQueryDeviceAttribEXT");
    EGLAttrib device = 0;
    if (query_display && query_device &&
        query_display(display, EGL_DEVICE_EXT, &device)) {
      EGLAttrib values[9]{};
      // EGL_ANGLE_device_vulkan, including the original FAMILIY spelling.
      const EGLint keys[] = {0x34A9, 0x34AB, 0x34AC, 0x34AF, 0x34D0,
                             0x34D1, 0x34A8, 0x34AA, 0x34AD};
      bool valid = true;
      for (int n = 0; n < 9; ++n)
        valid &= query_device(reinterpret_cast<EGLDeviceEXT>(device), keys[n],
                              &values[n]) != EGL_FALSE;
      if (valid)
        shared = borrow_angle_device(
            reinterpret_cast<VkInstance>(values[0]),
            reinterpret_cast<VkPhysicalDevice>(values[1]),
            reinterpret_cast<VkDevice>(values[2]),
            reinterpret_cast<VkQueue>(values[3]),
            static_cast<std::uint32_t>(values[4]),
            reinterpret_cast<PFN_vkGetInstanceProcAddr>(values[5]),
            static_cast<std::uint32_t>(values[6]),
            reinterpret_cast<const char* const*>(values[7]),
            reinterpret_cast<const char* const*>(values[8]));
    }
  }
  if (!shared) {
    failure = "ANGLE did not provide a compatible shared device and queue";
    angle_function<PFNEGLTERMINATEPROC>("eglTerminate")(display);
    display = EGL_NO_DISPLAY;
    if (callbacks) discard_angle_device();
    log::error("gfx", "{}", failure);
    return;
  }
  auto query_display = angle_function<PFNEGLQUERYDISPLAYATTRIBEXTPROC>(
      "eglQueryDisplayAttribEXT");
  auto query_device =
      angle_function<PFNEGLQUERYDEVICEATTRIBEXTPROC>("eglQueryDeviceAttribEXT");
  EGLAttrib device{}, features{};
  if (callbacks) general_layout = gpu_unified_image_layouts_enabled();
  else if (query_display && query_device &&
      query_display(display, EGL_DEVICE_EXT, &device) &&
      query_device(reinterpret_cast<EGLDeviceEXT>(device),
                   0x34AE /* EGL_VULKAN_FEATURES_ANGLE */, &features)) {
    // The extension specifies VkPhysicalDeviceFeatures2 plus its enabled chain.
    for (auto* node = reinterpret_cast<const VkBaseInStructure*>(features);
         node; node = node->pNext)
      if (node->sType ==
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFIED_IMAGE_LAYOUTS_FEATURES_KHR)
        general_layout =
            reinterpret_cast<
                const VkPhysicalDeviceUnifiedImageLayoutsFeaturesKHR*>(node)
                ->unifiedImageLayouts;
  }
  log::info("gfx", "ANGLE Vulkan EGL {}.{}, shared device {}", major, minor,
            shared);
  install_blob_cache(display);
}
}  // namespace
bool configure_angle(const std::filesystem::path& directory) {
  std::lock_guard lock(config_lock);
  if (started) return false;
  angle_directory = std::filesystem::absolute(directory);
  return true;
}
bool initialize_angle() {
  std::call_once(display_once, init_display);
  return display != EGL_NO_DISPLAY;
}
EGLDisplay angle_display() {
  return initialize_angle() ? display : EGL_NO_DISPLAY;
}
bool release_thread_for_os_quit() noexcept {
  if (display == EGL_NO_DISPLAY) return true;
  try {
    auto release = angle_function<PFNEGLRELEASETHREADPROC>("eglReleaseThread");
    return release && release() != EGL_FALSE;
  } catch (...) { return false; }
}
bool terminate_for_os_quit() {
  if (display == EGL_NO_DISPLAY) return true;
  auto terminate = angle_function<PFNEGLTERMINATEPROC>("eglTerminate");
  if (!terminate || terminate(display) == EGL_FALSE) return false;
  display = EGL_NO_DISPLAY;
  return true;
}
void* angle_proc(const char* name) {
  std::call_once(libraries_once, load_libraries);
  if (!name) return nullptr;
  // HLE handlers resolve their entry points on every call, and an export
  // lookup costs microseconds (more when it fails), so resolve each name once.
  // Results never change after the libraries load.
  struct Hash {
    using is_transparent = void;
    std::size_t operator()(std::string_view text) const { return std::hash<std::string_view>{}(text); }
  };
  static std::shared_mutex lock;
  static std::unordered_map<std::string, void*, Hash, std::equal_to<>> procs;
  {
    std::shared_lock read(lock);
    if (auto it = procs.find(std::string_view(name)); it != procs.end()) return it->second;
  }
  // EGL names are libEGL exports and GL names libGLESv2 exports; look there first.
  const bool egl = std::string_view(name).starts_with("egl");
  void* p = platform::graphics_symbol(egl ? egl_library : gles_library, name);
  if (!p) p = platform::graphics_symbol(egl ? gles_library : egl_library, name);
  if (!p && get_proc) p = reinterpret_cast<void*>(get_proc(name));
  std::unique_lock write(lock);
  procs.emplace(name, p);
  return p;
}
bool has_extension(std::string_view list, std::string_view name) {
  if (name.empty() || name.find(' ') != name.npos) return false;
  for (std::size_t at = list.find(name); at != list.npos;
       at = list.find(name, at + name.size()))
    if ((at == 0 || list[at - 1] == ' ') &&
        (at + name.size() == list.size() || list[at + name.size()] == ' '))
      return true;
  return false;
}
const std::string& angle_error() { return failure; }
bool angle_shared_device() { return initialize_angle() && shared; }
bool angle_uses_general_layout() {
  return initialize_angle() && general_layout;
}
bool lock_angle_queue() {
  if (!queue_lock || !queue_unlock) return false;
  queue_lock(display);
  return true;
}
void unlock_angle_queue() {
  if (queue_unlock) queue_unlock(display);
}
}  // namespace venture::gfx

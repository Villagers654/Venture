// SPDX-License-Identifier: GPL-3.0-or-later
// xr::host_xr() inside a game on an Android headset: the Khronos loader (linked
// in) initialized with the game's Java VM and activity, and one instance on the
// device's runtime with the extensions Venture uses wherever it runs
// (xr/host_extensions.h) plus the Android ones, each only where offered.
#include "android_game/vrapi/platform_xr.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "android_game/game_log.h"
#include "android_game/vrapi/native.h"
#include "common/log.h"
#include "xr/api.h"
#include "xr/host_extensions.h"
#include "xr/host_xr.h"

namespace venture::vrapi_native {
namespace {
std::mutex java_mutex;
JavaVM *g_vm = nullptr;
jobject g_activity = nullptr;
}  // namespace
void remember_java(const hle::vrapi::abi::Java *java) {
  if (!java || !java->vm || !java->activity) return;
  std::lock_guard lock(java_mutex);
  if (g_vm) return;
  g_vm = reinterpret_cast<JavaVM *>(java->vm);
  // VrApi's ovrJava carries a global reference to the activity (VrApi_Types.h).
  g_activity = reinterpret_cast<jobject>(java->activity);
  // This library's records for the Venture app's debug report.
  JNIEnv *env = nullptr;
  if (g_vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) == JNI_OK ||
      g_vm->AttachCurrentThread(&env, nullptr) == JNI_OK)
    android_game::start_log(android_game::obb_dir(env, g_activity), "vrapi");
}
JavaVM *java_vm() {
  std::lock_guard lock(java_mutex);
  return g_vm;
}
jobject activity() {
  std::lock_guard lock(java_mutex);
  return g_activity;
}
}  // namespace venture::vrapi_native

namespace venture::xr {
namespace {
constexpr const char *kAndroidExtensions[] = {"XR_KHR_android_create_instance", "XR_KHR_opengl_es_enable",
                                              "XR_KHR_android_thread_settings"};
std::atomic<std::uint64_t> procedure_generation{1};

// The game's package name, for the runtime's application name.
std::string package_name(JavaVM *vm, jobject activity) {
  JNIEnv *env = nullptr;
  if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK &&
      vm->AttachCurrentThread(&env, nullptr) != JNI_OK)
    return {};
  auto cls = env->GetObjectClass(activity);
  auto method = env->GetMethodID(cls, "getPackageName", "()Ljava/lang/String;");
  std::string out;
  if (method) {
    if (auto name = static_cast<jstring>(env->CallObjectMethod(activity, method))) {
      const char *chars = env->GetStringUTFChars(name, nullptr);
      out = chars ? chars : "";
      if (chars) env->ReleaseStringUTFChars(name, chars);
      env->DeleteLocalRef(name);
    }
  }
  if (env->ExceptionCheck()) env->ExceptionClear();
  env->DeleteLocalRef(cls);
  return out;
}

void initialize(HostXr &h, JavaVM *vm, jobject activity) {
  PFN_xrInitializeLoaderKHR initialize_loader = nullptr;
  xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
                        reinterpret_cast<PFN_xrVoidFunction *>(&initialize_loader));
  XrLoaderInitInfoAndroidKHR loader{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR, nullptr, vm, activity};
  if (!initialize_loader ||
      XR_FAILED(initialize_loader(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR *>(&loader)))) {
    h.error = "The OpenXR loader could not start in this game";
    return;
  }
  std::uint32_t count = 0;
  if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr))) {
    h.error = "No OpenXR runtime is active on this headset";
    h.system_result = XR_ERROR_RUNTIME_UNAVAILABLE;
    return;
  }
  std::vector<XrExtensionProperties> extensions(count, {XR_TYPE_EXTENSION_PROPERTIES});
  xrEnumerateInstanceExtensionProperties(nullptr, count, &count, extensions.data());
  for (const auto &e : extensions) {
    h.available_extensions.emplace(e.extensionName, e.extensionVersion);
    const std::string_view name = e.extensionName;
    bool usable = false;
    for (auto wanted : kHostExtensions) usable |= wanted == name;
    for (auto wanted : kAndroidExtensions) usable |= wanted == name;
    if (usable) h.enabled_extensions.insert(e.extensionName);
  }
  if (!h.enabled_extensions.contains("XR_KHR_opengl_es_enable") && !h.enabled_extensions.contains("XR_KHR_vulkan_enable")) {
    h.error = "The headset's OpenXR runtime supports neither OpenGL ES nor Vulkan";
    return;
  }
  std::vector<const char *> names;
  for (const auto &name : h.enabled_extensions) names.push_back(name.c_str());
  XrInstanceCreateInfoAndroidKHR android{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR, nullptr, vm, activity};
  XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
  if (h.enabled_extensions.contains("XR_KHR_android_create_instance")) info.next = &android;
  const auto package = package_name(vm, activity);
  std::strncpy(info.applicationInfo.applicationName, package.empty() ? "Venture" : package.c_str(),
               XR_MAX_APPLICATION_NAME_SIZE - 1);
  std::strncpy(info.applicationInfo.engineName, "Venture VrApi", XR_MAX_ENGINE_NAME_SIZE - 1);
  info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
  info.enabledExtensionCount = std::uint32_t(names.size());
  info.enabledExtensionNames = names.data();
  auto result = xrCreateInstance(&info, &h.instance);
  log::info("xr", "Host xrCreateInstance result={}", int(result));
  if (XR_FAILED(result)) {
    h.error = "The headset's OpenXR runtime refused an instance";
    return;
  }
  XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
  if (XR_SUCCEEDED(xrGetInstanceProperties(h.instance, &properties))) {
    h.runtime_name = properties.runtimeName;
    h.runtime_version = properties.runtimeVersion;
  }
  h.get_instance_proc_addr = reinterpret_cast<void *>(&xrGetInstanceProcAddr);
  XrSystemGetInfo system{XR_TYPE_SYSTEM_GET_INFO, nullptr, XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
  result = xrGetSystem(h.instance, &system, &h.system);
  h.system_result = result;
  if (XR_FAILED(result)) {
    h.error = "The headset's OpenXR runtime reports no headset";
    return;
  }
  h.available = true;
  log::info("xr", "Host runtime {} with {} of its {} extensions", h.runtime_name, h.enabled_extensions.size(),
            h.available_extensions.size());
}
}  // namespace

HostXr &host_xr() {
  static HostXr h;
  static std::mutex mutex;
  static bool started = false;
  std::lock_guard lock(mutex);
  // Without the game's Java VM yet (a query before vrapi_Initialize), the
  // runtime cannot be reached; the next call tries again.
  if (!started) {
    if (auto *vm = vrapi_native::java_vm()) {
      started = true;
      initialize(h, vm, vrapi_native::activity());
      if (!h.available) log::warn("xr", "VR is unavailable: {}", h.error);
    }
  }
  return h;
}
std::uint64_t host_procedure_generation() { return procedure_generation.load(std::memory_order_acquire); }
void invalidate_host_procedures() { procedure_generation.fetch_add(1, std::memory_order_acq_rel); }
}  // namespace venture::xr

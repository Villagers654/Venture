// The Quest layer's records (records.h).
#include <ctime>
#if __has_include(<vulkan/vulkan.h>)
#include <vulkan/vulkan.h>
#define XR_USE_GRAPHICS_API_VULKAN
#endif
#define XR_USE_TIMESPEC
#if defined(__ANDROID__)
#include <EGL/egl.h>
#include <android/log.h>
#include <jni.h>
#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#endif
#include "quest_layer/records.h"

#include <openxr/openxr_platform.h>
#include <openxr/openxr_reflection.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <system_error>
#include <utility>
#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace venture::quest_layer::records {
namespace {
// The functions whose errors are recorded: the core and the KHR, EXT, FB and
// META extensions a Quest game may call (openxr_reflection.h lists them).
#if defined(XR_USE_GRAPHICS_API_VULKAN)
#define VENTURE_VULKAN_FUNCTIONS(_) XR_LIST_FUNCTIONS_XR_KHR_vulkan_enable(_) XR_LIST_FUNCTIONS_XR_KHR_vulkan_enable2(_)
#else
#define VENTURE_VULKAN_FUNCTIONS(_)
#endif
#if defined(__ANDROID__)
#define VENTURE_ANDROID_FUNCTIONS(_)                                                              \
  XR_LIST_FUNCTIONS_XR_KHR_android_thread_settings(_) XR_LIST_FUNCTIONS_XR_KHR_android_surface_swapchain(_) \
  XR_LIST_FUNCTIONS_XR_KHR_opengl_es_enable(_)
#else
#define VENTURE_ANDROID_FUNCTIONS(_)
#endif
#define VENTURE_FUNCTIONS(_)                                                                                       \
  XR_LIST_FUNCTIONS_XR_VERSION_1_0(_)                                                                              \
  XR_LIST_FUNCTIONS_XR_VERSION_1_1(_)                                                                              \
  XR_LIST_FUNCTIONS_XR_KHR_visibility_mask(_)                                                                      \
  XR_LIST_FUNCTIONS_XR_KHR_locate_spaces(_)                                                                        \
  XR_LIST_FUNCTIONS_XR_KHR_convert_timespec_time(_)                                                                \
  XR_LIST_FUNCTIONS_XR_EXT_performance_settings(_)                                                                 \
  XR_LIST_FUNCTIONS_XR_EXT_thermal_query(_)                                                                        \
  XR_LIST_FUNCTIONS_XR_EXT_debug_utils(_)                                                                          \
  XR_LIST_FUNCTIONS_XR_EXT_hand_tracking(_)                                                                        \
  XR_LIST_FUNCTIONS_XR_EXT_plane_detection(_)                                                                      \
  XR_LIST_FUNCTIONS_XR_EXT_future(_)                                                                               \
  XR_LIST_FUNCTIONS_XR_EXT_render_model(_)                                                                         \
  XR_LIST_FUNCTIONS_XR_EXT_interaction_render_model(_)                                                             \
  XR_LIST_FUNCTIONS_XR_FB_swapchain_update_state(_)                                                                \
  XR_LIST_FUNCTIONS_XR_FB_body_tracking(_)                                                                         \
  XR_LIST_FUNCTIONS_XR_FB_display_refresh_rate(_)                                                                  \
  XR_LIST_FUNCTIONS_XR_FB_color_space(_)                                                                           \
  XR_LIST_FUNCTIONS_XR_FB_hand_tracking_mesh(_)                                                                    \
  XR_LIST_FUNCTIONS_XR_FB_spatial_entity(_)                                                                        \
  XR_LIST_FUNCTIONS_XR_FB_foveation(_)                                                                             \
  XR_LIST_FUNCTIONS_XR_FB_keyboard_tracking(_)                                                                     \
  XR_LIST_FUNCTIONS_XR_FB_triangle_mesh(_)                                                                         \
  XR_LIST_FUNCTIONS_XR_FB_passthrough(_)                                                                           \
  XR_LIST_FUNCTIONS_XR_FB_render_model(_)                                                                          \
  XR_LIST_FUNCTIONS_XR_FB_spatial_entity_query(_)                                                                  \
  XR_LIST_FUNCTIONS_XR_FB_spatial_entity_storage(_)                                                                \
  XR_LIST_FUNCTIONS_XR_FB_spatial_entity_sharing(_)                                                                \
  XR_LIST_FUNCTIONS_XR_FB_scene(_)                                                                                 \
  XR_LIST_FUNCTIONS_XR_FB_scene_capture(_)                                                                         \
  XR_LIST_FUNCTIONS_XR_FB_spatial_entity_container(_)                                                              \
  XR_LIST_FUNCTIONS_XR_FB_face_tracking(_)                                                                         \
  XR_LIST_FUNCTIONS_XR_FB_face_tracking2(_)                                                                        \
  XR_LIST_FUNCTIONS_XR_FB_eye_tracking_social(_)                                                                   \
  XR_LIST_FUNCTIONS_XR_FB_passthrough_keyboard_hands(_)                                                            \
  XR_LIST_FUNCTIONS_XR_FB_haptic_pcm(_)                                                                            \
  XR_LIST_FUNCTIONS_XR_FB_spatial_entity_storage_batch(_)                                                          \
  XR_LIST_FUNCTIONS_XR_FB_spatial_entity_user(_)                                                                   \
  XR_LIST_FUNCTIONS_XR_META_foveation_eye_tracked(_)                                                               \
  XR_LIST_FUNCTIONS_XR_META_passthrough_preferences(_)                                                             \
  XR_LIST_FUNCTIONS_XR_META_virtual_keyboard(_)                                                                    \
  XR_LIST_FUNCTIONS_XR_META_performance_metrics(_)                                                                 \
  XR_LIST_FUNCTIONS_XR_META_spatial_entity_discovery(_)                                                            \
  XR_LIST_FUNCTIONS_XR_META_recommended_layer_resolution(_)                                                        \
  XR_LIST_FUNCTIONS_XR_META_spatial_entity_persistence(_)                                                          \
  XR_LIST_FUNCTIONS_XR_META_passthrough_color_lut(_)                                                               \
  XR_LIST_FUNCTIONS_XR_META_spatial_entity_mesh(_)                                                                 \
  XR_LIST_FUNCTIONS_XR_META_body_tracking_calibration(_)                                                           \
  XR_LIST_FUNCTIONS_XR_META_body_tracking_fidelity(_)                                                              \
  XR_LIST_FUNCTIONS_XR_META_spatial_entity_sharing(_)                                                              \
  XR_LIST_FUNCTIONS_XR_META_environment_depth(_)                                                                   \
  XR_LIST_FUNCTIONS_XR_META_boundary_visibility(_)                                                                 \
  XR_LIST_FUNCTIONS_XR_META_simultaneous_hands_and_controllers(_)                                                  \
  XR_LIST_FUNCTIONS_XR_META_spatial_entity_room_mesh(_)                                                            \
  XR_LIST_FUNCTIONS_XR_META_colocation_discovery(_)                                                                \
  XR_LIST_FUNCTIONS_XR_META_environment_raycast(_)                                                                 \
  XR_LIST_FUNCTIONS_XR_META_hand_tracking_frequency_hint(_)                                                        \
  VENTURE_VULKAN_FUNCTIONS(_)                                                                                      \
  VENTURE_ANDROID_FUNCTIONS(_)

#define VENTURE_INDEX(name, extension) k##name,
enum Index : std::size_t { VENTURE_FUNCTIONS(VENTURE_INDEX) kCount };
#undef VENTURE_INDEX

// The target of each wrapper: the runtime's function, or the layer's own. The
// first instance's targets are kept while instances live; a later instance
// that resolves another target gets that target unwrapped.
std::atomic<PFN_xrVoidFunction> targets[kCount];
std::mutex check_mutex;     // targets and fresh
bool fresh[kCount];         // set: the target may be replaced (no instance kept it)
bool all_fresh = true;
std::map<std::pair<std::size_t, XrResult>, std::uint64_t> errors;  // under check_mutex

void failed(std::size_t index, XrResult result);

template <typename F, std::size_t I>
struct Wrap;
template <typename... A, std::size_t I>
struct Wrap<XrResult(XRAPI_PTR*)(A...), I> {
  static XRAPI_ATTR XrResult XRAPI_CALL call(A... args) {
    const auto target = reinterpret_cast<XrResult(XRAPI_PTR*)(A...)>(targets[I].load(std::memory_order_acquire));
    const XrResult result = target(args...);
    if (XR_FAILED(result)) [[unlikely]]
      failed(I, result);
    return result;
  }
};

struct Entry {
  const char* name;
  PFN_xrVoidFunction wrapper;
};
#define VENTURE_ENTRY(name, extension) \
  {"xr" #name, reinterpret_cast<PFN_xrVoidFunction>(&Wrap<PFN_xr##name, k##name>::call)},
const Entry kEntries[] = {VENTURE_FUNCTIONS(VENTURE_ENTRY)};
#undef VENTURE_ENTRY

#if defined(__aarch64__)
constexpr char kHeader[] = "questlayer for arm64-v8a";
#elif defined(__arm__)
constexpr char kHeader[] = "questlayer for armeabi-v7a";
#else
constexpr char kHeader[] = "questlayer";
#endif

std::mutex file_mutex;
std::FILE* file = nullptr;

bool power_of_ten(std::uint64_t n) {
  while (n >= 10 && n % 10 == 0) n /= 10;
  return n == 1;
}

void failed(std::size_t index, XrResult result) {
  std::uint64_t count = 0;
  {
    std::lock_guard lock(check_mutex);
    count = ++errors[{index, result}];
  }
  if (!power_of_ten(count)) return;
  auto line = std::string(kEntries[index].name) + " returned " + result_name(result);
  if (count > 1) line += " (" + std::to_string(count) + " times)";
  write(line);
}

std::filesystem::path from_environment() {
  const char* path = std::getenv("VENTURE_QUEST_LAYER_LOG");
  if (!path || !*path) return {};
#if defined(_WIN32)
  return std::filesystem::path(reinterpret_cast<const char8_t*>(path));
#else
  return path;
#endif
}

#if defined(__ANDROID__)
// Context.getObbDir() of the game's activity, from the create info.
std::filesystem::path obb_folder(const XrInstanceCreateInfo& info) {
  const XrInstanceCreateInfoAndroidKHR* android = nullptr;
  for (auto* next = static_cast<const XrBaseInStructure*>(info.next); next; next = next->next)
    if (next->type == XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR)
      android = reinterpret_cast<const XrInstanceCreateInfoAndroidKHR*>(next);
  if (!android || !android->applicationVM || !android->applicationActivity) return {};
  auto* vm = static_cast<JavaVM*>(android->applicationVM);
  auto activity = static_cast<jobject>(android->applicationActivity);
  JNIEnv* env = nullptr;
  bool attached = false;
  const auto state = vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
  if (state == JNI_EDETACHED) {
    if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return {};
    attached = true;
  } else if (state != JNI_OK) {
    return {};
  }
  std::filesystem::path out;
  jclass cls = env->GetObjectClass(activity);
  jmethodID get = cls ? env->GetMethodID(cls, "getObbDir", "()Ljava/io/File;") : nullptr;
  jobject folder = get ? env->CallObjectMethod(activity, get) : nullptr;
  if (folder && !env->ExceptionCheck()) {
    jclass file_class = env->GetObjectClass(folder);
    jmethodID path = env->GetMethodID(file_class, "getAbsolutePath", "()Ljava/lang/String;");
    if (auto text = path ? static_cast<jstring>(env->CallObjectMethod(folder, path)) : nullptr) {
      if (const char* chars = env->GetStringUTFChars(text, nullptr)) {
        out = chars;
        env->ReleaseStringUTFChars(text, chars);
      }
      env->DeleteLocalRef(text);
    }
    env->DeleteLocalRef(file_class);
  }
  if (env->ExceptionCheck()) env->ExceptionClear();
  if (folder) env->DeleteLocalRef(folder);
  if (cls) env->DeleteLocalRef(cls);
  if (attached) vm->DetachCurrentThread();
  return out;
}
#endif
}  // namespace

void start(const XrInstanceCreateInfo& info) {
  std::lock_guard lock(file_mutex);
  if (file) return;
  auto path = from_environment();
#if defined(__ANDROID__)
  // Beside the other Venture libraries' logs (android_game/game_log.h).
  if (path.empty())
    if (const auto obb = obb_folder(info); !obb.empty()) path = obb / "venture" / "questlayer.log";
#else
  (void)info;
#endif
  if (path.empty()) return;
  std::error_code ec;
  const auto folder = path.parent_path();
  if (!folder.empty()) std::filesystem::create_directories(folder, ec);
  // The loader unloads the layer with each instance, so a game that makes
  // several instances loads it several times: the file is this process's when
  // its first line names this process, and goes on.
#if defined(_WIN32)
  const auto process = static_cast<long>(_getpid());
#else
  const auto process = static_cast<long>(getpid());
#endif
  const auto header = std::string(kHeader) + ", process " + std::to_string(process) + "\n";
  bool ours = false;
  if (std::ifstream in(path, std::ios::binary); in) {
    std::string first;
    std::getline(in, first);
    ours = first + "\n" == header;
  }
  if (!ours) {
    auto previous = path;
    previous.replace_filename(path.stem().string() + ".prev.log");
    std::filesystem::rename(path, previous, ec);
  }
#if defined(_WIN32)
  file = _wfopen(path.c_str(), ours ? L"ab" : L"wb");
#else
  file = std::fopen(path.c_str(), ours ? "ab" : "wb");
#endif
  if (!file) return;
  if (!ours) std::fputs(header.c_str(), file);
#if defined(__ANDROID__)
  // As game_log.cpp: readable by the Venture app, which reads the game's OBB
  // folder through its group.
  using std::filesystem::perms;
  std::filesystem::permissions(folder, perms::group_read | perms::group_exec | perms::others_read | perms::others_exec,
                               std::filesystem::perm_options::add, ec);
  std::filesystem::permissions(path, perms::group_read | perms::others_read, std::filesystem::perm_options::add, ec);
#endif
  std::fflush(file);
}

void write(std::string_view line) {
#if defined(__ANDROID__)
  __android_log_print(ANDROID_LOG_INFO, "VentureQuestLayer", "%.*s", static_cast<int>(line.size()), line.data());
#endif
  // Local wall-clock time, as the other logs (the layer may be loaded more
  // than once per process).
  const auto now = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  const auto millis =
      std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
  std::tm local{};
#if defined(_WIN32)
  localtime_s(&local, &time);
#else
  localtime_r(&time, &local);
#endif
  char stamp[32];
  std::snprintf(stamp, sizeof stamp, "[%02d:%02d:%02d.%03d] ", local.tm_hour, local.tm_min, local.tm_sec,
                static_cast<int>(millis));
  std::string text = stamp;
  text.append(line);
  text += '\n';
  std::lock_guard lock(file_mutex);
  if (!file) return;
  std::fwrite(text.data(), 1, text.size(), file);
  std::fflush(file);
}

std::string result_name(XrResult result) {
  switch (result) {
#define VENTURE_RESULT(name, value) \
  case name:                        \
    return #name;
    XR_LIST_ENUM_XrResult(VENTURE_RESULT)
#undef VENTURE_RESULT
    default:
      return "XrResult " + std::to_string(static_cast<int>(result));
  }
}

PFN_xrVoidFunction checked(const char* name, PFN_xrVoidFunction target) {
  if (!name || !target || std::strcmp(name, "xrGetInstanceProcAddr") == 0) return target;
  for (std::size_t i = 0; i < kCount; ++i) {
    if (std::strcmp(name, kEntries[i].name) != 0) continue;
    std::lock_guard lock(check_mutex);
    if (all_fresh) {
      std::fill(std::begin(fresh), std::end(fresh), true);
      all_fresh = false;
    }
    const auto current = targets[i].load(std::memory_order_relaxed);
    if (fresh[i] || !current) {
      targets[i].store(target, std::memory_order_release);
      fresh[i] = false;
    } else if (current != target) {
      return target;
    }
    return kEntries[i].wrapper;
  }
  return target;
}

void summary(bool last) {
  std::string line;
  {
    std::lock_guard lock(check_mutex);
    for (const auto& [key, count] : errors) {
      line += line.empty() ? "errors since the last summary: " : ", ";
      line += std::string(kEntries[key.first].name) + " " + result_name(key.second) + " x" + std::to_string(count);
    }
    errors.clear();
    // The wrappers keep their targets for a game that still calls them; the
    // next instance may replace them.
    if (last) all_fresh = true;
  }
  if (!line.empty()) write(line);
}
}  // namespace venture::quest_layer::records

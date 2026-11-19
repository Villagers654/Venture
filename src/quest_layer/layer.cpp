#include "quest_layer/layer.h"

#include "quest_layer/frame_timing.h"
#include "quest_layer/records.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace venture::quest_layer {
namespace {
constexpr Extension kEmulated[] = {
#include "quest_layer/extensions.inc"
};

// XR_KHR_android_thread_settings is declared under XR_USE_PLATFORM_ANDROID;
// the layer answers it on every platform, so its one function is spelled out.
using PFN_setAndroidThread = XrResult(XRAPI_PTR*)(XrSession, std::int32_t, std::uint32_t);

struct Instance {
  XrInstance handle = XR_NULL_HANDLE;
  PFN_xrGetInstanceProcAddr next = nullptr;
  std::set<std::string, std::less<>> emulated;  // enabled by the game, answered here
  bool lowered = false;                          // runtime refused 1.1; created at 1.0
  bool runtime_local_floor = false;              // XR_EXT_local_floor enabled downstream
  bool runtime_refresh = false;                  // the runtime's XR_FB_display_refresh_rate is in use
  std::atomic<std::int64_t> display_period{0};   // from xrWaitFrame, for the emulated refresh rate
  std::atomic<std::uint32_t> refresh_fallbacks{0};
  PFN_xrDestroyInstance destroy_instance = nullptr;
  PFN_xrCreateSession create_session = nullptr;
  PFN_xrDestroySession destroy_session = nullptr;
  PFN_xrWaitFrame wait_frame = nullptr;
  PFN_xrEndFrame end_frame = nullptr;
  std::unique_ptr<FrameTiming> timing;  // the session's frame timing (frame_timing.h)
  XrSession timing_session = XR_NULL_HANDLE;
  PFN_xrLocateSpace locate_space = nullptr;
  PFN_xrCreateReferenceSpace create_reference_space = nullptr;
  PFN_xrEnumerateReferenceSpaces enumerate_reference_spaces = nullptr;
  PFN_xrEnumerateDisplayRefreshRatesFB enumerate_refresh_rates = nullptr;
  PFN_xrRequestDisplayRefreshRateFB request_refresh_rate = nullptr;
  bool emulates(std::string_view name) const { return emulated.find(name) != emulated.end(); }
};

std::mutex mutex;
std::unordered_map<XrInstance, std::unique_ptr<Instance>> instances;
std::unordered_map<XrSession, Instance*> sessions;
Instance* newest = nullptr;
// The next link of the latest xrCreateApiLayerInstance, for calls without an
// instance (global functions).
std::atomic<PFN_xrGetInstanceProcAddr> global_next{nullptr};
// The newest session and its instance, for the frame loop's calls without the
// lock (frame_instance).
std::atomic<XrSession> hot_session{XR_NULL_HANDLE};
std::atomic<Instance*> hot_instance{nullptr};
std::atomic<std::uint32_t> session_count{0};

Instance* find_instance(XrInstance handle) {
  std::lock_guard lock(mutex);
  const auto found = instances.find(handle);
  return found == instances.end() ? nullptr : found->second.get();
}
Instance* find_session(XrSession handle) {
  std::lock_guard lock(mutex);
  const auto found = sessions.find(handle);
  return found == sessions.end() ? nullptr : found->second;
}

template <typename T>
XrResult enumerate(std::uint32_t capacity, std::uint32_t* count, T* out, const std::vector<T>& values) {
  if (!count) return XR_ERROR_VALIDATION_FAILURE;
  *count = static_cast<std::uint32_t>(values.size());
  if (!capacity) return XR_SUCCESS;
  if (capacity < values.size()) return XR_ERROR_SIZE_INSUFFICIENT;
  if (!out) return XR_ERROR_VALIDATION_FAILURE;
  std::copy(values.begin(), values.end(), out);
  return XR_SUCCESS;
}

template <typename F>
F resolve(PFN_xrGetInstanceProcAddr next, XrInstance instance, const char* name) {
  PFN_xrVoidFunction function = nullptr;
  if (XR_FAILED(next(instance, name, &function))) return nullptr;
  return reinterpret_cast<F>(function);
}

std::set<std::string, std::less<>> runtime_extensions(PFN_xrGetInstanceProcAddr next) {
  std::set<std::string, std::less<>> names;
  const auto list = resolve<PFN_xrEnumerateInstanceExtensionProperties>(
      next, XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties");
  std::uint32_t count = 0;
  if (!list || XR_FAILED(list(nullptr, 0, &count, nullptr))) return names;
  std::vector<XrExtensionProperties> properties(count, {XR_TYPE_EXTENSION_PROPERTIES});
  if (XR_FAILED(list(nullptr, count, &count, properties.data()))) return names;
  for (std::uint32_t i = 0; i < count; ++i) names.emplace(properties[i].extensionName);
  return names;
}

const Extension* emulated_extension(std::string_view name) {
  for (const auto& extension : kEmulated)
    if (name == extension.name) return &extension;
  return nullptr;
}

// The rate a Quest game sees on a runtime without XR_FB_display_refresh_rate:
// the display period the runtime reports in xrWaitFrame. Before the first
// frame there is none; 90 Hz stands in until then.
float display_rate(const Instance& instance) {
  const auto period = instance.display_period.load(std::memory_order_relaxed);
  if (period <= 0) return 90.0f;
  return std::round(1e11f / static_cast<float>(period)) / 100.0f;
}

std::string version_text(XrVersion version) {
  return std::to_string(XR_VERSION_MAJOR(version)) + "." + std::to_string(XR_VERSION_MINOR(version)) + "." +
         std::to_string(XR_VERSION_PATCH(version));
}

std::string joined(const auto& names) {
  std::string out;
  for (const auto& name : names) {
    if (!out.empty()) out += ", ";
    out += name;
  }
  return out.empty() ? "none" : out;
}

std::string rate_text(float rate) {
  char text[32];
  std::snprintf(text, sizeof text, "%.2f Hz", rate);
  return text;
}

// --- Functions answered here ----------------------------------------------

XRAPI_ATTR XrResult XRAPI_CALL destroy_instance(XrInstance handle) {
  std::unique_ptr<Instance> owned;
  bool last = false;
  {
    std::lock_guard lock(mutex);
    const auto found = instances.find(handle);
    if (found == instances.end()) return XR_ERROR_HANDLE_INVALID;
    owned = std::move(found->second);
    instances.erase(found);
    std::erase_if(sessions, [&](const auto& entry) { return entry.second == owned.get(); });
    if (newest == owned.get()) newest = nullptr;
    if (hot_instance.load(std::memory_order_relaxed) == owned.get()) hot_session.store(XR_NULL_HANDLE);
    last = instances.empty();
  }
  const auto result = owned->destroy_instance(handle);
  owned->timing.reset();  // its final summary
  records::summary(last);
  records::write("instance destroyed");
  return result;
}

XRAPI_ATTR XrResult XRAPI_CALL create_session(XrInstance handle, const XrSessionCreateInfo* info, XrSession* session) {
  auto* instance = find_instance(handle);
  if (!instance) return XR_ERROR_HANDLE_INVALID;
  const auto result = instance->create_session(handle, info, session);
  if (XR_SUCCEEDED(result)) {
    // An instance has one session at a time; its frame timing starts here.
    auto timing = std::make_unique<FrameTiming>(session_count.fetch_add(1, std::memory_order_relaxed) + 1);
    std::unique_ptr<FrameTiming> previous;
    {
      std::lock_guard lock(mutex);
      sessions[*session] = instance;
      previous = std::move(instance->timing);
      instance->timing = std::move(timing);
      instance->timing_session = *session;
      hot_instance.store(instance, std::memory_order_release);
      hot_session.store(*session, std::memory_order_release);
    }
  }
  return result;
}

XRAPI_ATTR XrResult XRAPI_CALL destroy_session(XrSession session) {
  auto* instance = find_session(session);
  if (!instance) return XR_ERROR_HANDLE_INVALID;
  const auto result = instance->destroy_session(session);
  if (XR_SUCCEEDED(result)) {
    std::unique_ptr<FrameTiming> timing;  // its final summary, outside the lock
    std::lock_guard lock(mutex);
    sessions.erase(session);
    if (hot_session.load(std::memory_order_relaxed) == session) hot_session.store(XR_NULL_HANDLE);
    if (instance->timing_session == session) {
      timing = std::move(instance->timing);
      instance->timing_session = XR_NULL_HANDLE;
    }
  }
  return result;
}

// The instance of a session in the frame loop: the newest session's without a
// lock, the others' through the map.
Instance* frame_instance(XrSession session) {
  if (session != XR_NULL_HANDLE && session == hot_session.load(std::memory_order_acquire))
    return hot_instance.load(std::memory_order_acquire);
  return find_session(session);
}

XRAPI_ATTR XrResult XRAPI_CALL wait_frame(XrSession session, const XrFrameWaitInfo* info, XrFrameState* state) {
  auto* instance = frame_instance(session);
  if (!instance) return XR_ERROR_HANDLE_INVALID;
  const auto result = instance->wait_frame(session, info, state);
  if (XR_SUCCEEDED(result) && state && state->predictedDisplayPeriod > 0) {
    instance->display_period.store(state->predictedDisplayPeriod, std::memory_order_relaxed);
    if (auto* timing = instance->timing.get()) timing->waited(state->predictedDisplayPeriod);
  }
  return result;
}

XRAPI_ATTR XrResult XRAPI_CALL end_frame(XrSession session, const XrFrameEndInfo* info) {
  auto* instance = frame_instance(session);
  if (!instance) return XR_ERROR_HANDLE_INVALID;
  if (auto* timing = instance->timing.get()) timing->ended();
  return instance->end_frame(session, info);
}

// xrLocateSpaces (1.1 core) and xrLocateSpacesKHR, one xrLocateSpace per space.
XRAPI_ATTR XrResult XRAPI_CALL locate_spaces(XrSession session, const XrSpacesLocateInfo* info,
                                             XrSpaceLocations* locations) {
  auto* instance = find_session(session);
  if (!instance) return XR_ERROR_HANDLE_INVALID;
  if (!info || !locations || info->type != XR_TYPE_SPACES_LOCATE_INFO || locations->type != XR_TYPE_SPACE_LOCATIONS ||
      info->spaceCount != locations->locationCount || (info->spaceCount && (!info->spaces || !locations->locations)))
    return XR_ERROR_VALIDATION_FAILURE;
  XrSpaceVelocities* velocities = nullptr;
  for (auto* next = static_cast<XrBaseOutStructure*>(locations->next); next; next = next->next)
    if (next->type == XR_TYPE_SPACE_VELOCITIES) velocities = reinterpret_cast<XrSpaceVelocities*>(next);
  if (velocities && (velocities->velocityCount != info->spaceCount || (info->spaceCount && !velocities->velocities)))
    return XR_ERROR_VALIDATION_FAILURE;
  VENTURE_QUEST_RECORD_ONCE(std::string("shim: xrLocateSpaces answered with one xrLocateSpace per space"));
  for (std::uint32_t i = 0; i < info->spaceCount; ++i) {
    XrSpaceVelocity velocity{XR_TYPE_SPACE_VELOCITY};
    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION, velocities ? &velocity : nullptr};
    const auto result = instance->locate_space(info->spaces[i], info->baseSpace, info->time, &location);
    if (XR_FAILED(result)) return result;
    locations->locations[i] = {location.locationFlags, location.pose};
    if (velocities)
      velocities->velocities[i] = {velocity.velocityFlags, velocity.linearVelocity, velocity.angularVelocity};
  }
  return XR_SUCCESS;
}

// LOCAL_FLOOR (1.1 core) on a 1.0 runtime: the runtime's XR_EXT_local_floor
// space where it has one (the same enum value), otherwise its STAGE space,
// which is also floor-level.
XRAPI_ATTR XrResult XRAPI_CALL create_reference_space(XrSession session, const XrReferenceSpaceCreateInfo* info,
                                                      XrSpace* space) {
  auto* instance = find_session(session);
  if (!instance) return XR_ERROR_HANDLE_INVALID;
  if (info && info->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR && !instance->runtime_local_floor) {
    VENTURE_QUEST_RECORD_ONCE(std::string("shim: the LOCAL_FLOOR space is the runtime's STAGE space"));
    auto stage = *info;
    stage.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    return instance->create_reference_space(session, &stage, space);
  }
  return instance->create_reference_space(session, info, space);
}

XRAPI_ATTR XrResult XRAPI_CALL enumerate_reference_spaces(XrSession session, std::uint32_t capacity,
                                                          std::uint32_t* count, XrReferenceSpaceType* spaces) {
  auto* instance = find_session(session);
  if (!instance) return XR_ERROR_HANDLE_INVALID;
  std::uint32_t available = 0;
  auto result = instance->enumerate_reference_spaces(session, 0, &available, nullptr);
  if (XR_FAILED(result)) return result;
  std::vector<XrReferenceSpaceType> values(available);
  result = instance->enumerate_reference_spaces(session, available, &available, values.data());
  if (XR_FAILED(result)) return result;
  values.resize(available);
  const bool floor = std::ranges::find(values, XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR) != values.end();
  const bool stage = std::ranges::find(values, XR_REFERENCE_SPACE_TYPE_STAGE) != values.end();
  if (!floor && stage) {
    VENTURE_QUEST_RECORD_ONCE(std::string("shim: LOCAL_FLOOR added to the runtime's reference spaces"));
    values.push_back(XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR);
  }
  return enumerate(capacity, count, spaces, values);
}

XRAPI_ATTR XrResult XRAPI_CALL enumerate_refresh_rates(XrSession session, std::uint32_t capacity,
                                                       std::uint32_t* count, float* rates) {
  auto* instance = find_session(session);
  if (!instance) return XR_ERROR_HANDLE_INVALID;
  const float rate = display_rate(*instance);
  VENTURE_QUEST_RECORD_ONCE("shim: refresh rates emulated; the game sees the display rate, " + rate_text(rate));
  return enumerate(capacity, count, rates, std::vector<float>{rate});
}

XRAPI_ATTR XrResult XRAPI_CALL get_refresh_rate(XrSession session, float* rate) {
  auto* instance = find_session(session);
  if (!instance) return XR_ERROR_HANDLE_INVALID;
  if (!rate) return XR_ERROR_VALIDATION_FAILURE;
  *rate = display_rate(*instance);
  return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL request_refresh_rate(XrSession session, float rate) {
  auto* instance = find_session(session);
  if (!instance) return XR_ERROR_HANDLE_INVALID;
  if (!instance->runtime_refresh) {
    // Emulated: the one listed rate, or 0 (the runtime's choice).
    if (rate == 0.0f || std::fabs(rate - display_rate(*instance)) < 0.5f) return XR_SUCCESS;
    return XR_ERROR_DISPLAY_REFRESH_RATE_UNSUPPORTED_FB;
  }
  const auto result = instance->request_refresh_rate(session, rate);
  if (result != XR_ERROR_DISPLAY_REFRESH_RATE_UNSUPPORTED_FB && result != XR_ERROR_FEATURE_UNSUPPORTED) return result;
  // The runtime lists the rate but will not switch to it: a Quest game treats
  // a listed rate as granted, so the request succeeds at the runtime's rate.
  std::uint32_t count = 0;
  if (XR_FAILED(instance->enumerate_refresh_rates(session, 0, &count, nullptr))) return result;
  std::vector<float> listed(count);
  if (XR_FAILED(instance->enumerate_refresh_rates(session, count, &count, listed.data()))) return result;
  if (std::ranges::none_of(listed, [&](float value) { return std::fabs(value - rate) < 0.5f; })) return result;
  instance->refresh_fallbacks.fetch_add(1, std::memory_order_relaxed);
  records::write("shim: xrRequestDisplayRefreshRateFB(" + rate_text(rate) + "): the runtime lists the rate but returned " +
                 records::result_name(result) + "; the request succeeds at the runtime's rate");
  return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL enumerate_color_spaces(XrSession session, std::uint32_t capacity, std::uint32_t* count,
                                                      XrColorSpaceFB* spaces) {
  if (!find_session(session)) return XR_ERROR_HANDLE_INVALID;
  // The runtime presents in the display's own color space.
  return enumerate(capacity, count, spaces, std::vector<XrColorSpaceFB>{XR_COLOR_SPACE_UNMANAGED_FB});
}

XRAPI_ATTR XrResult XRAPI_CALL set_color_space(XrSession session, XrColorSpaceFB space) {
  if (!find_session(session)) return XR_ERROR_HANDLE_INVALID;
  // Quest games set the space they were mastered for and treat a refusal as
  // fatal; the display keeps its own space either way.
  if (space < XR_COLOR_SPACE_UNMANAGED_FB || space > XR_COLOR_SPACE_ADOBE_RGB_FB) return XR_ERROR_VALIDATION_FAILURE;
  VENTURE_QUEST_RECORD_ONCE("shim: xrSetColorSpaceFB(" + std::to_string(space) +
                            ") accepted; the display keeps its own color space");
  return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL set_android_thread(XrSession session, std::int32_t, std::uint32_t) {
  // Thread hints for the runtime's scheduler; without the extension there is
  // nothing to tell it.
  if (!find_session(session)) return XR_ERROR_HANDLE_INVALID;
  VENTURE_QUEST_RECORD_ONCE(std::string("shim: xrSetAndroidApplicationThreadKHR accepted; the runtime takes no thread hints"));
  return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL get_instance_proc_addr(XrInstance handle, const char* name, PFN_xrVoidFunction* function);

PFN_xrVoidFunction intercept(const Instance& instance, std::string_view name) {
  const auto fn = [](auto pointer) { return reinterpret_cast<PFN_xrVoidFunction>(pointer); };
  if (name == "xrGetInstanceProcAddr") return fn(&get_instance_proc_addr);
  if (name == "xrDestroyInstance") return fn(&destroy_instance);
  if (name == "xrCreateSession") return fn(&create_session);
  if (name == "xrDestroySession") return fn(&destroy_session);
  const bool emulated_refresh = instance.emulates("XR_FB_display_refresh_rate");
  // Every session's frame timing (frame_timing.h), and the display rate an
  // emulated XR_FB_display_refresh_rate reports.
  if (name == "xrWaitFrame") return fn(&wait_frame);
  if (name == "xrEndFrame") return fn(&end_frame);
  if (instance.lowered) {
    if (name == "xrLocateSpaces") return fn(&locate_spaces);
    if (name == "xrCreateReferenceSpace") return fn(&create_reference_space);
    if (name == "xrEnumerateReferenceSpaces") return fn(&enumerate_reference_spaces);
  }
  if (name == "xrLocateSpacesKHR" && instance.emulates("XR_KHR_locate_spaces")) return fn(&locate_spaces);
  if (emulated_refresh) {
    if (name == "xrEnumerateDisplayRefreshRatesFB") return fn(&enumerate_refresh_rates);
    if (name == "xrGetDisplayRefreshRateFB") return fn(&get_refresh_rate);
  }
  if (name == "xrRequestDisplayRefreshRateFB" && (emulated_refresh || instance.runtime_refresh))
    return fn(&request_refresh_rate);
  if (instance.emulates("XR_FB_color_space")) {
    if (name == "xrEnumerateColorSpacesFB") return fn(&enumerate_color_spaces);
    if (name == "xrSetColorSpaceFB") return fn(&set_color_space);
  }
  if (name == "xrSetAndroidApplicationThreadKHR" && instance.emulates("XR_KHR_android_thread_settings"))
    return fn(static_cast<PFN_setAndroidThread>(&set_android_thread));
  return nullptr;
}

XRAPI_ATTR XrResult XRAPI_CALL get_instance_proc_addr(XrInstance handle, const char* name, PFN_xrVoidFunction* function) {
  if (!name || !function) return XR_ERROR_VALIDATION_FAILURE;
  auto* instance = handle == XR_NULL_HANDLE ? nullptr : find_instance(handle);
  if (!instance) {
    if (handle != XR_NULL_HANDLE) return XR_ERROR_HANDLE_INVALID;
    // Global functions go to the next layer.
    const auto next = global_next.load(std::memory_order_acquire);
    if (!next) return XR_ERROR_FUNCTION_UNSUPPORTED;
    return next(handle, name, function);
  }
  // What the game gets records its error results (records.h).
  if (auto* own = intercept(*instance, name)) {
    *function = records::checked(name, own);
    return XR_SUCCESS;
  }
  const auto result = instance->next(handle, name, function);
  if (XR_SUCCEEDED(result)) *function = records::checked(name, *function);
  return result;
}

XRAPI_ATTR XrResult XRAPI_CALL create_api_layer_instance(const XrInstanceCreateInfo* info,
                                                        const XrApiLayerCreateInfo* layer_info, XrInstance* handle) {
  if (!info || !layer_info || !layer_info->nextInfo || !handle) return XR_ERROR_INITIALIZATION_FAILED;
  records::start(*info);
  {
    const auto& app = info->applicationInfo;
    std::vector<std::string_view> asked(info->enabledExtensionNames,
                                        info->enabledExtensionNames + info->enabledExtensionCount);
    records::write(std::string("instance: ") + app.applicationName + " " + std::to_string(app.applicationVersion) +
                   ", engine " + app.engineName + " " + std::to_string(app.engineVersion) + ", OpenXR " +
                   version_text(app.apiVersion) + "; extensions: " + joined(asked));
  }
  const auto* link = layer_info->nextInfo;
  auto state = std::make_unique<Instance>();
  state->next = link->nextGetInstanceProcAddr;
  global_next.store(state->next, std::memory_order_release);
  const auto available = runtime_extensions(state->next);

  // Extensions the runtime has go down; the ones it lacks and this layer
  // answers stay here. Anything else goes down too, for the runtime to refuse.
  std::vector<const char*> names;
  for (std::uint32_t i = 0; i < info->enabledExtensionCount; ++i) {
    const std::string_view name = info->enabledExtensionNames[i];
    if (!available.contains(name) && emulated_extension(name)) state->emulated.emplace(name);
    else names.push_back(info->enabledExtensionNames[i]);
  }
  state->runtime_refresh = available.contains("XR_FB_display_refresh_rate") &&
      std::ranges::any_of(names, [](const char* name) { return std::string_view(name) == "XR_FB_display_refresh_rate"; });

  XrApiLayerCreateInfo next_info = *layer_info;
  next_info.nextInfo = link->next;
  XrInstanceCreateInfo forwarded = *info;
  forwarded.enabledExtensionCount = static_cast<std::uint32_t>(names.size());
  forwarded.enabledExtensionNames = names.data();
  auto result = link->nextCreateApiLayerInstance(&forwarded, &next_info, handle);

  const auto requested = info->applicationInfo.apiVersion;
  if (result == XR_ERROR_API_VERSION_UNSUPPORTED && XR_VERSION_MAJOR(requested) == 1 && XR_VERSION_MINOR(requested) == 1) {
    // A 1.0 runtime: create the instance at 1.0 and answer the 1.1 core here.
    // Its XR_EXT_local_floor serves LOCAL_FLOOR where it has one.
    state->lowered = true;
    forwarded.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    const bool floor_listed = std::ranges::any_of(names, [](const char* name) {
      return std::string_view(name) == XR_EXT_LOCAL_FLOOR_EXTENSION_NAME;
    });
    if (available.contains(XR_EXT_LOCAL_FLOOR_EXTENSION_NAME)) {
      if (!floor_listed) names.push_back(XR_EXT_LOCAL_FLOOR_EXTENSION_NAME);
      state->runtime_local_floor = true;
    }
    forwarded.enabledExtensionCount = static_cast<std::uint32_t>(names.size());
    forwarded.enabledExtensionNames = names.data();
    result = link->nextCreateApiLayerInstance(&forwarded, &next_info, handle);
  }
  if (XR_FAILED(result)) {
    std::vector<std::string_view> lacking;
    for (const char* name : names)
      if (!available.contains(name)) lacking.push_back(name);
    records::write("instance creation failed: " + records::result_name(result) +
                   "; extensions the runtime lacks: " + joined(lacking));
    return result;
  }

  state->handle = *handle;
  const auto next = state->next;
  state->destroy_instance = resolve<PFN_xrDestroyInstance>(next, *handle, "xrDestroyInstance");
  state->create_session = resolve<PFN_xrCreateSession>(next, *handle, "xrCreateSession");
  state->destroy_session = resolve<PFN_xrDestroySession>(next, *handle, "xrDestroySession");
  state->wait_frame = resolve<PFN_xrWaitFrame>(next, *handle, "xrWaitFrame");
  state->end_frame = resolve<PFN_xrEndFrame>(next, *handle, "xrEndFrame");
  state->locate_space = resolve<PFN_xrLocateSpace>(next, *handle, "xrLocateSpace");
  state->create_reference_space = resolve<PFN_xrCreateReferenceSpace>(next, *handle, "xrCreateReferenceSpace");
  state->enumerate_reference_spaces =
      resolve<PFN_xrEnumerateReferenceSpaces>(next, *handle, "xrEnumerateReferenceSpaces");
  if (state->runtime_refresh) {
    state->enumerate_refresh_rates =
        resolve<PFN_xrEnumerateDisplayRefreshRatesFB>(next, *handle, "xrEnumerateDisplayRefreshRatesFB");
    state->request_refresh_rate =
        resolve<PFN_xrRequestDisplayRefreshRateFB>(next, *handle, "xrRequestDisplayRefreshRateFB");
    state->runtime_refresh = state->enumerate_refresh_rates && state->request_refresh_rate;
  }
  if (!state->destroy_instance || !state->create_session || !state->destroy_session || !state->wait_frame ||
      !state->end_frame ||
      !state->locate_space || !state->create_reference_space || !state->enumerate_reference_spaces) {
    if (state->destroy_instance) state->destroy_instance(*handle);
    records::write("instance creation failed: the runtime lacks a core function the layer calls");
    return XR_ERROR_INITIALIZATION_FAILED;
  }
  {
    std::string line = "created";
    XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
    const auto get_properties = resolve<PFN_xrGetInstanceProperties>(next, *handle, "xrGetInstanceProperties");
    if (get_properties && XR_SUCCEEDED(get_properties(*handle, &properties)))
      line += std::string(" on ") + properties.runtimeName + " " + version_text(properties.runtimeVersion);
    if (state->lowered)
      line += std::string("; at OpenXR 1.0, the runtime's version: the layer answers xrLocateSpaces and LOCAL_FLOOR") +
              (state->runtime_local_floor ? " (with the runtime's XR_EXT_local_floor)" : " (from STAGE)");
    line += "; answered here: " + joined(state->emulated);
    if (state->runtime_refresh) line += "; refresh rates the runtime lists but refuses are granted";
    records::write(line);
  }
  std::lock_guard lock(mutex);
  newest = state.get();
  instances[*handle] = std::move(state);
  return XR_SUCCESS;
}
}  // namespace

std::span<const Extension> emulated_extensions() { return kEmulated; }

XrResult negotiate(const XrNegotiateLoaderInfo* loader, const char*, XrNegotiateApiLayerRequest* request) {
  if (!loader || !request || loader->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
      loader->structVersion != XR_LOADER_INFO_STRUCT_VERSION || loader->structSize != sizeof(XrNegotiateLoaderInfo) ||
      request->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST ||
      request->structVersion != XR_API_LAYER_INFO_STRUCT_VERSION ||
      request->structSize != sizeof(XrNegotiateApiLayerRequest) ||
      loader->minInterfaceVersion > XR_CURRENT_LOADER_API_LAYER_VERSION ||
      loader->maxInterfaceVersion < XR_CURRENT_LOADER_API_LAYER_VERSION ||
      XR_VERSION_MAJOR(loader->minApiVersion) > 1 || XR_VERSION_MAJOR(loader->maxApiVersion) < 1)
    return XR_ERROR_INITIALIZATION_FAILED;
  request->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
  request->layerApiVersion = XR_API_VERSION_1_1;
  request->getInstanceProcAddr = &get_instance_proc_addr;
  request->createApiLayerInstance = &create_api_layer_instance;
  return XR_SUCCESS;
}

Report report() {
  std::lock_guard lock(mutex);
  if (!newest) return {};
  Report result{newest->lowered, {}, newest->refresh_fallbacks.load(std::memory_order_relaxed)};
  for (const auto& name : newest->emulated) {
    if (!result.emulated.empty()) result.emulated += ',';
    result.emulated += name;
  }
  return result;
}
}  // namespace venture::quest_layer

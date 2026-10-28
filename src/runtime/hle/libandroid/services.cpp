#include "android/errors.h"
#include "android/path.h"
#include "hle/libandroid/parts.h"
#include "android/handles.h"
#include "android/looper.h"
#include "android/native_activity.h"
#include "android/package_assets.h"
#include "core/process.h"
#include <filesystem>
#include <map>
#include <limits>
#include <thread>

namespace venture::hle::android_hle {
namespace {
using namespace venture::android;
int sensor_manager_token;
void* sensor_manager() { return &sensor_manager_token; }
void* sensor_manager_package(const char*) { return sensor_manager(); }
bool valid_sensor_manager(void* p) { return p == sensor_manager(); }
int sensor_list(void* p, GuestAddr* list) {
  if (!valid_sensor_manager(p) || !list) return -err::invalid;
  *list = 0;
  return 0;
}
void* default_sensor(void*, int) { return nullptr; }
struct SensorQueue {};
Handles<SensorQueue> sensor_queues;
SensorQueue* sensor_queue(void* manager, Looper* looper, int, GuestAddr, GuestAddr) {
  if (!valid_sensor_manager(manager) || !pin_looper(looper)) return nullptr;
  return sensor_queues.create();
}
int destroy_sensor_queue(void* manager, SensorQueue* queue) {
  if (!valid_sensor_manager(manager) || !sensor_queues.get(queue)) return -err::invalid;
  sensor_queues.erase(queue);
  return 0;
}
int sensor_enable(SensorQueue*, void*) { return -err::no_device; }
int sensor_rate(SensorQueue*, void*, std::int32_t) { return -err::no_device; }
int sensor_has(SensorQueue* q) { return sensor_queues.get(q) ? 0 : -err::invalid; }
std::int64_t sensor_events(SensorQueue* q, void*, std::uint64_t) { return sensor_has(q); }
const char* sensor_name(void*) { return nullptr; }
int sensor_type(void*) { return -1; }
int sensor_min_delay(void*) { return 0; }
float sensor_resolution(void*) { return 0; }

// No trace consumer is attached: Android permits begin/end calls while tracing
// is disabled. Do not log names, which may contain user data.
bool trace_enabled() { return false; }
void trace_begin(const char*) {}
void trace_end() {}
void trace_async(const char*, std::int32_t) {}
void trace_counter(const char*, std::int64_t) {}

void* thermal_acquire() {
  log::warn("thermal", "No host thermal service is available");
  fail(err::unsupported);
  return nullptr;
}
void thermal_release(void*) {}
int thermal_status(void*) {
  fail(err::invalid);
  return -1;
}
float thermal_headroom(void*, int) {
  fail(err::invalid);
  return std::numeric_limits<float>::quiet_NaN();
}
int thermal_register(void*, GuestAddr, GuestAddr) { return err::invalid; }

// Android performance hints are advisory. Store/report durations, but never
// claim to change scheduling policy or host power settings.
int hint_manager_token;
void* hint_manager() { return &hint_manager_token; }
struct HintSession {
  std::mutex mutex;
  std::vector<std::int32_t> tids;
  std::int64_t target = 0, last_actual = 0;
};
Handles<HintSession> hints;
HintSession* hint_create(void* manager, const std::int32_t* tids, std::uint64_t count, std::int64_t target) {
  if (manager != hint_manager() || !tids || !count || count > 1024 || target <= 0) return nullptr;
  for (std::uint64_t i = 0; i < count; ++i)
    if (tids[i] <= 0) return nullptr;
  auto* h = hints.create();
  h->tids.assign(tids, tids + count);
  h->target = target;
  log::debug("performance", "Created advisory work-duration session for {} threads", count);
  return h;
}
void hint_close(HintSession* p) { hints.erase(p); }
std::int64_t hint_rate(void* m) { return m == hint_manager() ? 16666667 : -1; }
template <bool Actual>
int hint_duration(HintSession* p, std::int64_t duration) {
  auto h = hints.get(p);
  if (!h || duration <= 0) return err::invalid;
  std::lock_guard lock(h->mutex);
  (Actual ? h->last_actual : h->target) = duration;
  log::trace("performance", "Work duration: target={} actual={} ns", h->target, h->last_actual);
  return 0;
}

struct Storage {
  std::mutex mutex;
  std::map<std::string, std::string> mounted;
};
Handles<Storage> storage;
Storage* storage_create() { return storage.create(); }
void storage_delete(Storage* p) { storage.erase(p); }
void storage_callback(GuestAddr cb, std::string file, int state, GuestAddr data) {
  if (!cb) return;
  auto fn = [cb, file = std::move(file), state, data] {
    GuestThread::adopt_current().call(cb, {h2g(file.c_str()), static_cast<std::uint64_t>(state), data});
  };
  if (auto l = pin_looper(thread_looper()))
    l->post(std::move(fn));
  else
    fn();
}
void mount_obb(Storage* p, const char* name, const char* key, GuestAddr cb, GuestAddr data) {
  auto s = storage.get(p);
  if (!s || !name) return;
  int state = 21;
  auto& config = process_config();
  auto filename = host_path(name).filename();
  auto host = config.obb_dir / filename;
  std::error_code ec;
  auto utf8 = filename.u8string();
  std::string basename(reinterpret_cast<const char*>(utf8.data()), utf8.size());
  bool known_path = std::string(name) == config.guest_obb_dir() + '/' + basename || host_path(name) == host;
  if ((!key || !*key) && known_path && std::filesystem::is_regular_file(host, ec)) {
    std::lock_guard lock(s->mutex);
    if (s->mounted.contains(name))
      state = 24;
    else {
      auto utf8_path = host.u8string();
      if (PackageAssets::get().open_obb(
              std::string(reinterpret_cast<const char*>(utf8_path.data()), utf8_path.size()))) {
        s->mounted.emplace(name, config.guest_obb_dir());
        state = 1;
      }
    }
  }
  if (state != 1) log::warn("storage", "OBB mount failed with Android state {}", state);
  storage_callback(cb, name, state, data);
}
void unmount_obb(Storage* p, const char* name, std::int32_t, GuestAddr cb, GuestAddr data) {
  auto s = storage.get(p);
  if (!s || !name) return;
  int state;
  {
    std::lock_guard lock(s->mutex);
    state = s->mounted.erase(name) ? 2 : 23;
  }
  storage_callback(cb, name, state, data);
}
int mounted(Storage* p, const char* name) {
  auto s = storage.get(p);
  if (!s || !name) return 0;
  std::lock_guard lock(s->mutex);
  return s->mounted.contains(name);
}
const char* mounted_path(Storage* p, const char* name) {
  auto s = storage.get(p);
  if (!s || !name) return nullptr;
  std::lock_guard lock(s->mutex);
  auto it = s->mounted.find(name);
  return it == s->mounted.end() ? nullptr : it->second.c_str();
}
int cpu_family() { return 4; }  // cpu-features.h ANDROID_CPU_FAMILY_ARM64
int cpu_count() { return static_cast<int>(std::max(1u, std::thread::hardware_concurrency())); }
// WindowManager.LayoutParams flags (KEEP_SCREEN_ON, FULLSCREEN...) shape a
// phone window; the headset view is the host's, so they have no effect here.
void set_window_flags(GuestAddr, std::uint32_t, std::uint32_t) {}
}  // namespace
void register_services(HleLibrary& lib) {
#define API(name, fn) add<&fn>(lib, name)
  API("ANativeActivity_setWindowFormat", android::native_activity_set_window_format);
  API("ANativeActivity_setWindowFlags", set_window_flags);
  API("ASensorManager_getInstance", sensor_manager);
  API("ASensorManager_getInstanceForPackage", sensor_manager_package);
  API("ASensorManager_getSensorList", sensor_list);
  API("ASensorManager_getDefaultSensor", default_sensor);
  API("ASensorManager_createEventQueue", sensor_queue);
  API("ASensorManager_destroyEventQueue", destroy_sensor_queue);
  API("ASensorEventQueue_enableSensor", sensor_enable);
  API("ASensorEventQueue_disableSensor", sensor_enable);
  API("ASensorEventQueue_setEventRate", sensor_rate);
  API("ASensorEventQueue_hasEvents", sensor_has);
  API("ASensorEventQueue_getEvents", sensor_events);
  API("ASensor_getName", sensor_name);
  API("ASensor_getVendor", sensor_name);
  API("ASensor_getType", sensor_type);
  API("ASensor_getMinDelay", sensor_min_delay);
  API("ASensor_getResolution", sensor_resolution);
  API("ATrace_isEnabled", trace_enabled);
  API("ATrace_beginSection", trace_begin);
  API("ATrace_endSection", trace_end);
  API("ATrace_beginAsyncSection", trace_async);
  API("ATrace_endAsyncSection", trace_async);
  API("ATrace_setCounter", trace_counter);
  API("AThermal_acquireManager", thermal_acquire);
  API("AThermal_releaseManager", thermal_release);
  API("AThermal_getCurrentThermalStatus", thermal_status);
  API("AThermal_getThermalHeadroom", thermal_headroom);
  API("AThermal_registerThermalStatusListener", thermal_register);
  API("AThermal_unregisterThermalStatusListener", thermal_register);
  API("APerformanceHint_getManager", hint_manager);
  API("APerformanceHint_createSession", hint_create);
  API("APerformanceHint_closeSession", hint_close);
  API("APerformanceHint_getPreferredUpdateRateNanos", hint_rate);
  API("APerformanceHint_updateTargetWorkDuration", hint_duration<false>);
  API("APerformanceHint_reportActualWorkDuration", hint_duration<true>);
  API("AStorageManager_new", storage_create);
  API("AStorageManager_delete", storage_delete);
  API("AStorageManager_mountObb", mount_obb);
  API("AStorageManager_unmountObb", unmount_obb);
  API("AStorageManager_isObbMounted", mounted);
  API("AStorageManager_getMountedObbPath", mounted_path);
  API("android_getCpuFamily", cpu_family);
  API("android_getCpuCount", cpu_count);
#undef API
}
}  // namespace venture::hle::android_hle

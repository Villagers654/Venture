#include "java/natives/display_bridge.h"

#include <atomic>
#include <mutex>
#include <jni.h>

#include "common/log.h"
#include "java/jvm.h"
#include "java/natives/display_clock.h"
#include "core/wait.h"

namespace venture::java {
namespace {
bool finish(JNIEnv* env) {
  const bool failed = env->ExceptionCheck();
  if (failed) {
    env->ExceptionDescribe();
    env->ExceptionClear();
    log::error("framework", "Java display callback failed");
  }
  return !failed;
}
struct Metadata {
  jclass runtime{}, choreographer{}, system{};
  jmethodID period{}, configure{}, vsync{}, xr_vsync{}, nano{};
};
const Metadata* metadata(JNIEnv* env) {
  // HotSpot starts once per process and cannot restart. Global references and
  // method IDs remain valid until DestroyJavaVM releases the VM. JNIEnv is
  // always obtained by the caller on its own thread.
  static Metadata storage;
  static std::atomic<const Metadata*> published{nullptr};
  static std::mutex mutex;
  if (auto* ready = published.load(std::memory_order_acquire)) return ready;
  std::lock_guard lock(mutex);
  if (auto* ready = published.load(std::memory_order_relaxed)) return ready;
  Metadata next;
  auto global = [&](const char* name) -> jclass {
    auto local = env->FindClass(name);
    if (!local) return nullptr;
    auto result = static_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);
    return result;
  };
  next.runtime = global("com/venture/runtime/RuntimeEnvironment");
  if (next.runtime) {
    next.period = env->GetStaticMethodID(next.runtime, "setDisplayPeriod", "(J)V");
    if (next.period) next.configure = env->GetStaticMethodID(next.runtime, "configureDisplay", "(IIIFZ)V");
  }
  if (next.configure) next.choreographer = global("android/view/Choreographer");
  if (next.choreographer) {
    next.vsync = env->GetStaticMethodID(next.choreographer, "dispatchHostVsync", "(J)V");
    if (next.vsync) next.xr_vsync = env->GetStaticMethodID(next.choreographer, "dispatchHostVsync", "(JJ)V");
  }
  if (next.xr_vsync) next.system = global("java/lang/System");
  if (next.system) next.nano = env->GetStaticMethodID(next.system, "nanoTime", "()J");
  if (!finish(env) || !next.nano) {
    for (auto cls : {next.runtime, next.choreographer, next.system})
      if (cls) env->DeleteGlobalRef(cls);
    return nullptr;
  }
  storage = next;
  published.store(&storage, std::memory_order_release);
  return &storage;
}
detail::DisplayClock clock;
std::mutex clock_mutex;
bool map_time(JNIEnv* env, const Metadata& ids, std::int64_t time, std::int64_t& mapped) {
  std::lock_guard lock(clock_mutex);
  // Retry a delayed initial sample immediately: a guest may be waiting for
  // this first pulse before it can produce another XR frame.
  for (int attempt = 0; attempt < 3; ++attempt) {
    const auto before = monotonic_ns();
    if (!clock.sample_due(before)) break;
    const auto java_time = env->CallStaticLongMethod(ids.system, ids.nano);
    const auto after = monotonic_ns();
    if (!finish(env)) return false;
    clock.sample(before, java_time, after);
    if (clock.valid()) break;
  }
  if (!clock.valid()) return false;
  mapped = clock.map(time);
  return true;
}
}  // namespace
bool update_framework_display_period(std::int64_t period) {
  auto* env = host_env();
  if (!env) return false;
  auto* ids = metadata(env);
  if (!ids) return false;
  // Establish the epoch during framework startup, before display-dependent
  // guest initialization can wait for its first XR pulse.
  std::int64_t mapped;
  map_time(env, *ids, monotonic_ns(), mapped);
  env->CallStaticVoidMethod(ids->runtime, ids->period, static_cast<jlong>(period));
  return finish(env);
}
bool configure_framework_display(std::int32_t width, std::int32_t height,
                                 std::int32_t density, float refresh,
                                 bool host_vsync) {
  auto* env = host_env();
  if (!env) return false;
  auto* ids = metadata(env);
  if (!ids) return false;
  std::int64_t mapped;
  map_time(env, *ids, monotonic_ns(), mapped);
  env->CallStaticVoidMethod(ids->runtime, ids->configure, width, height, density, refresh,
                           static_cast<jboolean>(host_vsync));
  return finish(env);
}
bool dispatch_framework_vsync(std::int64_t frame_time_ns) {
  auto* env = host_env();
  if (!env) return false;
  auto* ids = metadata(env);
  if (!ids) return false;
  env->CallStaticVoidMethod(ids->choreographer, ids->vsync, static_cast<jlong>(frame_time_ns));
  return finish(env);
}
bool dispatch_framework_xr_vsync(std::int64_t time, std::int64_t period) {
  auto* env = host_env();
  if (!env) return false;
  auto* ids = metadata(env);
  if (!ids) return false;
  std::int64_t mapped;
  if (!map_time(env, *ids, time, mapped)) return false;
  env->CallStaticVoidMethod(ids->choreographer, ids->xr_vsync,
                           static_cast<jlong>(mapped), static_cast<jlong>(period));
  return finish(env);
}
}  // namespace venture::java

#include "core/process_exit.h"
#include "java/natives/runtime_bridge.h"

#include <jni.h>
#include "platform/runtime_child.h"
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "android/framework_bridge.h"
#include "android/input.h"
#include "android/looper.h"
#include "android/native_activity.h"
#include "android/native_window.h"
#include "common/log.h"
#include "audio/audio.h"
#include "gfx/blob_cache.h"
#include "core/profile.h"
#include "core/pipeline_stats.h"
#include "core/address_space.h"
#include "core/linker.h"
#include "core/thread.h"
#include "core/thread_runtime.h"
#include "hle/libc/libc_parts.h"
#include "hle/libc/vfs.h"
#include "java/internal.h"
#include "java/jni32.h"
#include "java/natives/native_activity_bridge.h"
#include "java/natives/audio_bridge.h"
#include "java/natives/media_bridge.h"
#include "java/natives/surface_bridge.h"
#include "platform/process_info.h"
#include "hle/vulkan/vulkan.h"

namespace venture::java {
namespace {
template<class F> auto guarded(JNIEnv* env, F&& fn) -> decltype(fn()) {
  try { return fn(); }
  catch (const ThreadExit&) { throw_thread_death(env); }
  catch (const ProcessExit&) { throw_thread_death(env); }
  catch (const std::exception& e) { throw_java(env, "java/lang/IllegalStateException", e.what()); }
  catch (...) { throw_java(env, "java/lang/IllegalStateException", "Native Looper callback failed"); }
  if constexpr (!std::is_void_v<decltype(fn())>) return {};
}
jlong JNICALL prepare_looper(JNIEnv* env, jclass) {
  // A Java Looper need not execute guest code. Delay CPU/JIT allocation until
  // poll dispatches or waits; prepare must not delay publication of its Handler.
  return guarded(env, []() -> jlong { return h2g(android::prepare_looper(0)); });
}
jint JNICALL vulkan_hardware_version(JNIEnv* env, jobject) {
  return guarded(env, []() -> jint { return vulkan::android_hardware_version(); });
}
jint JNICALL set_environment(JNIEnv* env, jobject, jbyteArray name, jbyteArray value, jboolean overwrite) {
  return guarded(env, [&]() -> jint {
    auto bytes = [&](jbyteArray input) {
      if (!input) throw std::invalid_argument("Null guest environment string");
      std::string text(static_cast<std::size_t>(env->GetArrayLength(input)), '\0');
      if (!text.empty()) env->GetByteArrayRegion(input, 0, static_cast<jsize>(text.size()),
          reinterpret_cast<jbyte*>(text.data()));
      return text;
    };
    const auto key = bytes(name), text = bytes(value);
    if (env->ExceptionCheck()) return 0;
    return hle::libc::set_environment(key, text, overwrite);
  });
}
jstring JNICALL guest_error_string(JNIEnv* env, jobject, jint error) {
  return guarded(env, [&]() { return env->NewStringUTF(hle::libc::error_string(error)); });
}
std::string virtual_path(JNIEnv* env, jbyteArray input) {
  if (!input) throw std::invalid_argument("Null virtual path");
  std::string path(static_cast<std::size_t>(env->GetArrayLength(input)), '\0');
  if (!path.empty()) env->GetByteArrayRegion(input, 0, static_cast<jsize>(path.size()),
      reinterpret_cast<jbyte*>(path.data()));
  if (path.find('\0') != std::string::npos) throw std::invalid_argument("Invalid virtual path");
  hle::libc::MappedPath mapped;
  if (auto error = hle::libc::resolve_path(path, hle::libc::at_cwd, &mapped); error < 0)
    throw std::runtime_error(hle::libc::error_string(-error));
  if (!mapped.guest.starts_with("/proc/") && mapped.guest != "/proc" &&
      !mapped.guest.starts_with("/sys/") && mapped.guest != "/sys")
    throw std::invalid_argument("Path is outside the virtual filesystem");
  return mapped.guest;
}
jbyteArray JNICALL read_virtual_file(JNIEnv* env, jobject, jbyteArray input) {
  return guarded(env, [&]() -> jbyteArray {
    auto path = virtual_path(env, input);
    bool exists = false;
    auto value = hle::libc::proc_file(path, &exists);
    if (!exists) return nullptr;
    if (value.size() > INT32_MAX) throw std::length_error("Virtual file is too large");
    auto bytes = env->NewByteArray(static_cast<jsize>(value.size()));
    if (bytes && !value.empty()) env->SetByteArrayRegion(bytes, 0, static_cast<jsize>(value.size()),
        reinterpret_cast<const jbyte*>(value.data()));
    return bytes;
  });
}
jboolean JNICALL virtual_file_exists(JNIEnv* env, jobject, jbyteArray input) {
  return guarded(env, [&]() -> jboolean {
    auto path = virtual_path(env, input);
    if (hle::libc::proc_directory(path, nullptr)) return true;
    bool exists = false;
    hle::libc::proc_file(path, &exists);
    return exists;
  });
}
jbyteArray JNICALL system_property(JNIEnv* env, jobject, jstring key) {
  return guarded(env, [&]() -> jbyteArray {
    if (!key) throw std::invalid_argument("Null system property name");
    const auto* name = env->GetStringUTFChars(key, nullptr);
    if (!name) return nullptr;
    struct Release {
      JNIEnv* env; jstring key; const char* name;
      ~Release() { env->ReleaseStringUTFChars(key, name); }
    } release{env, key, name};
    auto value = hle::libc::system_property(name);
    if (value.size() > INT32_MAX) throw std::length_error("System property is too large");
    auto bytes = env->NewByteArray(static_cast<jsize>(value.size()));
    if (bytes && !value.empty()) env->SetByteArrayRegion(bytes, 0, static_cast<jsize>(value.size()),
        reinterpret_cast<const jbyte*>(value.data()));
    return bytes;
  });
}
void JNICALL poll_looper(JNIEnv* env, jclass, jlong handle, jint timeout) {
  guarded(env, [&] {
    auto looper = android::pin_looper(g2h<android::Looper>(handle));
    if (!looper || looper.get() != android::thread_looper()) throw std::invalid_argument("Looper polled from wrong thread");
    looper->poll(timeout, nullptr, nullptr, nullptr);
  });
}
void JNICALL wake_looper(JNIEnv* env, jclass, jlong handle) {
  guarded(env, [&] {
    auto looper = android::pin_looper(g2h<android::Looper>(handle));
    if (!looper) throw std::invalid_argument("Released native Looper");
    looper->wake();
  });
}
jobjectArray JNICALL audio_devices(JNIEnv* env, jobject) {
  return guarded(env, [&]() -> jobjectArray {
    const auto devices = audio::default_devices();
    auto cls = env->FindClass("android/media/AudioDeviceInfo");
    if (!cls) return nullptr;
    auto ctor = env->GetMethodID(cls, "<init>", "(IZLjava/lang/String;)V");
    auto string_cls = env->FindClass("java/lang/String");
    auto string_ctor = string_cls ? env->GetMethodID(string_cls, "<init>", "([BLjava/lang/String;)V") : nullptr;
    auto encoding = string_ctor ? env->NewStringUTF("UTF-8") : nullptr;
    if (!encoding) { env->DeleteLocalRef(cls); return nullptr; }
    auto result = ctor ? env->NewObjectArray(static_cast<jsize>(devices.size()), cls, nullptr) : nullptr;
    if (result) for (std::size_t i = 0; i < devices.size() && !env->ExceptionCheck(); ++i) {
      const bool input = devices[i].direction == audio::Direction::input;
      auto bytes = env->NewByteArray(static_cast<jsize>(devices[i].name.size()));
      if (!bytes) break;
      env->SetByteArrayRegion(bytes, 0, static_cast<jsize>(devices[i].name.size()),
                             reinterpret_cast<const jbyte*>(devices[i].name.data()));
      auto name = !env->ExceptionCheck() ? env->NewObject(string_cls, string_ctor, bytes, encoding) : nullptr;
      auto value = name ? env->NewObject(cls, ctor, audio::device_id(devices[i]),
                                        static_cast<jboolean>(input), name) : nullptr;
      if (value) env->SetObjectArrayElement(result, static_cast<jsize>(i), value);
      if (value) env->DeleteLocalRef(value);
      if (name) env->DeleteLocalRef(name);
      env->DeleteLocalRef(bytes);
    }
    env->DeleteLocalRef(encoding);
    env->DeleteLocalRef(string_cls);
    env->DeleteLocalRef(cls);
    return result;
  });
}
jintArray JNICALL output_properties(JNIEnv* env, jobject) {
  return guarded(env, [&]() -> jintArray {
    const auto [rate, period] = audio::Backend::get().output_properties();
    const jint values[]{rate, period};
    auto result = env->NewIntArray(2);
    if (result) env->SetIntArrayRegion(result, 0, 2, values);
    return result;
  });
}
jlongArray JNICALL process_memory(JNIEnv* env, jobject) {
  return guarded(env, [&]() -> jlongArray {
    platform::ProcessMemory memory;
    if (!platform::process_memory(memory)) throw std::runtime_error("Process residency query failed");
    const jlong values[]{static_cast<jlong>(memory.private_kib), static_cast<jlong>(memory.shared_pss_kib)};
    auto result = env->NewLongArray(2);
    if (result) env->SetLongArrayRegion(result, 0, 2, values);
    return result;
  });
}
jboolean JNICALL debugger_connected(JNIEnv* env, jobject) {
  return guarded(env, []() -> jboolean { return platform::process_debugger_attached(); });
}
jboolean JNICALL music_active(JNIEnv*, jobject) { return audio::Backend::get().output_active(); }
void JNICALL exit_process(JNIEnv* env, jobject, jint status) {
  // Once per process: the guest native call site explains an unrequested self-exit.
  {
    if (auto* thread = GuestThread::current(); thread && thread->cpu().isa() == GuestIsa::a32) {
      auto& cpu = thread->cpu();
      log::info("jni-trace", "A32 exit PC={:#x} LR={:#x}", cpu.pc(), cpu.lr());
      auto fp = cpu.x(11);
      // LLVM ARMFrameLowering.cpp's ARM frame-pointer convention stores saved
      // r11 and LR at [r11] and [r11+4]. This is a bounded diagnostic, not an
      // EHABI unwinder; reject unreadable records and stay in this thread's stack.
      for (unsigned i = 0; i < 12 && fp >= thread->stack_base &&
           fp + 8 <= thread->stack_base + thread->stack_size && !(fp & 3); ++i) {
        if (!AddressSpace::get().is_guest_accessible(fp, 8, false)) break;
        std::uint32_t record[2];
        std::memcpy(record, g2h(fp), sizeof(record));
        log::info("jni-trace", "A32 exit frame {} FP={:#x} LR={:#x}", i, fp, record[1]);
        if (record[0] <= fp) break;
        fp = record[0];
      }
    } else if (thread) {
      // AAPCS64 6.4.6 frame records: [FP] = caller FP, [FP+8] = saved LR. The
      // live LR returns into the guest caller of the JNI call that exited.
      auto& cpu = thread->cpu();
      auto pc = cpu.lr();
      auto fp = cpu.frame_pointer();
      for (unsigned i = 0; pc && i < 24; ++i) {
        DlInfo where;
        Linker::get().try_address_info(pc, &where);
        log::info("jni-trace", "A64 exit frame {} {}+{:#x} {}+{:#x}", i, where.path, pc - where.base,
                  where.symbol, where.symbol.empty() ? 0 : pc - where.symbol_address);
        if (!fp || (fp & 15) || fp < thread->stack_base || fp + 16 > thread->stack_base + thread->stack_size ||
            !AddressSpace::get().is_guest_accessible(fp, 16, false))
          break;
        std::uint64_t record[2];
        std::memcpy(record, g2h(fp), sizeof(record));
        if (record[0] <= fp) break;
        fp = record[0];
        pc = record[1];
      }
    }
  }
  guarded(env, [&] { exit_guest_process(status); });
}
void JNICALL begin_process_exit(JNIEnv*, jobject) {
  platform::notify_runtime_exit_wait(platform::RuntimeExitWait::arbitration, 0);
  worker_exit_mutex.lock();
  platform::notify_runtime_exit_wait(platform::RuntimeExitWait::log_flush, 0);
}
void JNICALL end_process_exit(JNIEnv*, jobject) { worker_exit_mutex.unlock(); }
void JNICALL restart_process(JNIEnv* env, jobject) {
  guarded(env, [] { platform::notify_runtime_exit_wait(platform::RuntimeExitWait::cache_flush, 0); gfx::flush_blob_cache();
      platform::notify_runtime_exit_wait(platform::RuntimeExitWait::profiler_flush, 0); profile::stop(); profile::report_pipelines("restart"); platform::restart_runtime_process(); });
}
void JNICALL restart_process_with_intent(JNIEnv* env, jobject, jbyteArray payload) {
  guarded(env, [&] {
    if (!payload) throw std::invalid_argument("Missing launch intent");
    const auto size = env->GetArrayLength(payload);
    if (size > platform::runtime_launch_payload_limit) throw std::length_error("Launch intent too large");
    std::string bytes(static_cast<std::size_t>(size), '\0');
    env->GetByteArrayRegion(payload, 0, size, reinterpret_cast<jbyte*>(bytes.data()));
    if (!env->ExceptionCheck()) {
      platform::notify_runtime_exit_wait(platform::RuntimeExitWait::cache_flush, 0); gfx::flush_blob_cache();
      platform::notify_runtime_exit_wait(platform::RuntimeExitWait::profiler_flush, 0); profile::stop(); profile::report_pipelines("restart"); platform::restart_runtime_process(bytes);
    }
  });
}
jbyteArray JNICALL cold_launch_intent(JNIEnv* env, jobject) {
  return guarded(env, [&]() -> jbyteArray {
    auto payload = platform::take_runtime_launch_payload();
    if (payload.empty()) return nullptr;
    auto result = env->NewByteArray(static_cast<jsize>(payload.size()));
    if (result) env->SetByteArrayRegion(result, 0, static_cast<jsize>(payload.size()),
        reinterpret_cast<const jbyte*>(payload.data()));
    return result;
  });
}
void JNICALL setup_shaders_disk_cache(JNIEnv* env, jobject, jstring file) {
  guarded(env, [&]() -> int {
    if (!file) return 0;
    std::u16string path(static_cast<std::size_t>(env->GetStringLength(file)), u' ');
    env->GetStringRegion(file, 0, static_cast<jsize>(path.size()), reinterpret_cast<jchar*>(path.data()));
    if (!env->ExceptionCheck()) gfx::set_blob_cache_file(std::filesystem::path(path));
    return 0;
  });
}
void register_loopers(JNIEnv* env) {
  auto cls = env->FindClass("com/venture/runtime/NativeHost");
  if (!cls) return;
  const JNINativeMethod methods[]{
      {const_cast<char*>("readVirtualFile"), const_cast<char*>("([B)[B"), reinterpret_cast<void*>(read_virtual_file)},
      {const_cast<char*>("virtualFileExists"), const_cast<char*>("([B)Z"), reinterpret_cast<void*>(virtual_file_exists)},
      {const_cast<char*>("setEnvironment"), const_cast<char*>("([B[BZ)I"), reinterpret_cast<void*>(set_environment)},
      {const_cast<char*>("guestErrorString"), const_cast<char*>("(I)Ljava/lang/String;"), reinterpret_cast<void*>(guest_error_string)},
      {const_cast<char*>("restartProcess"), const_cast<char*>("()V"), reinterpret_cast<void*>(restart_process)},
      {const_cast<char*>("restartProcess"), const_cast<char*>("([B)V"), reinterpret_cast<void*>(restart_process_with_intent)},
      {const_cast<char*>("coldLaunchIntent"), const_cast<char*>("()[B"), reinterpret_cast<void*>(cold_launch_intent)},
      {const_cast<char*>("vulkanHardwareVersion"), const_cast<char*>("()I"), reinterpret_cast<void*>(vulkan_hardware_version)},
      {const_cast<char*>("setupShadersDiskCache"), const_cast<char*>("(Ljava/lang/String;)V"), reinterpret_cast<void*>(setup_shaders_disk_cache)},
      {const_cast<char*>("processMemory"), const_cast<char*>("()[J"), reinterpret_cast<void*>(process_memory)},
      {const_cast<char*>("debuggerConnected"), const_cast<char*>("()Z"), reinterpret_cast<void*>(debugger_connected)},
      {const_cast<char*>("systemProperty"), const_cast<char*>("(Ljava/lang/String;)[B"), reinterpret_cast<void*>(system_property)},
      {const_cast<char*>("beginProcessExit"), const_cast<char*>("()V"), reinterpret_cast<void*>(begin_process_exit)},
      {const_cast<char*>("endProcessExit"), const_cast<char*>("()V"), reinterpret_cast<void*>(end_process_exit)},
      {const_cast<char*>("exitProcess"), const_cast<char*>("(I)V"), reinterpret_cast<void*>(exit_process)},
      {const_cast<char*>("audioOutputProperties"), const_cast<char*>("()[I"), reinterpret_cast<void*>(output_properties)},
      {const_cast<char*>("isMusicActive"), const_cast<char*>("()Z"), reinterpret_cast<void*>(music_active)},
      {const_cast<char*>("audioDevices"), const_cast<char*>("()[Landroid/media/AudioDeviceInfo;"), reinterpret_cast<void*>(audio_devices)},
      {const_cast<char*>("prepareLooper"), const_cast<char*>("()J"), reinterpret_cast<void*>(prepare_looper)},
      {const_cast<char*>("pollLooper"), const_cast<char*>("(JI)V"), reinterpret_cast<void*>(poll_looper)},
      {const_cast<char*>("wakeLooper"), const_cast<char*>("(J)V"), reinterpret_cast<void*>(wake_looper)},
  };
  if (env->RegisterNatives(cls, methods, static_cast<jint>(std::size(methods))) == JNI_OK) {
    auto available = env->GetStaticFieldID(cls, "nativeLoopersAvailable", "Z");
    if (available) env->SetStaticBooleanField(cls, available, JNI_TRUE);
  }
  env->DeleteLocalRef(cls);
}
struct ActivityState {
  std::unique_ptr<android::NativeActivity> native;
  jobject object = nullptr;
  std::uint32_t guest_object = 0;
  android::InputQueue* input = nullptr;
  std::thread::id owner = std::this_thread::get_id();
  ~ActivityState() {
    // The global ref must outlive all lifecycle callbacks, including onDestroy.
    native.reset();
    if (guest_object) jni32::delete_reference(guest_object, JNIGlobalRefType);
    if (input) android::destroy_input_queue(input);
    if (object && host_vm()) host_env()->DeleteGlobalRef(object);
  }
};
std::mutex activity_mutex;
std::map<GuestAddr, std::shared_ptr<ActivityState>> activities;

GuestAddr create_activity(JNIEnv* env, void* object, const std::string& name,
                          const std::string& entry, std::string* error) {
  LocalFrame frame(env);
  if (!frame.valid()) return 0;
  auto activity = static_cast<jobject>(object);
  auto cls = env->GetObjectClass(activity);
  auto class_class = env->FindClass("java/lang/Class");
  auto get_loader = class_class ? env->GetMethodID(class_class, "getClassLoader", "()Ljava/lang/ClassLoader;") : nullptr;
  auto loader = get_loader ? env->CallObjectMethod(cls, get_loader) : nullptr;
  if (env->ExceptionCheck()) return 0;
  auto library_name = name.find('/') == name.npos && !name.ends_with(".so") ? "lib" + name + ".so" : name;
  if (!load_guest_library(library_name, loader, error)) return 0;
  auto* library = Linker::get().open(library_name, kRtldNow | kRtldNoLoad, nullptr);
  if (!library) { if (error) *error = Linker::get().take_error(); return 0; }
  // NativeLoader retains its NODELETE reference; balance this temporary lookup.
  struct Close { Library* library; ~Close() { Linker::get().close(library); } } close{library};
  auto state = std::make_shared<ActivityState>();
  state->object = env->NewGlobalRef(activity);
  if (!state->object) return 0;
  auto& thread = GuestThread::adopt_current();
  LoaderScope scope(loader);
  if (thread.cpu().isa() == GuestIsa::a32) {
    state->guest_object = jni32::reference(env->NewGlobalRef(state->object), JNIGlobalRefType);
    if (!state->guest_object) return 0;
  }
  state->native = android::NativeActivity::create(
      *library, guest_vm(), guest_env(thread), state->guest_object ? state->guest_object : h2g(state->object),
      error, entry);
  if (!state->native) return 0;
  state->input = android::create_input_queue();
  // ViewRoot delivers input channels when the window attaches, after the
  // Activity's Java onCreate/onResume transaction. Native app-glue callbacks
  // may wait for their worker; entering one here blocks subclass startup.
  const auto address = state->native->address();
  std::lock_guard lock(activity_mutex);
  activities.emplace(address, std::move(state));
  return address;
}

bool activity_event(GuestAddr address, NativeActivityEvent event, GuestAddr surface, std::string* error) {
  std::shared_ptr<ActivityState> state;
  {
    std::lock_guard lock(activity_mutex);
    auto it = activities.find(address);
    if (it == activities.end()) { if (error) *error = "Unknown NativeActivity"; return false; }
    state = it->second;
    if (state->owner != std::this_thread::get_id()) {
      if (error) *error = "NativeActivity callback must run on its Android main thread";
      return false;
    }
    if (event == NativeActivityEvent::destroy) activities.erase(it);
  }
  auto& a = *state->native;
  using E = NativeActivityEvent;
  using A = android::ActivityEvent;
  switch (event) {
    case E::start: a.event(A::start); break;
    case E::resume: a.event(A::resume); break;
    case E::pause: a.event(A::pause); gfx::flush_blob_cache(); break;
    case E::stop: a.event(A::stop); break;
    case E::destroy: a.destroy(); break;
    case E::focus_gained: a.focus(true); break;
    case E::focus_lost: a.focus(false); break;
    case E::low_memory: a.event(A::low_memory); break;
    case E::surface_created:
    case E::surface_changed: {
      auto window = android::pin_native_window(surface);
      if (!window) { if (error) *error = "NativeActivity surface was released"; return false; }
      a.input_queue(state->input);
      a.window(window.get());
      if (event == E::surface_changed) {
        a.window_resized();
        std::int32_t width, height;
        { std::lock_guard lock(window->lock); width = window->width; height = window->height; }
        a.content_rect(0, 0, width, height);
      }
      break;
    }
    case E::surface_destroyed:
      a.window(nullptr);
      a.input_queue(nullptr);
      break;
    case E::surface_redraw: a.redraw(); break;
  }
  return true;
}
android::NativeWindow* from_surface(void*, jobject object) {
  return acquire_surface_window(host_env(), object);
}
jobject to_surface(void*, android::NativeWindow* window) {
  auto* env = host_env();
  auto cls = env->FindClass("android/view/Surface");
  auto ctor = cls ? env->GetMethodID(cls, "<init>", "(J)V") : nullptr;
  jobject result = nullptr;
  if (ctor) {
    android::acquire(window);
    result = env->NewObject(cls, ctor, static_cast<jlong>(h2g(window)));
    if (!result) android::release(window);
  }
  if (cls) env->DeleteLocalRef(cls);
  return result;
}
}
void install_runtime_framework_bridges() {
  static std::once_flag once;
  std::call_once(once, [] { add_framework_natives(register_loopers); add_framework_natives(register_audio_tracks); add_framework_natives(register_media_players); });
  android::set_surface_bridge({from_surface, to_surface});
  set_native_activity_bridge({create_activity, activity_event});
}
void destroy_native_activities() {
  std::map<GuestAddr, std::shared_ptr<ActivityState>> ending;
  { std::lock_guard lock(activity_mutex); ending.swap(activities); }
  // Guest callbacks may reenter the framework, so never run them under our lock.
  ending.clear();
}
}

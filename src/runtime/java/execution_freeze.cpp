#include "java/execution_freeze.h"
#include "java/jvm.h"
#include "java/internal.h"
#include "platform/file_activity.h"
#include <jni.h>
#include <jvmti.h>
#include <algorithm>
#include <atomic>
#include <format>
#include <string_view>
#include <vector>

namespace venture::java {
namespace {
std::atomic<bool> freezing{false};
// A Thread.start already in flight must not escape the suspension census.
// ThreadStart precedes Java execution. This terminal fence uses the OS-backed
// C++ wait; it neither changes scheduling nor runs guest code.
void JNICALL thread_start(jvmtiEnv*, JNIEnv*, jthread) {
  while (freezing.load(std::memory_order_acquire)) freezing.wait(true);
}
}
struct ExecutionFreeze::Impl {
  jvmtiEnv* ti = nullptr;
  std::vector<jthread> suspended;
  bool owner = false;
  jclass failure_class = nullptr;
  jfieldID failure_field = nullptr;
  ~Impl() {
    if (failure_class) host_env()->DeleteGlobalRef(failure_class);
    if (!owner) { if (ti) ti->DisposeEnvironment(); return; }
    freezing.store(false, std::memory_order_release);
    freezing.notify_all();
    if (!ti) return;
    ti->SetEventNotificationMode(JVMTI_DISABLE, JVMTI_EVENT_THREAD_START, nullptr);
    auto* env = host_env();
    for (auto thread : suspended) {
      ti->ResumeThread(thread);
      env->DeleteGlobalRef(thread);
    }
    ti->DisposeEnvironment();
  }
};
ExecutionFreeze::ExecutionFreeze() : impl_(std::make_unique<Impl>()) {
  auto* env = host_env();
  if (!env) return;
  auto cls = env->FindClass("com/venture/runtime/AppMain");
  if (cls) {
    impl_->failure_field = env->GetStaticFieldID(cls, "failureStatus", "I");
    impl_->failure_class = static_cast<jclass>(env->NewGlobalRef(cls));
    env->DeleteLocalRef(cls);
  }
  if (env->ExceptionCheck()) { env->ExceptionClear(); impl_->failure_field = nullptr; }
}
std::optional<int> ExecutionFreeze::recorded_failure() const {
  if (!impl_->failure_class || !impl_->failure_field) return {};
  auto* env = host_env();
  const auto status = env->GetStaticIntField(impl_->failure_class, impl_->failure_field);
  if (env->ExceptionCheck()) { env->ExceptionClear(); return {}; }
  return status;
}
bool ExecutionFreeze::save_work_quiet(const std::filesystem::path& root, std::string* error) {
  if (platform::data_writers(root) != platform::DataWriters::closed) {
    if (error) *error = "application data writer open or handle census unavailable";
    return false;
  }
  return application_quiet(error);
}
namespace {
// JVMTI reports a thread inside a native method as RUNNABLE. When that method
// is the game's own JNI code (declared by a class of the application loader),
// the thread is executing guest code: any file or host work it does goes
// through host calls, which native_save_work_quiet() classifies. GameActivity's
// onDestroy, for example, waits in terminateNativeCode for android_main, which
// Unity never returns. NativeHost's NativeActivity bridge is the same case: it
// runs the game's ANativeActivity callbacks (UE4's onStop waits there for its
// game thread). pollLooper is the idle main Looper of a task-removed (paused,
// stopped) Activity; callbacks it dispatches into the game are guest code too.
// JDK natives (file writes included) stay runnable work; network waits do not
// (in_network_wait below).
bool native_activity_bridge(jvmtiEnv* ti, jmethodID method, jclass cls) {
  char* signature = nullptr;
  char* name = nullptr;
  bool bridge = false;
  if (ti->GetClassSignature(cls, &signature, nullptr) == JVMTI_ERROR_NONE &&
      ti->GetMethodName(method, &name, nullptr, nullptr) == JVMTI_ERROR_NONE)
    bridge = std::string_view(signature) == "Lcom/venture/runtime/NativeHost;" &&
             (std::string_view(name) == "nativeActivityEvent" || std::string_view(name) == "createNativeActivity" ||
              std::string_view(name) == "pollLooper");
  if (signature) ti->Deallocate(reinterpret_cast<unsigned char*>(signature));
  if (name) ti->Deallocate(reinterpret_cast<unsigned char*>(name));
  return bridge;
}
bool in_application_native(jvmtiEnv* ti, JNIEnv* env, jthread thread, jint state) {
  if (!(state & JVMTI_THREAD_STATE_IN_NATIVE) || !current_loader()) return false;
  jvmtiFrameInfo top{};
  jint read{};
  if (ti->GetStackTrace(thread, 0, 1, &top, &read) != JVMTI_ERROR_NONE || read != 1) return false;
  jboolean native = JNI_FALSE;
  if (ti->IsMethodNative(top.method, &native) != JVMTI_ERROR_NONE || !native) return false;
  jclass cls{};
  if (ti->GetMethodDeclaringClass(top.method, &cls) != JVMTI_ERROR_NONE) return false;
  jobject loader{};
  const bool application = (ti->GetClassLoader(cls, &loader) == JVMTI_ERROR_NONE && loader &&
                            env->IsSameObject(loader, current_loader())) ||
                           native_activity_bridge(ti, top.method, cls);
  if (loader) env->DeleteLocalRef(loader);
  env->DeleteLocalRef(cls);
  return application;
}
// Whether the native at the top of the stack matches pred(class signature, method name).
template <typename Pred>
bool top_native_is(jvmtiEnv* ti, jthread thread, jint state, Pred pred) {
  if (!(state & JVMTI_THREAD_STATE_IN_NATIVE)) return false;
  jvmtiFrameInfo top{}; jint read{};
  if (ti->GetStackTrace(thread, 0, 1, &top, &read) != JVMTI_ERROR_NONE || read != 1) return false;
  char* method = nullptr; jclass cls{}; char* signature = nullptr;
  bool match = false;
  if (ti->GetMethodName(top.method, &method, nullptr, nullptr) == JVMTI_ERROR_NONE &&
      ti->GetMethodDeclaringClass(top.method, &cls) == JVMTI_ERROR_NONE &&
      ti->GetClassSignature(cls, &signature, nullptr) == JVMTI_ERROR_NONE)
    match = pred(std::string_view(signature), std::string_view(method));
  if (method) ti->Deallocate(reinterpret_cast<unsigned char*>(method));
  if (signature) ti->Deallocate(reinterpret_cast<unsigned char*>(signature));
  if (cls) host_env()->DeleteLocalRef(cls);
  return match;
}
// A JDK file native at the top of the stack: a write or sync the application
// is waiting on (SharedPreferences' QueuedWork in force0 on a slow disk).
bool in_file_native(jvmtiEnv* ti, jthread thread, jint state) {
  return top_native_is(ti, thread, state, [](std::string_view c, std::string_view m) {
    constexpr std::string_view writes[] = {"force0", "sync0", "write0", "writeBytes", "pwrite0", "writev0", "close0"};
    return (c.starts_with("Ljava/io/") || c.starts_with("Lsun/nio/ch/")) && std::ranges::find(writes, m) != std::end(writes);
  });
}
// A JDK network wait at the top of the stack: a socket poll, accept, connect
// or read, or a host name lookup. JVMTI reports it RUNNABLE in native, but the
// thread waits on a peer, not on a save; a Quest ends a removed task with such
// waits pending (an HTTP client's socket read in Net.poll, Puzzling Places).
bool in_network_wait(jvmtiEnv* ti, jthread thread, jint state) {
  return top_native_is(ti, thread, state, [](std::string_view c, std::string_view m) {
    if (c == "Lsun/nio/ch/Net;") return m == "poll" || m == "pollConnect" || m == "accept" || m == "connect0";
    if (c == "Lsun/nio/ch/SocketDispatcher;" || c == "Lsun/nio/ch/DatagramDispatcher;") return m == "read0" || m == "readv0";
    if (c == "Lsun/nio/ch/DatagramChannelImpl;") return m == "receive0";
    if (c == "Lsun/nio/ch/EPoll;") return m == "wait";
    return (c == "Ljava/net/Inet4AddressImpl;" || c == "Ljava/net/Inet6AddressImpl;") && m == "lookupAllHostAddr";
  });
}
// "<name> state=0x.. top=<class>.<method>" for failure reports.
std::string describe(jvmtiEnv* ti, jthread thread, jint state) {
  std::string out = "?";
  jvmtiThreadInfo info{};
  if (ti->GetThreadInfo(thread, &info) == JVMTI_ERROR_NONE && info.name) {
    out = info.name;
    ti->Deallocate(reinterpret_cast<unsigned char*>(info.name));
  }
  auto* env = host_env();
  if (info.thread_group) env->DeleteLocalRef(info.thread_group);
  if (info.context_class_loader) env->DeleteLocalRef(info.context_class_loader);
  out += std::format(" state=0x{:x}", state);
  jint frames = -1;
  if (ti->GetFrameCount(thread, &frames) == JVMTI_ERROR_NONE) out += std::format(" frames={}", frames);
  jvmtiFrameInfo top{}; jint read{};
  char* method = nullptr; jclass cls{}; char* signature = nullptr;
  if (ti->GetStackTrace(thread, 0, 1, &top, &read) == JVMTI_ERROR_NONE && read == 1 &&
      ti->GetMethodName(top.method, &method, nullptr, nullptr) == JVMTI_ERROR_NONE &&
      ti->GetMethodDeclaringClass(top.method, &cls) == JVMTI_ERROR_NONE &&
      ti->GetClassSignature(cls, &signature, nullptr) == JVMTI_ERROR_NONE)
    out += std::format(" top={}.{}", signature, method);
  if (method) ti->Deallocate(reinterpret_cast<unsigned char*>(method));
  if (signature) ti->Deallocate(reinterpret_cast<unsigned char*>(signature));
  if (cls) env->DeleteLocalRef(cls);
  return out;
}
}  // namespace
bool ExecutionFreeze::application_quiet(std::string* error, bool* saving) {
  if (saving) *saving = false;
  auto fail = [&](const char* reason) { if (error) *error = reason; return false; };
  auto& p = *impl_;
  if (!host_vm()) return fail("Java VM unavailable");
  if (!p.ti && host_vm()->GetEnv(reinterpret_cast<void**>(&p.ti), JVMTI_VERSION_1_2) != JNI_OK)
    return fail("JVMTI quiescence unavailable");
  jthread current{};
  if (p.ti->GetCurrentThread(&current) != JVMTI_ERROR_NONE) return fail("JVMTI current thread unavailable");
  auto* env = host_env();
  jint count{}; jthread* threads{};
  if (p.ti->GetAllThreads(&count, &threads) != JVMTI_ERROR_NONE) {
    env->DeleteLocalRef(current); return fail("JVMTI thread census unavailable");
  }
  bool quiet = true;
  // Census gaps (quiet cleared outside an application blocker) rule out a save.
  bool unknown = false, only_files = true;
  std::string blocker;  // the first application thread that kept the census open
  for (jint i = 0; i < count; ++i) {
    if (!env->IsSameObject(current, threads[i])) {
      jint state{};
      const auto result = p.ti->GetThreadState(threads[i], &state);
      if (result != JVMTI_ERROR_NONE && result != JVMTI_ERROR_THREAD_NOT_ALIVE) quiet = false, unknown = true;
      if (result == JVMTI_ERROR_NONE && (state & JVMTI_THREAD_STATE_RUNNABLE) &&
          !in_application_native(p.ti, env, threads[i], state) && !in_network_wait(p.ti, threads[i], state)) {
        bool application = false;
        std::string name = "?";
        jvmtiThreadInfo info{};
        if (p.ti->GetThreadInfo(threads[i], &info) == JVMTI_ERROR_NONE) {
          application = current_loader() && info.context_class_loader && env->IsSameObject(info.context_class_loader, current_loader());
          if (info.name) name = info.name;
          if (info.name) p.ti->Deallocate(reinterpret_cast<unsigned char*>(info.name));
          if (info.thread_group) env->DeleteLocalRef(info.thread_group);
          if (info.context_class_loader) env->DeleteLocalRef(info.context_class_loader);
        } else quiet = false, unknown = true;
        jint frames{};
        if (p.ti->GetFrameCount(threads[i], &frames) != JVMTI_ERROR_NONE || frames > 512) quiet = false, unknown = true;
        else if (frames) {
          std::vector<jvmtiFrameInfo> stack(frames); jint read{};
          if (p.ti->GetStackTrace(threads[i], 0, frames, stack.data(), &read) != JVMTI_ERROR_NONE) quiet = false, unknown = true;
          else for (jint j = 0; j < read && !application; ++j) {
            jclass cls{}; jobject loader{};
            if (p.ti->GetMethodDeclaringClass(stack[j].method, &cls) != JVMTI_ERROR_NONE) { quiet = false; unknown = true; break; }
            if (p.ti->GetClassLoader(cls, &loader) != JVMTI_ERROR_NONE) quiet = false, unknown = true;
            if (loader) { application = true; env->DeleteLocalRef(loader); }
            env->DeleteLocalRef(cls);
          }
        }
        if (application) {
          quiet = false;
          only_files = only_files && in_file_native(p.ti, threads[i], state);
          if (blocker.empty()) {
            blocker = describe(p.ti, threads[i], state);
            if (state & JVMTI_THREAD_STATE_IN_NATIVE)
              blocker.insert(std::min(blocker.find(" top="), blocker.size()), " in-native");
          }
        }
      }
    }
    env->DeleteLocalRef(threads[i]);
  }
  p.ti->Deallocate(reinterpret_cast<unsigned char*>(threads));
  env->DeleteLocalRef(current);
  if (saving) *saving = !quiet && !unknown && only_files;
  if (!quiet && error)
    *error = blocker.empty() ? "application thread runnable or quiescence unknown"
                             : "application thread runnable: " + blocker;
  return quiet;
}
ExecutionFreeze::~ExecutionFreeze() = default;
bool ExecutionFreeze::freeze(std::string* error) {
  auto fail = [&](const char* stage, int status) {
    if (error) *error = std::string(stage) + " status=" + std::to_string(status);
    return false;
  };
  auto& p = *impl_;
  if (p.owner || freezing.exchange(true)) return fail("Java freeze already active", -1);
  p.owner = true;
  auto* vm = host_vm();
  if (!vm) return fail("Java VM unavailable", -1);
  auto result = p.ti ? JNI_OK : vm->GetEnv(reinterpret_cast<void**>(&p.ti), JVMTI_VERSION_1_2);
  if (result != JNI_OK) return fail("JVMTI unavailable", result);
  jvmtiCapabilities capabilities{};
  capabilities.can_suspend = 1;
  auto status = p.ti->AddCapabilities(&capabilities);
  if (status != JVMTI_ERROR_NONE) return fail("JVMTI suspend capability", status);
  jvmtiEventCallbacks callbacks{};
  callbacks.ThreadStart = thread_start;
  status = p.ti->SetEventCallbacks(&callbacks, sizeof(callbacks));
  if (status != JVMTI_ERROR_NONE) return fail("JVMTI thread-start fence", status);
  status = p.ti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_THREAD_START, nullptr);
  if (status != JVMTI_ERROR_NONE) return fail("JVMTI enable thread-start fence", status);
  auto* env = host_env();
  auto loader_class = env->FindClass("java/lang/ClassLoader");
  auto system_method = loader_class ? env->GetStaticMethodID(loader_class, "getSystemClassLoader", "()Ljava/lang/ClassLoader;") : nullptr;
  auto system_loader = system_method ? env->CallStaticObjectMethod(loader_class, system_method) : nullptr;
  if (loader_class) env->DeleteLocalRef(loader_class);
  if (env->ExceptionCheck() || !system_loader) return fail("JVM system loader metadata", -1);
  jthread current{};
  status = p.ti->GetCurrentThread(&current);
  if (status != JVMTI_ERROR_NONE) return fail("JVMTI current thread", status);
  jint count = 0;
  jthread* threads = nullptr;
  status = p.ti->GetAllThreads(&count, &threads);
  if (status != JVMTI_ERROR_NONE) { env->DeleteLocalRef(current); return fail("JVMTI thread census", status); }
  bool complete = true;
  std::string blocker;  // the first application thread still running native code
  for (jint i = 0; i < count; ++i) {
    if (!env->IsSameObject(current, threads[i])) {
      jint frames = 0;
      status = p.ti->GetFrameCount(threads[i], &frames);
      // Pure attached native audio/device threads have no Java execution to
      // suspend; they must remain able to return from their host callbacks.
      if (status == JVMTI_ERROR_NONE && frames) {
        bool app_thread = false;
        jvmtiThreadInfo info{};
        if (p.ti->GetThreadInfo(threads[i], &info) == JVMTI_ERROR_NONE) {
          app_thread = info.context_class_loader && env->IsSameObject(info.context_class_loader, current_loader());
          if (info.name) p.ti->Deallocate(reinterpret_cast<unsigned char*>(info.name));
          if (info.thread_group) env->DeleteLocalRef(info.thread_group);
          if (info.context_class_loader) env->DeleteLocalRef(info.context_class_loader);
        } else complete = false;
        // A game can clear/change its context loader. Inspect actual Java
        // frames as well, rather than exempting such a thread's native I/O.
        if (!app_thread) {
          if (frames > 4096) complete = false;
          else {
            std::vector<jvmtiFrameInfo> stack(frames);
            jint read = 0;
            if (p.ti->GetStackTrace(threads[i], 0, frames, stack.data(), &read) == JVMTI_ERROR_NONE) {
              for (jint frame = 0; frame < read; ++frame) {
                jclass owner{}; jobject loader{};
                if (p.ti->GetMethodDeclaringClass(stack[frame].method, &owner) != JVMTI_ERROR_NONE) { complete = false; break; }
                if (p.ti->GetClassLoader(owner, &loader) != JVMTI_ERROR_NONE) complete = false;
                if (loader && !env->IsSameObject(loader, system_loader)) app_thread = true;
                if (loader) env->DeleteLocalRef(loader);
                env->DeleteLocalRef(owner);
              }
            } else complete = false;
          }
        }
        status = p.ti->SuspendThread(threads[i]);
        if (status == JVMTI_ERROR_NONE) {
          auto ref = static_cast<jthread>(env->NewGlobalRef(threads[i]));
          if (ref) p.suspended.push_back(ref);
          else { p.ti->ResumeThread(threads[i]); complete = false; }
          jint state = 0;
          if (p.ti->GetThreadState(threads[i], &state) != JVMTI_ERROR_NONE) complete = false;
          // JVMTI can mark a native call suspended before that call returns.
          // Do not call a blocked application write/JNI operation quiescent.
          // A thread inside the game's own JNI code or NativeHost's bridge
          // (pollLooper, NativeActivity callbacks) runs guest code, which the
          // OS quit has already stopped and drained before this freeze, as
          // application_quiet() classifies it.
          // A thread that left its last Java frame between the census and the
          // suspension is pure native now, like the attached threads above.
          jint suspended_frames = -1;
          const bool java_left = p.ti->GetFrameCount(threads[i], &suspended_frames) == JVMTI_ERROR_NONE &&
                                 suspended_frames == 0;
          if (app_thread && !java_left && (state & JVMTI_THREAD_STATE_IN_NATIVE) &&
              !(state & (JVMTI_THREAD_STATE_WAITING | JVMTI_THREAD_STATE_SLEEPING |
                         JVMTI_THREAD_STATE_BLOCKED_ON_MONITOR_ENTER)) &&
              !in_application_native(p.ti, env, threads[i], state) && !in_network_wait(p.ti, threads[i], state)) {
            complete = false;
            if (blocker.empty()) blocker = describe(p.ti, threads[i], state);
          }
        } else if (status != JVMTI_ERROR_THREAD_NOT_ALIVE) complete = false;
      } else if (status != JVMTI_ERROR_NONE && status != JVMTI_ERROR_THREAD_NOT_ALIVE) complete = false;
    }
    env->DeleteLocalRef(threads[i]);
  }
  p.ti->Deallocate(reinterpret_cast<unsigned char*>(threads));
  env->DeleteLocalRef(current);
  env->DeleteLocalRef(system_loader);
  if (!complete && error && !blocker.empty()) {
    *error = "JVMTI suspend application execution: " + blocker;
    return false;
  }
  return complete || fail("JVMTI suspend application execution", -1);
}
}  // namespace venture::java

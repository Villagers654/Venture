#include "common/environment.h"
#include "common/paths.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string_view>

#include "common/log.h"
#include "core/address_space.h"
#include "core/process.h"
#include "core/profile.h"
#include "java/internal.h"
#include "java/jni32.h"
#include "java/method_hooks.h"
#include "platform/java_host.h"

namespace venture::java {
namespace {
std::atomic<JavaVM*> vm{nullptr};
std::mutex lifecycle_mutex;
std::condition_variable lifecycle_changed;
enum class State { New, Starting, Running, Failed, Stopping, Stopped };
State state = State::New;
std::thread::id starter;
std::vector<NativeRegistrar> registrars;
std::string startup_error;
std::atomic<jobject> fallback_loader{nullptr};
thread_local ThreadState local;
}  // namespace
ThreadState& thread_state() { return local; }
ThreadState::~ThreadState() {
  if (process_config().isa() == GuestIsa::a32) jni32::clear_thread();
  if (attached_here)
    if (auto* current = host_vm()) current->DetachCurrentThread();
  if (wrapper) AddressSpace::get().free(wrapper);
}
JavaVM* host_vm() { return vm.load(); }
JNIEnv* env_no_attach() {
  auto* current = host_vm();
  if (!current) return nullptr;
  void* env = nullptr;
  if (current->GetEnv(&env, JNI_VERSION_1_6) != JNI_OK) return nullptr;
  local.env = static_cast<JNIEnv*>(env);
  return local.env;
}
jint attach_host(bool daemon, JavaVMAttachArgs* args, JNIEnv** out) {
  *out = nullptr;
  auto* current = host_vm();
  if (!current) return JNI_ERR;
  void* env = nullptr;
  jint status = current->GetEnv(&env, JNI_VERSION_1_6);
  if (status == JNI_EDETACHED) {
    status =
        daemon ? current->AttachCurrentThreadAsDaemon(&env, args) : current->AttachCurrentThread(&env, args);
    if (status == JNI_OK) local.attached_here = true;
  }
  if (status == JNI_OK) {
    local.env = static_cast<JNIEnv*>(env);
    *out = local.env;
  }
  return status;
}
jint detach_host() {
  if (local.native_depth) return JNI_ERR;
  auto* current = host_vm();
  if (!current) return JNI_ERR;
  if (process_config().isa() == GuestIsa::a32) jni32::clear_thread();
  jint status = current->DetachCurrentThread();
  if (status == JNI_OK) {
    local.env = nullptr;
    local.attached_here = false;
    if (auto* thread = GuestThread::current()) thread->jni_env = 0;
  }
  return status;
}
JNIEnv* host_env() {
  JNIEnv* env = nullptr;
  auto status = attach_host(false, nullptr, &env);
  if (status != JNI_OK) throw std::runtime_error("JVM thread attach failed: " + std::to_string(status));
  return env;
}
void throw_java(JNIEnv* env, const char* type, const char* message) noexcept {
  if (!env || env->ExceptionCheck()) return;
  if (auto cls = env->FindClass(type)) {
    env->ThrowNew(cls, message);
    env->DeleteLocalRef(cls);
  }
}
void throw_thread_death(JNIEnv* env) noexcept {
  if (env->ExceptionCheck()) return;
  auto cls = env->FindClass("java/lang/ThreadDeath");
  if (!cls) return;
  auto ctor = env->GetMethodID(cls, "<init>", "()V");
  auto error = ctor ? static_cast<jthrowable>(env->NewObject(cls, ctor)) : nullptr;
  if (error) { env->Throw(error); env->DeleteLocalRef(error); }
  env->DeleteLocalRef(cls);
}
jobject current_loader() { return local.has_loader ? local.loader : fallback_loader.load(); }
void set_app_class_loader(void* value) {
  JNIEnv* env = host_env();
  jobject ref = value ? env->NewGlobalRef(static_cast<jobject>(value)) : nullptr;
  if (value && !ref) throw std::runtime_error("cannot retain app class loader");
  // Retain previous loaders until shutdown: in-flight transitions borrow them.
  fallback_loader.store(ref);
}
bool start_jvm(const JvmOptions& options, std::string* error) {
  register_jni_library();
  {
    std::unique_lock lock(lifecycle_mutex);
    if (state == State::Starting && starter == std::this_thread::get_id()) return host_vm() != nullptr;
    lifecycle_changed.wait(lock, [] { return state != State::Starting; });
    if (state == State::Running) return true;
    if (state != State::New) {
      if (error) *error = startup_error.empty() ? "JVM cannot be restarted" : startup_error;
      return false;
    }
    state = State::Starting;
    starter = std::this_thread::get_id();
  }
  try {
    if (options.java_home.empty()) throw std::invalid_argument("java_home is required");
    std::u8string home(reinterpret_cast<const char8_t*>(options.java_home.data()), options.java_home.size());
    auto create = reinterpret_cast<jint(JNICALL*)(JavaVM**, void**, void*)>(
        platform::jvm_entry(std::filesystem::path(home)));
    // Native engines keep their large assets outside Java. HotSpot's RAM-based
    // ergonomics otherwise commit ~1 GiB initially on a 64 GiB test host.
    // Keep room to grow; explicit extra_options below can override these limits.
    // android.graphics uses java.desktop (java.awt.image, ImageIO, Font) as a
    // pure library; headless keeps it off any display, e.g. Wayland-only hosts.
    // The heap stays above 4 GiB: HotSpot would otherwise place it just below,
    // where native games reserve fixed windows (a 32-bit game address space at
    // 0x80000000). Oops stay compressed and zero-based.
    std::vector<std::string> values{"-Xss4m", "-XX:+UseG1GC", "-Xms32m", "-Xmx1g", "-XX:HeapBaseMinAddress=4g",
                                    "-Djava.awt.headless=true"};
    if (!options.agent_path.empty()) {
      const auto agent = paths::from_utf8(options.agent_path);
      if (!agent.is_absolute() || !std::filesystem::is_regular_file(agent))
        throw std::runtime_error("Bundled JVM agent is missing or not an absolute path");
      values.push_back("-agentpath:" + options.agent_path);
    }
    bool diagnostics = false;
#ifndef NDEBUG
    diagnostics = true;
#endif
    if (const auto* setting = venture::environment::getenv("VENTURE_JVM_DIAGNOSTICS")) {
      const std::string_view value(setting);
      if (value != "0" && value != "1")
        throw std::invalid_argument("VENTURE_JVM_DIAGNOSTICS must be 0 or 1");
      diagnostics = value == "1";
    }
    if (diagnostics) {
      values.push_back("-Xcheck:jni");
      values.push_back("-Xverify:all");
    }
    log::info("jvm", "JNI diagnostics {}", diagnostics ? "enabled" : "disabled");
    std::string path = "-Djava.class.path=";
    for (const auto& item : options.class_path) {
      if (path.back() != '=') path += platform::class_path_separator;
      path += item;
    }
    values.push_back(path);
    for (auto& option : platform::jvm_host_options()) values.push_back(std::move(option));
    values.insert(values.end(), options.extra_options.begin(), options.extra_options.end());
    const auto& device = *process_config().device_identity;
    values.push_back("-Dventure.device.model=" + std::string(device.model));
    values.push_back("-Dventure.device.name=" + std::string(device.device));
    values.push_back("-Dventure.guest.abi=" + process_config().abi);
    values.push_back("-Dventure.device.memory=" + std::to_string(guest_usable_memory()));
    std::vector<JavaVMOption> native;
    for (auto& value : values) native.push_back({value.data(), nullptr});
    JavaVMInitArgs args{JNI_VERSION_1_8, static_cast<jint>(native.size()), native.data(), JNI_FALSE};
    JavaVM* instance = nullptr;
    void* env = nullptr;
    jint result = create(&instance, &env, &args);
    if (result != JNI_OK) throw std::runtime_error("JNI_CreateJavaVM failed: " + std::to_string(result));
    vm.store(instance);
    local.env = static_cast<JNIEnv*>(env);
    if (!options.agent_path.empty())
      initialize_method_hooks(paths::from_utf8(options.agent_path));
    init_bridge(local.env);
    if (local.env->ExceptionCheck())
      throw std::runtime_error("NativeLoader initialization failed (pending Java exception)");
    std::size_t next = 0;
    for (;;) {
      NativeRegistrar registrar;
      {
        std::lock_guard lock(lifecycle_mutex);
        if (next == registrars.size()) {
          state = State::Running;
          break;
        }
        registrar = registrars[next++];
      }
      registrar(local.env);
      if (local.env->ExceptionCheck())
        throw std::runtime_error("framework native registrar left a pending exception");
    }
    lifecycle_changed.notify_all();
    log::info("java", "HotSpot started (JNI bridge {} / host x64)",
              process_config().isa() == GuestIsa::a32 ? "ARM32" : "ARM64");
    profile::mark("jvm_ready");
    return true;
  } catch (const std::exception& e) {
    {
      std::lock_guard lock(lifecycle_mutex);
      state = State::Failed;
      startup_error = e.what();
    }
    lifecycle_changed.notify_all();
    log::error("java", "{}", e.what());
    if (error) *error = e.what();
    return false;
  }
}
void add_framework_natives(NativeRegistrar registrar) {
  if (!registrar) throw std::invalid_argument("null framework registrar");
  bool invoke;
  {
    std::lock_guard lock(lifecycle_mutex);
    if (std::ranges::find(registrars, registrar) != registrars.end()) return;
    if (state == State::Failed || state == State::Stopped)
      throw std::runtime_error("JVM not available for framework registration");
    registrars.push_back(registrar);
    invoke = state == State::Running;
  }
  if (invoke) {
    auto* env = host_env();
    registrar(env);
    if (env->ExceptionCheck()) throw std::runtime_error("framework registrar left a pending exception");
  }
}
bool stop_jvm(std::string* error) {
  auto* current = host_vm();
  if (!current) return true;
  if (local.native_depth) {
    if (error) *error = "cannot destroy JVM from a Java-to-native transition";
    return false;
  }
  {
    std::lock_guard lock(lifecycle_mutex);
    if (state == State::Starting || state == State::Stopping) {
      if (error) *error = "JVM lifecycle transition already in progress";
      return false;
    }
    state = State::Stopping;
  }
  stop_method_hooks();
  jint status = current->DestroyJavaVM();
  if (status != JNI_OK) {
    {
      std::lock_guard lock(lifecycle_mutex);
      state = State::Failed;
    }
    if (error) *error = "DestroyJavaVM failed: " + std::to_string(status);
    return false;
  }
  vm.store(nullptr);
  local.env = nullptr;
  local.attached_here = false;
  {
    std::lock_guard lock(lifecycle_mutex);
    state = State::Stopped;
  }
  return true;
}
}  // namespace venture::java

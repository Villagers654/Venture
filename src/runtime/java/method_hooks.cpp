#include "java/method_hooks.h"
#include "java/internal.h"
#include "java/jni32.h"
#include "java/tooling_agent.h"
#include "core/address_space.h"
#include "core/guest_storage.h"
#include "core/hle_runtime.h"
#include "core/thread_runtime.h"
#include "core/linker.h"
#include "core/process.h"
#include "core/signals.h"
#include "platform/java_host.h"
#include <atomic>
#include <cstring>
#include <memory>
#include <array>

namespace venture::java {
namespace {
decltype(&VentureJvmtiInstallHook) install = nullptr;
decltype(&VentureJvmtiRemoveHook) remove = nullptr;
decltype(&VentureJvmtiDispatchHook) dispatch = nullptr;
decltype(&VentureJvmtiEnvironment) environment = nullptr;
std::atomic<bool> stopping{false};
struct LoadAlias {
  jmethodID canonical{};
  jclass owner{};  // process-lifetime global reference
  bool runtime{};
};
std::array<LoadAlias, 4> load_aliases;
// Bootstrap System/Runtime are shared with the host JVM. Their guest hooks
// apply only at the explicit guest native-load boundary, never to host JDK
// infrastructure (for example AWT loading its host-native graphics library).
thread_local jmethodID guest_load_dispatch{};
bool canonical_load(jmethodID id) {
  for (const auto& alias : load_aliases)
    if (alias.canonical == id) return true;
  return false;
}

jint JNICALL invoke_load_alias(JNIEnv* env, jobject, const jvalue* args, jint count,
                              jvalue* result, void* opaque) {
  auto& alias = *static_cast<LoadAlias*>(opaque);
  const auto offset = alias.runtime ? 1 : 0;
  if (count != 1 + offset) return -JVMTI_ERROR_ILLEGAL_ARGUMENT;
  if (alias.runtime && !args[0].l) return 0;  // let the wrapper raise the invocation's NullPointerException
  return dispatch_method_hook(env, alias.canonical, alias.runtime ? args[0].l : alias.owner,
                              {args + offset, 1}, result);
}
struct Context {
  MethodInfo method;
  jmethodID id{};
  GuestAddr callback, user, release;
};

jint JNICALL invoke(JNIEnv* env, jobject receiver, const jvalue* args, jint count,
                    jvalue* result, void* opaque) {
  auto& context = *static_cast<Context*>(opaque);
  if (canonical_load(context.id) && guest_load_dispatch != context.id) return 0;
  if (stopping.load(std::memory_order_acquire)) return 0;
  auto& tls = thread_state();
  ++tls.native_depth;
  struct Depth { unsigned& value; ~Depth() { --value; } } depth{tls.native_depth};
  try {
    LocalFrame frame(env, 64 + count);
    if (!frame.valid()) return 2;
    auto& thread = GuestThread::adopt_current();
    GuestStorage storage;
    const auto data = storage.resize((static_cast<std::size_t>(count) + 1) * sizeof(jvalue));
    const auto output = data + count * sizeof(jvalue);
    if (count) std::memcpy(g2h(data), args, count * sizeof(jvalue));
    std::memset(g2h(output), 0, sizeof(jvalue));
    jint handled;
    if (thread.cpu().isa() == GuestIsa::a32) {
      jni32::PinScope pins;
      jni32::NativeScope tokens;
      auto* values = g2h<jvalue>(data);
      for (jint i = 0; i < count; ++i)
        if (context.method.descriptor.parameters[i] == Type::Object) {
          values[i].j = 0;
          values[i].i = static_cast<jint>(jni32::reference(args[i].l));
        }
      hle::a32::CallBuilder32 call;
      call.pointer(guest_env(thread)).value(jni32::reference(receiver)).pointer(data).pointer(output).pointer(context.user);
      handled = static_cast<jint>(thread.call32(context.callback, call).x0);
      *result = *g2h<jvalue>(output);
      if (handled == 1 && context.method.descriptor.result == Type::Object)
        result->l = tokens.pop(env->ExceptionCheck() ? nullptr : jni32::object(static_cast<std::uint32_t>(result->i)));
    } else {
      handled = static_cast<jint>(thread.call(context.callback,
          {guest_env(thread), h2g(receiver), data, output, context.user}));
      *result = *g2h<jvalue>(output);
    }
    if (env->ExceptionCheck()) return 2;
    if (handled != 0 && handled != 1 && handled != 2)
      throw std::runtime_error("Invalid hosted Java callback disposition");
    if (handled == 2) throw std::runtime_error("Java callback requested exception propagation without an exception");
    if (handled == 1 && context.method.descriptor.result == Type::Object) result->l = frame.pop(result->l);
    return handled;
  } catch (const ThreadExit&) { throw_thread_death(env); }
    catch (const ProcessExit&) { throw_thread_death(env); }
    catch (const std::exception& error) { throw_java(env, "java/lang/InternalError", error.what()); }
    catch (...) { throw_java(env, "java/lang/InternalError", "Hosted Java callback failed"); }
  return 2;
}
void release(void* opaque) noexcept {
  std::unique_ptr<Context> context(static_cast<Context*>(opaque));
  if (!context->release || stopping.load(std::memory_order_acquire) || !environment || !environment()) return;
  try {
    auto& thread = GuestThread::adopt_current();
    if (thread.cpu().isa() == GuestIsa::a32)
      thread.call32(context->release, hle::a32::CallBuilder32{}.pointer(context->user));
    else thread.call(context->release, {context->user});
  } catch (...) {
    // Release notifications cannot unwind through a JVM agent destructor.
    // The embedding frontend must keep release callbacks nonthrowing.
  }
}
jmethodID method(hle::CallContext& call) {
  return call.cpu().isa() == GuestIsa::a32 ? jni32::method(static_cast<std::uint32_t>(call.x(0)))
                                         : g2h<_jmethodID>(call.x(0));
}
void install_guest(hle::CallContext& call) {
  try {
    const auto id = method(call);
    // Never pass untrusted native IDs to HotSpot, which may dereference them.
    auto info = method_info(id);
    if (!install || stopping.load(std::memory_order_acquire)) { call.return_x(JVMTI_ERROR_NOT_AVAILABLE); return; }
    if (!AddressSpace::get().is_guest_executable(call.x(1)) ||
        (call.x(3) && !AddressSpace::get().is_guest_executable(call.x(3)))) {
      call.return_x(JVMTI_ERROR_ILLEGAL_ARGUMENT); return;
    }
    auto context = std::make_unique<Context>(Context{std::move(info), id, call.x(1), call.x(2), call.x(3)});
    auto error = install(id, invoke, context.get(), release);
    if (error == JVMTI_ERROR_NONE) context.release();
    call.return_x(error);
  } catch (const std::invalid_argument&) { call.return_x(JVMTI_ERROR_INVALID_METHODID); }
}
void remove_guest(hle::CallContext& call) {
  try {
    const auto id = method(call);
    method_info(id);
    call.return_x(remove ? remove(id) : JVMTI_ERROR_NOT_AVAILABLE);
  } catch (const std::invalid_argument&) { call.return_x(JVMTI_ERROR_INVALID_METHODID); }
}
}
void initialize_method_hooks(const std::filesystem::path& agent) {
  auto symbol = [&](const char* name) { return platform::loaded_agent_symbol(agent, name); };
  auto status = reinterpret_cast<decltype(&VentureJvmtiStatus)>(symbol("VentureJvmtiStatus"));
  if (auto error = status(); error != JVMTI_ERROR_NONE)
    throw std::runtime_error("Bundled JVM method tooling unavailable: " + std::to_string(error));
  install = reinterpret_cast<decltype(install)>(symbol("VentureJvmtiInstallHook"));
  remove = reinterpret_cast<decltype(remove)>(symbol("VentureJvmtiRemoveHook"));
  dispatch = reinterpret_cast<decltype(dispatch)>(symbol("VentureJvmtiDispatchHook"));
  environment = reinterpret_cast<decltype(environment)>(symbol("VentureJvmtiEnvironment"));
  auto* env = host_env();
  LocalFrame frame(env);
  if (!frame.valid()) throw std::runtime_error("Cannot initialize Java load hooks");
  auto bridge = env->FindClass("com/venture/runtime/NativeLoader");
  auto system = env->FindClass("java/lang/System");
  auto runtime = env->FindClass("java/lang/Runtime");
  if (!bridge || !system || !runtime || env->ExceptionCheck())
    throw std::runtime_error("Cannot resolve Java native-load bridge");
  for (unsigned i = 0; i < load_aliases.size(); ++i) {
    auto& alias = load_aliases[i];
    alias.runtime = i >= 2;
    const char* name = i % 2 ? "load" : "loadLibrary";
    auto owner = alias.runtime ? runtime : system;
    alias.owner = static_cast<jclass>(env->NewGlobalRef(owner));
    alias.canonical = alias.runtime ? env->GetMethodID(owner, name, "(Ljava/lang/String;)V")
                                   : env->GetStaticMethodID(owner, name, "(Ljava/lang/String;)V");
    auto rewritten = env->GetStaticMethodID(bridge, name, alias.runtime
        ? "(Ljava/lang/Runtime;Ljava/lang/String;)V" : "(Ljava/lang/String;)V");
    if (!alias.owner || !alias.canonical || !rewritten || env->ExceptionCheck())
      throw std::runtime_error("Cannot resolve rewritten Java load hook");
    if (auto error = install(rewritten, invoke_load_alias, &alias, nullptr); error != JVMTI_ERROR_NONE)
      throw std::runtime_error("Cannot register rewritten Java load hook: " + std::to_string(error));
  }
}
jint dispatch_method_hook(JNIEnv* env, jmethodID id, jobject receiver, std::span<const jvalue> args, jvalue* result) {
  const auto previous = guest_load_dispatch;
  guest_load_dispatch = id;
  struct Restore { jmethodID previous; ~Restore() { guest_load_dispatch = previous; } } restore{previous};
  return dispatch && !stopping.load(std::memory_order_acquire)
      ? dispatch(env, id, receiver, args.data(), static_cast<jint>(args.size()), result) : 0;
}
void stop_method_hooks() { stopping.store(true, std::memory_order_release); }
void register_method_hook_library() {
  auto lib = std::make_shared<HleLibrary>("libventure_java_bridge.so");
  auto add = [&](const char* name, hle::Handler handler) {
    if (process_config().isa() == GuestIsa::a32) hle::a32::register_adapter(lib->soname(), name, handler);
    lib->add_function(name, hle::make_thunk(lib->soname(), name, handler));
  };
  add("VentureJavaInstallHook", install_guest);
  add("VentureJavaRemoveHook", remove_guest);
  Linker::get().register_hle_library(lib);
  if (!Linker::get().open(lib->soname(), kRtldNow | kRtldGlobal, nullptr))
    throw std::runtime_error("Cannot publish hosted Java interoperability API");
}
}  // namespace venture::java

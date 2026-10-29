#include "common/environment.h"
#include <array>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <cstdlib>
#include <unordered_set>

#include "common/log.h"
#include "core/address_space.h"
#include "core/process_paths.h"
#include "core/process_runtime.h"
#include "core/signals.h"
#include "core/thread_runtime.h"
#include "java/internal.h"
#include "java/jni32.h"
#include "java/method_hooks.h"
#include "core/process.h"
#include "core/linker.h"
#include "core/hle_runtime.h"
#include "platform/memory.h"

namespace venture::java {
namespace {
std::mutex method_mutex;
std::unordered_map<jmethodID, MethodInfo> methods;
std::unordered_set<jmethodID> path_methods;
std::unordered_set<jfieldID> path_fields;
std::unordered_set<jmethodID> path_constructors;
std::unordered_set<jmethodID> process_starts;
std::mutex utf_mutex;
std::unordered_map<const char*, jstring> utf_strings;
GuestAddr env_table = 0, vm_wrapper = 0;
std::once_flag env_once, vm_once;

template <class T>
T read(hle::ArgCursor& cursor) {
  if constexpr (std::is_same_v<T, float>)
    return cursor.next_float();
  else if constexpr (std::is_same_v<T, double>)
    return cursor.next_double();
  else if constexpr (std::is_pointer_v<T>)
    return reinterpret_cast<T>(cursor.next_gpr());
  else
    return static_cast<T>(cursor.next_gpr());
}
template <class T>
void result(hle::CallContext& ctx, T v) {
  if (ctx.cpu().isa() == GuestIsa::a32) {
    auto bits = jni32::encode(v);
    if constexpr (std::is_same_v<T, jmethodID> || std::is_same_v<T, jfieldID>) {
      if (v) {
        auto* env = env_no_attach();
        jclass owner = nullptr;
        const bool reflected = ctx.info().name.starts_with("FromReflected");
        if (reflected) {
          auto reflected_object = jni32::object(static_cast<std::uint32_t>(ctx.x(1)));
          auto cls = env->GetObjectClass(reflected_object);
          auto declaring = cls ? env->GetMethodID(cls, "getDeclaringClass", "()Ljava/lang/Class;") : nullptr;
          if (declaring) owner = static_cast<jclass>(env->CallObjectMethod(reflected_object, declaring));
          env->ExceptionCheck();
          if (cls) env->DeleteLocalRef(cls);
        } else
          owner = static_cast<jclass>(jni32::object(static_cast<std::uint32_t>(ctx.x(1))));
        if (!env->ExceptionCheck())
          jni32::retain_id_owner(static_cast<std::uint32_t>(bits), std::is_same_v<T, jmethodID>, owner);
        if (reflected && owner) env->DeleteLocalRef(owner);
      }
    }
    hle::a32::ResultWriter32(ctx.cpu()).scalar(ctx.info().a32->result, bits);
    return;
  }
  if constexpr (std::is_same_v<T, float>)
    ctx.return_s(v);
  else if constexpr (std::is_same_v<T, double>)
    ctx.return_d(v);
  else if constexpr (std::is_pointer_v<T>)
    ctx.return_x(h2g(v));
  else if constexpr (std::is_signed_v<T>)
    ctx.return_x(static_cast<std::int64_t>(v));
  else
    ctx.return_x(static_cast<std::uint64_t>(v));
}
template <class R, class... A>
void fixed(R(JNICALL* fn)(JNIEnv*, A...), JNIEnv* env, hle::CallContext& ctx) {
  hle::ArgCursor cursor(ctx);
  cursor.next_gpr();
  std::tuple<A...> args{read<A>(cursor)...};
  if constexpr (std::is_void_v<R>)
    std::apply([&](A... a) { fn(env, a...); }, args);
  else
    result(ctx, std::apply([&](A... a) { return fn(env, a...); }, args));
}
template <class R, class... A>
void fixed32(R(JNICALL* fn)(JNIEnv*, A...), JNIEnv* env, hle::CallContext& ctx) {
  if (jni32::special(env, ctx)) return;
  hle::a32::trace_enter(ctx);
  hle::a32::ArgCursor32 cursor(ctx.cpu());
  cursor.word();  // guard validates the owning thread's JNIEnv wrapper
  std::size_t index = 1;
  std::tuple<A...> args{jni32::decode<A>(cursor.next(ctx.info().a32->arguments[index++]))...};
  if constexpr (std::is_void_v<R>)
    std::apply([&](A... a) { fn(env, a...); }, args);
  else
    result(ctx, std::apply([&](A... a) { return fn(env, a...); }, args));
  hle::a32::trace_leave(ctx);
}
// Java retains real host File objects for JDK I/O. Native Android engines also
// parse paths before passing them to libc, so adapt path-valued API results at
// the JNI boundary rather than relying only on the VFS filename aliases.
jobject guest_path_result(JNIEnv* env, jobject object) {
  if (!object || env->ExceptionCheck()) return object;
  auto string = static_cast<jstring>(object);
  const auto* utf = env->GetStringUTFChars(string, nullptr);
  if (!utf) return object;
  auto mapped = host_path_to_guest(utf);
  env->ReleaseStringUTFChars(string, utf);
  if (!mapped) return object;
  auto replacement = env->NewStringUTF(mapped->c_str());
  env->DeleteLocalRef(object);
  return replacement;
}
// Diagnostic only (VENTURE_TRACE_JNI_SLOW_US): names guest JNI calls on A64
// whose host duration reaches the threshold, through Java reflection.
const long slow_call_us = [] {
  const char* value = venture::environment::getenv("VENTURE_TRACE_JNI_SLOW_US");
  return value ? std::strtol(value, nullptr, 10) : 0L;
}();
struct SlowCallStart {
  std::chrono::steady_clock::time_point begin;
  std::uint64_t x1, x2, x3;
};
SlowCallStart slow_call_start(hle::CallContext& ctx) {
  return {std::chrono::steady_clock::now(), ctx.x(1), ctx.x(2), ctx.x(3)};
}
// Names the called method through reflection. Entered only with no pending
// exception, so every exception raised here belongs to the diagnostic and is
// cleared before the local frame (and its references) is released.
// The pending exception and its first frames (and its causes'), for the
// runtime log: ExceptionDescribe reaches only stderr, which a debug report
// keeps for the last run alone. The exception stays pending.
std::string pending_exception(JNIEnv* env) {
  const auto thrown = env->ExceptionOccurred();
  if (!thrown) return "-";
  env->ExceptionClear();
  std::string text;
  if (env->PushLocalFrame(32) == JNI_OK) {
    struct Frame {
      JNIEnv* env;
      ~Frame() { env->ExceptionClear(); env->PopLocalFrame(nullptr); }
    } frame{env};
    const auto string = [&](jobject object) -> std::string {
      const auto cls = env->FindClass("java/lang/Object");
      const auto to_string = cls ? env->GetMethodID(cls, "toString", "()Ljava/lang/String;") : nullptr;
      const auto value = to_string ? static_cast<jstring>(env->CallObjectMethod(object, to_string)) : nullptr;
      if (!value || env->ExceptionCheck()) { env->ExceptionClear(); return "?"; }
      const auto* utf = env->GetStringUTFChars(value, nullptr);
      if (!utf) { env->ExceptionClear(); return "?"; }
      std::string out = utf;
      env->ReleaseStringUTFChars(value, utf);
      return out;
    };
    const auto throwable = env->FindClass("java/lang/Throwable");
    const auto trace = throwable ? env->GetMethodID(throwable, "getStackTrace", "()[Ljava/lang/StackTraceElement;") : nullptr;
    const auto cause = throwable ? env->GetMethodID(throwable, "getCause", "()Ljava/lang/Throwable;") : nullptr;
    jobject current = thrown;
    for (int depth = 0; current && trace && cause && depth < 3 && !env->ExceptionCheck(); ++depth) {
      text += (depth ? "; caused by " : "") + string(current);
      const auto frames = static_cast<jobjectArray>(env->CallObjectMethod(current, trace));
      if (frames && !env->ExceptionCheck())
        for (jsize i = 0, n = std::min<jsize>(env->GetArrayLength(frames), 6); i < n; ++i)
          text += " | at " + string(env->GetObjectArrayElement(frames, i));
      env->ExceptionClear();
      current = env->CallObjectMethod(current, cause);
    }
  }
  env->Throw(thrown);
  return text.empty() ? "-" : text;
}
std::string slow_call_method(JNIEnv* env, std::string_view name, const SlowCallStart& start) {
  if (env->PushLocalFrame(8) != JNI_OK) {
    env->ExceptionClear();  // the failed push raised OutOfMemoryError
    return "-";
  }
  struct Frame {
    JNIEnv* env;
    ~Frame() { env->ExceptionClear(); env->PopLocalFrame(nullptr); }
  } frame{env};
  const bool is_static = name.starts_with("CallStatic"), nonvirtual = name.starts_with("CallNonvirtual");
  jclass cls = is_static ? reinterpret_cast<jclass>(start.x1) : nonvirtual ? reinterpret_cast<jclass>(start.x2) : nullptr;
  if (!is_static && !nonvirtual && start.x1) cls = env->GetObjectClass(reinterpret_cast<jobject>(start.x1));
  const auto id = reinterpret_cast<jmethodID>(nonvirtual ? start.x3 : start.x2);
  if (!cls || !id || env->ExceptionCheck()) return "-";
  const auto reflected = env->ToReflectedMethod(cls, id, is_static);
  if (!reflected || env->ExceptionCheck()) return "-";
  const auto object = env->FindClass("java/lang/Object");
  if (!object || env->ExceptionCheck()) return "-";
  const auto to_string = env->GetMethodID(object, "toString", "()Ljava/lang/String;");
  if (!to_string || env->ExceptionCheck()) return "-";
  const auto text = static_cast<jstring>(env->CallObjectMethod(reflected, to_string));
  if (!text || env->ExceptionCheck()) return "-";
  const auto* utf = env->GetStringUTFChars(text, nullptr);
  if (!utf) return "-";
  std::string method;
  try { method = utf; } catch (...) { env->ReleaseStringUTFChars(text, utf); throw; }
  env->ReleaseStringUTFChars(text, utf);
  return method;
}
// Never lets a diagnostic failure reach the guest-exception handler.
void trace_slow_call(JNIEnv* env, hle::CallContext& ctx, std::string_view name, const SlowCallStart& start) noexcept {
  try {
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start.begin).count();
    static std::atomic<int> lines{0};
    if (us < slow_call_us || lines.fetch_add(1) >= 4000) return;
    std::string method = "-";
    if (name.starts_with("Call") && ctx.cpu().isa() == GuestIsa::a64 && !env->ExceptionCheck())
      method = slow_call_method(env, name, start);
    log::info("jni-slow", "{} {} {} us guest_tid={}", name, method, us, ctx.thread().tid());
  } catch (...) {
  }
}
template <class F>
void guard(hle::CallContext& ctx, F&& fn) {
  JNIEnv* env = nullptr;
  try {
    env = env_no_attach();
    if (!env || ctx.x(0) != thread_state().wrapper || !thread_state().wrapper)
      throw std::runtime_error("JNIEnv used on detached/wrong host thread");
    // JNI Design Overview, "Exception Handling": only exception inspection,
    // release/delete, monitor exit and local-frame operations may run while an
    // exception is pending. Diagnose guest misuse before HotSpot aborts the host.
    // https://docs.oracle.com/en/java/javase/21/docs/specs/jni/design.html
    const std::string_view name = ctx.info().name;
    const bool pending_safe = name.starts_with("Exception") || name.starts_with("Release") ||
        name == "DeleteLocalRef" || name == "DeleteGlobalRef" || name == "DeleteWeakGlobalRef" ||
        name == "MonitorExit" || name == "PushLocalFrame" || name == "PopLocalFrame";
    if (!pending_safe && env->ExceptionCheck()) {
      log::error("jni", "guest called {} with a pending Java exception: {}", name, pending_exception(env));
      env->ExceptionDescribe();
      if (venture::environment::getenv("VENTURE_TRACE_THREADS")) signals::log_guest_threads("JNI call with a pending exception");
      signals::deliver_synchronous(ctx.thread(), {signals::kSigAbrt, 0, ctx.cpu().pc()});
      exit_guest_process(134);
    }
    static const bool trace = venture::environment::getenv("VENTURE_TRACE_JNI") != nullptr;
    if (trace && name == "FindClass") log::info("jni-trace", "FindClass {}", g2h<const char>(ctx.x(1)));
    if (trace && (name == "GetMethodID" || name == "GetStaticMethodID" || name == "GetFieldID" || name == "GetStaticFieldID"))
      log::info("jni-trace", "{} {} {}", name, g2h<const char>(ctx.x(2)), g2h<const char>(ctx.x(3)));
    if (trace && name == "ExceptionClear" && env->ExceptionCheck()) env->ExceptionDescribe();
    std::optional<jni32::PinScope> pins;
    if (ctx.cpu().isa() == GuestIsa::a32) pins.emplace();
    const auto slow_start = slow_call_us > 0 ? slow_call_start(ctx) : SlowCallStart{};
    fn(env);
    if (slow_call_us > 0) [[unlikely]] trace_slow_call(env, ctx, name, slow_start);
    // JNI Call*Method requires an exception check before subsequent host JNI
    // calls, including reference cleanup. Inspect without clearing: the guest
    // still owns handling any pending exception (JNI Functions, Exceptions).
    env->ExceptionCheck();
  } catch (const ProcessExit&) { throw;
  } catch (const ThreadExit&) { throw;
  } catch (const std::exception& e) {
    log::error("jni", "{}: {}", ctx.info().name, e.what());
    throw_java(env, "java/lang/IllegalStateException", e.what());
    ctx.return_x(0);
    if (ctx.cpu().isa() == GuestIsa::a32)
      ctx.return_x1(0);
    else
      ctx.return_d(0);
  }
  // The core's nonlocal guest-return control flow must unwind through JNI HLE
  // handlers (e.g. a guest signal handler longjmps across a Java callback).
}
enum class Kind { Virtual, Static, Nonvirtual, Constructor };
enum class Return { Object, Boolean, Byte, Char, Short, Int, Long, Float, Double, Void };

template <class T>
T reference_arg(hle::CallContext& ctx, std::uint64_t value) {
  return ctx.cpu().isa() == GuestIsa::a32 ? jni32::decode<T>(value) : reinterpret_cast<T>(value);
}

void special_GetStringUTFChars(JNIEnv* env, hle::CallContext& ctx) {
  // A32 guests pass Token32 references and receive low-memory copies (jni32).
  if (ctx.cpu().isa() == GuestIsa::a32) { fixed32(env->functions->GetStringUTFChars, env, ctx); return; }
  auto string = g2h<_jstring>(ctx.x(1));
  if (!string) { ctx.return_x(0); return; }
  auto owner = static_cast<jstring>(env->NewGlobalRef(string));
  if (!owner) { ctx.return_x(0); return; }
  const auto* chars = env->GetStringUTFChars(owner, g2h<jboolean>(ctx.x(2)));
  if (!chars) { env->DeleteGlobalRef(owner); ctx.return_x(0); return; }
  try {
    std::lock_guard lock(utf_mutex);
    utf_strings.emplace(chars, owner);
  } catch (...) {
    env->ReleaseStringUTFChars(owner, chars);
    env->DeleteGlobalRef(owner);
    throw_java(env, "java/lang/OutOfMemoryError", "JNI UTF buffer ownership");
    ctx.return_x(0);
    return;
  }
  ctx.return_x(h2g(chars));
}
void special_ReleaseStringUTFChars(JNIEnv* env, hle::CallContext& ctx) {
  if (ctx.cpu().isa() == GuestIsa::a32) { fixed32(env->functions->ReleaseStringUTFChars, env, ctx); return; }
  // ART android-12.1.0_r1 runtime/jni/jni_internal.cc ignores the jstring
  // argument and deletes the UTF copy. Unity 2020 passes a non-reference
  // argument during filename discovery. Keep a private global only to release
  // HotSpot's buffer safely; it is never exposed as a guest reference.
  const auto* chars = g2h<const char>(ctx.x(2));
  if (!chars) return;
  jstring owner;
  {
    std::lock_guard lock(utf_mutex);
    auto found = utf_strings.find(chars);
    if (found == utf_strings.end()) throw std::runtime_error("unknown JNI UTF buffer");
    owner = found->second;
    utf_strings.erase(found);
  }
  env->ReleaseStringUTFChars(owner, chars);
  env->DeleteGlobalRef(owner);
}

template <Kind K, Return R, bool A32 = false>
void call(JNIEnv* env, hle::CallContext& ctx, ArgumentForm form) {
  auto cursor = [&] {
    if constexpr (A32)
      return hle::a32::ArgCursor32(ctx.cpu());
    else
      return hle::ArgCursor(ctx);
  }();
  auto word = [&] {
    if constexpr (A32)
      return std::uint64_t(cursor.word());
    else
      return cursor.next_gpr();
  };
  word();
  jobject receiver = reference_arg<jobject>(ctx, word());
  jclass cls = nullptr;
  if constexpr (K == Kind::Nonvirtual) cls = reference_arg<jclass>(ctx, word());
  if constexpr (K == Kind::Static || K == Kind::Constructor) cls = static_cast<jclass>(receiver);
  auto mid = reference_arg<jmethodID>(ctx, word());
  auto info = method_info(mid);
  auto args = [&] {
    if constexpr (A32)
      return read_arguments32(info.descriptor, cursor, form);
    else
      return read_arguments(info.descriptor, cursor, form);
  }();
  if constexpr (K == Kind::Virtual && R == Return::Object) {
    bool process_start;
    { std::lock_guard lock(method_mutex); process_start = process_starts.contains(mid); }
    if (process_start) {
      auto commands = find_class(env, "com/venture/runtime/ProcessCommands", current_loader());
      auto start = commands ? env->GetStaticMethodID(commands, "start", "(Ljava/lang/ProcessBuilder;)Ljava/lang/Process;") : nullptr;
      auto process = start ? env->CallStaticObjectMethod(commands, start, receiver) : nullptr;
      if (commands) env->DeleteLocalRef(commands);
      result(ctx, process);
      return;
    }
  }
  if (info.redirect == LoadRedirect::Reflection) {
    result(ctx, invoke_reflective(env, receiver, args.at(0).l, static_cast<jobjectArray>(args.at(1).l)));
    return;
  }
  if (info.redirect != LoadRedirect::None) {
    if constexpr (K != Kind::Static)
      if (!receiver) {
        throw_java(env, "java/lang/NullPointerException", "Runtime.load receiver");
        return;
      }
    jvalue replacement{};
    if (dispatch_method_hook(env, mid, receiver, args, &replacement) != 0) return;
    auto value = static_cast<jstring>(args.at(0).l);
    if (!value) {
      throw_java(env, "java/lang/NullPointerException", "library name");
      return;
    }
    const char* chars = env->GetStringUTFChars(value, nullptr);
    if (!chars) return;
    std::string name(chars);
    env->ReleaseStringUTFChars(value, chars);
    if (info.redirect == LoadRedirect::Library) {
      if (name.empty() || name.find_first_of("/\\") != name.npos) {
        throw_java(env, "java/lang/UnsatisfiedLinkError", "invalid guest library name");
        return;
      }
      name = "lib" + name + ".so";
    } else if (!name.starts_with('/')) {
      throw_java(env, "java/lang/UnsatisfiedLinkError", "guest load requires absolute path");
      return;
    }
    std::string error;
    if (!load_guest_library(name, current_loader(), &error))
      throw_java(env, "java/lang/UnsatisfiedLinkError", error.c_str());
    return;
  }
  if constexpr (K == Kind::Constructor) {
    bool path_argument;
    { std::lock_guard lock(method_mutex); path_argument = path_constructors.contains(mid); }
    jstring mapped = nullptr;
    if (path_argument && !args.empty() && args[0].l) {
      auto string = static_cast<jstring>(args[0].l);
      const auto* utf = env->GetStringUTFChars(string, nullptr);
      if (!utf) return;
      auto host = resolve_guest_path(utf);
      env->ReleaseStringUTFChars(string, utf);
      if (!host.empty()) {
        const auto path = host.generic_u8string();
        mapped = env->NewStringUTF(reinterpret_cast<const char*>(path.c_str()));
        if (!mapped) return;
        args[0].l = mapped;
      }
    }
    auto object = env->NewObjectA(cls, mid, args.data());
    if (mapped) env->DeleteLocalRef(mapped);
    result(ctx, object);
    return;
  }
#define CALL_RETURN(Name)                                                               \
  if constexpr (R == Return::Name) {                                                    \
    if constexpr (K == Kind::Virtual)                                                   \
      result(ctx, env->Call##Name##MethodA(receiver, mid, args.data()));                \
    else if constexpr (K == Kind::Static)                                               \
      result(ctx, env->CallStatic##Name##MethodA(cls, mid, args.data()));               \
    else if constexpr (K == Kind::Nonvirtual)                                           \
      result(ctx, env->CallNonvirtual##Name##MethodA(receiver, cls, mid, args.data())); \
  }
  if constexpr (R == Return::Object) {
    jobject object{};
    if constexpr (K == Kind::Virtual) object = env->CallObjectMethodA(receiver, mid, args.data());
    else if constexpr (K == Kind::Static) object = env->CallStaticObjectMethodA(cls, mid, args.data());
    else if constexpr (K == Kind::Nonvirtual) object = env->CallNonvirtualObjectMethodA(receiver, cls, mid, args.data());
    bool is_path;
    { std::lock_guard lock(method_mutex); is_path = path_methods.contains(mid); }
    result(ctx, is_path ? guest_path_result(env, object) : object);
  }
  CALL_RETURN(Boolean);
  CALL_RETURN(Byte);
  CALL_RETURN(Char);
  CALL_RETURN(Short);
  CALL_RETURN(Int);
  CALL_RETURN(Long);
  CALL_RETURN(Float);
  CALL_RETURN(Double);
#undef CALL_RETURN
  if constexpr (R == Return::Void) {
    if constexpr (K == Kind::Virtual)
      env->CallVoidMethodA(receiver, mid, args.data());
    else if constexpr (K == Kind::Static)
      env->CallStaticVoidMethodA(cls, mid, args.data());
    else if constexpr (K == Kind::Nonvirtual)
      env->CallNonvirtualVoidMethodA(receiver, cls, mid, args.data());
  }
}

void special_GetVersion(JNIEnv*, hle::CallContext& ctx) { ctx.return_x(JNI_VERSION_1_6); }
void special_GetJavaVM(JNIEnv*, hle::CallContext& ctx) {
  auto* out = g2h<GuestAddr>(ctx.x(1));
  if (!out) {
    ctx.return_x(JNI_EINVAL);
    return;
  }
  if (ctx.cpu().isa() == GuestIsa::a32) {
    auto value = static_cast<std::uint32_t>(guest_vm());
    hle::a32::write_bytes(ctx.x(1), &value, 4);
  } else
    *out = guest_vm();
  ctx.return_x(JNI_OK);
}
void special_FindClass(JNIEnv* env, hle::CallContext& ctx) {
  if (env->ExceptionCheck()) {
    ctx.return_x(0);
    return;
  }
  auto cls = find_class(env, g2h<const char>(ctx.x(1)), current_loader());
  if (!cls && env->ExceptionCheck())
    log::warn("jni", "FindClass {} failed", g2h<const char>(ctx.x(1)));
  if (!cls && env->ExceptionCheck()) {
    // Class.forName throws CNFE, while JNI FindClass specifies NCDFE. Translate
    // only that lookup failure; retain the original exception as its cause.
    auto cause = env->ExceptionOccurred();
    env->ExceptionClear();
    auto missing = env->FindClass("java/lang/ClassNotFoundException");
    bool is_missing = missing && env->IsInstanceOf(cause, missing);
    if (missing) env->DeleteLocalRef(missing);
    if (is_missing) {
      auto error_class = env->FindClass("java/lang/NoClassDefFoundError");
      if (error_class) {
        auto ctor = env->GetMethodID(error_class, "<init>", "(Ljava/lang/String;)V");
        if (ctor) {
          auto name = env->NewStringUTF(g2h<const char>(ctx.x(1)));
          if (name) {
            auto failure = static_cast<jthrowable>(env->NewObject(error_class, ctor, name));
            env->DeleteLocalRef(name);
            if (!env->ExceptionCheck() && failure) {
              auto init_cause =
                  env->GetMethodID(error_class, "initCause", "(Ljava/lang/Throwable;)Ljava/lang/Throwable;");
              if (init_cause) {
                auto ignored = env->CallObjectMethod(failure, init_cause, cause);
                bool failed = env->ExceptionCheck();
                if (ignored) env->DeleteLocalRef(ignored);
                if (!failed) env->Throw(failure);
              }
              env->DeleteLocalRef(failure);
            }
          }
        }
        env->DeleteLocalRef(error_class);
      }
    } else if (!env->ExceptionCheck())
      env->Throw(cause);
    env->DeleteLocalRef(cause);
  }
  result(ctx, cls);
}
void special_DefineClass(JNIEnv* env, hle::CallContext& ctx) {
  if (env->ExceptionCheck()) {
    ctx.return_x(0);
    return;
  }
  std::uint32_t length;
  if (ctx.cpu().isa() == GuestIsa::a32)
    hle::a32::read_bytes(ctx.cpu().sp(), &length, 4);
  else
    length = static_cast<std::uint32_t>(ctx.x(4));
  auto cls = env->DefineClass(g2h<const char>(ctx.x(1)), reference_arg<jobject>(ctx, ctx.x(2)),
                              g2h<const jbyte>(ctx.x(3)), static_cast<jsize>(length));
  if (cls && !env->ExceptionCheck()) bind_class(env, cls);
  result(ctx, cls);
}
LoadRedirect load_redirect(JNIEnv* env, jclass cls, const char* name, const char* sig) {
  if (std::string_view(name) == "invoke" &&
      std::string_view(sig) == "(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;") {
    auto method = env->FindClass("java/lang/reflect/Method");
    if (!method) return LoadRedirect::None;
    bool matches = env->IsSameObject(cls, method);
    env->DeleteLocalRef(method);
    if (matches) return LoadRedirect::Reflection;
  }
  if (std::string_view(sig) != "(Ljava/lang/String;)V" ||
      (std::string_view(name) != "loadLibrary" && std::string_view(name) != "load"))
    return LoadRedirect::None;
  LocalFrame frame(env);
  if (!frame.valid()) return LoadRedirect::None;
  jclass system = env->FindClass("java/lang/System");
  if (!system) return LoadRedirect::None;
  bool match = env->IsSameObject(cls, system);
  if (!match) {
    jclass runtime = env->FindClass("java/lang/Runtime");
    if (!runtime) return LoadRedirect::None;
    match = env->IsSameObject(cls, runtime);
  }
  return match ? (std::string_view(name) == "load" ? LoadRedirect::Path : LoadRedirect::Library)
               : LoadRedirect::None;
}
void remember_process_start(JNIEnv* env, jmethodID mid, jclass cls, std::string_view name,
                            std::string_view sig) {
  if (name != "start" || sig != "()Ljava/lang/Process;") return;
  auto type = env->FindClass("java/lang/ProcessBuilder");
  if (type && env->IsSameObject(cls, type)) {
    std::lock_guard lock(method_mutex);
    process_starts.insert(mid);
  }
  if (type) env->DeleteLocalRef(type);
}
// Native code may pass an Android path back to JDK file/ZIP constructors and
// StatFs. Adapt only filesystem APIs, never arbitrary String arguments.
void remember_path_constructor(JNIEnv* env, jmethodID mid, jclass cls) {
  for (const auto* owner : {"java/io/File", "java/io/FileInputStream", "java/io/FileOutputStream",
                            "java/io/RandomAccessFile", "java/util/zip/ZipFile", "android/os/StatFs"}) {
    auto type = env->FindClass(owner);
    const bool matches = type && env->IsAssignableFrom(cls, type);
    if (type) env->DeleteLocalRef(type);
    else env->ExceptionClear();
    if (matches) { std::lock_guard lock(method_mutex); path_constructors.insert(mid); break; }
  }
}
void get_method(JNIEnv* env, hle::CallContext& ctx, bool is_static) {
  auto cls = reference_arg<jclass>(ctx, ctx.x(1));
  auto* name = g2h<const char>(ctx.x(2));
  auto* sig = g2h<const char>(ctx.x(3));
  jmethodID mid = is_static ? env->GetStaticMethodID(cls, name, sig) : env->GetMethodID(cls, name, sig);
  if (mid) remember_method(mid, sig, is_static, load_redirect(env, cls, name, sig));
  if (mid && !is_static) remember_process_start(env, mid, cls, name, sig);
  if (mid && !is_static && std::string_view(name) == "<init>" &&
      std::string_view(sig).starts_with("(Ljava/lang/String;"))
    remember_path_constructor(env, mid, cls);
  if (mid && !is_static && std::string_view(sig) == "()Ljava/lang/String;") {
    const std::string_view n(name);
    const char* owner = (n == "getPath" || n == "toString" || n == "getAbsolutePath" || n == "getCanonicalPath" ||
                        n == "getParent") ? "java/io/File"
        : (n == "getPackageCodePath" || n == "getPackageResourcePath") ? "android/content/Context" : nullptr;
    if (owner) {
      auto type = env->FindClass(owner);
      if (type && env->IsAssignableFrom(cls, type)) { std::lock_guard lock(method_mutex); path_methods.insert(mid); }
      if (type) env->DeleteLocalRef(type);
    }
  }
  result(ctx, mid);
}
void special_GetFieldID(JNIEnv* env, hle::CallContext& ctx) {
  auto cls = reference_arg<jclass>(ctx, ctx.x(1));
  auto name = g2h<const char>(ctx.x(2));
  auto sig = g2h<const char>(ctx.x(3));
  auto field = env->GetFieldID(cls, name, sig);
  const std::string_view n(name);
  if (field && std::string_view(sig) == "Ljava/lang/String;" &&
      (n == "sourceDir" || n == "publicSourceDir" || n == "dataDir" || n == "nativeLibraryDir")) {
    auto type = env->FindClass("android/content/pm/ApplicationInfo");
    if (type && env->IsAssignableFrom(cls, type)) { std::lock_guard lock(method_mutex); path_fields.insert(field); }
    if (type) env->DeleteLocalRef(type);
  }
  result(ctx, field);
}
void special_GetObjectField(JNIEnv* env, hle::CallContext& ctx) {
  auto field = reference_arg<jfieldID>(ctx, ctx.x(2));
  auto value = env->GetObjectField(reference_arg<jobject>(ctx, ctx.x(1)), field);
  bool is_path;
  { std::lock_guard lock(method_mutex); is_path = path_fields.contains(field); }
  result(ctx, is_path ? guest_path_result(env, value) : value);
}
void special_GetMethodID(JNIEnv* env, hle::CallContext& ctx) { get_method(env, ctx, false); }
void special_GetStaticMethodID(JNIEnv* env, hle::CallContext& ctx) { get_method(env, ctx, true); }
void special_FromReflectedMethod(JNIEnv* env, hle::CallContext& ctx) {
  jobject method = reference_arg<jobject>(ctx, ctx.x(1));
  ctx.return_x(0);
  auto mid = env->FromReflectedMethod(method);
  if (mid) {
    auto sig = reflected_descriptor(env, method);
    if (!env->ExceptionCheck()) {
      LocalFrame frame(env);
      if (!frame.valid()) return;
      auto cls = env->GetObjectClass(method);
      auto modifiers = env->GetMethodID(cls, "getModifiers", "()I");
      if (!modifiers) return;
      jint flags = env->CallIntMethod(method, modifiers);
      if (env->ExceptionCheck()) return;
      auto redirect = reflected_load_redirect(env, method);
      if (env->ExceptionCheck()) return;
      remember_method(mid, sig, (flags & 8) != 0, redirect);
      // Unity's AndroidJavaObject finds constructors by reflection.
      if (sig.starts_with("(Ljava/lang/String;")) {
        auto constructor = env->FindClass("java/lang/reflect/Constructor");
        auto declaring = env->GetMethodID(cls, "getDeclaringClass", "()Ljava/lang/Class;");
        auto owner = constructor && declaring && env->IsInstanceOf(method, constructor)
                         ? static_cast<jclass>(env->CallObjectMethod(method, declaring)) : nullptr;
        if (owner && !env->ExceptionCheck()) remember_path_constructor(env, mid, owner);
        env->ExceptionClear();
      }
      if ((flags & 8) == 0 && sig == "()Ljava/lang/Process;") {
        auto declaring = env->GetMethodID(cls, "getDeclaringClass", "()Ljava/lang/Class;");
        auto get_name = env->GetMethodID(cls, "getName", "()Ljava/lang/String;");
        if (!declaring || !get_name) return;
        auto owner = static_cast<jclass>(env->CallObjectMethod(method, declaring));
        auto name = static_cast<jstring>(env->CallObjectMethod(method, get_name));
        if (env->ExceptionCheck()) return;
        const auto* text = env->GetStringUTFChars(name, nullptr);
        if (!text) return;
        remember_process_start(env, mid, owner, text, sig);
        env->ReleaseStringUTFChars(name, text);
      }
    }
  }
  result(ctx, mid);
}
void special_RegisterNatives(JNIEnv* env, hle::CallContext& ctx) {
  if (ctx.cpu().isa() == GuestIsa::a32) {
    // Android jni.h: JNINativeMethod is three 32-bit pointer words (12 bytes).
    struct Method32 {
      std::uint32_t name, signature, function;
    };
    static_assert(sizeof(Method32) == 12);
    auto count = static_cast<jint>(ctx.x(3));
    if (count < 0) {
      result(ctx, JNI_ERR);
      return;
    }
    std::vector<JNINativeMethod> methods;
    for (jint i = 0; i < count; ++i) {
      Method32 m;
      hle::a32::read_bytes(ctx.x(2) + std::uint64_t(i) * 12, &m, 12);
      methods.push_back({g2h<char>(m.name), g2h<char>(m.signature), g2h(m.function)});
    }
    result(ctx, register_natives(env, reference_arg<jclass>(ctx, ctx.x(1)), methods.data(), count));
    return;
  }
  // Android jni.h: LP64 JNINativeMethod = three pointers, 24 bytes.
  static_assert(sizeof(JNINativeMethod) == 24);
  result(ctx, register_natives(env, g2h<_jclass>(ctx.x(1)), g2h<const JNINativeMethod>(ctx.x(2)),
                               static_cast<jint>(ctx.x(3))));
}
void special_UnregisterNatives(JNIEnv* env, hle::CallContext& ctx) {
  result(ctx, unregister_natives(env, reference_arg<jclass>(ctx, ctx.x(1))));
}

// JNI FatalError terminates the guest process. Calling HotSpot's implementation
// would abort the host before the core could report the guest call stack.
[[noreturn]] void guest_fatal(hle::CallContext& c) {
  log::error("jni", "guest requested JNI FatalError");
  if (auto* env = env_no_attach(); env && env->ExceptionCheck()) env->ExceptionDescribe();
  signals::deliver_synchronous(c.thread(), {signals::kSigAbrt, 0, c.cpu().pc()});
  exit_guest_process(134);
}
#define JNI_FIXED(Slot, Name) \
  {Slot, #Name, hle::a32::bind_dynamic<std::remove_pointer_t<decltype(JNINativeInterface_::Name)>>([](hle::CallContext& c) { \
    if constexpr (Slot == 18) guest_fatal(c); \
    else guard(c, [&](JNIEnv* e) { fixed(e->functions->Name, e, c); }); }, \
    [](hle::CallContext& c) { guard(c, [&](JNIEnv* e) { fixed32(e->functions->Name, e, c); }); })},
#define JNI_SPECIAL(Slot, Name) \
  {Slot, #Name, [](hle::CallContext& c) { guard(c, [&](JNIEnv* e) { special_##Name(e, c); }); }},
#define JNI_CALL(Slot, Name, Mode, Result, Form)                                               \
  {Slot, #Name, [](hle::CallContext& c) {                                                      \
     guard(c, [&](JNIEnv* e) { call<Kind::Mode, Return::Result>(e, c, ArgumentForm::Form); }); \
   }},
const JniEntry entries[] = {
#include "java/jni_slots.inc"
};
#undef JNI_FIXED
#undef JNI_SPECIAL
#undef JNI_CALL
static_assert(std::size(entries) == 229);
#define JNI_FIXED(Slot, Name)                                           \
  {Slot, #Name, [](hle::CallContext& c) {                               \
     if constexpr (Slot == 18)                                          \
       guest_fatal(c);                                                  \
     else                                                               \
       guard(c, [&](JNIEnv* e) { fixed32(e->functions->Name, e, c); }); \
   }},
#define JNI_SPECIAL(Slot, Name) \
  {Slot, #Name, [](hle::CallContext& c) { guard(c, [&](JNIEnv* e) { special_##Name(e, c); }); }},
#define JNI_CALL(Slot, Name, Mode, Result, Form)                                                     \
  {Slot, #Name, [](hle::CallContext& c) {                                                            \
     guard(c, [&](JNIEnv* e) { call<Kind::Mode, Return::Result, true>(e, c, ArgumentForm::Form); }); \
   }},
const JniEntry entries32[] = {
#include "java/jni_slots.inc"
};
#undef JNI_FIXED
#undef JNI_SPECIAL
#undef JNI_CALL
#define JNI_FIXED(Slot, Name) static_assert(offsetof(JNINativeInterface_, Name) == Slot * sizeof(void*));
#define JNI_SPECIAL(Slot, Name) JNI_FIXED(Slot, Name)
#define JNI_CALL(Slot, Name, Mode, Result, Form) JNI_FIXED(Slot, Name)
#include "java/jni_slots.inc"
#undef JNI_FIXED
#undef JNI_SPECIAL
#undef JNI_CALL
bool version_supported(jint version) {
  return version == JNI_VERSION_1_1 || version == JNI_VERSION_1_2 || version == JNI_VERSION_1_4 ||
         version == JNI_VERSION_1_6;
}
template <class F>
void vm_guard(hle::CallContext& ctx, F fn) noexcept {
  try {
    if (ctx.x(0) != guest_vm()) {
      ctx.return_x(JNI_EINVAL);
      return;
    }
    fn();
  } catch (const std::exception& e) {
    log::error("jni", "JavaVM: {}", e.what());
    ctx.return_x(JNI_ERR);
  } catch (...) {
    log::error("jni", "JavaVM invocation failed");
    ctx.return_x(JNI_ERR);
  }
}
void attach(hle::CallContext& ctx, bool daemon) {
  auto* out = g2h<GuestAddr>(ctx.x(1));
  if (!out) {
    ctx.return_x(JNI_EINVAL);
    return;
  }
  const bool a32 = ctx.cpu().isa() == GuestIsa::a32;
  std::optional<jni32::PinScope> pins;
  if (a32) pins.emplace();
  auto store = [&](GuestAddr value) {
    if (a32) {
      auto word = static_cast<std::uint32_t>(value);
      hle::a32::write_bytes(ctx.x(1), &word, 4);
    } else
      *out = value;
  };
  store(0);
  auto* args = g2h<JavaVMAttachArgs>(ctx.x(2));
  JavaVMAttachArgs host_args{};
  if (a32 && args) {
    // Android jni.h JavaVMAttachArgs: jint, char*, jobject, all four bytes.
    std::uint32_t words[3];
    hle::a32::read_bytes(ctx.x(2), words, sizeof(words));
    host_args = {static_cast<jint>(words[0]), g2h<char>(words[1]), jni32::object(words[2])};
    args = &host_args;
  }
  if (args && !version_supported(args->version)) {
    ctx.return_x(JNI_EVERSION);
    return;
  }
  JNIEnv* env = nullptr;
  jint status = attach_host(daemon, args, &env);
  if (status == JNI_OK) store(guest_env(GuestThread::adopt_current()));
  ctx.return_x(status);
}
}  // namespace
std::span<const JniEntry> jni_entries() {
  return process_config().isa() == GuestIsa::a32 ? entries32 : entries;
}
void remember_method(jmethodID id, std::string_view signature, bool is_static, LoadRedirect redirect) {
  MethodInfo info{std::string(signature), Descriptor::parse(signature), is_static, redirect};
  std::lock_guard lock(method_mutex);
  methods.insert_or_assign(id, std::move(info));
}
MethodInfo method_info(jmethodID id) {
  std::lock_guard lock(method_mutex);
  auto found = methods.find(id);
  if (found == methods.end())
    throw std::invalid_argument("unknown jmethodID (not obtained through guest JNI)");
  return found->second;
}
GuestAddr guest_env(GuestThread& thread) {
  if (&thread != GuestThread::current())
    throw std::invalid_argument("guest_env must run on the owning thread");
  host_env();
  std::call_once(env_once, [] {
    const auto width = process_config().isa() == GuestIsa::a32 ? 4u : 8u;
    env_table = AddressSpace::get().allocate(233 * width, kProtRead | kProtWrite, "[jni:env-table]");
    if (!env_table) throw std::bad_alloc();
    std::memset(g2h(env_table), 0, 233 * width);
    for (const auto& entry : jni_entries()) {
      if (width == 4) hle::a32::register_adapter("JNIEnv", entry.name, entry.handler);
      auto thunk = hle::make_thunk("JNIEnv", entry.name, entry.handler);
      if (width == 4)
        g2h<std::uint32_t>(env_table)[entry.slot] = static_cast<std::uint32_t>(thunk);
      else
        g2h<GuestAddr>(env_table)[entry.slot] = thunk;
    }
    if (AddressSpace::get().protect(env_table, 233 * width, kProtRead) < 0)
      throw std::runtime_error("cannot protect JNI table");
  });
  auto& tls = thread_state();
  if (!tls.wrapper) {
    tls.wrapper = AddressSpace::get().allocate(8, kProtRead | kProtWrite, "[jni:env]");
    if (!tls.wrapper) throw std::bad_alloc();
    if (process_config().isa() == GuestIsa::a32)
      *g2h<std::uint32_t>(tls.wrapper) = static_cast<std::uint32_t>(env_table);
    else
      *g2h<GuestAddr>(tls.wrapper) = env_table;
  }
  return thread.jni_env = tls.wrapper;
}
GuestAddr guest_vm() {
  std::call_once(vm_once, [] {
    const auto width = process_config().isa() == GuestIsa::a32 ? 4u : 8u;
    GuestAddr table = AddressSpace::get().allocate(8 * width, kProtRead | kProtWrite, "[jni:vm-table]");
    if (!table) throw std::bad_alloc();
    std::array<GuestAddr, 8> words{};
    auto thunk = [width](const char* name, hle::Handler handler) {
      if (width == 4) hle::a32::register_adapter("JavaVM", name, handler);
      return hle::make_thunk("JavaVM", name, handler);
    };
    words[3] = thunk("DestroyJavaVM", [](hle::CallContext& c) {
      vm_guard(c, [&] {
        std::string error;
        bool ok = stop_jvm(&error);
        if (!ok) log::error("jni", "{}", error);
        c.return_x(ok ? JNI_OK : JNI_ERR);
      });
    });
    words[4] =
        thunk("AttachCurrentThread", [](hle::CallContext& c) { vm_guard(c, [&] { attach(c, false); }); });
    words[5] = thunk("DetachCurrentThread",
                     [](hle::CallContext& c) { vm_guard(c, [&] { c.return_x(detach_host()); }); });
    words[6] = thunk("GetEnv", [](hle::CallContext& c) {
      vm_guard(c, [&] {
        auto* out = g2h<GuestAddr>(c.x(1));
        if (!out) {
          c.return_x(JNI_EINVAL);
          return;
        }
        auto store = [&](GuestAddr value) {
          if (c.cpu().isa() == GuestIsa::a32) {
            auto word = static_cast<std::uint32_t>(value);
            hle::a32::write_bytes(c.x(1), &word, 4);
          } else
            *out = value;
        };
        store(0);
        if (!version_supported(static_cast<jint>(c.x(2)))) {
          c.return_x(JNI_EVERSION);
          return;
        }
        if (!env_no_attach()) {
          c.return_x(JNI_EDETACHED);
          return;
        }
        store(guest_env(GuestThread::adopt_current()));
        c.return_x(JNI_OK);
      });
    });
    words[7] = thunk("AttachCurrentThreadAsDaemon",
                     [](hle::CallContext& c) { vm_guard(c, [&] { attach(c, true); }); });
    for (unsigned i = 0; i < words.size(); ++i) {
      if (width == 4)
        g2h<std::uint32_t>(table)[i] = static_cast<std::uint32_t>(words[i]);
      else
        g2h<GuestAddr>(table)[i] = words[i];
    }
    if (AddressSpace::get().protect(table, 8 * width, kProtRead) < 0)
      throw std::runtime_error("cannot protect JavaVM table");
    vm_wrapper = AddressSpace::get().allocate(8, kProtRead | kProtWrite, "[jni:vm]");
    if (!vm_wrapper) throw std::bad_alloc();
    if (width == 4)
      *g2h<std::uint32_t>(vm_wrapper) = static_cast<std::uint32_t>(table);
    else
      *g2h<GuestAddr>(vm_wrapper) = table;
  });
  return vm_wrapper;
}
void register_jni_library() {
  static std::once_flag once;
  std::call_once(once, [] {
    register_method_hook_library();
    auto lib = std::make_shared<HleLibrary>("libart.so");
    auto enumerate = +[](hle::CallContext& c) {
      const auto buffer = c.x(0), count_out = c.x(2);
      const auto capacity = static_cast<jint>(c.x(1));
      if (capacity < 0 || (capacity > 0 && !buffer)) {
        c.return_x(JNI_EINVAL);
        return;
      }
      // JNI Invocation API: report the total independently of buffer capacity.
      const jint count = host_vm() ? 1 : 0;
      auto write = [](GuestAddr address, const void* value, std::size_t bytes) {
        if (!platform::write_memory(address, value, bytes)) throw hle::MemoryFault{address, true};
      };
      if (count_out) write(count_out, &count, sizeof(count));
      if (count && capacity > 0) {
        const auto wrapper = guest_vm();
        if (c.cpu().isa() == GuestIsa::a32) {
          const auto word = static_cast<std::uint32_t>(wrapper);
          write(buffer, &word, sizeof(word));
        } else {
          write(buffer, &wrapper, sizeof(wrapper));
        }
      }
      c.return_x(JNI_OK);
    };
    if (process_config().isa() == GuestIsa::a32)
      hle::a32::register_adapter("libart.so", "JNI_GetCreatedJavaVMs", enumerate);
    lib->add_function("JNI_GetCreatedJavaVMs",
                      hle::make_thunk("libart.so", "JNI_GetCreatedJavaVMs", enumerate));
    Linker::get().register_hle_library(lib);
    // ART is an app_process dependency, visible to RTLD_DEFAULT VM discovery.
    if (!Linker::get().open("libart.so", kRtldNow | kRtldGlobal, nullptr))
      throw std::runtime_error("cannot publish guest VM invocation API");
  });
}
}  // namespace venture::java

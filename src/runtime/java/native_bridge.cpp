#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>

#include "common/log.h"
#include "common/environment.h"
#include "core/linker.h"
#include "core/shutdown_trace.h"
#include "core/os_quit.h"
#include "core/process_paths.h"
#include "core/thread_runtime.h"
#include "java/elf_exports.h"
#include "java/internal.h"
#include "java/jni32.h"
#include "java/method_hooks.h"
#include "platform/java_host.h"

extern "C" void venture_java_native_entry();
namespace venture::java {
namespace {
jclass loader_class = nullptr;
jmethodID descriptor_method = nullptr, native_methods_method = nullptr, load_kind_method = nullptr,
          reflect_invoke = nullptr;
jclass class_class = nullptr;
jclass string_class = nullptr;
jmethodID for_name = nullptr, class_loader = nullptr, class_name = nullptr;
std::string string_value(JNIEnv* env, jstring string) {
  if (!string) throw std::runtime_error("missing Java string (pending exception)");
  const char* chars = env->GetStringUTFChars(string, nullptr);
  if (!chars) throw std::runtime_error("cannot read Java string");
  std::string result(chars);
  env->ReleaseStringUTFChars(string, chars);
  return result;
}
struct NativeRecord {
  jclass owner;    // global reference; JNI code/IDs cannot outlive the class
  jobject loader;  // global reference, nullable bootstrap loader
  std::string name, signature;
  Descriptor descriptor;
  GuestAddr function;
  void* stub = nullptr;
  bool registered = true;  // protected by records_mutex; storage survives unregistration
  bool platform_destroy = false;
};
std::mutex records_mutex;
std::vector<std::unique_ptr<NativeRecord>> records;  // live through process shutdown
struct Loaded {
  Library* image;
  jobject loader;
  enum class State { Loading, Ready, Failed } state = State::Loading;
  std::thread::id owner = std::this_thread::get_id();
  std::string error;
  std::map<std::string, GuestAddr> exports;
  bool exports_ready = false;  // once published, exports is immutable
};
std::mutex libraries_mutex;
std::condition_variable libraries_changed;
std::vector<std::shared_ptr<Loaded>> libraries;
void* make_stub(NativeRecord* record) {
  // ENDBR64; movabs r10,record; movabs r11,entry; jmp r11.
  // Tail jump preserves the JVM's return address and arguments. No live RWX.
  std::array<std::byte, 27> code{};
  const unsigned char prefix[] = {0xf3, 0x0f, 0x1e, 0xfa, 0x49, 0xba};
  std::memcpy(code.data(), prefix, sizeof(prefix));
  auto context = reinterpret_cast<std::uintptr_t>(record);
  std::memcpy(code.data() + 6, &context, 8);
  code[14] = std::byte{0x49};
  code[15] = std::byte{0xbb};
  auto entry = reinterpret_cast<std::uintptr_t>(&venture_java_native_entry);
  std::memcpy(code.data() + 16, &entry, 8);
  code[24] = std::byte{0x41};
  code[25] = std::byte{0xff};
  code[26] = std::byte{0xe3};
  return platform::executable_stub(code);
}
void JNICALL loader_load(JNIEnv* env, jclass, jstring name, jobject loader) noexcept {
  try {
    std::string error;
    if (!load_guest_library(string_value(env, name), loader, &error))
      throw_java(env, "java/lang/UnsatisfiedLinkError", error.c_str());
  } catch (const std::exception& e) {
    throw_java(env, "java/lang/UnsatisfiedLinkError", e.what());
  } catch (...) {
    throw_java(env, "java/lang/InternalError", "guest native library load failed");
  }
}
void JNICALL loader_bind(JNIEnv* env, jclass, jclass cls) noexcept {
  try {
    bind_class(env, cls);
  } catch (const std::exception& e) {
    throw_java(env, "java/lang/UnsatisfiedLinkError", e.what());
  } catch (...) {
    throw_java(env, "java/lang/InternalError", "guest native symbol binding failed");
  }
}
jboolean JNICALL loader_load_hook(JNIEnv* env, jclass, jobject method, jobject receiver, jstring name) noexcept {
  try {
    auto id = env->FromReflectedMethod(method);
    if (env->ExceptionCheck()) return JNI_TRUE;
    jvalue argument{}, result{};
    argument.l = name;
    const auto handled = dispatch_method_hook(env, id, receiver, {&argument, 1}, &result);
    if (handled < 0) throw std::runtime_error("Invalid reflected Java load hook arguments");
    return handled ? JNI_TRUE : JNI_FALSE;
  } catch (const std::exception& error) { throw_java(env, "java/lang/InternalError", error.what()); }
    catch (...) { throw_java(env, "java/lang/InternalError", "Reflected Java load hook failed"); }
  return JNI_TRUE;
}
HostArguments guest_path_arguments(JNIEnv* env, const Descriptor& descriptor,
                                  const HostArguments& host, HostAbi abi,
                                  std::vector<std::uint64_t>& copied_stack) {
  auto out = host;
  unsigned gp = 2, fp = 0, position = 2, stack = 0, stack_size = 0;
  for (auto type : descriptor.parameters) {
    const bool floating = type == Type::Float || type == Type::Double;
    if (abi == HostAbi::Win64) stack_size += position++ >= 4;
    else if (floating) stack_size += fp++ >= 8;
    else stack_size += gp++ >= 6;
  }
  gp = position = 2; fp = 0;
  for (auto type : descriptor.parameters) {
    const bool floating = type == Type::Float || type == Type::Double;
    bool spilled;
    unsigned index;
    if (abi == HostAbi::Win64) {
      spilled = position >= 4;
      index = spilled ? position - 4 : position;
      ++position;
    } else if (floating) {
      spilled = fp >= 8;
      index = spilled ? stack++ : fp++;
    } else {
      spilled = gp >= 6;
      index = spilled ? stack++ : gp++;
    }
    if (type != Type::Object) continue;
    auto object = g2h<_jobject>(spilled ? host.stack[index] : host.gp[index]);
    if (!object || !env->IsInstanceOf(object, string_class)) continue;
    auto string = static_cast<jstring>(object);
    const char* chars = env->GetStringUTFChars(string, nullptr);
    if (!chars) return out;
    std::optional<std::string> mapped;
    const std::string_view path(chars);
    const bool absolute = path.starts_with('/') || path.starts_with("\\\\") ||
        (path.size() >= 3 && path[1] == ':' && (path[2] == '/' || path[2] == '\\'));
    try {
      if (absolute) {
        const auto utf8 = std::filesystem::path(from_modified_utf8(path)).generic_u8string();
        mapped = host_path_to_guest(std::string_view(reinterpret_cast<const char*>(utf8.data()), utf8.size()));
      }
    }
    catch (const std::runtime_error&) {
      // Arbitrary Java strings (including unpaired UTF-16 surrogates) need
      // not be valid host filenames. Preserve those strings unchanged.
    }
    env->ReleaseStringUTFChars(string, chars);
    if (!mapped) continue;
    const auto utf16 = std::filesystem::path(std::u8string(mapped->begin(), mapped->end())).u16string();
    auto replacement = env->NewString(reinterpret_cast<const jchar*>(utf16.data()), static_cast<jsize>(utf16.size()));
    if (!replacement) return out;
    // Java retains host File paths for JDK I/O. Strings passed into native
    // methods need the same Android mount aliases as JNI File API results:
    // UE4 normalizes these paths before libc can recognize their host aliases.
    // The transition's LocalFrame owns the replacement, including nested calls.
    if (spilled) {
      if (copied_stack.empty()) copied_stack.assign(host.stack, host.stack + stack_size);
      copied_stack[index] = h2g(replacement);
      out.stack = copied_stack.data();
    } else out.gp[index] = h2g(replacement);
  }
  return out;
}
}  // namespace

extern "C" std::uint64_t venture_java_native_dispatch(NativeRecord* record, const HostArguments* host) noexcept {
  auto* env = reinterpret_cast<JNIEnv*>(host->gp[0]);
  auto& tls = thread_state();
  ++tls.native_depth;
  struct Depth {
    unsigned& value;
    ~Depth() { --value; }
  } depth{tls.native_depth};
  try {
    if (env_no_attach() != env) throw std::runtime_error("host JNIEnv mismatch at native transition");
    LocalFrame frame(env, 64);
    if (!frame.valid()) return 0;
    LoaderScope scope(record->loader);
    static const bool trace = venture::environment::getenv("VENTURE_TRACE_JNI") != nullptr;
    if (trace)
      log::info("jni-trace", "native {}{} target={:#x}", record->name,
                record->signature, record->function);
#ifdef _WIN32
    constexpr auto host_abi = HostAbi::Win64;
#else
    constexpr auto host_abi = HostAbi::SysV;
#endif
    std::vector<std::uint64_t> path_stack;
    const auto path_host = guest_path_arguments(env, record->descriptor, *host, host_abi, path_stack);
    if (env->ExceptionCheck()) return 0;
    auto& thread = GuestThread::adopt_current();
    if (record->platform_destroy) os_quit::native_destroy_delivered();
    struct WatchNative {
      const NativeRecord& native;
      explicit WatchNative(const NativeRecord& record) : native(record) {
        if (shutdown_trace::enabled) shutdown_trace::sample_watch("JNI enter " + native.name + native.signature);
      }
      ~WatchNative() {
        if (shutdown_trace::enabled) shutdown_trace::sample_watch("JNI leave " + native.name + native.signature);
      }
    } watch_native(*record);
    if (thread.cpu().isa() == GuestIsa::a32) {
      jni32::PinScope pins;
      // Keep tokens within this native transition; host LocalFrame owns the
      // corresponding JVM local refs. Both frames survive nested JNI calls.
      std::uint64_t returned;
      {
        jni32::NativeScope tokens;
        auto args = jni32::native_arguments(record->descriptor, path_host, host_abi, guest_env(thread));
        auto value = thread.call32(record->function, args);
        returned =
            record->descriptor.result == Type::Object
                ? h2g(tokens.pop(env->ExceptionCheck() ? nullptr
                                                       : jni32::object(static_cast<std::uint32_t>(value.x0))))
                : native_result(record->descriptor.result, value);
      }
      return record->descriptor.result == Type::Object ? h2g(frame.pop(g2h<_jobject>(returned))) : returned;
    }
    auto args = native_arguments(record->descriptor, path_host, host_abi, guest_env(thread));
    auto value = thread.call(record->function, args.view());
    if (record->descriptor.result == Type::Object) {
      // Promote the returned local out of the transition frame, including null
      // while an exception is pending; never create a frame per JNI SVC.
      return h2g(frame.pop(env->ExceptionCheck() ? nullptr : g2h<_jobject>(value.x0)));
    }
    return native_result(record->descriptor.result, value);
  } catch (const ThreadExit&) {
    // A pthread exit or owner-requested interruption is control flow. Preserve
    // stack unwinding through Java finally blocks without fabricating a fault.
    throw_thread_death(env);
  } catch (const ProcessExit&) {
    // The process status is retained by the core and observed by the launcher.
    throw_thread_death(env);
  } catch (const std::exception& e) {
    log::error("jni", "native {}{}: {}", record->name, record->signature, e.what());
    throw_java(env, "java/lang/InternalError", e.what());
  } catch (...) {
    throw_java(env, "java/lang/InternalError", "guest native callback failed");
  }
  return 0;
}

void JNICALL loader_exit(JNIEnv* env, jclass, jint status) noexcept {
  try { exit_guest_process(status); }
  catch (const ProcessExit&) { throw_thread_death(env); }
  catch (...) { throw_java(env, "java/lang/InternalError", "guest process exit failed"); }
}
void init_bridge(JNIEnv* env) {
  auto strings = env->FindClass("java/lang/String");
  if (!strings) return;
  string_class = static_cast<jclass>(env->NewGlobalRef(strings));
  env->DeleteLocalRef(strings);
  if (!string_class) return;
  auto cls = env->FindClass("com/venture/runtime/NativeLoader");
  if (!cls) return;
  loader_class = static_cast<jclass>(env->NewGlobalRef(cls));
  env->DeleteLocalRef(cls);
  if (!loader_class) return;
  JNINativeMethod native[] = {
      {const_cast<char*>("load0"), const_cast<char*>("(Ljava/lang/String;Ljava/lang/ClassLoader;)V"),
       reinterpret_cast<void*>(&loader_load)},
      {const_cast<char*>("bindClass"), const_cast<char*>("(Ljava/lang/Class;)V"),
       reinterpret_cast<void*>(&loader_bind)},
      {const_cast<char*>("exit0"), const_cast<char*>("(I)V"), reinterpret_cast<void*>(&loader_exit)}};
  if (env->RegisterNatives(loader_class, native, 3) != JNI_OK) return;
  auto hooks = env->FindClass("com/venture/runtime/MethodHooks");
  if (!hooks) return;
  JNINativeMethod hook_native{const_cast<char*>("invoke0"),
      const_cast<char*>("(Ljava/lang/reflect/Method;Ljava/lang/Object;Ljava/lang/String;)Z"),
      reinterpret_cast<void*>(&loader_load_hook)};
  if (env->RegisterNatives(hooks, &hook_native, 1) != JNI_OK) { env->DeleteLocalRef(hooks); return; }
  auto enabled = env->GetStaticFieldID(hooks, "enabled", "Z");
  if (enabled) env->SetStaticBooleanField(hooks, enabled, JNI_TRUE);
  env->DeleteLocalRef(hooks);
  if (env->ExceptionCheck()) return;
  descriptor_method = env->GetStaticMethodID(loader_class, "descriptor",
                                             "(Ljava/lang/reflect/Executable;)Ljava/lang/String;");
  if (!descriptor_method) return;
  native_methods_method =
      env->GetStaticMethodID(loader_class, "nativeMethods", "(Ljava/lang/Class;)[Ljava/lang/String;");
  if (!native_methods_method) return;
  load_kind_method = env->GetStaticMethodID(loader_class, "loadKind", "(Ljava/lang/reflect/Executable;)I");
  if (!load_kind_method) return;
  reflect_invoke = env->GetStaticMethodID(loader_class, "invokeWithLoader",
                                          "(Ljava/lang/reflect/Method;Ljava/lang/Object;[Ljava/lang/"
                                          "Object;Ljava/lang/ClassLoader;)Ljava/lang/Object;");
  if (!reflect_invoke) return;
  cls = env->FindClass("java/lang/Class");
  if (!cls) return;
  class_class = static_cast<jclass>(env->NewGlobalRef(cls));
  env->DeleteLocalRef(cls);
  if (!class_class) return;
  for_name = env->GetStaticMethodID(class_class, "forName",
                                    "(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;");
  if (!for_name) return;
  class_loader = env->GetMethodID(class_class, "getClassLoader", "()Ljava/lang/ClassLoader;");
  if (!class_loader) return;
  class_name = env->GetMethodID(class_class, "getName", "()Ljava/lang/String;");
}
std::string reflected_descriptor(JNIEnv* env, jobject method) {
  jstring text = static_cast<jstring>(env->CallStaticObjectMethod(loader_class, descriptor_method, method));
  if (env->ExceptionCheck()) return {};
  auto result = string_value(env, text);
  env->DeleteLocalRef(text);
  return result;
}
LoadRedirect reflected_load_redirect(JNIEnv* env, jobject method) {
  jint kind = env->CallStaticIntMethod(loader_class, load_kind_method, method);
  if (env->ExceptionCheck()) return LoadRedirect::None;
  return static_cast<LoadRedirect>(kind);
}
jobject invoke_reflective(JNIEnv* env, jobject method, jobject target, jobjectArray args) {
  auto result =
      env->CallStaticObjectMethod(loader_class, reflect_invoke, method, target, args, current_loader());
  env->ExceptionCheck();
  return result;
}
jclass find_class(JNIEnv* env, const char* name, jobject loader) {
  if (!name) {
    throw_java(env, "java/lang/NullPointerException", "FindClass name");
    return nullptr;
  }
  std::string dotted(name);
  std::replace(dotted.begin(), dotted.end(), '/', '.');
  auto string = env->NewStringUTF(dotted.c_str());
  if (!string) return nullptr;
  auto cls =
      static_cast<jclass>(env->CallStaticObjectMethod(class_class, for_name, string, JNI_FALSE, loader));
  bool failed = env->ExceptionCheck();
  env->DeleteLocalRef(string);
  if (!failed && cls) bind_class(env, cls);
  return cls;
}
jint register_natives(JNIEnv* env, jclass owner, const JNINativeMethod* methods, jint count) {
  if (count < 0 || (count && !methods)) {
    throw_java(env, "java/lang/IllegalArgumentException", "invalid native registration array");
    return JNI_ERR;
  }
  LocalFrame frame(env);
  if (!frame.valid()) return JNI_ENOMEM;
  auto loader = env->CallObjectMethod(owner, class_loader);
  if (env->ExceptionCheck()) return JNI_ERR;
  std::vector<JNINativeMethod> host_methods;
  std::vector<std::unique_ptr<NativeRecord>> added;
  for (jint i = 0; i < count; ++i) {
    auto& method = methods[i];
    if (!method.name || !method.signature || !method.fnPtr) {
      throw_java(env, "java/lang/IllegalArgumentException", "invalid JNINativeMethod");
      return JNI_ERR;
    }
    auto record = std::make_unique<NativeRecord>();
    record->name = method.name;
    record->signature = method.signature;
    if (os_quit::enabled && class_name && record->signature == "(J)V" &&
        (record->name == "terminateNativeCode" || record->name == "unloadNativeCode")) {
      auto name = static_cast<jstring>(env->CallObjectMethod(owner, class_name));
      if (env->ExceptionCheck()) return JNI_ERR;
      record->platform_destroy = string_value(env, name) == "com.google.androidgamesdk.GameActivity";
      env->DeleteLocalRef(name);
    }
    record->descriptor = Descriptor::parse(method.signature);
    record->function = h2g(method.fnPtr);
    record->owner = static_cast<jclass>(env->NewGlobalRef(owner));
    record->loader = loader ? env->NewGlobalRef(loader) : nullptr;
    if (!record->owner || (loader && !record->loader)) return JNI_ENOMEM;
    record->stub = make_stub(record.get());
    host_methods.push_back({record->name.data(), record->signature.data(), record->stub});
    added.push_back(std::move(record));
  }
  // Publish storage before RegisterNatives: it can register a prefix even on
  // failure, and another Java thread may immediately enter a replaced binding.
  {
    std::lock_guard lock(records_mutex);
    for (auto& record : added) records.push_back(std::move(record));
  }
  return env->RegisterNatives(owner, host_methods.data(), count);
}
jint unregister_natives(JNIEnv* env, jclass cls) {
  jint status = env->UnregisterNatives(cls);
  if (status == JNI_OK) {
    std::lock_guard lock(records_mutex);
    for (auto& record : records)
      if (env->IsSameObject(record->owner, cls)) record->registered = false;
  }
  return status;
}
void bind_class(JNIEnv* env, jclass cls) {
  if (env->ExceptionCheck()) return;
  // Recursion is possible when Class.forName is called by a user classloader.
  static thread_local std::set<std::string> binding;
  LocalFrame frame(env, 64);
  if (!frame.valid()) return;
  auto loader = env->CallObjectMethod(cls, class_loader);
  if (env->ExceptionCheck()) return;
  auto text = static_cast<jstring>(env->CallObjectMethod(cls, class_name));
  if (env->ExceptionCheck()) return;
  std::string owner = string_value(env, text);
  std::replace(owner.begin(), owner.end(), '.', '/');
  if (binding.contains(owner)) return;
  std::vector<std::shared_ptr<Loaded>> visible;
  {
    std::lock_guard lock(libraries_mutex);
    for (const auto& lib : libraries)
      if (lib->exports_ready && lib->state != Loaded::State::Failed && env->IsSameObject(lib->loader, loader))
        visible.push_back(lib);
  }
  if (visible.empty()) return;
  auto encoded = mangle(from_modified_utf8(owner));
  bool relevant = false;
  for (const auto& lib : visible)
    for (const auto& [name, addr] : lib->exports)
      if (name.starts_with("Java_" + encoded + "_")) relevant = true;
  if (!relevant) return;
  binding.insert(owner);
  struct Erase {
    std::set<std::string>& set;
    std::string key;
    ~Erase() { set.erase(key); }
  } erase{binding, owner};
  auto methods =
      static_cast<jobjectArray>(env->CallStaticObjectMethod(loader_class, native_methods_method, cls));
  if (env->ExceptionCheck() || !methods) return;
  jint n = env->GetArrayLength(methods);
  for (jint i = 0; i + 1 < n; i += 2) {
    auto method_name = static_cast<jstring>(env->GetObjectArrayElement(methods, i));
    auto method_sig = static_cast<jstring>(env->GetObjectArrayElement(methods, i + 1));
    auto name = string_value(env, method_name), sig = string_value(env, method_sig);
    env->DeleteLocalRef(method_name);
    env->DeleteLocalRef(method_sig);
    auto short_name = native_name(from_modified_utf8(owner), from_modified_utf8(name));
    auto params = from_modified_utf8(std::string_view(sig).substr(1, sig.find(')') - 1));
    auto long_name = native_name(from_modified_utf8(owner), from_modified_utf8(name), params);
    GuestAddr address = 0;
    // JNI resolution order: short name first across visible libraries, then long.
    for (const auto& key : {short_name, long_name}) {
      for (const auto& lib : visible)
        if (auto it = lib->exports.find(key); it != lib->exports.end()) {
          address = it->second;
          break;
        }
      if (address) break;
    }
    if (!address) continue;
    // Explicit RegisterNatives wins over automatic exported-symbol binding.
    bool registered = false;
    {
      std::lock_guard lock(records_mutex);
      for (const auto& record : records)
        if (record->registered && record->name == name && record->signature == sig &&
            env->IsSameObject(record->owner, cls)) {
          registered = true;
          break;
        }
    }
    if (registered) continue;
    JNINativeMethod native{name.data(), sig.data(), g2h(address)};
    if (register_natives(env, cls, &native, 1) != JNI_OK) return;
  }
}
bool load_guest_library(const std::string& name, void* loader_ref, std::string* error) {
  std::shared_ptr<Loaded> record;
  try {
    auto* env = host_env();
    if (env->ExceptionCheck()) throw std::runtime_error("pending exception before guest library load");
    LocalFrame frame(env, 64);
    if (!frame.valid()) throw std::bad_alloc();
    auto loader = static_cast<jobject>(loader_ref);
    LoaderScope scope(loader);
    auto& thread = GuestThread::adopt_current();
    guest_env(thread);
    // Constructors and JNI_OnLoad share this host transition's local frame.
    std::unique_ptr<jni32::NativeScope> tokens;
    if (thread.cpu().isa() == GuestIsa::a32) tokens = std::make_unique<jni32::NativeScope>();
    log::info("java", "Loading guest library {}", name);
    auto* image = Linker::get().open(name, kRtldNow | kRtldLocal | kRtldNoDelete, nullptr);
    if (!image) throw std::runtime_error(Linker::get().take_error());
    {
      std::unique_lock lock(libraries_mutex);
      for (auto existing : libraries)
        if (existing->image == image) {
          if (!env->IsSameObject(existing->loader, loader))
            throw std::runtime_error("guest library already owned by another classloader");
          if (existing->state == Loaded::State::Loading && existing->owner == std::this_thread::get_id())
            return true;
          // ART also waits for an in-flight JNI_OnLoad after dlopen returns.
          // JNI_OnLoad runs outside the loader lock; ELF constructors do not.
          libraries_changed.wait(lock, [&] { return existing->state != Loaded::State::Loading; });
          if (existing->state == Loaded::State::Failed) throw std::runtime_error(existing->error);
          return true;
        }
      record = std::make_shared<Loaded>();
      record->image = image;
      record->loader = loader ? env->NewGlobalRef(loader) : nullptr;
      if (loader && !record->loader) throw std::bad_alloc();
      libraries.push_back(record);
    }
    // r2-java gameactivity_elf_evidence.json: Bait/Gorilla libgame have Java_*
    // exports and NO JNI_OnLoad. Enumeration is unconditional.
    // A preserved JNI_OnLoad may activate the original backing. Enumerate it
    // before executing OnLoad, so its JNI calls can resolve exported natives.
    const auto on_load = image->find_symbol("JNI_OnLoad");
    std::map<std::string, GuestAddr> exports;
    exported_java_symbols(
        *image, [&](std::string_view symbol, GuestAddr address) { exports.emplace(symbol, address); });
    {
      std::lock_guard lock(libraries_mutex);
      record->exports = std::move(exports);
      record->exports_ready = true;
    }
    if (on_load) {
      jint version =
          thread.cpu().isa() == GuestIsa::a32
              ? thread.call32(on_load, hle::a32::CallBuilder32{}.pointer(guest_vm()).pointer(0)).i32()
              : static_cast<jint>(thread.call(on_load, {guest_vm(), 0}));
      if (env->ExceptionCheck()) throw std::runtime_error("guest JNI_OnLoad left a pending exception");
      if (version != JNI_VERSION_1_2 && version != JNI_VERSION_1_4 && version != JNI_VERSION_1_6)
        throw std::runtime_error("guest JNI_OnLoad returned unsupported JNI version " +
                                 std::to_string(version));
    }
    std::set<std::u16string> owners;
    for (const auto& [symbol, address] : record->exports) owners.insert(demangle(symbol).owner);
    for (const auto& owner : owners) {
      auto cls = find_class(env, to_modified_utf8(owner).c_str(), loader);
      if (env->ExceptionCheck()) {
        // JNI exported-name resolution is lazy (JNI spec, Design / Resolving
        // Native Method Names). This eager pass must also defer a missing
        // superclass/interface of an optional export owner. HotSpot reports
        // that as NoClassDefFoundError caused by ClassNotFoundException.
        // Verification/initializer errors still fail; bindClass retries when
        // the app actually loads the class, preserving the real failure there.
        auto exception = env->ExceptionOccurred();
        env->ExceptionClear();
        auto missing = env->FindClass("java/lang/ClassNotFoundException");
        bool defer = missing && env->IsInstanceOf(exception, missing);
        auto definition = env->FindClass("java/lang/NoClassDefFoundError");
        if (!defer && missing && definition && env->IsInstanceOf(exception, definition)) {
          auto cause_method = env->GetMethodID(definition, "getCause", "()Ljava/lang/Throwable;");
          auto cause = cause_method ? env->CallObjectMethod(exception, cause_method) : nullptr;
          if (!env->ExceptionCheck()) defer = cause && env->IsInstanceOf(cause, missing);
          if (cause) env->DeleteLocalRef(cause);
        }
        if (definition) env->DeleteLocalRef(definition);
        if (missing) env->DeleteLocalRef(missing);
        if (!defer) {
          env->Throw(exception);
          env->DeleteLocalRef(exception);
          throw std::runtime_error("JNI export class binding failed");
        }
        env->DeleteLocalRef(exception);
        log::debug("java", "Deferring JNI exports for {}: missing class definition", to_modified_utf8(owner));
      }
      if (cls) env->DeleteLocalRef(cls);
    }
    {
      std::lock_guard lock(libraries_mutex);
      record->state = Loaded::State::Ready;
    }
    libraries_changed.notify_all();
    return true;
  } catch (const std::exception& e) {
    if (record) {
      std::lock_guard lock(libraries_mutex);
      record->state = Loaded::State::Failed;
      record->error = e.what();
    }
    libraries_changed.notify_all();
    if (error) *error = e.what();
    log::error("java", "load_guest_library: {}", e.what());
    return false;
  } catch (...) {
    constexpr auto message = "guest native library load raised an unknown native exception";
    if (record) {
      std::lock_guard lock(libraries_mutex);
      record->state = Loaded::State::Failed;
      record->error = message;
    }
    libraries_changed.notify_all();
    if (error) *error = message;
    log::error("java", "{}", message);
    return false;
  }
}
}  // namespace venture::java

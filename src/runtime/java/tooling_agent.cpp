// Generic hosted-JVM method interception. Standard JNI/JVMTI only.
#include "tooling_agent.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
namespace {
JavaVM* machine = nullptr;
jvmtiEnv* tooling = nullptr;
std::atomic<bool> alive{false};
std::atomic<jint> status{JVMTI_ERROR_NOT_AVAILABLE};
std::mutex registry_mutex, mutation_mutex;
thread_local std::unordered_set<jmethodID> executing;
struct Signature {
  std::vector<char> arguments;
  char result{};
  static char type(const char*& cursor, bool allow_void) {
    const char first = *cursor++;
    if (first == '[') { type(cursor, false); return 'L'; }
    if (first == 'L') {
      while (*cursor && *cursor != ';') ++cursor;
      if (*cursor++ != ';') throw 1;
      return 'L';
    }
    if (std::string("ZBCSIJFD").find(first) != std::string::npos) return first;
    if (first == 'V' && allow_void) return first;
    throw 1;
  }
  explicit Signature(const char* descriptor) {
    auto cursor = descriptor;
    if (*cursor++ != '(') throw 1;
    while (*cursor && *cursor != ')') arguments.push_back(type(cursor, false));
    if (*cursor++ != ')') throw 1;
    result = type(cursor, true);
    if (*cursor) throw 1;
  }
};
struct Hook {
  jmethodID method{};
  jlocation location{};
  jclass owner{};
  bool is_static{};
  Signature signature;
  VentureJvmtiCallback callback{};
  void* context{};
  VentureJvmtiRelease release{};
  explicit Hook(const char* descriptor): signature(descriptor) {}
  ~Hook() {
    if (owner && alive.load() && machine) {
      JNIEnv* env{}; bool attached = false;
      auto code = machine->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
      if (code == JNI_EDETACHED) {
        attached = machine->AttachCurrentThreadAsDaemon(reinterpret_cast<void**>(&env), nullptr) == JNI_OK;
        if (!attached) env = nullptr;
      }
      if (env) env->DeleteGlobalRef(owner);
      if (attached) machine->DetachCurrentThread();
    }
    if (release) release(context);
  }
};
std::unordered_map<jmethodID, std::shared_ptr<Hook>> hooks;
std::shared_ptr<Hook> find(jmethodID method) {
  std::lock_guard lock(registry_mutex);
  auto found = hooks.find(method);
  return found == hooks.end() ? nullptr : found->second;
}
int dispatch(const std::shared_ptr<Hook>& hook, JNIEnv* env, jobject receiver,
             const jvalue* arguments, jint count, jvalue* result) {
  if (!hook || executing.contains(hook->method)) return 0;
  if (static_cast<std::size_t>(count) != hook->signature.arguments.size()) return -JVMTI_ERROR_ILLEGAL_ARGUMENT;
  executing.insert(hook->method);
  struct Leave { jmethodID method; ~Leave() { executing.erase(method); } } leave{hook->method};
  try { return hook->callback(env, receiver, arguments, count, result, hook->context); }
  catch (...) {
    if (!env->ExceptionCheck()) {
      auto cls = env->FindClass("java/lang/InternalError");
      if (cls) { env->ThrowNew(cls, "hosted JVM method callback failed"); env->DeleteLocalRef(cls); }
    }
    return 2;
  }
}
jvmtiError local(jthread thread, jint slot, char type, jvalue& value) {
  switch (type) {
    case 'L': return tooling->GetLocalObject(thread, 0, slot, &value.l);
    case 'J': return tooling->GetLocalLong(thread, 0, slot, &value.j);
    case 'F': return tooling->GetLocalFloat(thread, 0, slot, &value.f);
    case 'D': return tooling->GetLocalDouble(thread, 0, slot, &value.d);
    default: return tooling->GetLocalInt(thread, 0, slot, &value.i);
  }
}
jvmtiError return_value(jthread thread, char type, const jvalue& value) {
  switch (type) {
    case 'V': return tooling->ForceEarlyReturnVoid(thread);
    case 'L': return tooling->ForceEarlyReturnObject(thread, value.l);
    case 'J': return tooling->ForceEarlyReturnLong(thread, value.j);
    case 'F': return tooling->ForceEarlyReturnFloat(thread, value.f);
    case 'D': return tooling->ForceEarlyReturnDouble(thread, value.d);
    case 'Z': return tooling->ForceEarlyReturnInt(thread, value.z ? 1 : 0);
    case 'B': return tooling->ForceEarlyReturnInt(thread, value.b);
    case 'C': return tooling->ForceEarlyReturnInt(thread, value.c);
    case 'S': return tooling->ForceEarlyReturnInt(thread, value.s);
    default: return tooling->ForceEarlyReturnInt(thread, value.i);
  }
}
void JNICALL breakpoint(jvmtiEnv*, JNIEnv* env, jthread thread, jmethodID method, jlocation) {
  auto hook = find(method);
  if (!hook || executing.contains(method)) return;
  jthrowable saved = env->ExceptionOccurred();
  if (saved) env->ExceptionClear();
  std::vector<jvalue> arguments(hook->signature.arguments.size());
  jint slot = hook->is_static ? 0 : 1;
  jobject receiver = hook->owner;
  jvmtiError error = JVMTI_ERROR_NONE;
  if (!hook->is_static) error = tooling->GetLocalObject(thread, 0, 0, &receiver);
  for (std::size_t i = 0; error == JVMTI_ERROR_NONE && i < arguments.size(); ++i) {
    const char type = hook->signature.arguments[i];
    error = local(thread, slot, type, arguments[i]);
    slot += type == 'J' || type == 'D' ? 2 : 1;
  }
  if (error == JVMTI_ERROR_NONE) {
    jvalue result{};
    const int handled = dispatch(hook, env, receiver, arguments.data(), static_cast<jint>(arguments.size()), &result);
    if (handled == 1) error = return_value(thread, hook->signature.result, result);
    else if (handled == 2) {
      // An exception must unwind from the original bytecode location. Forcing
      // a return first advances the caller beyond its protected call site.
      if (!env->ExceptionCheck()) error = JVMTI_ERROR_INTERNAL;
    } else if (handled != 0) error = JVMTI_ERROR_INTERNAL;
  }
  if (saved) {
    env->ExceptionClear();
    env->Throw(saved);
    env->DeleteLocalRef(saved);
  }
  if (error != JVMTI_ERROR_NONE) status.store(error);
}
void JNICALL vm_death(jvmtiEnv*, JNIEnv*) { alive = false; }
}
extern "C" JNIEXPORT jint JNICALL Agent_OnLoad(JavaVM* vm, char*, void*) {
  machine = vm;
  if (vm->GetEnv(reinterpret_cast<void**>(&tooling), JVMTI_VERSION_1_2) != JNI_OK) return JNI_OK;
  jvmtiCapabilities capabilities{};
  capabilities.can_generate_breakpoint_events = 1;
  capabilities.can_access_local_variables = 1;
  capabilities.can_force_early_return = 1;
  status = tooling->AddCapabilities(&capabilities);
  if (status != JVMTI_ERROR_NONE) return JNI_OK;
  jvmtiEventCallbacks callbacks{};
  callbacks.Breakpoint = breakpoint;
  callbacks.VMDeath = vm_death;
  status = tooling->SetEventCallbacks(&callbacks, sizeof(callbacks));
  if (status != JVMTI_ERROR_NONE) return JNI_OK;
  status = tooling->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_BREAKPOINT, nullptr);
  if (status != JVMTI_ERROR_NONE) return JNI_OK;
  status = tooling->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_VM_DEATH, nullptr);
  alive = status == JVMTI_ERROR_NONE;
  return JNI_OK;
}
extern "C" JNIEXPORT void JNICALL Agent_OnUnload(JavaVM*) {
  alive = false;
  std::unordered_map<jmethodID, std::shared_ptr<Hook>> removed;
  { std::lock_guard lock(registry_mutex); removed.swap(hooks); }
  tooling = nullptr;
}
extern "C" JNIEXPORT jvmtiEnv* VentureJvmtiEnvironment() { return alive.load() ? tooling : nullptr; }
extern "C" JNIEXPORT jint VentureJvmtiStatus() { return status.load(); }
extern "C" JNIEXPORT jint VentureJvmtiInstallHook(jmethodID method, VentureJvmtiCallback callback,
                                                  void* context, VentureJvmtiRelease release) {
  if (!alive || !tooling || status != JVMTI_ERROR_NONE) return JVMTI_ERROR_NOT_AVAILABLE;
  if (!method || !callback) return JVMTI_ERROR_NULL_POINTER;
  std::unique_lock mutation(mutation_mutex);
  char* name{}; char* descriptor{};
  auto error = tooling->GetMethodName(method, &name, &descriptor, nullptr);
  if (error != JVMTI_ERROR_NONE) return error;
  const bool constructor = name && name[0] == '<';
  tooling->Deallocate(reinterpret_cast<unsigned char*>(name));
  std::shared_ptr<Hook> hook;
  try { hook = std::make_shared<Hook>(descriptor); }
  catch (...) { error = JVMTI_ERROR_ILLEGAL_ARGUMENT; }
  tooling->Deallocate(reinterpret_cast<unsigned char*>(descriptor));
  if (error != JVMTI_ERROR_NONE) return error;
  if (constructor) return JVMTI_ERROR_ILLEGAL_ARGUMENT;
  hook->method = method;
  jint modifiers{};
  error = tooling->GetMethodModifiers(method, &modifiers);
  if (error != JVMTI_ERROR_NONE) return error;
  hook->is_static = (modifiers & 8) != 0;
  jlocation end{};
  error = tooling->GetMethodLocation(method, &hook->location, &end);
  if (error != JVMTI_ERROR_NONE) return error;
  JNIEnv* env{};
  if (machine->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) return JVMTI_ERROR_UNATTACHED_THREAD;
  jclass owner{};
  error = tooling->GetMethodDeclaringClass(method, &owner);
  if (error != JVMTI_ERROR_NONE) return error;
  hook->owner = static_cast<jclass>(env->NewGlobalRef(owner));
  env->DeleteLocalRef(owner);
  if (!hook->owner) return JVMTI_ERROR_OUT_OF_MEMORY;
  error = tooling->SetBreakpoint(method, hook->location);
  if (error != JVMTI_ERROR_NONE && error != JVMTI_ERROR_DUPLICATE) return error;
  hook->callback = callback; hook->context = context; hook->release = release;
  std::shared_ptr<Hook> prior;
  { std::lock_guard lock(registry_mutex); prior = hooks[method]; hooks[method] = hook; }
  mutation.unlock();
  prior.reset();
  return JVMTI_ERROR_NONE;
}
extern "C" JNIEXPORT jint VentureJvmtiRemoveHook(jmethodID method) {
  if (!alive || !tooling) return JVMTI_ERROR_NOT_AVAILABLE;
  std::unique_lock mutation(mutation_mutex);
  std::shared_ptr<Hook> removed;
  { std::lock_guard lock(registry_mutex); auto found = hooks.find(method);
    if (found == hooks.end()) return JVMTI_ERROR_NOT_FOUND;
    removed = found->second; hooks.erase(found); }
  const auto error = tooling->ClearBreakpoint(method, removed->location);
  mutation.unlock();
  removed.reset();
  return error;
}
extern "C" JNIEXPORT jint VentureJvmtiDispatchHook(JNIEnv* env, jmethodID method, jobject receiver,
                                                   const jvalue* arguments, jint count, jvalue* result) {
  if (!env || !result || count < 0 || (count && !arguments)) return -JVMTI_ERROR_ILLEGAL_ARGUMENT;
  return dispatch(find(method), env, receiver, arguments, count, result);
}

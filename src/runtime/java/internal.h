#pragma once
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <unordered_map>

#include "java/abi.h"
#include "java/jvm.h"

namespace venture::java {
struct ThreadState {
  JNIEnv* env = nullptr;
  bool attached_here = false;
  GuestAddr wrapper = 0;
  jobject loader = nullptr;  // borrowed from transition record, valid throughout transition
  bool has_loader = false;
  unsigned native_depth = 0;
  std::shared_ptr<void> a32_state;  // JNI token frames, owned through detach
  ~ThreadState();
};
ThreadState& thread_state();
JNIEnv* env_no_attach();
jint attach_host(bool daemon, JavaVMAttachArgs* args, JNIEnv** out);
jint detach_host();
void throw_java(JNIEnv*, const char* type, const char* message) noexcept;
void throw_thread_death(JNIEnv*) noexcept;
void init_bridge(JNIEnv*);
jobject current_loader();

class LoaderScope {
 public:
  explicit LoaderScope(jobject loader) : old_(thread_state().loader), had_loader_(thread_state().has_loader) {
    thread_state().loader = loader;
    thread_state().has_loader = true;
  }
  ~LoaderScope() {
    thread_state().loader = old_;
    thread_state().has_loader = had_loader_;
  }

 private:
  jobject old_;
  bool had_loader_;
};
class LocalFrame {
 public:
  explicit LocalFrame(JNIEnv* env, int capacity = 32) : env_(env) {
    valid_ = env->PushLocalFrame(capacity) == 0;
  }
  ~LocalFrame() {
    if (valid_) env_->PopLocalFrame(nullptr);
  }
  bool valid() const { return valid_; }
  jobject pop(jobject result) {
    if (!valid_) return nullptr;
    valid_ = false;
    return env_->PopLocalFrame(result);
  }

 private:
  JNIEnv* env_;
  bool valid_;
};
enum class LoadRedirect { None, Library, Path, Reflection };
struct MethodInfo {
  std::string signature;
  Descriptor descriptor;
  bool is_static;
  LoadRedirect redirect = LoadRedirect::None;
};
void remember_method(jmethodID, std::string_view signature, bool is_static,
                     LoadRedirect redirect = LoadRedirect::None);
MethodInfo method_info(jmethodID);
std::string reflected_descriptor(JNIEnv*, jobject executable);
jclass find_class(JNIEnv*, const char* name, jobject loader);
void bind_class(JNIEnv*, jclass cls);
jint register_natives(JNIEnv*, jclass, const JNINativeMethod*, jint);  // fnPtr holds guest address
jint unregister_natives(JNIEnv*, jclass);
LoadRedirect reflected_load_redirect(JNIEnv*, jobject);
jobject invoke_reflective(JNIEnv*, jobject method, jobject target, jobjectArray args);
// Kept independently callable for FakeCpu tests; handlers never use host x64 tables as guest tables.
struct JniEntry {
  unsigned slot;
  const char* name;
  hle::Handler handler;
};
std::span<const JniEntry> jni_entries();
}  // namespace venture::java

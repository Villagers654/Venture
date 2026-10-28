// The embedded host JVM (OpenJDK HotSpot) running the game's Java code.
//
// The APK's classes.dex is converted to JVM bytecode at install time (cached by
// DEX hash + converter version). The JVM's class path holds the Venture
// Android framework (venture-framework.jar) and the converted app jar, loaded
// through a dalvik.system.PathClassLoader-compatible app class loader.
//
// Guest native code never sees host JNI function tables: it receives a guest
// JavaVM* and per-thread guest JNIEnv* whose function tables are HLE thunks
// that forward to the host JNIEnv of the calling thread.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/guest.h"

struct JNIEnv_;
struct JavaVM_;

namespace venture {
class GuestThread;
}

namespace venture::java {

struct JvmOptions {
  std::string java_home;                 // JDK/JRE root containing bin/server/jvm.dll or lib/server/libjvm.so
  std::vector<std::string> class_path;   // framework jar first, then converted app jar(s)
  std::vector<std::string> extra_options;  // e.g. -Xcheck:jni in debug builds
  std::string agent_path;  // bundled JVMTI support; optional only for standalone fixture JVMs
};

// Starts the JVM once per process. Thread-safe.
bool start_jvm(const JvmOptions& options, std::string* error);
JavaVM_* host_vm();
// Host JNIEnv* of the calling host thread, attaching it to the JVM if needed.
JNIEnv_* host_env();
// Call only during process shutdown, after guest work and callbacks have stopped.
// HotSpot cannot be restarted in the same process after this succeeds.
bool stop_jvm(std::string* error);
// App-loader fallback for FindClass on native-created threads. Retains a global
// reference; explicit transition loaders take precedence. nullptr = bootstrap.
void set_app_class_loader(void* loader);

// Guest-visible JavaVM* (process-wide) and JNIEnv* (per guest thread).
GuestAddr guest_vm();
GuestAddr guest_env(GuestThread& thread);
// Invocation APIs exported by Android's VM implementation.
void register_jni_library();

// Hooks for host natives of framework classes (com.venture.*, android.*
// natives implemented by Venture). Called once after the JVM starts.
using NativeRegistrar = void (*)(JNIEnv_* env);
void add_framework_natives(NativeRegistrar registrar);

// Loads a guest native library for a Java caller (System.loadLibrary/load in
// app code is redirected here): links it through the guest Linker, runs
// JNI_OnLoad, binds exported Java_* symbols of classes visible to `loader`.
// Returns false and sets `error` like UnsatisfiedLinkError.
bool load_guest_library(const std::string& name_or_path, void* class_loader_global_ref, std::string* error);

}  // namespace venture::java

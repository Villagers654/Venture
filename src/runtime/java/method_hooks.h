#pragma once
#include <filesystem>
#include <span>
#include "java/abi.h"

namespace venture::java {
// Guest C ABI (libventure_java_bridge.so):
// int VentureJavaInstallHook(jmethodID, callback, void* user, release);
// int callback(JNIEnv*, jobject, const jvalue* args, jvalue* result, void* user);
// void release(void* user); int VentureJavaRemoveHook(jmethodID);
// Callback returns 0/original, 1/replacement or 2/pending Java exception.
// Successful install owns a context until the last in-flight call finishes,
// including after replacement/removal. Keep callback code/data alive until its
// nonthrowing release notification returns. Failed installs consume nothing.
// A null release callback requires process-lifetime callback storage. Shutdown
// suppresses guest notifications after guest execution has stopped.
void initialize_method_hooks(const std::filesystem::path& agent);
void stop_method_hooks();
void register_method_hook_library();
// Returns 0 for original, 1 for replacement, 2 for pending Java exception.
jint dispatch_method_hook(JNIEnv*, jmethodID, jobject, std::span<const jvalue>, jvalue*);
}  // namespace venture::java

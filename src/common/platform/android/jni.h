// SPDX-License-Identifier: GPL-3.0-or-later
// The Venture app's Java side as the C++ core sees it: the process's JavaVM,
// a JNIEnv for any thread, and global references to the app's helper classes
// (org.venture_emu.venture.*), resolved once by Native.nativeInit on a thread
// that has the app's class loader. Worker threads never call FindClass.
#pragma once
#include <jni.h>

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace venture::android {
struct App {
  std::filesystem::path data, cache, apk;  // filesDir/venture, cacheDir, the APK itself
  int sdk = 0;                             // Build.VERSION.SDK_INT
};
struct Classes {
  jclass http = nullptr, transfer = nullptr, keys = nullptr, sign_in = nullptr, device = nullptr,
         installer = nullptr;
};
// Sets the process's JavaVM, Context and helper classes, once; called on a
// thread with the app's class loader (Native.nativeInit in the Venture app,
// PlatformInit in a game Venture installed).
void attach(JNIEnv*, jobject context, App);
// Whether attach ran.
bool ready();
const App& app();
const Classes& classes();
JavaVM* vm();
// The calling thread's environment; a native thread is attached for its
// lifetime and detached when it exits.
JNIEnv* env();
// The application Context (a global reference).
jobject context();

// UTF-8 <-> Java strings (Java's modified UTF-8 is never assumed).
std::string utf8(JNIEnv*, jstring);
jstring java_string(JNIEnv*, std::string_view);
jbyteArray java_bytes(JNIEnv*, std::span<const unsigned char>);
std::vector<unsigned char> bytes(JNIEnv*, jbyteArray);
// Clears a pending Java exception and returns its text (empty without one).
std::string take_exception(JNIEnv*);

// Opens a link in the device's browser (Device.openUrl); false when it cannot.
bool open_url(std::string_view url);

// A local reference frame for the scope: every local made inside is freed.
class LocalFrame {
public:
  explicit LocalFrame(JNIEnv* env, int capacity = 16) : env_(env) { env_->PushLocalFrame(capacity); }
  ~LocalFrame() { env_->PopLocalFrame(nullptr); }
  LocalFrame(const LocalFrame&) = delete;
  LocalFrame& operator=(const LocalFrame&) = delete;

private:
  JNIEnv* env_;
};
}  // namespace venture::android
